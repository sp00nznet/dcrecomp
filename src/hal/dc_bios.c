/**
 * Dreamcast BIOS syscall emulation. See dc_bios.h.
 *
 * Deliberately minimal: each syscall returns what a healthy machine with
 * default settings would, and anything unrecognised is logged and failed
 * rather than silently returning success. A game that gets a plausible answer
 * carries on; one that gets silence stalls, which is the failure we are here
 * to remove.
 */

#include "hal/dc_bios.h"
#include "hal/dc_hardware.h"
#include <stdio.h>
#include <string.h>

static const uint8_t *g_flash = NULL;
static uint32_t g_flash_size = 0;

/* Flashrom partition layout, as the BIOS reports it. Offsets and sizes are
 * fixed across retail units. */
static const struct { uint32_t offset, size; } FLASH_PARTS[] = {
    { 0x00000, 0x08000 },   /* 0: factory settings */
    { 0x08000, 0x08000 },   /* 1: reserved */
    { 0x10000, 0x04000 },   /* 2: block 1 user settings */
    { 0x14000, 0x04000 },   /* 3: block 1 game settings */
    { 0x18000, 0x04000 },   /* 4: block 2 user settings */
    { 0x1C000, 0x04000 },   /* 5: block 2 game settings */
};
#define FLASH_PART_COUNT ((int)(sizeof FLASH_PARTS / sizeof FLASH_PARTS[0]))

void sh4_bios_set_flashrom(const void *data, uint32_t size) {
    g_flash = (const uint8_t *)data;
    g_flash_size = size;
}

void sh4_bios_install_vectors(SH4CPU *cpu) {
    /* Each vector holds its own address, so a call through the table lands
     * somewhere sh4_bios_syscall() can identify. */
    for (uint32_t v = BIOS_VEC_FIRST; v <= BIOS_VEC_LAST; v += 4)
        sh4_write32(cpu, v, v);
    printf("[BIOS] syscall vectors installed (0x%08X-0x%08X)\n",
           BIOS_VEC_FIRST, BIOS_VEC_LAST);
}

static int syscall_flashrom(SH4CPU *cpu) {
    uint32_t sel = cpu->r[7];
    switch (sel) {
    case 0: {   /* info(part, &offset, &size) -> 0 ok, -1 bad part */
        int part = (int)cpu->r[4];
        if (part < 0 || part >= FLASH_PART_COUNT)
            return -1;
        sh4_write32(cpu, cpu->r[5], FLASH_PARTS[part].offset);
        sh4_write32(cpu, cpu->r[5] + 4, FLASH_PARTS[part].size);
        return 0;
    }
    case 1: {   /* read(offset, dest, count) -> bytes read, or -1 */
        uint32_t off = cpu->r[4], dest = cpu->r[5], count = cpu->r[6];
        for (uint32_t i = 0; i < count; i++) {
            uint8_t b = (g_flash && off + i < g_flash_size) ? g_flash[off + i] : 0;
            sh4_write8(cpu, dest + i, b);
        }
        return (int)count;
    }
    case 2:     /* write(offset, src, count) - accept and drop */
        return (int)cpu->r[6];
    case 3:     /* delete(offset) */
        return 0;
    default:
        printf("[BIOS] flashrom: unknown selector %u\n", sel);
        return -1;
    }
}

static int syscall_sysinfo(SH4CPU *cpu) {
    switch (cpu->r[7]) {
    case 0:     /* init - copies the unit ID out of flash */
        return 0;
    case 2:     /* id() -> pointer to the 8-byte unit ID */
        return (int)0x8C000068u;
    case 3:     /* icon() */
        return 0;
    default:
        printf("[BIOS] sysinfo: unknown selector %u\n", cpu->r[7]);
        return -1;
    }
}

static int syscall_romfont(SH4CPU *cpu) {
    switch (cpu->r[7]) {
    case 0:     /* address of the ROM font. We have none; say so. */
        return 0;
    case 1:     /* lock */
        return 0;
    case 2:     /* unlock */
        return 0;
    default:
        return 0;
    }
}

/* ---- GD-ROM (GDC) ------------------------------------------------------
 *
 * Games reach the drive through vector 0x8C0000BC, with r7 selecting a call:
 *
 *   0 ReqCmd(cmd, param)     queue a command, returns a request id
 *   1 GetCmdStat(id, stat)   0 = done, 1 = still processing
 *   2 ExecServer()           let the drive make progress
 *   3 InitSystem()           reset the driver
 *   4 GetDrvStat(stat)       stat[0] = drive state, stat[1] = disc type
 *
 * We complete commands synchronously inside ReqCmd - there is no drive to wait
 * for - and report "done" from the first GetCmdStat. Sector reads are served
 * from the disc's data track, which is what the game actually wants: its files
 * live at absolute LBAs on the disc, not in the extracted directory.
 */

#define GDC_CMD_PIOREAD   16
#define GDC_CMD_DMAREAD   17
#define GDC_CMD_GETTOC    18
#define GDC_CMD_GETTOC2   19
#define GDC_CMD_INIT      24
#define GDC_CMD_GETSES    35

#define GDC_SECTOR_SIZE   2048
#define GDC_RAW_SECTOR    2352
#define GDC_DATA_OFFSET   16      /* MODE1: sync + header before user data */

#define GDC_MAX_TRACKS 24

static struct {
    FILE *f;
    uint32_t start_lba;
    uint32_t sectors;
} g_tracks[GDC_MAX_TRACKS];
static int g_track_count = 0;
static uint32_t g_disc_start_lba = 0;   /* first data track, for the TOC */
static uint32_t g_gdc_next_id = 1;
static uint32_t g_gdc_last_id = 0;

void sh4_bios_set_gdrom_track(const char *path, uint32_t start_lba) {
    if (g_track_count >= GDC_MAX_TRACKS) {
        printf("[BIOS] gdrom: too many tracks, ignoring %s\n", path);
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("[BIOS] gdrom: cannot open %s\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    g_tracks[g_track_count].f = f;
    g_tracks[g_track_count].start_lba = start_lba;
    g_tracks[g_track_count].sectors = (uint32_t)(size / GDC_RAW_SECTOR);
    if (g_track_count == 0)
        g_disc_start_lba = start_lba;
    printf("[BIOS] gdrom: track %d = %s, LBA %u..%u\n",
           g_track_count, path, start_lba,
           start_lba + g_tracks[g_track_count].sectors - 1);
    g_track_count++;
}

/* Copy `count` 2048-byte sectors starting at absolute `lba` into guest memory. */
static int gdc_read_sectors(SH4CPU *cpu, uint32_t lba, uint32_t count, uint32_t dest) {
    static uint8_t sector[GDC_RAW_SECTOR];
    for (uint32_t i = 0; i < count; i++) {
        memset(sector, 0, sizeof sector);
        uint32_t want = lba + i;
        for (int t = 0; t < g_track_count; t++) {
            if (want < g_tracks[t].start_lba ||
                want >= g_tracks[t].start_lba + g_tracks[t].sectors)
                continue;
            long off = (long)(want - g_tracks[t].start_lba) * GDC_RAW_SECTOR;
            if (fseek(g_tracks[t].f, off, SEEK_SET) == 0)
                fread(sector, 1, GDC_RAW_SECTOR, g_tracks[t].f);
            break;
        }
        const uint8_t *user = sector + GDC_DATA_OFFSET;
        for (uint32_t b = 0; b < GDC_SECTOR_SIZE; b++)
            sh4_write8(cpu, dest + i * GDC_SECTOR_SIZE + b, user[b]);
    }
    return 0;
}

static int gdc_exec(SH4CPU *cpu, uint32_t cmd, uint32_t param) {
    static int logged = 0;
    switch (cmd) {
    case GDC_CMD_PIOREAD:
    case GDC_CMD_DMAREAD: {
        /* param -> { lba, count, buffer, ... } */
        uint32_t lba   = sh4_read32(cpu, param);
        uint32_t count = sh4_read32(cpu, param + 4);
        uint32_t dest  = sh4_read32(cpu, param + 8);
        if (logged < 12) {
            logged++;
            printf("[BIOS] gdrom read: lba %u x%u -> 0x%08X\n", lba, count, dest);
        }
        return gdc_read_sectors(cpu, lba, count, dest);
    }
    case GDC_CMD_INIT:
        return 0;
    case GDC_CMD_GETTOC:
    case GDC_CMD_GETTOC2:
        /* A TOC the game can parse: one data track starting where ours does. */
        if (param) {
            uint32_t out = sh4_read32(cpu, param + 4);
            if (out) {
                for (int i = 0; i < 99; i++)
                    sh4_write32(cpu, out + i * 4, 0xFFFFFFFFu);
                sh4_write32(cpu, out + 0, 0x41000000u | g_disc_start_lba);
                sh4_write32(cpu, out + 99 * 4, 0x41000000u | g_disc_start_lba);
            }
        }
        return 0;
    default:
        if (logged < 12) {
            logged++;
            printf("[BIOS] gdrom: unhandled command %u (param 0x%08X)\n", cmd, param);
        }
        return 0;
    }
}

static int syscall_gdrom(SH4CPU *cpu) {
    switch (cpu->r[7]) {
    case 0: {   /* ReqCmd(cmd, param) -> request id, 0 on refusal */
        static int reqlog = 0;
        if (reqlog < 20) {
            reqlog++;
            printf("[BIOS] gdrom ReqCmd cmd=%u param=0x%08X\n", cpu->r[4], cpu->r[5]);
        }
        g_gdc_last_id = g_gdc_next_id++;
        gdc_exec(cpu, cpu->r[4], cpu->r[5]);
        /* Tell the driver its command finished. It registered a completion
         * callback and is waiting on the drive's interrupt, so finishing
         * quietly leaves it waiting forever. */
        dc_gdrom_signal_complete(sh4_get_hardware());
        return (int)g_gdc_last_id;
    }
    case 1: {   /* GetCmdStat(id, stat[4]).
                 * 0 NO_ACTIVE, 1 PROCESSING, 2 COMPLETED, 3 ABORTED. Callers
                 * spin while PROCESSING and treat anything but COMPLETED as a
                 * failure - returning 0 here made every command look like it
                 * had never been queued, so the game retried CMD_INIT forever. */
        if (cpu->r[5]) {
            for (int i = 0; i < 4; i++)
                sh4_write32(cpu, cpu->r[5] + i * 4, 0);
        }
        return 2;   /* COMPLETED */
    }
    case 2:     /* ExecServer - nothing to advance, we finish synchronously */
        return 0;
    case 3:     /* InitSystem */
        g_gdc_next_id = 1;
        return 0;
    case 4:     /* GetDrvStat(stat[2]): standby, and a GD-ROM in the drive */
        if (cpu->r[4]) {
            /* Drive status: 0 busy, 1 pause, 2 standby, 3 play, 4 seek,
             * 5 scan, 6 open, 7 no disc, 8 retry, 9 error.
             * Disc type: 0x00 CDDA, 0x10 CDROM, 0x20 XA, 0x30 CDI, 0x80 GDROM. */
            sh4_write32(cpu, cpu->r[4], 2);        /* standby: disc in, idle */
            sh4_write32(cpu, cpu->r[4] + 4, 0x80); /* GD-ROM */
        }
        return 0;
    default:
        return 0;
    }
}

bool sh4_bios_syscall(SH4CPU *cpu) {
    uint32_t pc = cpu->pc & 0x1FFFFFFF;
    uint32_t vec = pc | 0x8C000000u;
    if (vec < BIOS_VEC_FIRST || vec > BIOS_VEC_LAST)
        return false;

    int result;
    switch (vec) {
    case BIOS_VEC_SYSINFO:  result = syscall_sysinfo(cpu);  break;
    case BIOS_VEC_ROMFONT:  result = syscall_romfont(cpu);  break;
    case BIOS_VEC_FLASHROM: result = syscall_flashrom(cpu); break;
    case BIOS_VEC_GDROM:    result = syscall_gdrom(cpu);    break;
    default:                return false;
    }

    cpu->r[0] = (uint32_t)result;
    /* The thunk reached us with `jmp`, so the return address is still in pr and
     * our C caller returns there naturally. */
    return true;
}
