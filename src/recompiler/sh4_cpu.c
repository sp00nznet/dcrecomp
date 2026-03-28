/**
 * SH-4 CPU State Implementation
 *
 * Manages CPU state and memory access for statically recompiled
 * Dreamcast/Naomi games. Memory accesses are routed through this layer
 * to handle the memory map, including mirrored regions and hardware registers.
 */

#include "recompiler/sh4_cpu.h"
#include "hal/dc_hardware.h"
#include "hal/pvr2.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* External hardware reference (set during init) */
static DCHardware *g_hardware = NULL;
static SH4CPU *g_cpu_ref = NULL;

void sh4_set_hardware(DCHardware *hw) {
    g_hardware = hw;
}

void sh4_set_cpu_ref(SH4CPU *cpu) {
    g_cpu_ref = cpu;
}

uint8_t *sh4_get_ram_ptr(void) {
    return g_cpu_ref ? g_cpu_ref->ram : NULL;
}

uint8_t *sh4_get_vram_ptr(void) {
    return g_cpu_ref ? g_cpu_ref->vram : NULL;
}

void sh4_init_ex(SH4CPU *cpu, uint32_t ram_size) {
    memset(cpu, 0, sizeof(SH4CPU));

    /* Set RAM size and mask */
    cpu->ram_size = ram_size;
    cpu->ram_mask = ram_size - 1;

    /* Allocate memory regions */
    cpu->ram = (uint8_t *)calloc(1, ram_size);
    cpu->vram = (uint8_t *)calloc(1, DC_VRAM_SIZE);
    cpu->aica_ram = (uint8_t *)calloc(1, DC_AICA_SIZE);

    if (!cpu->ram || !cpu->vram || !cpu->aica_ram) {
        fprintf(stderr, "FATAL: Failed to allocate memory (RAM=%uMB)\n",
                ram_size / (1024 * 1024));
        exit(1);
    }

    printf("[SH4] Allocated %uMB RAM, %uMB VRAM, %uMB AICA\n",
           ram_size / (1024 * 1024),
           DC_VRAM_SIZE / (1024 * 1024),
           DC_AICA_SIZE / (1024 * 1024));

    sh4_reset(cpu);
}

void sh4_init(SH4CPU *cpu) {
    sh4_init_ex(cpu, DC_RAM_SIZE);
}

void sh4_reset(SH4CPU *cpu) {
    /* Reset registers to power-on defaults */
    memset(cpu->r, 0, sizeof(cpu->r));
    memset(cpu->r_bank, 0, sizeof(cpu->r_bank));

    cpu->sr = 0x700000F0;   /* MD=1, RB=1, BL=1, IMASK=0xF */
    cpu->gbr = 0;
    cpu->vbr = 0;
    cpu->mach = 0;
    cpu->macl = 0;
    cpu->pr = 0;
    cpu->pc = GAME_LOAD_ADDR;

    /* FPU defaults */
    cpu->fpscr = 0x00040001; /* DN=1, RM=nearest */

    /* QACR defaults: route SQ0/SQ1 to TA FIFO area (0x10000000) */
    cpu->qacr[0] = 0x10;
    cpu->qacr[1] = 0x10;
    cpu->fpul = 0;
    memset(cpu->fr, 0, sizeof(cpu->fr));
    memset(cpu->xf, 0, sizeof(cpu->xf));

    /* Stack pointer */
    cpu->r[15] = 0x8C00F400;

    cpu->running = true;
    cpu->cycles = 0;
    cpu->delay_slot = 0;

    /* MMU starts disabled */
    cpu->mmucr = 0;

    printf("[MMU] Initialized (heuristic P0/P3 -> RAM mapping active)\n");
}

void sh4_destroy(SH4CPU *cpu) {
    free(cpu->ram);
    free(cpu->vram);
    free(cpu->aica_ram);
    cpu->ram = NULL;
    cpu->vram = NULL;
    cpu->aica_ram = NULL;
}

/* ========== Memory Access ========== */

static uint32_t translate_addr_cpu(SH4CPU *cpu, uint32_t addr) {
    /* P2 area (0xA0000000-0xBFFFFFFF): kernel, uncached */
    if (addr >= 0xA0000000 && addr < 0xC0000000) {
        return addr & ADDR_MASK_P1;
    }

    /* P1 area (0x80000000-0x9FFFFFFF): kernel, cached */
    if (addr >= 0x80000000 && addr < 0xA0000000) {
        uint32_t phys = addr & ADDR_MASK_P1;
        if (phys < 0x01000000) return phys;
        if (phys >= DC_VRAM_BASE && phys < DC_VRAM_BASE + DC_VRAM_SIZE) return phys;
        if (phys >= 0x05000000 && phys < 0x05800000) return phys;
        if (phys >= DC_RAM_BASE && phys < DC_RAM_BASE + cpu->ram_size) return phys;
        if (phys >= 0x005F6800 && phys < 0x005FA000) return phys;
        return DC_RAM_BASE + (addr & cpu->ram_mask);
    }

    /* P4 area (0xE0000000-0xFFFFFFFF): on-chip resources */
    if (addr >= 0xE0000000) {
        return 0xFFFFFFFF;
    }

    /* P0/U0 and P3: use TLB if MMU enabled */
    if (cpu && (cpu->mmucr & 1)) {
        for (int i = 0; i < 64; i++) {
            uint32_t entry_addr = cpu->utlb_addr[i];
            if (!(entry_addr & 0x100)) continue;

            uint32_t entry_data = cpu->utlb_data1[i];
            int sz = ((entry_data >> 6) & 2) | ((entry_data >> 4) & 1);
            uint32_t page_mask;
            switch (sz) {
            case 0: page_mask = 0xFFFFFC00; break;
            case 1: page_mask = 0xFFFFF000; break;
            case 2: page_mask = 0xFFFF0000; break;
            case 3: page_mask = 0xFFF00000; break;
            default: page_mask = 0xFFF00000; break;
            }

            uint32_t vpn = entry_addr & page_mask;
            if ((addr & page_mask) == vpn) {
                uint32_t ppn = entry_data & 0x1FFFFC00;
                uint32_t offset = addr & ~page_mask;
                return ppn | offset;
            }
        }
    }

    /* Direct map with heuristics */
    uint32_t raw_phys = addr & ADDR_MASK_P1;
    if (raw_phys < 0x01000000) return raw_phys;
    if (raw_phys >= DC_VRAM_BASE && raw_phys < DC_VRAM_BASE + DC_VRAM_SIZE)
        return raw_phys;
    if (raw_phys >= 0x05000000 && raw_phys < 0x05800000)
        return raw_phys;
    if (raw_phys >= DC_RAM_BASE && raw_phys < DC_RAM_BASE + (cpu ? cpu->ram_size : DC_RAM_SIZE))
        return raw_phys;

    return DC_RAM_BASE + (addr & (cpu ? cpu->ram_mask : DC_RAM_MASK_16MB));
}

static uint32_t translate_addr(uint32_t addr) {
    return translate_addr_cpu(g_cpu_ref, addr);
}

static bool is_hw_register(uint32_t phys_addr) {
    if (phys_addr >= 0x005F6800 && phys_addr < 0x005FA000)
        return true;
    if (phys_addr >= 0x10000000 && phys_addr < 0x10800000)
        return true;
    if (phys_addr >= 0x10800000 && phys_addr < 0x11000000)
        return true;
    return false;
}

uint8_t sh4_read8(SH4CPU *cpu, uint32_t addr) {
    uint32_t phys = translate_addr(addr);

    if (phys >= DC_RAM_BASE && phys < DC_RAM_BASE + cpu->ram_size) {
        return cpu->ram[phys & cpu->ram_mask];
    }
    if (phys >= DC_VRAM_BASE && phys < DC_VRAM_BASE + DC_VRAM_SIZE) {
        return cpu->vram[phys - DC_VRAM_BASE];
    }
    if (phys >= 0x05000000 && phys < 0x05800000) {
        return cpu->vram[phys - 0x05000000];
    }
    if (phys >= DC_AICA_BASE && phys < DC_AICA_BASE + DC_AICA_SIZE) {
        return cpu->aica_ram[phys - DC_AICA_BASE];
    }
    if (is_hw_register(phys) && g_hardware) {
        uint32_t aligned = phys & ~3;
        uint32_t val = dc_hw_read32(g_hardware, aligned);
        int shift = (phys & 3) * 8;
        return (val >> shift) & 0xFF;
    }
    return 0;
}

uint16_t sh4_read16(SH4CPU *cpu, uint32_t addr) {
    uint32_t phys = translate_addr(addr);

    if (phys >= DC_RAM_BASE && phys < DC_RAM_BASE + cpu->ram_size) {
        return *(uint16_t *)(cpu->ram + (phys & cpu->ram_mask));
    }
    if (phys >= DC_VRAM_BASE && phys < DC_VRAM_BASE + DC_VRAM_SIZE) {
        return *(uint16_t *)(cpu->vram + (phys - DC_VRAM_BASE));
    }
    if (phys >= 0x05000000 && phys < 0x05800000) {
        return *(uint16_t *)(cpu->vram + (phys - 0x05000000));
    }
    if (phys >= DC_AICA_BASE && phys < DC_AICA_BASE + DC_AICA_SIZE) {
        return *(uint16_t *)(cpu->aica_ram + (phys - DC_AICA_BASE));
    }
    if (is_hw_register(phys) && g_hardware) {
        uint32_t aligned = phys & ~3;
        uint32_t val = dc_hw_read32(g_hardware, aligned);
        int shift = (phys & 2) * 8;
        return (val >> shift) & 0xFFFF;
    }
    return 0;
}

uint32_t sh4_read32(SH4CPU *cpu, uint32_t addr) {
    /* P4 control registers */
    if (addr >= 0xFF000000) {
        switch (addr) {
        case 0xFF000010: return cpu->mmucr;
        case 0xFF000038: return cpu->qacr[0];
        case 0xFF00003C: return cpu->qacr[1];
        }
        if (addr >= 0xFFA00000 && addr <= 0xFFA00040) {
            uint32_t idx = (addr - 0xFFA00000) / 4;
            if (idx < 17) return cpu->dmac_regs[idx];
        }
        if (addr >= 0xFFD80000 && addr <= 0xFFD8002F) {
            uint32_t idx = (addr - 0xFFD80000) / 4;
            if (idx < 12) return cpu->tmu_regs[idx];
        }
        return 0;
    }

    /* UTLB arrays */
    if (addr >= 0xF6000000 && addr < 0xF7000000) {
        return cpu->utlb_addr[(addr >> 8) & 63];
    }
    if (addr >= 0xF7000000 && addr < 0xF7800000) {
        return cpu->utlb_data1[(addr >> 8) & 63];
    }
    if (addr >= 0xF7800000 && addr < 0xF8000000) {
        return cpu->utlb_data2[(addr >> 8) & 63];
    }

    uint32_t phys = translate_addr(addr);

    if (phys >= DC_RAM_BASE && phys < DC_RAM_BASE + cpu->ram_size) {
        return *(uint32_t *)(cpu->ram + (phys & cpu->ram_mask));
    }
    if (phys >= DC_VRAM_BASE && phys < DC_VRAM_BASE + DC_VRAM_SIZE) {
        return *(uint32_t *)(cpu->vram + (phys - DC_VRAM_BASE));
    }
    if (phys >= 0x05000000 && phys < 0x05800000) {
        return *(uint32_t *)(cpu->vram + (phys - 0x05000000));
    }
    if (phys >= DC_AICA_BASE && phys < DC_AICA_BASE + DC_AICA_SIZE) {
        return *(uint32_t *)(cpu->aica_ram + (phys - DC_AICA_BASE));
    }
    if (is_hw_register(phys) && g_hardware) {
        return dc_hw_read32(g_hardware, phys);
    }
    return 0;
}

float sh4_read_float(SH4CPU *cpu, uint32_t addr) {
    union { uint32_t u; float f; } conv;
    conv.u = sh4_read32(cpu, addr);
    return conv.f;
}

void sh4_write8(SH4CPU *cpu, uint32_t addr, uint8_t val) {
    uint32_t phys = translate_addr(addr);

    if (phys >= DC_RAM_BASE && phys < DC_RAM_BASE + cpu->ram_size) {
        cpu->ram[phys & cpu->ram_mask] = val;
        return;
    }
    if (phys >= DC_VRAM_BASE && phys < DC_VRAM_BASE + DC_VRAM_SIZE) {
        cpu->vram[phys - DC_VRAM_BASE] = val;
        return;
    }
    if (phys >= 0x05000000 && phys < 0x05800000) {
        cpu->vram[phys - 0x05000000] = val;
        return;
    }
    if (phys >= DC_AICA_BASE && phys < DC_AICA_BASE + DC_AICA_SIZE) {
        cpu->aica_ram[phys - DC_AICA_BASE] = val;
        return;
    }
    if (is_hw_register(phys) && g_hardware) {
        uint32_t aligned = phys & ~3;
        uint32_t cur = dc_hw_read32(g_hardware, aligned);
        int shift = (phys & 3) * 8;
        cur &= ~(0xFF << shift);
        cur |= (uint32_t)val << shift;
        dc_hw_write32(g_hardware, aligned, cur);
    }
}

void sh4_write16(SH4CPU *cpu, uint32_t addr, uint16_t val) {
    uint32_t phys = translate_addr(addr);

    if (phys >= DC_RAM_BASE && phys < DC_RAM_BASE + cpu->ram_size) {
        *(uint16_t *)(cpu->ram + (phys & cpu->ram_mask)) = val;
        return;
    }
    if (phys >= DC_VRAM_BASE && phys < DC_VRAM_BASE + DC_VRAM_SIZE) {
        *(uint16_t *)(cpu->vram + (phys - DC_VRAM_BASE)) = val;
        return;
    }
    if (phys >= 0x05000000 && phys < 0x05800000) {
        *(uint16_t *)(cpu->vram + (phys - 0x05000000)) = val;
        return;
    }
    if (phys >= DC_AICA_BASE && phys < DC_AICA_BASE + DC_AICA_SIZE) {
        *(uint16_t *)(cpu->aica_ram + (phys - DC_AICA_BASE)) = val;
        return;
    }
    if (is_hw_register(phys) && g_hardware) {
        uint32_t aligned = phys & ~3;
        uint32_t cur = dc_hw_read32(g_hardware, aligned);
        int shift = (phys & 2) * 8;
        cur &= ~(0xFFFF << shift);
        cur |= (uint32_t)val << shift;
        dc_hw_write32(g_hardware, aligned, cur);
    }
}

uint32_t g_write_seq = 0;

void sh4_write32(SH4CPU *cpu, uint32_t addr, uint32_t val) {
    g_write_seq++;

    /* Handle ALL P4 area (0xE0000000+) BEFORE address translation */
    if (addr >= 0xE0000000) {
        /* Store Queue writes (0xE0000000-0xE3FFFFFF) */
        if (addr <= 0xE3FFFFFF) {
            int sq_idx = (addr >> 5) & 1;
            int word_idx = (addr >> 2) & 7;
            cpu->sq[sq_idx][word_idx] = val;
            return;
        }

        /* UTLB Address Array writes */
        if (addr >= 0xF6000000 && addr < 0xF7000000) {
            int entry = (addr >> 8) & 63;
            cpu->utlb_addr[entry] = val;
            static int utlb_log = 0;
            if (utlb_log < 20) {
                utlb_log++;
                printf("[UTLB] addr[%d] = 0x%08X (VPN=0x%08X V=%d)\n",
                       entry, val, val & 0xFFFFFC00, (val >> 8) & 1);
            }
            return;
        }
        /* UTLB Data Array 1 writes */
        if (addr >= 0xF7000000 && addr < 0xF7800000) {
            int entry = (addr >> 8) & 63;
            cpu->utlb_data1[entry] = val;
            static int utlb_d1_log = 0;
            if (utlb_d1_log < 20) {
                utlb_d1_log++;
                int sz = ((val >> 6) & 2) | ((val >> 4) & 1);
                static const char *sz_names[] = {"1KB", "4KB", "64KB", "1MB"};
                printf("[UTLB] data1[%d] = 0x%08X (PPN=0x%08X sz=%s)\n",
                       entry, val, val & 0x1FFFFC00, sz_names[sz]);
            }
            return;
        }
        /* UTLB Data Array 2 writes */
        if (addr >= 0xF7800000 && addr < 0xF8000000) {
            cpu->utlb_data2[(addr >> 8) & 63] = val;
            return;
        }

        /* P4 control registers (0xFF000000+) */
        if (addr >= 0xFF000000) {
            switch (addr) {
            case 0xFF000010: {
                int old_at = cpu->mmucr & 1;
                int new_at = val & 1;
                cpu->mmucr = val;
                if (val & 4) {
                    for (int i = 0; i < 64; i++)
                        cpu->utlb_addr[i] &= ~0x100;
                    cpu->mmucr &= ~4;
                    printf("[MMU] TLB invalidated\n");
                }
                if (old_at != new_at) {
                    printf("[MMU] Address translation %s (MMUCR=0x%08X seq=%u)\n",
                           new_at ? "ENABLED" : "DISABLED", cpu->mmucr, g_write_seq);
                }
                return;
            }
            case 0xFF000038: cpu->qacr[0] = val; return;
            case 0xFF00003C: cpu->qacr[1] = val; return;
            }
            if (addr >= 0xFFA00000 && addr <= 0xFFA00040) {
                uint32_t idx = (addr - 0xFFA00000) / 4;
                if (idx < 17) cpu->dmac_regs[idx] = val;
                return;
            }
            if (addr >= 0xFFD80000 && addr <= 0xFFD8002F) {
                uint32_t idx = (addr - 0xFFD80000) / 4;
                if (idx < 12) cpu->tmu_regs[idx] = val;
                return;
            }
            return;
        }
        return;
    }

    /* Normal address translation */
    uint32_t phys = translate_addr(addr);

    if (phys >= DC_RAM_BASE && phys < DC_RAM_BASE + cpu->ram_size) {
        /* Protect allocator vtable pointer: keep default vtable (0x8C1733A4)
         * The init code overwrites it with a free list node address, which
         * breaks the vtable dispatch. Block the second write. */
        if (phys == 0x0C2FB7A4) {
            static int wp_count = 0;
            if (wp_count == 0) {
                /* First write: allow (sets up default vtable) */
                wp_count++;
            } else {
                /* Subsequent writes: block (would break vtable) */
                return;
            }
        }
        *(uint32_t *)(cpu->ram + (phys & cpu->ram_mask)) = val;
        return;
    }
    if (phys >= DC_VRAM_BASE && phys < DC_VRAM_BASE + DC_VRAM_SIZE) {
        *(uint32_t *)(cpu->vram + (phys - DC_VRAM_BASE)) = val;
        return;
    }
    if (phys >= 0x05000000 && phys < 0x05800000) {
        *(uint32_t *)(cpu->vram + (phys - 0x05000000)) = val;
        return;
    }
    if (phys >= DC_AICA_BASE && phys < DC_AICA_BASE + DC_AICA_SIZE) {
        *(uint32_t *)(cpu->aica_ram + (phys - DC_AICA_BASE)) = val;
        return;
    }
    if (is_hw_register(phys) && g_hardware) {
        dc_hw_write32(g_hardware, phys, val);
    }
}

void sh4_write_float(SH4CPU *cpu, uint32_t addr, float val) {
    union { float f; uint32_t u; } conv;
    conv.f = val;
    sh4_write32(cpu, addr, conv.u);
}

/* ========== Store Queue Prefetch ========== */

static int sq_log_count = 0;

void sh4_sq_prefetch(SH4CPU *cpu, uint32_t addr) {
    if (addr < 0xE0000000 || addr > 0xE3FFFFFF) return;

    if (sq_log_count < 5) {
        sq_log_count++;
        printf("[SQ] prefetch addr=0x%08X qacr0=0x%X qacr1=0x%X\n",
               addr, cpu->qacr[0], cpu->qacr[1]);
    }

    int sq_idx = (addr >> 5) & 1;
    uint32_t qacr = cpu->qacr[sq_idx];
    uint32_t dest = (((qacr >> 2) & 7) << 26) | (addr & 0x03FFFFE0);

    if (dest >= 0x10000000 && dest < 0x10800000) {
        pvr2_ta_write(cpu->sq[sq_idx]);
    } else if (dest >= DC_VRAM_BASE && dest < DC_VRAM_BASE + DC_VRAM_SIZE) {
        uint32_t offset = dest - DC_VRAM_BASE;
        if (offset + 32 <= DC_VRAM_SIZE) {
            memcpy(cpu->vram + offset, cpu->sq[sq_idx], 32);
        }
    } else {
        for (int i = 0; i < 8; i++) {
            sh4_write32(cpu, dest + i * 4, cpu->sq[sq_idx][i]);
        }
    }
}
