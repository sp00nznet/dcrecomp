/**
 * Naomi Arcade I/O - JVS (Japanese Video Standard) Interface
 *
 * The Sega Naomi uses JVS for all arcade I/O instead of the Dreamcast's
 * Maple Bus. JVS is an RS485-based serial protocol supporting chained
 * peripherals including buttons, coins, analog controls, and card readers.
 *
 * Naomi-specific registers (Maple area repurposed for JVS):
 *   0x005F6C00 - JVS/Maple registers (shared base address)
 *   0x005F6C80 - Naomi board-specific registers
 */

#ifndef NAOMI_IO_H
#define NAOMI_IO_H

#include <stdint.h>
#include <stdbool.h>

/* ========== JVS Button Definitions ========== */

/* Player 1/2 button bits (active HIGH for JVS, unlike Dreamcast active LOW) */
#define JVS_BTN_TEST     (1 << 0)   /* Test/Service button */
#define JVS_BTN_SERVICE  (1 << 1)   /* Service credit */
#define JVS_BTN_START    (1 << 2)   /* Start button */
#define JVS_BTN_UP       (1 << 3)   /* D-Pad up */
#define JVS_BTN_DOWN     (1 << 4)   /* D-Pad down */
#define JVS_BTN_LEFT     (1 << 5)   /* D-Pad left */
#define JVS_BTN_RIGHT    (1 << 6)   /* D-Pad right */
#define JVS_BTN_1        (1 << 7)   /* Button 1 */
#define JVS_BTN_2        (1 << 8)   /* Button 2 */
#define JVS_BTN_3        (1 << 9)   /* Button 3 */
#define JVS_BTN_4        (1 << 10)  /* Button 4 */
#define JVS_BTN_5        (1 << 11)  /* Button 5 */
#define JVS_BTN_6        (1 << 12)  /* Button 6 */

/* System button bits */
#define JVS_SYS_TEST     (1 << 0)
#define JVS_SYS_TILT1    (1 << 1)
#define JVS_SYS_TILT2    (1 << 2)

/* ========== JVS Card Reader ========== */

#define JVS_CARD_DATA_SIZE  64  /* Max card data bytes */

typedef enum {
    CARD_STATE_EMPTY = 0,       /* No card inserted */
    CARD_STATE_INSERTING,       /* Card being read */
    CARD_STATE_READY,           /* Card data available */
    CARD_STATE_EJECTING,        /* Card being ejected */
} JVSCardState;

typedef struct {
    JVSCardState state;
    uint8_t data[JVS_CARD_DATA_SIZE];
    uint32_t data_len;
    uint32_t card_id;           /* Beetle/card type identifier */
} JVSCardReader;

/* ========== JVS Player State ========== */

typedef struct {
    uint32_t buttons;           /* Button state bitmask */
    uint8_t coin_count;         /* Coins inserted */
    int16_t analog[4];          /* Analog channels (if any) */
} JVSPlayerState;

/* ========== Naomi I/O State ========== */

typedef struct NaomiIO NaomiIO;

/* Initialize Naomi JVS I/O system */
NaomiIO* naomi_io_init(void);

/* Destroy I/O state */
void naomi_io_destroy(NaomiIO *io);

/* Register read/write (called from dc_hardware for Naomi-specific addrs) */
uint32_t naomi_io_read32(NaomiIO *io, uint32_t addr);
void naomi_io_write32(NaomiIO *io, uint32_t addr, uint32_t val);

/* Player input */
JVSPlayerState* naomi_io_get_player(NaomiIO *io, int player);
void naomi_io_set_system_buttons(NaomiIO *io, uint32_t buttons);

/* Coin management */
void naomi_io_insert_coin(NaomiIO *io, int player);

/* Card reader operations */
JVSCardReader* naomi_io_get_card_reader(NaomiIO *io);
void naomi_io_insert_card(NaomiIO *io, const uint8_t *data, uint32_t len, uint32_t card_id);
void naomi_io_eject_card(NaomiIO *io);

/* DMA-based JVS communication (Naomi processes JVS via Maple DMA) */
void naomi_io_process_jvs_dma(NaomiIO *io, uint8_t *ram, uint32_t dma_addr);

#endif /* NAOMI_IO_H */
