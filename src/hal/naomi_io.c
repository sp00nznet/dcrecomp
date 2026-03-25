/**
 * Naomi Arcade JVS I/O Implementation
 *
 * Emulates the JVS I/O board as seen by the Naomi mainboard.
 * On real hardware, the Naomi communicates with JVS devices via the
 * Maple DMA registers (same base address as Dreamcast Maple, but
 * different protocol). The BIOS handles JVS command framing and the
 * game reads results from a known RAM location.
 *
 * For Mushiking and similar card games, the card reader appears as
 * a JVS sub-device that reports card insertion state and card data.
 */

#include "hal/naomi_io.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* JVS command IDs */
#define JVS_CMD_RESET       0xF0
#define JVS_CMD_SETADDR     0xF1
#define JVS_CMD_IOIDENT     0x10
#define JVS_CMD_CMDREV      0x11
#define JVS_CMD_JVSREV      0x12
#define JVS_CMD_COMMREV     0x13
#define JVS_CMD_FEATCHECK   0x14
#define JVS_CMD_MAINID      0x15
#define JVS_CMD_SWINPUT     0x20  /* Switch (digital) input */
#define JVS_CMD_COININPUT   0x21  /* Coin input */
#define JVS_CMD_ANLINPUT    0x22  /* Analog input */
#define JVS_CMD_SCRPOS      0x23  /* Screen position (lightgun) */
#define JVS_CMD_COINDEC     0x30  /* Decrease coin count */
#define JVS_CMD_COININC     0x35  /* Increase coin count */

/* JVS response codes */
#define JVS_STATUS_OK       0x01
#define JVS_REPORT_OK       0x01

/* Naomi board ID register */
#define NAOMI_BOARD_ID_ADDR 0x005F7000

struct NaomiIO {
    /* Player state (2 players) */
    JVSPlayerState players[2];

    /* System buttons (test/tilt) */
    uint32_t system_buttons;

    /* Card reader */
    JVSCardReader card_reader;

    /* JVS node state */
    uint8_t jvs_node_addr;
    bool jvs_initialized;

    /* Naomi board registers */
    uint32_t board_regs[64];
};

NaomiIO* naomi_io_init(void) {
    NaomiIO *io = (NaomiIO *)calloc(1, sizeof(NaomiIO));
    if (!io) return NULL;

    io->card_reader.state = CARD_STATE_EMPTY;
    io->jvs_node_addr = 0;
    io->jvs_initialized = false;

    printf("[JVS] Naomi I/O initialized\n");
    return io;
}

void naomi_io_destroy(NaomiIO *io) {
    if (io) {
        printf("[JVS] Naomi I/O destroyed\n");
        free(io);
    }
}

uint32_t naomi_io_read32(NaomiIO *io, uint32_t addr) {
    if (!io) return 0;

    uint32_t phys = addr & 0x1FFFFFFF;

    switch (phys) {
    case NAOMI_BOARD_ID_ADDR:
        return 0x00000000; /* Naomi board ID */

    default:
        if (phys >= 0x005F7000 && phys < 0x005F7100) {
            uint32_t idx = (phys - 0x005F7000) / 4;
            if (idx < 64) return io->board_regs[idx];
        }
        break;
    }

    return 0;
}

void naomi_io_write32(NaomiIO *io, uint32_t addr, uint32_t val) {
    if (!io) return;

    uint32_t phys = addr & 0x1FFFFFFF;

    if (phys >= 0x005F7000 && phys < 0x005F7100) {
        uint32_t idx = (phys - 0x005F7000) / 4;
        if (idx < 64) io->board_regs[idx] = val;
    }
}

JVSPlayerState* naomi_io_get_player(NaomiIO *io, int player) {
    if (!io || player < 0 || player >= 2) return NULL;
    return &io->players[player];
}

void naomi_io_set_system_buttons(NaomiIO *io, uint32_t buttons) {
    if (io) io->system_buttons = buttons;
}

void naomi_io_insert_coin(NaomiIO *io, int player) {
    if (!io || player < 0 || player >= 2) return;
    io->players[player].coin_count++;
    printf("[JVS] Coin inserted for player %d (total: %d)\n",
           player + 1, io->players[player].coin_count);
}

JVSCardReader* naomi_io_get_card_reader(NaomiIO *io) {
    if (!io) return NULL;
    return &io->card_reader;
}

void naomi_io_insert_card(NaomiIO *io, const uint8_t *data, uint32_t len, uint32_t card_id) {
    if (!io) return;

    JVSCardReader *cr = &io->card_reader;
    cr->state = CARD_STATE_READY;
    cr->card_id = card_id;
    cr->data_len = len < JVS_CARD_DATA_SIZE ? len : JVS_CARD_DATA_SIZE;
    memcpy(cr->data, data, cr->data_len);

    printf("[JVS] Card inserted: ID=0x%04X, %u bytes\n", card_id, cr->data_len);
}

void naomi_io_eject_card(NaomiIO *io) {
    if (!io) return;

    io->card_reader.state = CARD_STATE_EMPTY;
    io->card_reader.card_id = 0;
    io->card_reader.data_len = 0;
    memset(io->card_reader.data, 0, JVS_CARD_DATA_SIZE);

    printf("[JVS] Card ejected\n");
}

/**
 * Process JVS DMA transaction.
 *
 * On Naomi, the game (via BIOS) writes JVS commands to a RAM buffer,
 * triggers Maple DMA, and the I/O board responds. We intercept the
 * DMA and fill in the response directly.
 *
 * Naomi JVS DMA format:
 *   Command buffer at dma_addr in system RAM
 *   Response written back to RAM at specified offset
 */
void naomi_io_process_jvs_dma(NaomiIO *io, uint8_t *ram, uint32_t dma_addr) {
    if (!io || !ram) return;

    /* Read the Maple/JVS DMA command from RAM */
    uint32_t ram_offset = dma_addr & 0x01FFFFFF;
    uint32_t *cmd = (uint32_t *)(ram + ram_offset);

    /* Naomi Maple DMA descriptor format:
     * Word 0: command pattern (device function codes)
     * Word 1: destination address for response
     * ...
     * The exact format depends on the BIOS version.
     * For now, generate a basic successful JVS response. */

    uint32_t pattern = cmd[0];
    uint32_t resp_addr = cmd[1] & 0x01FFFFFF;

    static int jvs_log = 0;
    if (jvs_log < 20) {
        jvs_log++;
        printf("[JVS-DMA] pattern=0x%08X resp_addr=0x%08X\n", pattern, resp_addr);
    }

    /* Write a basic JVS response indicating I/O board present */
    uint32_t *resp = (uint32_t *)(ram + resp_addr);

    /* Response format varies by Naomi BIOS call.
     * Common pattern: status word + player button data + coin data */
    resp[0] = 0x00000001;  /* Device present, status OK */

    /* Button state - Naomi BIOS reads this for player input */
    /* Byte layout: [system] [p1_hi] [p1_lo] [p2_hi] [p2_lo] ... */
    uint8_t *resp_bytes = (uint8_t *)&resp[1];
    resp_bytes[0] = (uint8_t)(io->system_buttons & 0xFF);
    resp_bytes[1] = (uint8_t)((io->players[0].buttons >> 8) & 0xFF);
    resp_bytes[2] = (uint8_t)(io->players[0].buttons & 0xFF);
    resp_bytes[3] = (uint8_t)((io->players[1].buttons >> 8) & 0xFF);
    resp_bytes[4] = (uint8_t)(io->players[1].buttons & 0xFF);

    /* Coin counts */
    resp_bytes[5] = 0; /* coin status */
    resp_bytes[6] = io->players[0].coin_count;
    resp_bytes[7] = 0;
    resp_bytes[8] = io->players[1].coin_count;

    /* Card reader status (if present) */
    resp_bytes[9] = (uint8_t)io->card_reader.state;
    if (io->card_reader.state == CARD_STATE_READY) {
        resp_bytes[10] = (uint8_t)(io->card_reader.card_id >> 8);
        resp_bytes[11] = (uint8_t)(io->card_reader.card_id & 0xFF);
    }
}
