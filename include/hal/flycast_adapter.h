/**
 * Flycast Hardware Adapter
 *
 * Bridges dcrecomp's C-based recompiled code to Flycast's C++ hardware
 * emulation subsystems. This is the integration layer that routes our
 * sh4_read/write calls through Flycast's address space handlers.
 *
 * Architecture:
 *   Recompiled game code (C)
 *     → sh4_read32() / sh4_write32()     [dcrecomp sh4_cpu.c]
 *       → flycast_mem_read32() etc.       [this adapter]
 *         → addrspace::read32()           [Flycast memory routing]
 *           → pvr/aica/maple/naomi handlers
 *
 * Usage:
 *   1. Call flycast_hw_init() at startup
 *   2. Set flycast_set_ram_ptr() with the CPU's RAM buffer
 *   3. Memory accesses from recompiled code route through Flycast
 *   4. Call flycast_hw_frame() each frame for VBlank/rendering
 *   5. Call flycast_hw_destroy() at shutdown
 */

#ifndef FLYCAST_ADAPTER_H
#define FLYCAST_ADAPTER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== Initialization ========== */

/**
 * Initialize all Flycast hardware subsystems.
 * Call this after allocating CPU memory but before running game code.
 *
 * @param ram_ptr    Pointer to main RAM buffer
 * @param ram_size   RAM size in bytes (16MB for DC, 32MB for Naomi)
 * @param vram_ptr   Pointer to VRAM buffer (8MB)
 * @param aica_ptr   Pointer to AICA RAM buffer (2MB)
 * @param is_naomi   true for Naomi arcade, false for Dreamcast
 * @return 0 on success, -1 on failure
 */
int flycast_hw_init(uint8_t *ram_ptr, uint32_t ram_size,
                    uint8_t *vram_ptr, uint8_t *aica_ptr,
                    bool is_naomi);

/**
 * Destroy all hardware subsystems and free resources.
 */
void flycast_hw_destroy(void);

/* ========== Memory Access (routed through Flycast address space) ========== */

uint8_t  flycast_mem_read8(uint32_t addr);
uint16_t flycast_mem_read16(uint32_t addr);
uint32_t flycast_mem_read32(uint32_t addr);
void     flycast_mem_write8(uint32_t addr, uint8_t val);
void     flycast_mem_write16(uint32_t addr, uint16_t val);
void     flycast_mem_write32(uint32_t addr, uint32_t val);

/* ========== Frame / Timing ========== */

/**
 * Process one frame. Handles:
 * - VBlank interrupt generation
 * - PVR2 render submission
 * - Maple/JVS controller polling
 * - Audio sample generation
 *
 * Call this once per frame (~60Hz).
 */
void flycast_hw_frame(void);

/**
 * Get current virtual cycle count.
 * Used by recompiled code that checks timing.
 */
uint64_t flycast_get_cycles(void);

/* ========== Naomi-Specific ========== */

/**
 * Load a Naomi ROM cartridge.
 * Sets up the cartridge memory mapping and decryption.
 *
 * @param rom_data   Raw (decrypted) ROM data
 * @param rom_size   ROM size in bytes
 * @return 0 on success
 */
int flycast_naomi_load_cart(const uint8_t *rom_data, uint32_t rom_size);

/**
 * Initialize Naomi peripherals for a specific game.
 * Sets up card reader, hopper, etc. based on game ID.
 *
 * @param game_id    Game identifier string (e.g., "MUSHIKING")
 */
void flycast_naomi_init_peripherals(const char *game_id);

/* ========== Input ========== */

/**
 * Set button state for a player.
 * @param player   Player number (0 or 1)
 * @param buttons  Button bitmask (NAOMI_BTN0..BTN8, START, UP/DOWN/LEFT/RIGHT)
 * @param pressed  true if buttons are pressed
 */
void flycast_input_set_buttons(int player, uint32_t buttons, bool pressed);

/**
 * Insert a coin for a player.
 */
void flycast_input_coin(int player);

/**
 * Set analog axis value.
 * @param player   Player number
 * @param axis     Axis number (0-7)
 * @param value    Axis value (0-65535)
 */
void flycast_input_set_axis(int player, int axis, uint16_t value);

/* ========== Card Reader ========== */

/**
 * Initialize the barcode card reader (for Mushiking, Dinosaur King, etc.)
 */
void flycast_card_reader_init_barcode(void);

/**
 * Insert a card into the reader.
 * @param player     Player number (0 or 1)
 * @param card_data  Card barcode/data
 * @param data_len   Data length
 */
void flycast_card_reader_insert(int player, const uint8_t *card_data, uint32_t data_len);

/* ========== Rendering ========== */

/**
 * Initialize the renderer (OpenGL/Vulkan).
 * Call after creating the window and GL context.
 *
 * @param width   Window width
 * @param height  Window height
 * @return 0 on success
 */
int flycast_renderer_init(int width, int height);

/**
 * Resize the renderer viewport.
 */
void flycast_renderer_resize(int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* FLYCAST_ADAPTER_H */
