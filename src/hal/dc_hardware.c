/**
 * Dreamcast Hardware Abstraction Layer Implementation
 *
 * Maps Dreamcast hardware registers and subsystems to modern equivalents.
 * GPU rendering is handled via OpenGL, input via SDL2, sound via SDL2 audio.
 */

#include "hal/dc_hardware.h"
#include "hal/pvr2.h"
#include "recompiler/sh4_cpu.h"
#include "platform/platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Internal hardware state */
struct DCHardware {
    /* Hardware registers (mapped 0x005F6800 - 0x005FA000) */
    uint32_t hw_regs[0x3800 / 4];

    /* System Board state */
    uint32_t sb_istnrm;    /* Normal interrupt status */
    uint32_t sb_istext;    /* External interrupt status */
    uint32_t sb_isterr;    /* Error interrupt status */

    /* PVR state */
    uint32_t pvr_fb_addr1;
    uint32_t pvr_fb_addr2;
    uint32_t pvr_render_addr;
    bool pvr_rendering;
    int frame_count;

    /* Maple (controller) state */
    MapleController controllers[4];
    bool controller_connected[4];

    /* AICA state */
    bool aica_arm_running;

    /* GD-ROM state */
    int gdrom_status;

    /* TA (Tile Accelerator) FIFO buffer */
    uint32_t ta_fifo[32];
    int ta_fifo_pos;

    /* Timing */
    uint64_t vblank_count;
    uint64_t last_vblank_time;
};

/* Register offset calculation */
static inline uint32_t hw_reg_idx(uint32_t addr) {
    if (addr >= 0x005F6800 && addr < 0x005FA000)
        return (addr - 0x005F6800) / 4;
    return 0;
}

DCHardware* dc_hw_init(void) {
    DCHardware *hw = (DCHardware *)calloc(1, sizeof(DCHardware));
    if (!hw) return NULL;

    /* Set PVR ID (Holly chip) */
    hw->hw_regs[hw_reg_idx(PVR_ID)] = 0x17FD11DB;

    /* Default framebuffer setup (640x480) */
    hw->pvr_fb_addr1 = 0x00000000;
    hw->pvr_fb_addr2 = 0x00000000;

    /* Connect controller in port 0 */
    hw->controller_connected[0] = true;
    hw->controllers[0].buttons = 0xFFFF; /* All buttons released (active low) */
    hw->controllers[0].ltrig = 0;
    hw->controllers[0].rtrig = 0;
    hw->controllers[0].joyx = 0;
    hw->controllers[0].joyy = 0;

    printf("[HAL] Dreamcast hardware initialized\n");
    return hw;
}

void dc_hw_destroy(DCHardware *hw) {
    if (hw) {
        printf("[HAL] Hardware destroyed (frames rendered: %d)\n", hw->frame_count);
        free(hw);
    }
}

/* Polling detector state */
static uint32_t poll_last_addr = 0;
static int poll_count = 0;
static int poll_reported = 0;

uint32_t dc_hw_read32(DCHardware *hw, uint32_t addr) {
    if (!hw) return 0;

    /* Strip P2 area bits if present */
    uint32_t phys = addr & 0x1FFFFFFF;

    /* Global read counter for heartbeat */
    static uint64_t hw_read_count = 0;
    hw_read_count++;
    if ((hw_read_count & 0xFFFFFFF) == 0) {
        printf("[HEARTBEAT] %llu HW reads, last=0x%08X\n",
               (unsigned long long)hw_read_count, phys);
    }

    /* Detect polling loops */
    if (phys == poll_last_addr) {
        poll_count++;
        if (poll_count == 10 && poll_reported < 50) {
            poll_reported++;
            printf("[POLL] register 0x%08X read 10+ times\n", phys);
        }
        if (poll_count == 1000 && poll_reported < 60) {
            poll_reported++;
            printf("[POLL] register 0x%08X read 1000+ times — likely stuck\n", phys);
        }
    } else {
        if (poll_count >= 10 && poll_reported < 60) {
            poll_reported++;
            printf("[POLL] register 0x%08X polled %d times, moving to 0x%08X\n",
                   poll_last_addr, poll_count, phys);
        }
        poll_last_addr = phys;
        poll_count = 1;
    }

    dc_g2_retire_finished(hw);

    switch (phys) {
    case PVR_ID:
        return 0x17FD11DB;

    case PVR_REVISION:
        return 0x00000011;  /* Revision 1.1 */

    case SB_ISTNRM:
        /* VBlank is raised by sh4_poll_irq, which owns the 60Hz clock.
         * Generating it here as well gave us two competing timebases. */
        return hw->sb_istnrm;

    case SB_ISTEXT:
        return hw->sb_istext;

    case SB_ISTERR:
        return hw->sb_isterr;

    case PVR_FB_ADDR1:
        return hw->pvr_fb_addr1;

    case PVR_FB_ADDR2:
        return hw->pvr_fb_addr2;

    case 0x005F688C: { /* SB_FFST - FIFO Status (always report empty/ready) */
        static int ffst_log = 0;
        if (ffst_log < 3) { ffst_log++; printf("[FFST] returning 0\n"); }
        return 0;
    }

    case 0x005F810C: { /* SPG_STATUS - sync pulse generator status */
        /* One line per read.
         *
         * Deriving the line from the millisecond clock looks more honest but
         * is not: the value then changes once per millisecond, so it steps by
         * about 31 lines at a time and a game waiting for one particular line
         * mostly steps straight over it. ChuChu Rocket's frame loop spent
         * 300,000 reads per frame that way.
         *
         * ponytail: the line count no longer measures time, so a game that
         * counts lines to time something will be wrong. None seen yet - they
         * wait for a line or for vblank, and both arrive promptly. Frame pace
         * still comes from the 60Hz VBlank interrupt, not from here. Anchor
         * this to a microsecond clock if a game ever needs the real rate. */
        uint64_t now = platform_get_ticks_ms();
        static uint32_t line = 0;
        line = (line + 1) % 525;
        uint32_t scanline = line;
        uint32_t vsync = (scanline >= 480) ? 1 : 0;  /* VSync during lines 480-524 */
        uint32_t blank = vsync;  /* Blank during VBlank */
        uint32_t fieldnum = (uint32_t)(now / 16) & 1; /* Alternate fields */
        uint32_t result = (scanline & 0x3FF) | (fieldnum << 10) | (blank << 11) | (vsync << 13);
        static int spg_log = 0;
        static uint32_t spg_last = 0;
        if (result != spg_last && spg_log < 20) {
            spg_log++;
            printf("[SPG] scanline=%u vsync=%u blank=%u field=%u val=0x%08X\n",
                   scanline, vsync, blank, fieldnum, result);
            spg_last = result;
        }
        return result;
    }

    default:
        if (phys >= 0x005F6800 && phys < 0x005FA000) {
            return hw->hw_regs[hw_reg_idx(phys)];
        }
        break;
    }

    return 0;
}


/* ---- G2 and PVR DMA ------------------------------------------------------
 *
 * Four G2 channels (AICA, Ext1, Ext2, Dev) and the PVR channel share a layout:
 * seven registers 4 bytes apart holding the device-side address, the root-bus
 * (system RAM) address, the length, direction, trigger select, enable and
 * start. Writing 1 to start runs the transfer; hardware clears it when done
 * and raises a bit in SB_ISTNRM. A game polls the start register to wait, so
 * the clear is the part that matters - a transfer we cannot place still has to
 * finish.
 *
 * ponytail: transfers complete inside the write. Real DMA overlaps with the
 * CPU, and a game that reads the destination before polling would see it early
 * - none do, because on hardware the data is not there yet either. If timing
 * ever matters, this is where a completion deadline would go.
 */
#define DMA_OFF_DEVICE 0x00   /* device-side address (AICA RAM, VRAM, TA) */
#define DMA_OFF_ROOT   0x04   /* system RAM address */
#define DMA_OFF_LEN    0x08
#define DMA_OFF_DIR    0x0C   /* 0 = root -> device, 1 = device -> root */
#define DMA_OFF_START  0x18

/* Where a DMA address points, as a pointer into the memory we own. Returns
 * NULL for anything unmapped, and clamps `len` to what actually fits. */
static uint8_t *dma_resolve(uint32_t addr, uint32_t *len, const char **what) {
    uint32_t phys = addr & 0x1FFFFFFF;

    if (phys >= 0x00800000 && phys < 0x00A00000) {
        uint8_t *aica = sh4_get_aica_ram_ptr();
        uint32_t off = phys - 0x00800000;
        if (!aica) return NULL;
        if (off + *len > DC_AICA_SIZE) *len = DC_AICA_SIZE - off;
        *what = "AICA";
        return aica + off;
    }
    /* 0x04000000 is VRAM as the CPU sees it, 0x11000000 the TA texture path;
     * both land in the same 8MB. */
    uint32_t vram_off = 0xFFFFFFFF;
    if (phys >= 0x04000000 && phys < 0x04800000)      vram_off = phys - 0x04000000;
    else if (phys >= 0x05000000 && phys < 0x05800000) vram_off = phys - 0x05000000;
    else if (phys >= 0x11000000 && phys < 0x11800000) vram_off = phys - 0x11000000;
    if (vram_off != 0xFFFFFFFF) {
        uint8_t *vram = sh4_get_vram_ptr();
        if (!vram) return NULL;
        if (vram_off + *len > DC_VRAM_SIZE) *len = DC_VRAM_SIZE - vram_off;
        *what = "VRAM";
        return vram + vram_off;
    }
    if (phys >= DC_RAM_BASE && phys < DC_RAM_BASE + 0x01000000) {
        uint8_t *ram = sh4_get_ram_ptr();
        if (!ram) return NULL;
        *what = "RAM";
        return ram + ((phys - DC_RAM_BASE) & sh4_get_ram_mask());
    }
    return NULL;
}

/* A G2 channel that has copied its data but is not admitting to being done
 * yet. The wait is the point: a driver starts a channel and then records the
 * transfer, and clearing the start bit before it gets that far means the
 * transfer is never in its list to complete. 4MB/s is about what G2 sustains,
 * and a millisecond floor covers the small transfers. */
#define G2_BYTES_PER_MS 4096
static struct {
    uint32_t base;
    int      bit;
    uint64_t done_ms;
    int      busy;
} g_g2_busy[5];

static void g2_start_busy(uint32_t base, int bit, uint32_t len) {
    uint64_t ms = len / G2_BYTES_PER_MS;
    if (ms < 1) ms = 1;
    for (int i = 0; i < 5; i++) {
        if (g_g2_busy[i].busy && g_g2_busy[i].base != base)
            continue;
        g_g2_busy[i].base = base;
        g_g2_busy[i].bit = bit;
        g_g2_busy[i].done_ms = platform_get_ticks_ms() + ms;
        g_g2_busy[i].busy = 1;
        return;
    }
}

/* Called from the register read and from the interrupt poll: retire any
 * channel whose time is up, clear its start bit and raise its interrupt. */
void dc_g2_retire_finished(DCHardware *hw) {
    if (!hw) return;
    uint64_t now = platform_get_ticks_ms();
    for (int i = 0; i < 5; i++) {
        if (!g_g2_busy[i].busy || now < g_g2_busy[i].done_ms)
            continue;
        g_g2_busy[i].busy = 0;
        hw->hw_regs[hw_reg_idx(g_g2_busy[i].base + DMA_OFF_START)] = 0;
        hw->hw_regs[hw_reg_idx(g_g2_busy[i].base + DMA_OFF_LEN)] = 0;
        hw->sb_istnrm |= (1u << g_g2_busy[i].bit);
    }
}

static void run_dma(DCHardware *hw, uint32_t base, int istnrm_bit,
                    const char *name) {
    uint32_t device = hw->hw_regs[hw_reg_idx(base + DMA_OFF_DEVICE)];
    uint32_t root   = hw->hw_regs[hw_reg_idx(base + DMA_OFF_ROOT)];
    uint32_t len    = hw->hw_regs[hw_reg_idx(base + DMA_OFF_LEN)] & 0x1FFFFFFF;
    uint32_t dir    = hw->hw_regs[hw_reg_idx(base + DMA_OFF_DIR)] & 1;

    static int dma_log = 0;
    int log = dma_log < 24;

    /* The TA FIFO is a register, not memory - feed it packet by packet. */
    if (!dir && (device & 0x1FFFFFFF) >= 0x10000000
             && (device & 0x1FFFFFFF) < 0x10800000) {
        uint32_t srclen = len;
        const char *sw = "?";
        uint8_t *src = dma_resolve(root, &srclen, &sw);
        int packets = 0;
        if (src) {
            for (uint32_t off = 0; off + 32 <= srclen; off += 32) {
                pvr2_ta_write((const uint32_t *)(src + off));
                packets++;
            }
        }
        if (log) {
            dma_log++;
            printf("[%s-DMA] %d packets (%u bytes) to TA FIFO\n",
                   name, packets, len);
        }
    } else {
        uint32_t copy = len;
        const char *dw = "?", *sw = "?";
        uint8_t *devp = dma_resolve(device, &copy, &dw);
        uint8_t *rootp = dma_resolve(root, &copy, &sw);
        if (devp && rootp && copy) {
            if (dir) memcpy(rootp, devp, copy);
            else     memcpy(devp, rootp, copy);
            if (log) {
                dma_log++;
                printf("[%s-DMA] %u bytes %s 0x%08X(%s) %s 0x%08X(%s)\n",
                       name, copy, dir ? "from" : "to", device, dw,
                       dir ? "to" : "from", root, sw);
            }
        } else if (log) {
            dma_log++;
            printf("[%s-DMA] unmapped: device 0x%08X dest %s, root 0x%08X %s, len %u\n",
                   name, device, devp ? "ok" : "no", root, rootp ? "ok" : "no", len);
        }
    }

    /* The data has moved, but the channel is still busy as far as the game
     * is concerned - see the note on g2_busy. */
    g2_start_busy(base, istnrm_bit, len);
}

static int hw_write_log = 0;

/* SH4 DMAC register indices (0xFFA00000 base, 4 bytes each) */
#define DMAC_SAR2_IDX  8   /* Source Address Register ch2 (0xFFA00020) */

void dc_hw_write32(DCHardware *hw, uint32_t addr, uint32_t val) {
    if (!hw) return;

    uint32_t phys = addr & 0x1FFFFFFF;

    if (hw_write_log < 100) {
        hw_write_log++;
        printf("[HW] write32 phys=0x%08X val=0x%08X\n", phys, val);
    }
    /* Always log DMA-related writes */
    if (phys >= 0x005F7C00 && phys <= 0x005F7C18) {
        printf("[PVR-DMA] write 0x%08X = 0x%08X\n", phys, val);
    }

    /* SB_MDST - Maple DMA start/status */
    if (phys == 0x005F6C18) {
        if (val & 1) {
            uint32_t mdstar = hw->hw_regs[hw_reg_idx(0x005F6C04)];
            dc_maple_dma(hw, mdstar);
            hw->hw_regs[hw_reg_idx(0x005F6C18)] = 0;
            hw->sb_istnrm |= (1 << 12); /* Maple DMA complete */
        }
        return;
    }

    switch (phys) {
    case PVR_SOFTRESET:
        if (val & 1) {
            printf("[PVR] Soft reset\n");
        }
        break;

    case PVR_STARTRENDER:
        hw->pvr_rendering = true;
        hw->frame_count++;
        if (hw->frame_count <= 3) {
            int pkts, verts, polys;
            pvr2_ta_get_stats(&pkts, &verts, &polys);
            printf("[PVR] STARTRENDER frame %d: %d packets, %d verts, %d polys\n",
                   hw->frame_count, pkts, verts, polys);
        }
        /* Render submitted geometry via OpenGL */
        { static int skip = -1; if (skip < 0) skip = getenv("DCRECOMP_NO_RENDER") ? 1 : 0;
          if (!skip) pvr2_render_frame(); }

        /* Take the picture before the swap, while the frame is still the back
         * buffer. */
        { static const char *shot = NULL; static long want = 0; static long n = 0;
          static int checked = 0;
          if (!checked) {
              checked = 1;
              shot = getenv("DCRECOMP_SCREENSHOT");
              const char *w = getenv("DCRECOMP_SCREENSHOT_FRAME");
              want = w ? atol(w) : 1;
          }
          if (shot && ++n == want) {
              pvr2_screenshot(shot);
              shot = NULL;
          } }
        platform_swap_buffers();
        pvr2_ta_reset();
        /* Poll input events to keep window responsive */
        platform_poll_events(hw);
        /* Signal render complete via interrupt */
        { static unsigned frames = 0; static uint64_t t0 = 0;
          uint64_t now = platform_get_ticks_ms();
          if (!t0) t0 = now;
          if (++frames % 60 == 0)
              printf("[FRAME] %u rendered, %.1f fps\n", frames,
                     frames * 1000.0 / (double)(now - t0 + 1)); }
        hw->sb_istnrm |= (1 << 0) | (1 << 1) | (1 << 2); /* render done: video, isp, tsp */
        hw->pvr_rendering = false;
        break;

    case PVR_FB_ADDR1:
        hw->pvr_fb_addr1 = val;
        break;

    case PVR_FB_ADDR2:
        hw->pvr_fb_addr2 = val;
        break;

    case PVR_FB_RENDER:
        hw->pvr_render_addr = val;
        break;

    case SB_ISTNRM:
        /* Writing 1 bits clears them */
        hw->sb_istnrm &= ~val;
        break;

    case SB_ISTEXT:
        hw->sb_istext &= ~val;
        break;

    case SB_ISTERR:
        hw->sb_isterr &= ~val;
        break;

    case AICA_ARM_RESET:
        hw->aica_arm_running = !(val & 1);
        printf("[AICA] ARM %s\n", hw->aica_arm_running ? "started" : "reset");
        break;

    case TA_LIST_INIT:
        hw->ta_fifo_pos = 0;
        pvr2_ta_reset();
        break;

    /* G2 channels: AICA, Ext1, Ext2, Dev - and the PVR channel. */
    case 0x005F7818: if (val & 1) run_dma(hw, 0x005F7800, 15, "AICA"); return;
    case 0x005F7838: if (val & 1) run_dma(hw, 0x005F7820, 16, "EXT1"); return;
    case 0x005F7858: if (val & 1) run_dma(hw, 0x005F7840, 17, "EXT2"); return;
    case 0x005F7878: if (val & 1) run_dma(hw, 0x005F7860, 18, "DEV");  return;

    case SB_C2DST:
        if (val & 1) {
            /* CH2-DMA: SH4 DMAC Channel 2 → PVR (TA FIFO or VRAM)
             * Source: SH4 DMAC SAR2 register (system RAM address)
             * Dest:   SB_C2DSTAT (PVR address, default 0x10000000 = TA FIFO)
             * Length: SB_C2DLEN */
            uint32_t c2d_dest = hw->hw_regs[hw_reg_idx(SB_C2DSTAT)];
            uint32_t c2d_len  = hw->hw_regs[hw_reg_idx(SB_C2DLEN)];
            uint32_t c2d_src  = sh4_get_dmac_reg(DMAC_SAR2_IDX);
            uint32_t ram_mask = sh4_get_ram_mask();

            static int ch2_log = 0;
            if (ch2_log < 20) {
                ch2_log++;
                printf("[CH2-DMA] TRIGGER: src=0x%08X dest=0x%08X len=%u\n",
                       c2d_src, c2d_dest, c2d_len);
            }

            if (c2d_len > 0) {
                uint8_t *ram = sh4_get_ram_ptr();
                if (ram) {
                    uint32_t src_offset = (c2d_src & 0x1FFFFFFF) & ram_mask;
                    uint32_t dest_phys = c2d_dest & 0x1FFFFFFF;

                    if (dest_phys >= 0x10000000 && dest_phys < 0x10800000) {
                        /* DMA to TA FIFO — send as 32-byte packets */
                        int packets = 0;
                        for (uint32_t off = 0; off + 32 <= c2d_len; off += 32) {
                            const uint32_t *pkt = (const uint32_t *)(ram + ((src_offset + off) & ram_mask));
                            pvr2_ta_write(pkt);
                            packets++;
                        }
                        if (ch2_log <= 20) {
                            printf("[CH2-DMA] Sent %d packets (%u bytes) to TA FIFO\n",
                                   packets, c2d_len);
                        }
                    } else if (dest_phys >= 0x11000000 && dest_phys < 0x12000000) {
                        /* DMA to TA texture path → VRAM write */
                        uint8_t *vram = sh4_get_vram_ptr();
                        if (vram) {
                            uint32_t vram_offset = dest_phys - 0x11000000;
                            uint32_t copy_len = c2d_len;
                            if (vram_offset + copy_len > DC_VRAM_SIZE)
                                copy_len = DC_VRAM_SIZE - vram_offset;
                            memcpy(vram + vram_offset, ram + src_offset, copy_len);
                            if (ch2_log <= 20)
                                printf("[CH2-DMA] Texture upload: %u bytes to VRAM[0x%06X]\n",
                                       copy_len, vram_offset);
                        }
                    } else if (dest_phys >= DC_VRAM_BASE && dest_phys < DC_VRAM_BASE + DC_VRAM_SIZE) {
                        /* DMA to VRAM (direct) */
                        uint8_t *vram = sh4_get_vram_ptr();
                        if (vram) {
                            uint32_t vram_offset = dest_phys - DC_VRAM_BASE;
                            uint32_t copy_len = c2d_len;
                            if (vram_offset + copy_len > DC_VRAM_SIZE)
                                copy_len = DC_VRAM_SIZE - vram_offset;
                            memcpy(vram + vram_offset, ram + src_offset, copy_len);
                            if (ch2_log <= 20)
                                printf("[CH2-DMA] Direct VRAM: %u bytes to VRAM[0x%06X]\n",
                                       copy_len, vram_offset);
                        }
                    } else {
                        if (ch2_log <= 20)
                            printf("[CH2-DMA] Unknown dest 0x%08X (len=%u)\n",
                                   c2d_dest, c2d_len);
                    }
                }
            }

            /* Update SB_C2DSTAT to reflect end position */
            hw->hw_regs[hw_reg_idx(SB_C2DSTAT)] = c2d_dest + c2d_len;
            hw->hw_regs[hw_reg_idx(SB_C2DLEN)] = 0;
            hw->sb_istnrm |= (1 << 19); /* CH2-DMA complete interrupt */
            hw->hw_regs[hw_reg_idx(SB_C2DST)] = 0;
        }
        break;

    case SB_SDST:
        if (val & 1) {
            /* Sort-DMA: system RAM → TA with sorting */
            uint32_t sd_tag  = hw->hw_regs[hw_reg_idx(SB_SDSTAG)];
            uint32_t sd_star = hw->hw_regs[hw_reg_idx(SB_SDSTAR)];
            uint32_t sd_len  = hw->hw_regs[hw_reg_idx(SB_SDLEN)];
            uint32_t sd_dir  = hw->hw_regs[hw_reg_idx(SB_SDDIR)];
            printf("[SORT-DMA] TRIGGER: tag=0x%08X star=0x%08X len=%u dir=%u\n",
                   sd_tag, sd_star, sd_len, sd_dir);
            if (sd_dir == 0 && sd_len > 0) {
                uint8_t *ram = sh4_get_ram_ptr();
                if (ram) {
                    /* Sort-DMA uses a table of pointers (sort table) to
                     * traverse linked list entries and send to TA in sorted order.
                     * For now, do a simple linear DMA from sd_star to TA. */
                    uint32_t ram_offset = sd_star & 0x00FFFFFF;
                    int packets = 0;
                    for (uint32_t off = 0; off + 32 <= sd_len; off += 32) {
                        const uint32_t *pkt = (const uint32_t *)(ram + ram_offset + off);
                        pvr2_ta_write(pkt);
                        packets++;
                    }
                    printf("[SORT-DMA] Sent %d packets (%u bytes) to TA\n",
                           packets, sd_len);
                }
            }
            hw->sb_istnrm |= (1 << 20); /* Sort-DMA complete */
            hw->hw_regs[hw_reg_idx(SB_SDST)] = 0;
        }
        break;

    /* SB_MDST handled above the switch via if-check */

    case SB_PDST:
        if (val & 1) {
            /* PVR DMA trigger - transfer data from system RAM to TA FIFO */
            uint32_t pvr_addr = hw->hw_regs[hw_reg_idx(SB_PDSTAP)];
            uint32_t sys_addr = hw->hw_regs[hw_reg_idx(SB_PDSTAR)];
            uint32_t length   = hw->hw_regs[hw_reg_idx(SB_PDLEN)];
            uint32_t dir      = hw->hw_regs[hw_reg_idx(SB_PDDIR)];
            printf("[PVR-DMA] TRIGGER: sys=0x%08X pvr=0x%08X len=%u dir=%u\n",
                   sys_addr, pvr_addr, length, dir);
            if (dir == 0 && length > 0) {
                /* DMA to PVR (TA FIFO or VRAM) */
                uint8_t *ram = sh4_get_ram_ptr();
                if (ram) {
                    /* System address needs P1/P2 strip */
                    uint32_t ram_offset = sys_addr & 0x00FFFFFF;
                    uint32_t bytes = length;
                    int packets = 0;
                    if (pvr_addr >= 0x10000000 && pvr_addr < 0x10800000) {
                        /* DMA to TA FIFO - send as 32-byte packets */
                        for (uint32_t off = 0; off + 32 <= bytes; off += 32) {
                            const uint32_t *pkt = (const uint32_t *)(ram + ram_offset + off);
                            pvr2_ta_write(pkt);
                            packets++;
                        }
                        printf("[PVR-DMA] Sent %d packets (%u bytes) to TA FIFO\n",
                               packets, bytes);
                    } else {
                        printf("[PVR-DMA] Non-TA destination 0x%08X (len=%u) - TODO\n",
                               pvr_addr, bytes);
                    }
                }
            }
            /* Signal DMA complete */
            hw->sb_istnrm |= (1 << 11); /* PVR-DMA complete */
            hw->hw_regs[hw_reg_idx(SB_PDST)] = 0; /* Clear start bit */
        }
        break;

    default:
        if (phys >= 0x005F6800 && phys < 0x005FA000) {
            hw->hw_regs[hw_reg_idx(phys)] = val;
        }
        /* TA FIFO writes (0x10000000 - 0x107FFFFF)
         * Direct word-by-word writes accumulate into 32-byte packets.
         * NOTE: Only process if TA list has been initialized (ta_fifo_pos >= 0).
         * Scattered writes to random offsets are likely VRAM/texture ops, not TA. */
        if (phys >= 0x10000000 && phys < 0x10800000) {
            static int ta_direct_count = 0;
            ta_direct_count++;
            if (ta_direct_count <= 5) {
                printf("[TA-DIRECT] word #%d to 0x%08X val=0x%08X (fifo_pos=%d)\n",
                       ta_direct_count, phys, val, hw->ta_fifo_pos);
            } else if (ta_direct_count == 1000) {
                printf("[TA-DIRECT] 1000 direct writes to TA range (suppressing)\n");
            }
            /* Accumulate into FIFO and dispatch 32-byte packets */
            hw->ta_fifo[hw->ta_fifo_pos & 7] = val;
            hw->ta_fifo_pos++;
            if ((hw->ta_fifo_pos & 7) == 0) {
                pvr2_ta_write(hw->ta_fifo);
            }
        }
        break;
    }
}

/* ========== PVR GPU ========== */

void dc_pvr_init(DCHardware *hw) {
    printf("[PVR] PowerVR2 initialized\n");
    hw->pvr_rendering = false;
    hw->frame_count = 0;
}

void dc_pvr_start_render(DCHardware *hw) {
    hw->pvr_rendering = true;
    hw->frame_count++;
}

void dc_pvr_submit_vertex(DCHardware *hw, const PVRVertex *vtx) {
    (void)hw;
    (void)vtx;
    /* TODO: Buffer vertex for rendering via OpenGL */
}

void dc_pvr_begin_list(DCHardware *hw, PVRListType type) {
    (void)hw;
    (void)type;
    /* TODO: Begin polygon list submission */
}

void dc_pvr_end_list(DCHardware *hw) {
    (void)hw;
    /* TODO: End polygon list */
}

/* ---- AICA registers ----------------------------------------------------
 *
 * 0x00700000-0x00707FFF. Mostly channel state the ARM7 owns; the one that
 * matters to us is 0x00702C00, whose bit 0 holds that processor in reset. */
#define AICA_REG_SIZE   0x8000
#define AICA_ARM_RESET  0x2C00

static uint32_t g_aica_regs[AICA_REG_SIZE / 4];

uint32_t dc_aica_reg_read(uint32_t off) {
    return g_aica_regs[(off & (AICA_REG_SIZE - 1)) / 4];
}

void dc_aica_reg_write(uint32_t off, uint32_t val) {
    off &= AICA_REG_SIZE - 1;
    uint32_t prev = g_aica_regs[off / 4];
    g_aica_regs[off / 4] = val;

    if (off == AICA_ARM_RESET && (prev & 1) && !(val & 1)) {
        /* Released. On hardware the driver boots, works through the commands
         * the game left in sound RAM and interrupts the SH-4 to report them
         * done. Report for it - the game's handler is what moves its own state
         * machine on, and without this it sits waiting on a processor that is
         * never going to answer. */
        printf("[AICA] sound processor released from reset - "
               "answering for the driver (stub)\n");
        sh4_aica_arm_released();
    }
}

uint32_t dc_hw_get_istnrm(DCHardware *hw) {
    return hw ? hw->sb_istnrm : 0;
}

uint32_t dc_hw_get_istext(DCHardware *hw) {
    return hw ? hw->sb_istext : 0;
}

void dc_hw_raise_istnrm(int bit) {
    DCHardware *hw = sh4_get_hardware();
    if (hw)
        hw->sb_istnrm |= (1u << bit);
}

void dc_pvr_wait_vblank(DCHardware *hw) {
    hw->vblank_count++;

    /* Scan out whatever is in the framebuffer. A game fills VRAM either through
     * the tile accelerator or by writing it directly, and until it reaches its
     * render loop only the latter has happened - so presenting here is what
     * makes early frames visible at all. */
    static int nopresent = -1;
    if (nopresent < 0) nopresent = getenv("DCRECOMP_NO_PRESENT") ? 1 : 0;
    if (!nopresent && !pvr2_ta_has_drawn() &&
        pvr2_present_framebuffer(hw->pvr_fb_addr1,
                                 hw->hw_regs[hw_reg_idx(PVR_FB_R_CTRL)],
                                 DC_SCREEN_W, DC_SCREEN_H) > 0)
        platform_swap_buffers();
    platform_poll_events(hw);
    /* Set VBLANK interrupt */
    hw->sb_istnrm |= (1 << 3); /* VBlank-IN */
}

/* ========== Maple (Controllers) ========== */

void dc_maple_init(DCHardware *hw) {
    printf("[MAPLE] Controller bus initialized\n");
    hw->controller_connected[0] = true;
}

void dc_maple_poll(DCHardware *hw) {
    /* TODO: Read from SDL2 game controller */
    /* For now, controllers maintain their current state */
    (void)hw;
}

/* Process Maple DMA command table */
void dc_maple_dma(DCHardware *hw, uint32_t mdstar) {
    uint8_t *ram = sh4_get_ram_ptr();
    if (!ram) return;

    uint32_t addr = mdstar & 0x1FFFFFFF;
    bool last = false;
    int cmd_count = 0;
    static int maple_log = 0;

    while (!last && cmd_count < 32) {
        cmd_count++;

        /* Read DMA descriptor */
        uint32_t header_1 = *(uint32_t *)(ram + (addr & 0x01FFFFFF));
        uint32_t header_2 = *(uint32_t *)(ram + ((addr + 4) & 0x01FFFFFF)) & 0x1FFFFFE0;

        last = (header_1 >> 31) & 1;
        uint32_t plen = (header_1 & 0xFF) + 1;  /* length in 32-bit words */
        uint32_t maple_op = (header_1 >> 8) & 7;
        uint32_t bus = (header_1 >> 16) & 3;

        if (maple_op == 0) {
            /* MP_Start - process Maple frame */
            uint32_t *p_data = (uint32_t *)(ram + ((addr + 8) & 0x01FFFFFF));
            uint32_t frame_header = p_data[0];
            uint32_t command = frame_header & 0xFF;
            uint32_t reci = (frame_header >> 8) & 0xFF;
            uint32_t port = reci >> 6;

            /* Response destination */
            uint32_t *resp = (uint32_t *)(ram + (header_2 & 0x01FFFFFF));

            if (maple_log < 40) {
                maple_log++;
                printf("[MAPLE] bus=%u cmd=0x%02X port=%u reci=0x%02X resp_before=0x%08X resp_addr=0x%08X\n",
                       bus, command, port, reci, resp[0], header_2);
            }

            /* Maple response: byte0=response_code, byte1=src, byte2=dst, byte3=len_words */
            uint32_t src_addr = reci;
            uint32_t dst_addr = (frame_header >> 16) & 0xFF;
            uint8_t *resp_bytes = (uint8_t *)resp;
            uint8_t *in_bytes = (uint8_t *)p_data;

            /* Check if port has a connected device */
            bool has_device = (port < 4) && hw->controller_connected[port];

            /* Non-main-unit subdevices (reci & 0x1F != 0) or no device → -1 */
            if (!has_device || (reci & 0x1F) != 0) {
                resp[0] = (uint32_t)-1;
            } else {
                MapleController *ctrl = &hw->controllers[port];
                switch (command) {
                case 0x01: { /* MDC_DeviceRequest - get device info */
                    /* MDRS_DeviceStatus (0x05), 28 words of device info */
                    resp[0] = 0x05 | (src_addr << 8) | (dst_addr << 16) | (0x1C << 24);
                    /* Function types: controller (bit 0) */
                    resp[1] = 0x01000000; /* ft0 = Controller */
                    resp[2] = 0x00000000; /* ft1 = none */
                    resp[3] = 0x00000000; /* ft2 = none */
                    /* Function definition block 0: controller capabilities
                     * Bits[31:24] = 0xFF = all standard buttons
                     * Bits[23:20] = 0x0  = R trigger present
                     * Bits[19:16] = 0x6  = L trigger + analog axes */
                    resp[4] = 0xFE060000;
                    resp[5] = 0x00000000;
                    resp[6] = 0x00000000;
                    /* Product name (30 bytes) + license (60 bytes) + current */
                    memset(&resp[7], 0x20, 84);
                    memcpy(&resp[7], "Dreamcast Controller          ", 30);
                    memcpy(&resp[15], "Produced By or Under License From SEGA ENTERPRISES,LTD.   ", 58);
                    resp[29] = 0x01AE; /* standby 430mA / max 430mA */
                    break;
                }

                case 0x03: /* MDC_DeviceReset */
                    resp[0] = 0x07 | (src_addr << 8) | (dst_addr << 16) | (0x00 << 24);
                    break;

                case 0x05: /* MDC_AllStatusReq */
                    resp[0] = 0x07 | (src_addr << 8) | (dst_addr << 16) | (0x00 << 24);
                    break;

                case 0x09: { /* MDC_GetCondition */
                    /* Request data[1] = function type to query */
                    uint32_t func_type = p_data[1];
                    if ((func_type & 0x01000000) || func_type == 0x01) {
                        /* Controller condition: 3 data words */
                        resp[0] = 0x08 | (src_addr << 8) | (dst_addr << 16) | (0x03 << 24);
                        resp[1] = 0x01000000; /* ft = Controller */
                        /* Condition data word 1:
                         * bits[15:0] = buttons (active LOW - 0xFFFF = all released)
                         * bits[23:16] = R trigger (0-255)
                         * bits[31:24] = L trigger (0-255) */
                        resp[2] = (uint32_t)ctrl->buttons
                                | ((uint32_t)ctrl->rtrig << 16)
                                | ((uint32_t)ctrl->ltrig << 24);
                        /* Condition data word 2:
                         * bits[7:0]   = joystick X (0=left, 128=center, 255=right)
                         * bits[15:8]  = joystick Y (0=up, 128=center, 255=down)
                         * bits[23:16] = joy2 X
                         * bits[31:24] = joy2 Y */
                        resp[3] = (uint32_t)(uint8_t)(ctrl->joyx + 128)
                                | ((uint32_t)(uint8_t)(ctrl->joyy + 128) << 8)
                                | ((uint32_t)(uint8_t)(ctrl->joy2x + 128) << 16)
                                | ((uint32_t)(uint8_t)(ctrl->joy2y + 128) << 24);
                    } else {
                        resp[0] = (uint32_t)-1; /* Unknown function type */
                    }
                    break;
                }

                /* === Naomi JVS commands (0x80+) === */
                case 0x80: { /* MDC_JVSUploadFirmware */
                    uint8_t sum = 0;
                    for (uint32_t i = 4; i < plen * 4; i++)
                        sum += in_bytes[i];
                    resp[0] = 0x80 | (src_addr << 8) | (dst_addr << 16) | (0x01 << 24);
                    resp[1] = (uint32_t)sum;
                    resp[2] = 0x07 | (src_addr << 8) | (dst_addr << 16) | (0x00 << 24);
                    break;
                }

                case 0x82: { /* MDC_JVSGetId */
                    static const char MIE_ID[56] = "315-6149    COPYRIGHT SEGA ENTERPRISES CO,LTD.  1998";
                    resp[0] = 0x83 | (src_addr << 8) | (dst_addr << 16) | (0x07 << 24);
                    memcpy(&resp[1], MIE_ID, 28);
                    resp[8] = 0x83 | (src_addr << 8) | (dst_addr << 16) | (0x05 << 24);
                    memcpy(&resp[9], MIE_ID + 28, 20);
                    break;
                }

                case 0x84: /* MDC_JVSSelfTest */
                    resp[0] = 0x85 | (src_addr << 8) | (dst_addr << 16) | (0x01 << 24);
                    resp[1] = 0;
                    break;

                case 0x86: { /* MDC_JVSCommand */
                    uint8_t subcmd = in_bytes[4];
                    if (maple_log < 20) {
                        maple_log++;
                        printf("[JVS] subcmd=0x%02X\n", subcmd);
                    }
                    /* Generic OK response for all JVS subcommands */
                    resp[0] = 0x87 | (src_addr << 8) | (dst_addr << 16) | (0x01 << 24);
                    resp[1] = 0;
                    if (subcmd == 0x15) {
                        /* Read controls: return 14 words */
                        resp[0] = 0x87 | (src_addr << 8) | (dst_addr << 16) | (0x0E << 24);
                        resp[1] = 0x00000001;
                        for (int i = 2; i <= 14; i++) resp[i] = 0;
                    }
                    break;
                }

                default:
                    resp[0] = (uint32_t)-1;
                    break;
                }
            }
        }
        /* else: NOP, Reset, SDCKB - just skip */

        /* Advance to next descriptor */
        addr += 8 + plen * 4;
    }
}

MapleController* dc_maple_get_controller(DCHardware *hw, int port) {
    if (port < 0 || port >= 4) return NULL;
    if (!hw->controller_connected[port]) return NULL;
    return &hw->controllers[port];
}

/* ========== AICA Sound ========== */

void dc_aica_init(DCHardware *hw) {
    printf("[AICA] Sound processor initialized\n");
    hw->aica_arm_running = false;
}

void dc_aica_write_channel(DCHardware *hw, int ch, uint32_t offset, uint32_t val) {
    (void)hw;
    (void)ch;
    (void)offset;
    (void)val;
    /* TODO: Implement AICA channel control */
}

void dc_aica_update(DCHardware *hw) {
    (void)hw;
    /* TODO: Mix audio channels and output via SDL2 */
}

/* ========== GD-ROM ========== */

/* The drive signals completion with an external interrupt: SB_ISTEXT bit 0,
 * summarised into SB_ISTNRM bit 30. A game's interrupt dispatcher is what then
 * runs the driver's completion callback, so a command that finishes without
 * this is a command the driver never learns about. */
void dc_gdrom_signal_complete(DCHardware *hw) {
    if (!hw) return;
    hw->sb_istext |= 1u;          /* external: GD-ROM */
    hw->sb_istnrm |= (1u << 30);  /* summary bit for external interrupts */
    hw->sb_istnrm |= (1u << 14);  /* G1 DMA end - how a DMAREAD finishes */
}

void dc_gdrom_init(DCHardware *hw) {
    printf("[GDROM] Drive initialized\n");
    hw->gdrom_status = 0;
}

int dc_gdrom_read_sectors(DCHardware *hw, uint32_t lba, uint32_t count, void *buf) {
    (void)hw;
    (void)lba;
    (void)count;
    (void)buf;
    /* TODO: Read from extracted disc files */
    return 0;
}
