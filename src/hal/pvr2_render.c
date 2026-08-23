/**
 * PowerVR2 OpenGL Renderer
 *
 * Renders PVR2 triangle lists using OpenGL 3.3 core.
 * Currently renders colored triangles without texturing.
 */

#include "hal/pvr2.h"
#include "recompiler/sh4_cpu.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef HAS_OPENGL

#include <GL/glew.h>

static GLuint g_shader;
static GLuint g_vao;
static GLuint g_vbo;
static GLint  g_uniform_screen_size;
static int    g_width = 640;
static int    g_height = 480;

/* Vertex shader: PVR2 screen-space coords → NDC */
static const char *vert_shader_src =
    "#version 330 core\n"
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec2 aUV;\n"
    "layout(location = 2) in vec4 aColor;\n"
    "out vec4 vColor;\n"
    "uniform vec2 uScreenSize;\n"
    "void main() {\n"
    "    float x = (aPos.x / uScreenSize.x) * 2.0 - 1.0;\n"
    "    float y = 1.0 - (aPos.y / uScreenSize.y) * 2.0;\n"
    "    float z = clamp(aPos.z * 0.0001, 0.0, 1.0);\n"
    "    gl_Position = vec4(x, y, z, 1.0);\n"
    "    vColor = aColor;\n"
    "}\n";

/* Fragment shader: pass-through color */
static const char *frag_shader_src =
    "#version 330 core\n"
    "in vec4 vColor;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    FragColor = vColor;\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "[PVR2] Shader compile error: %s\n", log);
    }
    return shader;
}

int pvr2_render_init(int width, int height) {
    g_width = width;
    g_height = height;

    /* Initialize GLEW */
    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        fprintf(stderr, "[PVR2] GLEW init failed: %s\n", glewGetErrorString(err));
        return -1;
    }
    /* Clear any GL error from glewInit */
    while (glGetError() != GL_NO_ERROR) {}

    printf("[PVR2] OpenGL %s (%s)\n",
           glGetString(GL_VERSION), glGetString(GL_RENDERER));

    /* Create shader program */
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_shader_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_shader_src);

    g_shader = glCreateProgram();
    glAttachShader(g_shader, vs);
    glAttachShader(g_shader, fs);
    glLinkProgram(g_shader);

    GLint ok;
    glGetProgramiv(g_shader, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(g_shader, sizeof(log), NULL, log);
        fprintf(stderr, "[PVR2] Shader link error: %s\n", log);
        return -1;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    g_uniform_screen_size = glGetUniformLocation(g_shader, "uScreenSize");

    /* Create VAO/VBO */
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);

    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    /* Vertex layout: PVR2RenderVertex = { x,y,z, u,v, r,g,b,a } */
    /* Position (vec3) at location 0, offset 0 */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
        sizeof(PVR2RenderVertex), (void*)0);
    glEnableVertexAttribArray(0);

    /* UV (vec2) at location 1, offset 12 */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
        sizeof(PVR2RenderVertex), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    /* Color (vec4) at location 2, offset 20 */
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE,
        sizeof(PVR2RenderVertex), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);

    /* Initial GL state */
    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.1f, 1.0f);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    printf("[PVR2] Renderer initialized (%dx%d)\n", width, height);
    return 0;
}

void pvr2_render_destroy(void) {
    if (g_vbo) { glDeleteBuffers(1, &g_vbo); g_vbo = 0; }
    if (g_vao) { glDeleteVertexArrays(1, &g_vao); g_vao = 0; }
    if (g_shader) { glDeleteProgram(g_shader); g_shader = 0; }
}


/* ---- Framebuffer presentation ------------------------------------------ */

static GLuint g_fb_tex = 0;
static GLuint g_fb_shader = 0;
static GLuint g_fb_vao = 0, g_fb_vbo = 0;
static unsigned char *g_fb_pixels = NULL;
static int g_fb_logged = 0;

static const char *fb_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "out vec2 uv;\n"
    "void main(){ uv = vec2((pos.x+1.0)*0.5, 1.0-(pos.y+1.0)*0.5);\n"
    "             gl_Position = vec4(pos,0.0,1.0); }\n";

static const char *fb_frag_src =
    "#version 330 core\n"
    "in vec2 uv; out vec4 frag; uniform sampler2D fb;\n"
    "void main(){ frag = texture(fb, uv); }\n";

static void fb_init(void) {
    if (g_fb_tex)
        return;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, fb_vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fb_frag_src);
    g_fb_shader = glCreateProgram();
    glAttachShader(g_fb_shader, vs);
    glAttachShader(g_fb_shader, fs);
    glLinkProgram(g_fb_shader);
    glDeleteShader(vs);
    glDeleteShader(fs);

    static const float quad[] = { -1,-1,  1,-1,  -1, 1,   1,-1,  1, 1,  -1, 1 };
    glGenVertexArrays(1, &g_fb_vao);
    glGenBuffers(1, &g_fb_vbo);
    glBindVertexArray(g_fb_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_fb_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);

    glGenTextures(1, &g_fb_tex);
    glBindTexture(GL_TEXTURE_2D, g_fb_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

int pvr2_present_framebuffer(uint32_t fb_addr, uint32_t fb_ctrl, int width, int height) {
    const uint8_t *vram = sh4_get_vram_ptr();
    if (!vram || width <= 0 || height <= 0)
        return 0;

    fb_init();
    if (!g_fb_pixels)
        g_fb_pixels = (unsigned char *)malloc(1024 * 1024 * 4);
    if (!g_fb_pixels)
        return 0;

    /* FB_R_CTRL bits 2:0 select the scanout format. */
    int fmt = (int)(fb_ctrl & 0x7);
    uint32_t base = fb_addr & 0x00FFFFFF;
    if (base >= DC_VRAM_SIZE)
        base = 0;

    int nonzero = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t r = 0, g = 0, b = 0;
            if (fmt == 0 || fmt == 1) {                 /* RGB555 / RGB565 */
                uint32_t off = base + (uint32_t)(y * width + x) * 2;
                if (off + 1 >= DC_VRAM_SIZE) continue;
                uint16_t p = (uint16_t)(vram[off] | (vram[off + 1] << 8));
                if (fmt == 0) {
                    r = ((p >> 10) & 0x1F) << 3;
                    g = ((p >> 5) & 0x1F) << 3;
                    b = (p & 0x1F) << 3;
                } else {
                    r = ((p >> 11) & 0x1F) << 3;
                    g = ((p >> 5) & 0x3F) << 2;
                    b = (p & 0x1F) << 3;
                }
                if (p) nonzero++;
            } else if (fmt == 2) {                      /* RGB888, 3 bytes */
                uint32_t off = base + (uint32_t)(y * width + x) * 3;
                if (off + 2 >= DC_VRAM_SIZE) continue;
                b = vram[off]; g = vram[off + 1]; r = vram[off + 2];
                if (r | g | b) nonzero++;
            } else {                                    /* 0888 / 8888 */
                uint32_t off = base + (uint32_t)(y * width + x) * 4;
                if (off + 3 >= DC_VRAM_SIZE) continue;
                b = vram[off]; g = vram[off + 1]; r = vram[off + 2];
                if (r | g | b) nonzero++;
            }
            unsigned char *px = g_fb_pixels + ((size_t)y * width + x) * 4;
            px[0] = (unsigned char)r; px[1] = (unsigned char)g;
            px[2] = (unsigned char)b; px[3] = 255;
        }
    }

    if (g_fb_logged < 3) {
        g_fb_logged++;
        printf("[PVR2] present: addr 0x%06X fmt %d %dx%d, %d non-black pixels\n",
               base, fmt, width, height, nonzero);
    }

    glBindTexture(GL_TEXTURE_2D, g_fb_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, g_fb_pixels);

    glDisable(GL_DEPTH_TEST);
    glUseProgram(g_fb_shader);
    glBindVertexArray(g_fb_vao);
    glBindTexture(GL_TEXTURE_2D, g_fb_tex);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    return nonzero;
}

void pvr2_render_frame(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(g_shader);
    glUniform2f(g_uniform_screen_size, (float)g_width, (float)g_height);

    glBindVertexArray(g_vao);

    /* Draw lists in order: opaque → punch-through → translucent */
    int draw_order[] = {
        PVR2_LIST_OPAQUE,
        PVR2_LIST_PUNCHTHRU,
        PVR2_LIST_TRANS
    };

    for (int i = 0; i < 3; i++) {
        const PVR2TriList *list = pvr2_ta_get_list(draw_order[i]);
        if (!list || list->count == 0) continue;

        /* Upload vertex data */
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(list->count * sizeof(PVR2RenderVertex)),
                     list->vertices, GL_STREAM_DRAW);

        /* Blending for translucent list */
        if (draw_order[i] == PVR2_LIST_TRANS) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        } else {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }

        glDrawArrays(GL_TRIANGLES, 0, list->count);
    }

    /* Reset state */
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    glBindVertexArray(0);
    glUseProgram(0);
}

void pvr2_render_resize(int width, int height) {
    g_width = width;
    g_height = height;
    glViewport(0, 0, width, height);
}

#else /* !HAS_OPENGL */

int pvr2_render_init(int w, int h) {
    (void)w; (void)h;
    printf("[PVR2] Renderer disabled (no OpenGL)\n");
    return 0;
}
void pvr2_render_destroy(void) {}
void pvr2_render_frame(void) {}
int pvr2_present_framebuffer(uint32_t a, uint32_t c, int w, int h) {
    (void)a; (void)c; (void)w; (void)h; return 0;
}
void pvr2_render_resize(int w, int h) { (void)w; (void)h; }

#endif /* HAS_OPENGL */
