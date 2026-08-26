/**
 * ARM7DI interpreter - the processor inside the AICA.
 *
 * ARMv3: ARM state only, no Thumb, no MMU, no long multiplies. Written from the
 * ARM Architecture Reference; deliberately not derived from any emulator, since
 * the obvious reference for this hardware is GPLv2 and this is MIT.
 *
 * Two things are easy to get wrong and worth stating up front.
 *
 * The PC reads eight ahead. The ARM7 fetches two instructions past the one it
 * is executing, so an instruction that reads r15 sees its own address plus
 * eight. This interpreter keeps r15 at the *next* instruction between steps and
 * moves it to address+8 for the duration of one, which is the same thing seen
 * from the instruction's side.
 *
 * The barrel shifter's carry is not the ALU's carry. A shift produces a carry
 * of its own, it feeds the flags when the operation does not otherwise set
 * them, and the zero-length forms mean something different from what they look
 * like - LSR #0 is LSR #32, ROR #0 is rotate-right-extended through carry.
 */

#include "hal/arm7.h"
#include "hal/dc_hardware.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* The ARM's view: sound RAM low, the AICA's own registers at 8MB. */
#define ARM7_REG_BASE  0x00800000u
#define ARM7_REG_SIZE  0x00008000u

static uint8_t  *g_ram;
static uint32_t  g_ram_mask;

/* Which AICA register is the driver sitting on? A sound driver that stops
 * making progress is nearly always polling one that never changes. */
static uint32_t g_reg_reads[ARM7_REG_SIZE / 4];
static int      g_count_reads = -1;

/* Sound RAM the driver reads most. Same question as the register counter, for
 * the case where a driver is spinning on a mailbox rather than a register. */
static uint16_t *g_ram_reads;

void arm7_dump_polled_ram(void) {
    if (!g_ram_reads) return;
    uint32_t top[6] = {0};
    uint32_t n = (g_ram_mask + 1) >> 2;
    for (uint32_t i = 0; i < n; i++) {
        for (int k = 0; k < 6; k++) {
            if (g_ram_reads[i] > g_ram_reads[top[k]]) {
                for (int j = 5; j > k; j--) top[j] = top[j - 1];
                top[k] = i;
                break;
            }
        }
    }
    printf("[ARM7] most-read sound RAM:");
    for (int k = 0; k < 6 && g_ram_reads[top[k]]; k++)
        printf("  %06X x%u", top[k] * 4, g_ram_reads[top[k]]);
    printf("\n");
    memset(g_ram_reads, 0, (size_t)n * sizeof *g_ram_reads);
}

void arm7_dump_polled_regs(void) {
    int top[6] = {0};
    for (uint32_t i = 0; i < ARM7_REG_SIZE / 4; i++) {
        for (int k = 0; k < 6; k++) {
            if (g_reg_reads[i] > g_reg_reads[top[k]]) {
                for (int j = 5; j > k; j--) top[j] = top[j - 1];
                top[k] = (int)i;
                break;
            }
        }
    }
    printf("[ARM7] most-read registers:");
    for (int k = 0; k < 6 && g_reg_reads[top[k]]; k++)
        printf("  %04X x%u", top[k] * 4, g_reg_reads[top[k]]);
    printf("\n");
    memset(g_reg_reads, 0, sizeof g_reg_reads);
}

/* ---- memory ------------------------------------------------------------- */

static void note_reg_read(uint32_t off) {
    if (g_count_reads < 0) g_count_reads = getenv("DCRECOMP_ARM7") ? 1 : 0;
    if (g_count_reads) g_reg_reads[(off & (ARM7_REG_SIZE - 1)) / 4]++;
}

static uint32_t arm7_read32(uint32_t addr) {
    if (addr >= ARM7_REG_BASE && addr < ARM7_REG_BASE + ARM7_REG_SIZE) {
        note_reg_read(addr - ARM7_REG_BASE);
        return dc_aica_reg_read(addr - ARM7_REG_BASE);
    }
    if (!g_ram)
        return 0;
    addr &= g_ram_mask & ~3u;
    if (g_count_reads > 0) {
        if (!g_ram_reads) g_ram_reads = calloc((g_ram_mask + 1) >> 2, sizeof *g_ram_reads);
        if (g_ram_reads && g_ram_reads[addr >> 2] < 0xFFFF) g_ram_reads[addr >> 2]++;
    }
    return (uint32_t)g_ram[addr] | ((uint32_t)g_ram[addr + 1] << 8) |
           ((uint32_t)g_ram[addr + 2] << 16) | ((uint32_t)g_ram[addr + 3] << 24);
}

static void arm7_write32(uint32_t addr, uint32_t val) {
    if (addr >= ARM7_REG_BASE && addr < ARM7_REG_BASE + ARM7_REG_SIZE) {
        dc_aica_reg_write(addr - ARM7_REG_BASE, val);
        return;
    }
    if (!g_ram)
        return;
    addr &= g_ram_mask & ~3u;
    g_ram[addr]     = (uint8_t)val;
    g_ram[addr + 1] = (uint8_t)(val >> 8);
    g_ram[addr + 2] = (uint8_t)(val >> 16);
    g_ram[addr + 3] = (uint8_t)(val >> 24);
}

static uint8_t arm7_read8(uint32_t addr) {
    if (addr >= ARM7_REG_BASE && addr < ARM7_REG_BASE + ARM7_REG_SIZE) {
        note_reg_read(addr - ARM7_REG_BASE);
        return (uint8_t)(dc_aica_reg_read((addr - ARM7_REG_BASE) & ~3u) >>
                         ((addr & 3) * 8));
    }
    if (!g_ram)
        return 0;
    return g_ram[addr & g_ram_mask];
}

static void arm7_write8(uint32_t addr, uint8_t val) {
    if (addr >= ARM7_REG_BASE && addr < ARM7_REG_BASE + ARM7_REG_SIZE) {
        uint32_t off = (addr - ARM7_REG_BASE) & ~3u;
        int shift = (addr & 3) * 8;
        uint32_t cur = dc_aica_reg_read(off);
        cur = (cur & ~(0xFFu << shift)) | ((uint32_t)val << shift);
        dc_aica_reg_write(off, cur);
        return;
    }
    if (!g_ram)
        return;
    g_ram[addr & g_ram_mask] = val;
}

static uint16_t arm7_read16(uint32_t addr) {
    return (uint16_t)(arm7_read8(addr) | (arm7_read8(addr + 1) << 8));
}

static void arm7_write16(uint32_t addr, uint16_t val) {
    arm7_write8(addr, (uint8_t)val);
    arm7_write8(addr + 1, (uint8_t)(val >> 8));
}

/* ---- mode banking ------------------------------------------------------- */

/* Save r8-r14 and SPSR of `mode` out of the visible set. */
static void bank_out(ARM7 *cpu, uint32_t mode) {
    switch (mode) {
    case ARM7_MODE_FIQ:
        memcpy(cpu->bank_fiq, &cpu->r[8], 7 * sizeof(uint32_t));
        cpu->spsr_fiq = cpu->spsr;
        break;
    case ARM7_MODE_IRQ:
        cpu->bank_irq[0] = cpu->r[13]; cpu->bank_irq[1] = cpu->r[14];
        cpu->spsr_irq = cpu->spsr;
        goto shared;
    case ARM7_MODE_SVC:
        cpu->bank_svc[0] = cpu->r[13]; cpu->bank_svc[1] = cpu->r[14];
        cpu->spsr_svc = cpu->spsr;
        goto shared;
    case ARM7_MODE_ABT:
        cpu->bank_abt[0] = cpu->r[13]; cpu->bank_abt[1] = cpu->r[14];
        cpu->spsr_abt = cpu->spsr;
        goto shared;
    case ARM7_MODE_UND:
        cpu->bank_und[0] = cpu->r[13]; cpu->bank_und[1] = cpu->r[14];
        cpu->spsr_und = cpu->spsr;
        goto shared;
    default:                    /* user */
        memcpy(cpu->bank_usr, &cpu->r[8], 7 * sizeof(uint32_t));
        break;
    shared:
        /* Every mode but FIQ shares r8-r12 with user. */
        memcpy(cpu->bank_usr, &cpu->r[8], 5 * sizeof(uint32_t));
        break;
    }
}

/* Bring r8-r14 and SPSR of `mode` into the visible set. */
static void bank_in(ARM7 *cpu, uint32_t mode) {
    switch (mode) {
    case ARM7_MODE_FIQ:
        memcpy(&cpu->r[8], cpu->bank_fiq, 7 * sizeof(uint32_t));
        cpu->spsr = cpu->spsr_fiq;
        break;
    case ARM7_MODE_IRQ:
        memcpy(&cpu->r[8], cpu->bank_usr, 5 * sizeof(uint32_t));
        cpu->r[13] = cpu->bank_irq[0]; cpu->r[14] = cpu->bank_irq[1];
        cpu->spsr = cpu->spsr_irq;
        break;
    case ARM7_MODE_SVC:
        memcpy(&cpu->r[8], cpu->bank_usr, 5 * sizeof(uint32_t));
        cpu->r[13] = cpu->bank_svc[0]; cpu->r[14] = cpu->bank_svc[1];
        cpu->spsr = cpu->spsr_svc;
        break;
    case ARM7_MODE_ABT:
        memcpy(&cpu->r[8], cpu->bank_usr, 5 * sizeof(uint32_t));
        cpu->r[13] = cpu->bank_abt[0]; cpu->r[14] = cpu->bank_abt[1];
        cpu->spsr = cpu->spsr_abt;
        break;
    case ARM7_MODE_UND:
        memcpy(&cpu->r[8], cpu->bank_usr, 5 * sizeof(uint32_t));
        cpu->r[13] = cpu->bank_und[0]; cpu->r[14] = cpu->bank_und[1];
        cpu->spsr = cpu->spsr_und;
        break;
    default:
        memcpy(&cpu->r[8], cpu->bank_usr, 7 * sizeof(uint32_t));
        break;
    }
}

static void set_mode(ARM7 *cpu, uint32_t mode) {
    uint32_t old = cpu->cpsr & 0x1F;
    if (old == mode)
        return;
    bank_out(cpu, old);
    cpu->cpsr = (cpu->cpsr & ~0x1Fu) | mode;
    bank_in(cpu, mode);
}

/* ---- flags and shifting -------------------------------------------------- */

static void set_nz(ARM7 *cpu, uint32_t v) {
    cpu->cpsr = (cpu->cpsr & ~(ARM7_N | ARM7_Z)) |
                (v & 0x80000000u ? ARM7_N : 0) | (v == 0 ? ARM7_Z : 0);
}

static void set_c(ARM7 *cpu, int c) {
    cpu->cpsr = c ? (cpu->cpsr | ARM7_C) : (cpu->cpsr & ~ARM7_C);
}

static void set_v(ARM7 *cpu, int v) {
    cpu->cpsr = v ? (cpu->cpsr | ARM7_V) : (cpu->cpsr & ~ARM7_V);
}

static int get_c(const ARM7 *cpu) { return (cpu->cpsr & ARM7_C) != 0; }

/* The shifter's own carry out. `carry` comes in holding CPSR.C and is updated
 * only when the shift actually produces one - the zero-length forms are the
 * whole subtlety here. */
static uint32_t barrel(uint32_t val, int type, uint32_t amount,
                       int *carry, int amount_from_register) {
    if (amount_from_register) {
        if (amount == 0)
            return val;                       /* value and carry both untouched */
        if (amount >= 32) {
            switch (type) {
            case 0: *carry = (amount == 32) ? (val & 1) : 0; return 0;
            case 1: *carry = (amount == 32) ? ((val >> 31) & 1) : 0; return 0;
            case 2: *carry = (val >> 31) & 1;
                    return (val & 0x80000000u) ? 0xFFFFFFFFu : 0;
            case 3: amount &= 31;
                    if (amount == 0) { *carry = (val >> 31) & 1; return val; }
                    break;
            }
        }
    } else if (amount == 0) {
        /* Immediate shift of zero: only LSL means "no shift". */
        switch (type) {
        case 0: return val;
        case 1: *carry = (val >> 31) & 1; return 0;             /* LSR #32 */
        case 2: *carry = (val >> 31) & 1;
                return (val & 0x80000000u) ? 0xFFFFFFFFu : 0;   /* ASR #32 */
        case 3: {                                               /* RRX */
            uint32_t out = ((uint32_t)*carry << 31) | (val >> 1);
            *carry = val & 1;
            return out;
        }
        }
    }

    switch (type) {
    case 0: *carry = (val >> (32 - amount)) & 1; return val << amount;
    case 1: *carry = (val >> (amount - 1)) & 1; return val >> amount;
    case 2: *carry = (uint32_t)((int32_t)val >> (amount - 1)) & 1;
            return (uint32_t)((int32_t)val >> amount);
    default: *carry = (val >> (amount - 1)) & 1;
            return (val >> amount) | (val << (32 - amount));
    }
}

static int cond_holds(const ARM7 *cpu, uint32_t op) {
    uint32_t c = cpu->cpsr;
    int N = (c & ARM7_N) != 0, Z = (c & ARM7_Z) != 0;
    int C = (c & ARM7_C) != 0, V = (c & ARM7_V) != 0;
    switch (op >> 28) {
    case 0x0: return Z;
    case 0x1: return !Z;
    case 0x2: return C;
    case 0x3: return !C;
    case 0x4: return N;
    case 0x5: return !N;
    case 0x6: return V;
    case 0x7: return !V;
    case 0x8: return C && !Z;
    case 0x9: return !C || Z;
    case 0xA: return N == V;
    case 0xB: return N != V;
    case 0xC: return !Z && (N == V);
    case 0xD: return Z || (N != V);
    case 0xE: return 1;
    default:  return 0;         /* NV: never, on ARMv3 */
    }
}

/* ---- exceptions ---------------------------------------------------------- */

static bool g_fiq_line;

static void take_fiq(ARM7 *cpu) {
    uint32_t ret = cpu->r[15] + 4;
    uint32_t saved = cpu->cpsr;
    set_mode(cpu, ARM7_MODE_FIQ);
    cpu->spsr = saved;
    cpu->r[14] = ret;
    cpu->cpsr |= ARM7_I | ARM7_F;
    cpu->r[15] = 0x1C;
}

/* ---- execution ----------------------------------------------------------- */

void arm7_init(ARM7 *cpu, uint8_t *sound_ram, uint32_t sound_ram_size) {
    memset(cpu, 0, sizeof *cpu);
    g_ram = sound_ram;
    g_ram_mask = sound_ram_size ? (sound_ram_size - 1) : 0;
    cpu->cpsr = ARM7_MODE_SVC | ARM7_I | ARM7_F;
    cpu->running = false;
    g_fiq_line = false;
}

void arm7_set_reset(ARM7 *cpu, bool held) {
    if (held) {
        cpu->running = false;
        return;
    }
    if (cpu->running)
        return;
    /* Out of reset the ARM starts at the reset vector in supervisor mode with
     * both interrupt lines masked, and the driver enables them itself. */
    memset(cpu->r, 0, sizeof cpu->r);
    cpu->cpsr = ARM7_MODE_SVC | ARM7_I | ARM7_F;
    cpu->r[15] = 0;
    cpu->running = true;
    cpu->instructions = 0;
}

void arm7_set_fiq(ARM7 *cpu, bool asserted) {
    (void)cpu;
    g_fiq_line = asserted;
}

static void step(ARM7 *cpu) {
    if (g_fiq_line && !(cpu->cpsr & ARM7_F)) {
        take_fiq(cpu);
        return;
    }

    uint32_t addr = cpu->r[15] & ~3u;
    uint32_t op = arm7_read32(addr);
    cpu->r[15] = addr + 8;          /* what this instruction sees */
    cpu->instructions++;

    if (!cond_holds(cpu, op)) {
        cpu->r[15] = addr + 4;
        return;
    }

    /* ---- branch / branch-with-link ---- */
    if ((op & 0x0E000000u) == 0x0A000000u) {
        int32_t off = (int32_t)(op << 8) >> 6;      /* sign-extend 24, times 4 */
        if (op & (1u << 24))
            cpu->r[14] = addr + 4;
        cpu->r[15] = (uint32_t)((int32_t)cpu->r[15] + off);
        return;
    }

    /* ---- software interrupt ---- */
    if ((op & 0x0F000000u) == 0x0F000000u) {
        uint32_t saved = cpu->cpsr;
        set_mode(cpu, ARM7_MODE_SVC);
        cpu->spsr = saved;
        cpu->r[14] = addr + 4;
        cpu->cpsr |= ARM7_I;
        cpu->r[15] = 0x08;
        return;
    }

    /* ---- block data transfer ---- */
    if ((op & 0x0E000000u) == 0x08000000u) {
        int pre = (op >> 24) & 1, up = (op >> 23) & 1;
        int use_user = (op >> 22) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
        int rn = (op >> 16) & 15;
        uint16_t list = (uint16_t)op;
        uint32_t base = cpu->r[rn];
        int count = 0;
        for (int i = 0; i < 16; i++) if (list & (1u << i)) count++;
        if (count == 0) count = 16;             /* empty list transfers r15 */

        uint32_t lowest = up ? base : base - (uint32_t)count * 4;
        uint32_t addr_i = lowest + (pre == up ? 4u : 0u);
        uint32_t writeback = up ? base + (uint32_t)count * 4
                                : base - (uint32_t)count * 4;

        /* The S bit without r15 in the list means "use the user bank". Swap it
         * in around the transfer rather than special-casing every register. */
        uint32_t mode = cpu->cpsr & 0x1F;
        int swapped = use_user && !(list & 0x8000) && mode != ARM7_MODE_USR;
        if (swapped) { bank_out(cpu, mode); bank_in(cpu, ARM7_MODE_USR); }

        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i)))
                continue;
            if (load)
                cpu->r[i] = arm7_read32(addr_i);
            else
                arm7_write32(addr_i, i == 15 ? addr + 12 : cpu->r[i]);
            addr_i += 4;
        }

        if (swapped) { bank_out(cpu, ARM7_MODE_USR); bank_in(cpu, mode); }

        if (wb && !(load && (list & (1u << rn))))
            cpu->r[rn] = writeback;

        if (load && (list & 0x8000)) {
            if (use_user)                       /* LDM ^ with r15: mode return */
                cpu->cpsr = cpu->spsr;
            return;                             /* r15 came from memory */
        }
        cpu->r[15] = addr + 4;
        return;
    }

    /* ---- single data transfer ---- */
    if ((op & 0x0C000000u) == 0x04000000u) {
        int imm_off = !((op >> 25) & 1);        /* note: inverted vs data proc */
        int pre = (op >> 24) & 1, up = (op >> 23) & 1;
        int byte = (op >> 22) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
        int rn = (op >> 16) & 15, rd = (op >> 12) & 15;
        uint32_t offset;

        if (imm_off) {
            offset = op & 0xFFF;
        } else {
            int carry = get_c(cpu);
            offset = barrel(cpu->r[op & 15], (op >> 5) & 3, (op >> 7) & 31,
                            &carry, 0);
        }

        uint32_t base = cpu->r[rn];
        uint32_t ea = pre ? (up ? base + offset : base - offset) : base;

        if (load) {
            uint32_t v;
            if (byte) {
                v = arm7_read8(ea);
            } else {
                /* An unaligned word load rotates rather than faulting. */
                v = arm7_read32(ea & ~3u);
                int rot = (ea & 3) * 8;
                if (rot) v = (v >> rot) | (v << (32 - rot));
            }
            if (!pre) base = up ? base + offset : base - offset;
            if (pre && wb) cpu->r[rn] = ea;
            if (!pre) cpu->r[rn] = base;
            cpu->r[rd] = v;
            if (rd == 15) return;
        } else {
            uint32_t v = (rd == 15) ? addr + 12 : cpu->r[rd];
            if (byte) arm7_write8(ea, (uint8_t)v);
            else      arm7_write32(ea & ~3u, v);
            if (!pre) cpu->r[rn] = up ? base + offset : base - offset;
            else if (wb) cpu->r[rn] = ea;
        }
        cpu->r[15] = addr + 4;
        return;
    }

    /* Everything below is in the 00 space: data processing, and the handful of
     * things that hide in it behind bits 7 and 4. */

    /* ---- swap ---- */
    if ((op & 0x0FB00FF0u) == 0x01000090u) {
        int byte = (op >> 22) & 1;
        int rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
        uint32_t ea = cpu->r[rn];
        if (byte) {
            uint8_t v = arm7_read8(ea);
            arm7_write8(ea, (uint8_t)cpu->r[rm]);
            cpu->r[rd] = v;
        } else {
            uint32_t v = arm7_read32(ea & ~3u);
            arm7_write32(ea & ~3u, cpu->r[rm]);
            cpu->r[rd] = v;
        }
        cpu->r[15] = addr + 4;
        return;
    }

    /* ---- multiply ---- */
    if ((op & 0x0FC000F0u) == 0x00000090u) {
        int acc = (op >> 21) & 1, s = (op >> 20) & 1;
        int rd = (op >> 16) & 15, rn = (op >> 12) & 15;
        int rs = (op >> 8) & 15, rm = op & 15;
        uint32_t v = cpu->r[rm] * cpu->r[rs];
        if (acc) v += cpu->r[rn];
        cpu->r[rd] = v;
        if (s) set_nz(cpu, v);
        cpu->r[15] = addr + 4;
        return;
    }

    /* ---- halfword and signed byte transfer ----
     * Not in ARMv3, but harmless to honour and it removes a whole class of
     * mystery if a driver was built for a later core. */
    if ((op & 0x0E000090u) == 0x00000090u && ((op >> 5) & 3) != 0) {
        int pre = (op >> 24) & 1, up = (op >> 23) & 1;
        int imm = (op >> 22) & 1, wb = (op >> 21) & 1, load = (op >> 20) & 1;
        int rn = (op >> 16) & 15, rd = (op >> 12) & 15;
        int sh = (op >> 5) & 3;
        uint32_t offset = imm ? (((op >> 8) & 15) << 4) | (op & 15)
                              : cpu->r[op & 15];
        uint32_t base = cpu->r[rn];
        uint32_t ea = pre ? (up ? base + offset : base - offset) : base;

        if (load) {
            uint32_t v;
            if (sh == 1)      v = arm7_read16(ea & ~1u);
            else if (sh == 2) v = (uint32_t)(int32_t)(int8_t)arm7_read8(ea);
            else              v = (uint32_t)(int32_t)(int16_t)arm7_read16(ea & ~1u);
            cpu->r[rd] = v;
        } else {
            arm7_write16(ea & ~1u, (uint16_t)cpu->r[rd]);
        }
        if (!pre) cpu->r[rn] = up ? base + offset : base - offset;
        else if (wb) cpu->r[rn] = ea;
        cpu->r[15] = addr + 4;
        return;
    }

    /* ---- MRS / MSR ---- */
    if ((op & 0x0FBF0FFFu) == 0x010F0000u) {            /* MRS Rd, PSR */
        int rd = (op >> 12) & 15;
        cpu->r[rd] = (op & (1u << 22)) ? cpu->spsr : cpu->cpsr;
        cpu->r[15] = addr + 4;
        return;
    }
    if ((op & 0x0DB0F000u) == 0x0120F000u) {            /* MSR PSR{_flg}, op */
        int spsr = (op >> 22) & 1;
        int all  = (op >> 16) & 1;                      /* control bits too */
        uint32_t v;
        if (op & (1u << 25)) {
            uint32_t imm = op & 0xFF, rot = ((op >> 8) & 15) * 2;
            v = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
        } else {
            v = cpu->r[op & 15];
        }
        if (spsr) {
            uint32_t mask = all ? 0xFFFFFFFFu : 0xF0000000u;
            cpu->spsr = (cpu->spsr & ~mask) | (v & mask);
        } else if (all && (cpu->cpsr & 0x1F) != ARM7_MODE_USR) {
            set_mode(cpu, v & 0x1F);
            cpu->cpsr = (cpu->cpsr & 0x1F) | (v & ~0x1Fu);
        } else {
            cpu->cpsr = (cpu->cpsr & ~0xF0000000u) | (v & 0xF0000000u);
        }
        cpu->r[15] = addr + 4;
        return;
    }

    /* ---- data processing ---- */
    {
        int opcode = (op >> 21) & 15, s = (op >> 20) & 1;
        int rn = (op >> 16) & 15, rd = (op >> 12) & 15;
        int carry = get_c(cpu);
        uint32_t a = cpu->r[rn], b;

        if (op & (1u << 25)) {
            uint32_t imm = op & 0xFF, rot = ((op >> 8) & 15) * 2;
            if (rot) {
                b = (imm >> rot) | (imm << (32 - rot));
                carry = (b >> 31) & 1;
            } else {
                b = imm;
            }
        } else {
            int from_reg = (op >> 4) & 1;
            uint32_t amount;
            if (from_reg) {
                amount = cpu->r[(op >> 8) & 15] & 0xFF;
                /* A register-specified shift costs a cycle, and r15 reads one
                 * instruction further on as a result. */
                if (rn == 15) a += 4;
            } else {
                amount = (op >> 7) & 31;
            }
            uint32_t rm = cpu->r[op & 15];
            if (from_reg && (op & 15) == 15) rm += 4;
            b = barrel(rm, (op >> 5) & 3, amount, &carry, from_reg);
        }

        uint32_t res = 0;
        int write = 1, logical = 1;
        uint32_t cin = (uint32_t)get_c(cpu);

        switch (opcode) {
        case 0x0: res = a & b; break;                       /* AND */
        case 0x1: res = a ^ b; break;                       /* EOR */
        case 0x2: res = a - b; logical = 0; break;          /* SUB */
        case 0x3: res = b - a; logical = 0; break;          /* RSB */
        case 0x4: res = a + b; logical = 0; break;          /* ADD */
        case 0x5: res = a + b + cin; logical = 0; break;    /* ADC */
        case 0x6: res = a - b - (1 - cin); logical = 0; break;  /* SBC */
        case 0x7: res = b - a - (1 - cin); logical = 0; break;  /* RSC */
        case 0x8: res = a & b; write = 0; break;            /* TST */
        case 0x9: res = a ^ b; write = 0; break;            /* TEQ */
        case 0xA: res = a - b; write = 0; logical = 0; break;   /* CMP */
        case 0xB: res = a + b; write = 0; logical = 0; break;   /* CMN */
        case 0xC: res = a | b; break;                       /* ORR */
        case 0xD: res = b; break;                           /* MOV */
        case 0xE: res = a & ~b; break;                      /* BIC */
        default:  res = ~b; break;                          /* MVN */
        }

        if (s) {
            if (rd == 15 && write) {
                /* Writing the PC with S set is how an exception returns. */
                cpu->cpsr = cpu->spsr;
                set_mode(cpu, cpu->cpsr & 0x1F);
            } else {
                set_nz(cpu, res);
                if (logical) {
                    set_c(cpu, carry);
                } else {
                    int add = (opcode == 0x4 || opcode == 0x5 || opcode == 0xB);
                    uint32_t x = a, y = b;
                    if (opcode == 0x3 || opcode == 0x7) { x = b; y = a; }
                    if (add) {
                        uint64_t wide = (uint64_t)x + y +
                                        (opcode == 0x5 ? cin : 0);
                        set_c(cpu, (wide >> 32) & 1);
                        set_v(cpu, (~(x ^ y) & (x ^ res)) >> 31);
                    } else {
                        uint64_t wide = (uint64_t)x - y -
                                        ((opcode == 0x6 || opcode == 0x7)
                                         ? (1 - cin) : 0);
                        set_c(cpu, (wide >> 32) == 0);
                        set_v(cpu, ((x ^ y) & (x ^ res)) >> 31);
                    }
                }
            }
        }

        if (write) {
            cpu->r[rd] = res;
            if (rd == 15)
                return;                 /* branched */
        }
        cpu->r[15] = addr + 4;
        return;
    }
}

int arm7_run(ARM7 *cpu, int budget) {
    if (!cpu->running)
        return 0;
    int n = 0;
    while (n < budget) {
        step(cpu);
        n++;
    }
    return n;
}

uint64_t arm7_instruction_count(const ARM7 *cpu) { return cpu->instructions; }
