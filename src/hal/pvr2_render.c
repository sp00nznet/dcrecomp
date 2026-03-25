/**
 * PowerVR2 OpenGL Renderer
 *
 * Renders PVR2 triangle lists using OpenGL 3.3 core.
 * Currently renders colored triangles without texturing.
 */

#include "hal/pvr2.h"
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
void pvr2_render_resize(int w, int h) { (void)w; (void)h; }

#endif /* HAS_OPENGL */
