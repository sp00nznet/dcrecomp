/**
 * PowerVR2 (Holly) GPU - Tile Accelerator & Renderer
 *
 * Handles TA FIFO packet parsing and OpenGL rendering for the
 * Dreamcast's PowerVR2 GPU.
 */

#ifndef PVR2_H
#define PVR2_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum vertices per frame per list */
#define PVR2_MAX_VERTICES  300000

/* Polygon list types */
#define PVR2_LIST_OPAQUE        0
#define PVR2_LIST_OPAQUE_MOD    1
#define PVR2_LIST_TRANS         2
#define PVR2_LIST_TRANS_MOD     3
#define PVR2_LIST_PUNCHTHRU     4
#define PVR2_NUM_LISTS          5

/* TA parameter types (bits [31:29] of PCW) */
#define TA_PARAM_END_OF_LIST    0
#define TA_PARAM_USER_CLIP      1
#define TA_PARAM_OBJ_LIST_SET   2
#define TA_PARAM_POLYGON        4
#define TA_PARAM_SPRITE         5
#define TA_PARAM_VERTEX         7

/* Vertex for rendering (strip→triangle converted) */
typedef struct {
    float x, y, z;       /* Screen-space coords (z = 1/w) */
    float u, v;          /* Texture coordinates */
    float r, g, b, a;    /* Base color (RGBA 0-1) */
} PVR2RenderVertex;

/* Per-list triangle buffer */
typedef struct {
    PVR2RenderVertex *vertices;
    int count;
    int capacity;
} PVR2TriList;

/* ========== TA FIFO API ========== */

/* Initialize TA state */
void pvr2_ta_init(void);

/* Destroy TA state */
void pvr2_ta_destroy(void);

/* Submit 32 bytes (8 x uint32) to the TA FIFO */
void pvr2_ta_write(const uint32_t *data);

/* Reset TA for new frame (called after render) */
void pvr2_ta_reset(void);

/* Get the triangle list for a given list type */
const PVR2TriList* pvr2_ta_get_list(int list_type);

/* Get TA statistics for current frame */
void pvr2_ta_get_stats(int *packets, int *vertices, int *polygons);

/* ========== OpenGL Renderer API ========== */

/* Initialize OpenGL renderer (call after GL context created) */
int pvr2_render_init(int width, int height);

/* Destroy renderer */
void pvr2_render_destroy(void);

/* Render all submitted geometry to the current framebuffer */
void pvr2_render_frame(void);

/* Write the frame just rendered to a binary PPM. Returns 0 on success.
 * Driven by DCRECOMP_SCREENSHOT and DCRECOMP_SCREENSHOT_FRAME. */
int pvr2_screenshot(const char *path);

/* True once the tile accelerator has produced geometry. After that the
 * rendered frame is the picture and the framebuffer must not be drawn over
 * it - on hardware the render lands in the framebuffer, but our renderer
 * draws to the window instead, so the two would fight. */
int pvr2_ta_has_drawn(void);

/* Draw the PVR2 scanout framebuffer as a fullscreen quad. `fb_addr` is FB_R_SOF1
 * and `fb_ctrl` is FB_R_CTRL, whose low bits select the pixel format. Returns
 * the number of non-black pixels, which is a useful "is anything there yet"
 * signal during bring-up. */
int pvr2_present_framebuffer(uint32_t fb_addr, uint32_t fb_ctrl, int width, int height);

/* Resize viewport */
void pvr2_render_resize(int width, int height);

#endif /* PVR2_H */
