/**
 * PowerVR2 Tile Accelerator FIFO Parser
 *
 * Receives 32-byte packets from Store Queue flushes or direct TA writes,
 * parses them according to the PVR2 TA protocol, and builds triangle
 * lists for the OpenGL renderer.
 */

#include "hal/pvr2.h"
#include "hal/dc_hardware.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static struct {
    /* Current polygon header state */
    uint32_t pcw;
    uint32_t isp;
    uint32_t tsp;
    uint32_t tcw;
    int current_list;
    int vert_type;
    int vert_size;       /* 32 or 64 bytes */

    /* Face color (for intensity vertex types) */
    float face_r, face_g, face_b, face_a;

    /* Strip → triangle conversion */
    PVR2RenderVertex strip_prev[2];
    int strip_count;

    /* 64-byte parameter buffering */
    uint32_t pending[8];
    int awaiting_second;   /* 0=no, 1=poly header, 2=vertex */

    /* Triangle buffers per list type */
    PVR2TriList lists[PVR2_NUM_LISTS];

    /* Statistics */
    int total_vertices;
    int total_polygons;
    int total_strips;
    int packets_received;

    /* Logging */
    int log_countdown;
} g_ta;


/* ========== Helpers ========== */

static void unpack_color(uint32_t packed, float *r, float *g, float *b, float *a) {
    *a = ((packed >> 24) & 0xFF) / 255.0f;
    *r = ((packed >> 16) & 0xFF) / 255.0f;
    *g = ((packed >>  8) & 0xFF) / 255.0f;
    *b = ((packed >>  0) & 0xFF) / 255.0f;
}

static void emit_triangle_vertex(const PVR2RenderVertex *v) {
    int list = g_ta.current_list;
    if (list < 0 || list >= PVR2_NUM_LISTS) return;

    PVR2TriList *tl = &g_ta.lists[list];
    if (tl->count >= tl->capacity) return;

    tl->vertices[tl->count++] = *v;
}

static void process_strip_vertex(const PVR2RenderVertex *v, int end_of_strip) {
    if (g_ta.strip_count >= 2) {
        /* Emit triangle from strip */
        if ((g_ta.strip_count & 1) == 0) {
            emit_triangle_vertex(&g_ta.strip_prev[0]);
            emit_triangle_vertex(&g_ta.strip_prev[1]);
            emit_triangle_vertex(v);
        } else {
            emit_triangle_vertex(&g_ta.strip_prev[1]);
            emit_triangle_vertex(&g_ta.strip_prev[0]);
            emit_triangle_vertex(v);
        }
        g_ta.total_polygons++;
    }

    g_ta.strip_prev[0] = g_ta.strip_prev[1];
    g_ta.strip_prev[1] = *v;
    g_ta.strip_count++;
    g_ta.total_vertices++;

    if (end_of_strip) {
        g_ta.strip_count = 0;
        g_ta.total_strips++;
    }
}


/* ========== Vertex Type Determination ========== */

static int ta_get_vert_type(uint32_t pcw) {
    int para_type = (pcw >> 29) & 7;
    int list_type = (pcw >> 24) & 7;
    int texture   = (pcw >>  3) & 1;
    int col_type  = (pcw >>  4) & 3;
    int volume    = (pcw >>  6) & 1;
    int uv_16bit  = pcw & 1;

    /* Modifier volumes */
    if (list_type == PVR2_LIST_OPAQUE_MOD || list_type == PVR2_LIST_TRANS_MOD)
        return 17;

    /* Sprites */
    if (para_type == TA_PARAM_SPRITE)
        return texture ? 16 : 15;

    /* Two-volume */
    if (volume) {
        if (texture) {
            if (col_type == 0) return uv_16bit ? 12 : 11;
            return uv_16bit ? 14 : 13;
        }
        if (col_type == 0) return 9;
        return 10;
    }

    /* Textured */
    if (texture) {
        if (col_type == 0) return uv_16bit ? 4 : 3;
        if (col_type == 1) return uv_16bit ? 6 : 5;
        return uv_16bit ? 8 : 7;
    }

    /* Non-textured */
    if (col_type == 0) return 0;
    if (col_type == 1) return 1;
    return 2;
}

static int ta_vert_size(int vert_type) {
    switch (vert_type) {
    case 5: case 6:      /* Textured, float color */
    case 11: case 12:    /* Two-volume, textured, packed */
    case 13: case 14:    /* Two-volume, textured, intensity */
    case 15: case 16:    /* Sprite */
        return 64;
    default:
        return 32;
    }
}

static int ta_poly_size(uint32_t pcw) {
    int col_type = (pcw >> 4) & 3;
    int volume   = (pcw >> 6) & 1;
    int offset   = (pcw >> 2) & 1;
    int texture  = (pcw >> 3) & 1;

    /* Intensity + textured + offset → 64 bytes */
    if (col_type == 2 && texture && offset) return 64;
    /* Two-volume + intensity → 64 bytes */
    if (volume && col_type == 2) return 64;

    return 32;
}


/* ========== Parameter Processing ========== */

static void ta_process_polygon(const uint32_t *data, int size) {
    g_ta.pcw = data[0];
    g_ta.isp = data[1];
    g_ta.tsp = data[2];
    g_ta.tcw = data[3];
    g_ta.current_list = (g_ta.pcw >> 24) & 7;
    g_ta.vert_type = ta_get_vert_type(g_ta.pcw);
    g_ta.vert_size = ta_vert_size(g_ta.vert_type);
    g_ta.strip_count = 0;

    /* Extract face color for intensity mode */
    int col_type = (g_ta.pcw >> 4) & 3;
    if (col_type == 2 || col_type == 3) {
        union { uint32_t u; float f; } c;
        if (size >= 64) {
            c.u = data[8];  g_ta.face_a = c.f;
            c.u = data[9];  g_ta.face_r = c.f;
            c.u = data[10]; g_ta.face_g = c.f;
            c.u = data[11]; g_ta.face_b = c.f;
        } else {
            c.u = data[4]; g_ta.face_a = c.f;
            c.u = data[5]; g_ta.face_r = c.f;
            c.u = data[6]; g_ta.face_g = c.f;
            c.u = data[7]; g_ta.face_b = c.f;
        }
    }
}

/* TA diagnostic counters (file scope) */
static int type_counts[8];
static int eos_count;
static int list_type_hist[8];

static int vert_log = 0;

static void ta_process_vertex(const uint32_t *data) {
    PVR2RenderVertex v = {0};
    union { uint32_t u; float f; } c;
    int end_of_strip = (data[0] >> 28) & 1;
    if (end_of_strip) eos_count++;

    if (vert_log < 10) {
        vert_log++;
        union { uint32_t u; float f; } x, y, z;
        x.u = data[1]; y.u = data[2]; z.u = data[3];
        printf("[VERT] #%d type=%d eos=%d PCW=0x%08X pos=(%.1f, %.1f, %.1f) list=%d strip=%d\n",
               vert_log, g_ta.vert_type, end_of_strip, data[0],
               x.f, y.f, z.f, g_ta.current_list, g_ta.strip_count);
    }

    switch (g_ta.vert_type) {
    case 0: /* Non-textured, packed color */
        c.u = data[1]; v.x = c.f;
        c.u = data[2]; v.y = c.f;
        c.u = data[3]; v.z = c.f;
        unpack_color(data[6], &v.r, &v.g, &v.b, &v.a);
        break;

    case 1: /* Non-textured, float RGBA */
        c.u = data[1]; v.x = c.f;
        c.u = data[2]; v.y = c.f;
        c.u = data[3]; v.z = c.f;
        c.u = data[4]; v.a = c.f;
        c.u = data[5]; v.r = c.f;
        c.u = data[6]; v.g = c.f;
        c.u = data[7]; v.b = c.f;
        break;

    case 2: /* Non-textured, intensity */
        c.u = data[1]; v.x = c.f;
        c.u = data[2]; v.y = c.f;
        c.u = data[3]; v.z = c.f;
        c.u = data[6]; {
            float intensity = c.f;
            v.r = g_ta.face_r * intensity;
            v.g = g_ta.face_g * intensity;
            v.b = g_ta.face_b * intensity;
            v.a = g_ta.face_a;
        }
        break;

    case 3: /* Textured, packed color, 32-bit UV */
        c.u = data[1]; v.x = c.f;
        c.u = data[2]; v.y = c.f;
        c.u = data[3]; v.z = c.f;
        c.u = data[4]; v.u = c.f;
        c.u = data[5]; v.v = c.f;
        unpack_color(data[6], &v.r, &v.g, &v.b, &v.a);
        break;

    case 4: /* Textured, packed color, 16-bit UV */
        c.u = data[1]; v.x = c.f;
        c.u = data[2]; v.y = c.f;
        c.u = data[3]; v.z = c.f;
        /* 16-bit UV packed in data[4] - skip for now */
        v.u = 0; v.v = 0;
        unpack_color(data[6], &v.r, &v.g, &v.b, &v.a);
        break;

    case 5: /* Textured, float RGBA, 32-bit UV (64-byte) */
        c.u = data[1]; v.x = c.f;
        c.u = data[2]; v.y = c.f;
        c.u = data[3]; v.z = c.f;
        c.u = data[4]; v.u = c.f;
        c.u = data[5]; v.v = c.f;
        /* Colors in second 32 bytes (data[8..11]) */
        c.u = data[8];  v.a = c.f;
        c.u = data[9];  v.r = c.f;
        c.u = data[10]; v.g = c.f;
        c.u = data[11]; v.b = c.f;
        break;

    case 7: /* Textured, intensity, 32-bit UV */
        c.u = data[1]; v.x = c.f;
        c.u = data[2]; v.y = c.f;
        c.u = data[3]; v.z = c.f;
        c.u = data[4]; v.u = c.f;
        c.u = data[5]; v.v = c.f;
        c.u = data[6]; {
            float intensity = c.f;
            v.r = g_ta.face_r * intensity;
            v.g = g_ta.face_g * intensity;
            v.b = g_ta.face_b * intensity;
            v.a = g_ta.face_a;
        }
        break;

    case 8: /* Textured, intensity, 16-bit UV */
        c.u = data[1]; v.x = c.f;
        c.u = data[2]; v.y = c.f;
        c.u = data[3]; v.z = c.f;
        v.u = 0; v.v = 0;
        c.u = data[6]; {
            float intensity = c.f;
            v.r = g_ta.face_r * intensity;
            v.g = g_ta.face_g * intensity;
            v.b = g_ta.face_b * intensity;
            v.a = g_ta.face_a;
        }
        break;

    default:
        /* Unsupported vertex type - use white with position */
        c.u = data[1]; v.x = c.f;
        c.u = data[2]; v.y = c.f;
        c.u = data[3]; v.z = c.f;
        v.r = v.g = v.b = v.a = 1.0f;
        break;
    }

    process_strip_vertex(&v, end_of_strip);
}


/* ========== Packet Entry Point ========== */

/* The list a game is currently filling. An End Of List parameter carries no
 * usable list type of its own, so the interrupt it raises is the one for
 * whatever list the polygons before it belonged to. */
static int open_list = 0;

static void ta_process_packet(const uint32_t *data) {
    int para_type = (data[0] >> 29) & 7;
    type_counts[para_type]++;

    switch (para_type) {
    case TA_PARAM_END_OF_LIST: {
        /* One interrupt per list type, and the game waits for the one
         * belonging to the list it just filled. Opaque, opaque modifier,
         * translucent and translucent modifier are consecutive; punch-through
         * sits on its own bit. */
        static const int eol_bit[8] = { 7, 8, 9, 10, 21, 21, 21, 21 };
        dc_hw_raise_istnrm(eol_bit[open_list & 7]);
        g_ta.strip_count = 0;
        break;
    }

    case TA_PARAM_USER_CLIP:
        /* TODO: implement user clipping */
        break;

    case TA_PARAM_POLYGON:
    case TA_PARAM_SPRITE: {
        int lt = (data[0] >> 24) & 7;
        open_list = lt;
        list_type_hist[lt]++;
        int poly_size = ta_poly_size(data[0]);
        if (poly_size == 64) {
            memcpy(g_ta.pending, data, 32);
            g_ta.awaiting_second = 1;
        } else {
            ta_process_polygon(data, 32);
        }
        break;
    }

    case TA_PARAM_VERTEX:
        if (g_ta.vert_size == 64) {
            memcpy(g_ta.pending, data, 32);
            g_ta.awaiting_second = 2;
        } else {
            ta_process_vertex(data);
        }
        break;

    default:
        /* Unknown parameter type - ignore */
        break;
    }
}


/* ========== Public API ========== */

void pvr2_ta_init(void) {
    memset(&g_ta, 0, sizeof(g_ta));

    for (int i = 0; i < PVR2_NUM_LISTS; i++) {
        g_ta.lists[i].capacity = PVR2_MAX_VERTICES;
        g_ta.lists[i].vertices = (PVR2RenderVertex *)calloc(
            PVR2_MAX_VERTICES, sizeof(PVR2RenderVertex));
        g_ta.lists[i].count = 0;
    }

    g_ta.current_list = -1;
    g_ta.log_countdown = 60;  /* Log stats for first 60 frames */
    printf("[PVR2] TA initialized\n");
}

void pvr2_ta_destroy(void) {
    for (int i = 0; i < PVR2_NUM_LISTS; i++) {
        free(g_ta.lists[i].vertices);
        g_ta.lists[i].vertices = NULL;
    }
}

static int ta_write_log = 0;

void pvr2_ta_write(const uint32_t *data) {
    g_ta.packets_received++;
    if (ta_write_log < 3) {
        ta_write_log++;
        int para_type = (data[0] >> 29) & 7;
        printf("[TA] packet #%d: PCW=0x%08X para_type=%d list=%d\n",
               g_ta.packets_received, data[0], para_type, (data[0] >> 24) & 7);
    }

    if (g_ta.awaiting_second) {
        /* Combine with pending data into full 64 bytes */
        uint32_t full[16];
        memcpy(full, g_ta.pending, 32);
        memcpy(full + 8, data, 32);
        int was_type = g_ta.awaiting_second;
        g_ta.awaiting_second = 0;

        if (was_type == 1) {
            ta_process_polygon(full, 64);
        } else {
            ta_process_vertex(full);
        }
    } else {
        ta_process_packet(data);
    }
}

void pvr2_ta_reset(void) {
    /* Log stats for first 3 frames */
    if (g_ta.log_countdown > 57) {
        printf("[PVR2] TA frame stats: %d pkts, %d verts, %d polys, %d strips\n",
               g_ta.packets_received, g_ta.total_vertices,
               g_ta.total_polygons, g_ta.total_strips);
        printf("[PVR2]   type breakdown: end=%d clip=%d _=%d _=%d poly=%d sprite=%d _=%d vert=%d\n",
               type_counts[0], type_counts[1], type_counts[2], type_counts[3],
               type_counts[4], type_counts[5], type_counts[6], type_counts[7]);
        printf("[PVR2]   eos_count=%d list_types: %d %d %d %d %d %d %d %d\n",
               eos_count, list_type_hist[0], list_type_hist[1], list_type_hist[2],
               list_type_hist[3], list_type_hist[4], list_type_hist[5],
               list_type_hist[6], list_type_hist[7]);
        printf("[PVR2]   List verts: opaque=%d trans=%d punchthru=%d\n",
               g_ta.lists[PVR2_LIST_OPAQUE].count,
               g_ta.lists[PVR2_LIST_TRANS].count,
               g_ta.lists[PVR2_LIST_PUNCHTHRU].count);
    }
    g_ta.log_countdown--;
    memset(type_counts, 0, sizeof(type_counts));
    memset(list_type_hist, 0, sizeof(list_type_hist));
    eos_count = 0;

    for (int i = 0; i < PVR2_NUM_LISTS; i++) {
        g_ta.lists[i].count = 0;
    }
    g_ta.strip_count = 0;
    g_ta.awaiting_second = 0;
    g_ta.total_vertices = 0;
    g_ta.total_polygons = 0;
    g_ta.total_strips = 0;
    g_ta.packets_received = 0;
    g_ta.current_list = -1;
}

const PVR2TriList* pvr2_ta_get_list(int list_type) {
    if (list_type < 0 || list_type >= PVR2_NUM_LISTS) return NULL;
    return &g_ta.lists[list_type];
}

void pvr2_ta_get_stats(int *packets, int *vertices, int *polygons) {
    if (packets)  *packets  = g_ta.packets_received;
    if (vertices) *vertices = g_ta.total_vertices;
    if (polygons) *polygons = g_ta.total_polygons;
}
