/**
 * Platform Abstraction
 *
 * Provides a common interface for windowing, input, and timing
 * across different backends (SDL2, Win32, etc.)
 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include "hal/dc_hardware.h"

/* Initialize the platform (create window, init audio, etc.) */
int platform_init(int width, int height, const char *title);

/* Shutdown and cleanup */
void platform_shutdown(void);

/* Poll input events, returns false if quit requested */
bool platform_poll_events(DCHardware *hw);

/* Swap framebuffer (present frame) */
void platform_swap_buffers(void);

/* Set window title */
void platform_set_title(const char *title);

/* Get current time in milliseconds */
uint64_t platform_get_ticks_ms(void);

/* Sleep for specified milliseconds */
void platform_sleep_ms(uint32_t ms);

#endif /* PLATFORM_H */
