/**
 * SDL2 Platform Layer
 *
 * Provides window creation, input handling, and audio output using SDL2.
 * This is the bridge between the Dreamcast HAL and the host PC.
 */

#include "recompiler/sh4_cpu.h"
#include "hal/dc_hardware.h"
#include <stdio.h>
#include <stdbool.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* Forward declarations - will use SDL2 when available */
/* For initial build, we use stub implementations */

#ifndef HAS_SDL2

/* Stub platform layer for initial build without SDL2 */

typedef struct {
    int width;
    int height;
    bool fullscreen;
    bool running;
    uint64_t frame_count;
} PlatformState;

static PlatformState g_platform = {0};

int platform_init(int width, int height, const char *title) {
    g_platform.width = width;
    g_platform.height = height;
    g_platform.fullscreen = false;
    g_platform.running = true;
    g_platform.frame_count = 0;

    printf("[PLATFORM] Initialized %dx%d window: %s\n", width, height, title);
    printf("[PLATFORM] (SDL2 not linked - running in headless mode)\n");
    return 0;
}

void platform_shutdown(void) {
    printf("[PLATFORM] Shutdown after %llu frames\n",
           (unsigned long long)g_platform.frame_count);
}

bool platform_poll_events(DCHardware *hw) {
    (void)hw;
    g_platform.frame_count++;

    /* In headless mode, run for a limited number of frames for testing */
    if (g_platform.frame_count >= 300) {
        printf("[PLATFORM] Headless frame limit reached\n");
        g_platform.running = false;
    }

    return g_platform.running;
}

void platform_swap_buffers(void) {
    /* No-op in headless mode */
}

void platform_set_title(const char *title) {
    printf("[PLATFORM] Title: %s\n", title);
}

uint64_t platform_get_ticks_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

uint64_t platform_get_ticks_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

void platform_sleep_ms(uint32_t ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
#endif
}

#else /* HAS_SDL2 */

#include <SDL.h>
#include <SDL_opengl.h>

static SDL_Window *g_window = NULL;
static SDL_GLContext g_gl_context = NULL;
static bool g_running = true;

int platform_init(int width, int height, const char *title) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[PLATFORM] SDL2 init failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    g_window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);

    if (!g_window) {
        fprintf(stderr, "[PLATFORM] Window creation failed: %s\n", SDL_GetError());
        return -1;
    }

    g_gl_context = SDL_GL_CreateContext(g_window);
    if (!g_gl_context) {
        fprintf(stderr, "[PLATFORM] GL context failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_GL_SetSwapInterval(1); /* VSync */

    printf("[PLATFORM] SDL2 initialized: %dx%d\n", width, height);
    return 0;
}

void platform_shutdown(void) {
    if (g_gl_context) SDL_GL_DeleteContext(g_gl_context);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
    printf("[PLATFORM] SDL2 shutdown\n");
}

bool platform_poll_events(DCHardware *hw) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            g_running = false;
            break;

        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            MapleController *ctrl = dc_maple_get_controller(hw, 0);
            if (!ctrl) break;
            bool pressed = (event.type == SDL_KEYDOWN);
            uint32_t bit = 0;

            switch (event.key.keysym.sym) {
            case SDLK_UP:     bit = CONT_DPAD_UP; break;
            case SDLK_DOWN:   bit = CONT_DPAD_DOWN; break;
            case SDLK_LEFT:   bit = CONT_DPAD_LEFT; break;
            case SDLK_RIGHT:  bit = CONT_DPAD_RIGHT; break;
            case SDLK_z:      bit = CONT_A; break;
            case SDLK_x:      bit = CONT_B; break;
            case SDLK_a:      bit = CONT_X; break;
            case SDLK_s:      bit = CONT_Y; break;
            case SDLK_RETURN: bit = CONT_START; break;
            case SDLK_q:      /* L trigger */
                ctrl->ltrig = pressed ? 255 : 0;
                break;
            case SDLK_w:      /* R trigger */
                ctrl->rtrig = pressed ? 255 : 0;
                break;
            default: break;
            }

            if (bit) {
                /* Buttons are active LOW on Dreamcast */
                if (pressed) ctrl->buttons &= ~bit;
                else ctrl->buttons |= bit;
            }
            break;
        }
        }
    }
    return g_running;
}

void platform_swap_buffers(void) {
    SDL_GL_SwapWindow(g_window);
}

void platform_set_title(const char *title) {
    if (g_window) SDL_SetWindowTitle(g_window, title);
}

uint64_t platform_get_ticks_ms(void) {
    return SDL_GetTicks64();
}

uint64_t platform_get_ticks_us(void) {
    static Uint64 freq;
    if (!freq) freq = SDL_GetPerformanceFrequency();
    return (uint64_t)((SDL_GetPerformanceCounter() * 1000000) / freq);
}

void platform_sleep_ms(uint32_t ms) {
    SDL_Delay(ms);
}

#endif /* HAS_SDL2 */
