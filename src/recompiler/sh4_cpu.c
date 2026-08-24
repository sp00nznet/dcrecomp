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
#include "platform/platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Native backtrace of the recompiled call chain.
 *
 * Recompiled functions are ordinary C functions, so the C stack IS the SH-4
 * call chain. cpu->pr and cpu->pc only move on indirect branches, which makes
 * them useless for locating a stall reached through direct calls - this is not.
 * Needs debug info; without a PDB the frames print as bare addresses. */
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
static SH4CPU *g_cpu_ref;

static LONG WINAPI sh4_crash_filter(EXCEPTION_POINTERS *info) {
    printf("[CRASH] exception 0x%08lX at 0x%p\n",
           (unsigned long)info->ExceptionRecord->ExceptionCode,
           info->ExceptionRecord->ExceptionAddress);
    if (info->ExceptionRecord->NumberParameters >= 2)
        printf("[CRASH] accessing 0x%p\n",
               (void *)info->ExceptionRecord->ExceptionInformation[1]);
    if (g_cpu_ref)
        printf("[CRASH] sh4 pc=0x%08X pr=0x%08X sp=0x%08X sr=0x%08X\n",
               g_cpu_ref->pc, g_cpu_ref->pr, g_cpu_ref->r[15], g_cpu_ref->sr);
    fflush(stdout);
    { /* Walk from the faulting context. */
      CONTEXT ctx = *info->ContextRecord;
      STACKFRAME64 f; memset(&f, 0, sizeof f);
      HANDLE proc = GetCurrentProcess(), thr = GetCurrentThread();
      char nbuf[sizeof(SYMBOL_INFO) + 256];
      SYMBOL_INFO *sym = (SYMBOL_INFO *)nbuf;
      SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
      SymInitialize(proc, NULL, TRUE);
      sym->SizeOfStruct = sizeof(SYMBOL_INFO);
      sym->MaxNameLen = 255;
      f.AddrPC.Offset = ctx.Rip;    f.AddrPC.Mode = AddrModeFlat;
      f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
      f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
      printf("[CRASH] rip=0x%llX rsp=0x%llX rbp=0x%llX\n",
             (unsigned long long)ctx.Rip, (unsigned long long)ctx.Rsp,
             (unsigned long long)ctx.Rbp);
      printf("[CRASH] stack:");
      for (int i = 0; i < 32; i++) {
          if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thr, &f, &ctx,
                           NULL, SymFunctionTableAccess64, SymGetModuleBase64,
                           NULL))
              break;
          if (!f.AddrPC.Offset) break;
          DWORD64 disp = 0;
          if (SymFromAddr(proc, f.AddrPC.Offset, &disp, sym))
              printf(" %s", sym->Name);
          else
              printf(" 0x%llX", (unsigned long long)f.AddrPC.Offset);
      }
      printf("\n"); }
    sh4_dump_native_stack("crash");
    { void dcrecomp_dump_calltrace(int);
      dcrecomp_dump_calltrace(24); }
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

void sh4_install_crash_handler(void) {
    SetUnhandledExceptionFilter(sh4_crash_filter);
}

void sh4_dump_native_stack(const char *tag) {
    void *frames[40];
    USHORT n = CaptureStackBackTrace(0, 40, frames, NULL);
    HANDLE proc = GetCurrentProcess();
    static int inited = 0;
    if (!inited) {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        SymInitialize(proc, NULL, TRUE);
        inited = 1;
    }
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    printf("[STACK %s]", tag);
    for (USHORT i = 0; i < n; i++) {
        DWORD64 disp = 0;
        if (SymFromAddr(proc, (DWORD64)(uintptr_t)frames[i], &disp, sym))
            printf(" %s", sym->Name);
        else
            printf(" 0x%p", frames[i]);
    }
    printf("\n");
}
#else
void sh4_dump_native_stack(const char *tag) { (void)tag; }
#endif

/* Write watchpoint. Set DCRECOMP_WATCH to an address (hex, 0x-prefixed) and
 * every 32-bit write to it prints the value and the recompiled call chain.
 * Answers "which code sets this flag", which grep cannot when the address is
 * register-relative in the generated C. */
static SH4CPU *g_cpu_ref;
static uint64_t g_irq_delivered;
static uint64_t g_irq_masked;
static unsigned g_irq_repeats;
static uint32_t g_watch_addr = 0;
static uint32_t g_watch_addr2 = 0;
static int g_watch_init = 0;

static void watch_check(uint32_t addr, uint32_t val) {
    { static int rl = -2; static uint32_t lo, hi;
      if (rl == -2) { const char *e = getenv("DCRECOMP_WATCHRANGE");
          if (e) { lo = (uint32_t)strtoul(e, NULL, 0); hi = lo + 0x20; rl = 1; }
          else rl = 0; }
      if (rl == 1) { uint32_t a = addr & 0x1FFFFFFF;
          if (val && a >= (lo & 0x1FFFFFFF) && a < (hi & 0x1FFFFFFF)) {
              static int n = 0;
              if (++n < 24) {
                  printf("[RANGE] 0x%08X = 0x%08X\n", addr, val);
                  sh4_dump_native_stack("range");
              } } } }
    if (!g_watch_init) {
        const char *e = getenv("DCRECOMP_WATCH");
        g_watch_addr = e ? (uint32_t)strtoul(e, NULL, 0) : 0;
        const char *e2 = getenv("DCRECOMP_WATCH2");
        g_watch_addr2 = e2 ? (uint32_t)strtoul(e2, NULL, 0) : 0;
        g_watch_init = 1;
    }
    int hit = (g_watch_addr  && (addr & 0x1FFFFFFF) == (g_watch_addr  & 0x1FFFFFFF))
           || (g_watch_addr2 && (addr & 0x1FFFFFFF) == (g_watch_addr2 & 0x1FFFFFFF));
    if (hit) {
        static unsigned long n = 0;
        n++;
        if (n <= 60 || (n % 5000) == 0) {
            printf("[WATCH] t=%llu irq=%llu masked=%llu sr=%08X write 0x%08X = 0x%08X\n", (unsigned long long)platform_get_ticks_ms(), (unsigned long long)g_irq_delivered, (unsigned long long)g_irq_masked, g_cpu_ref ? g_cpu_ref->sr : 0, addr, val);
            sh4_dump_native_stack("watch");
        }
    }
}

/* External hardware reference (set during init) */
static DCHardware *g_hardware = NULL;
static SH4CPU *g_cpu_ref = NULL;

/* TMU time base: set when TCNTn is written */
static uint64_t tmu_write_time_ms[3] = {0, 0, 0};

/* ========== Interrupt delivery ==========
 *
 * See sh4_cpu.h. The handler is called from sh4_read32, i.e. between the
 * memory accesses of recompiled code rather than between SH-4 instructions.
 * That is close enough to hardware, with one catch: recompiled code keeps all
 * live state in cpu->r[] instead of C locals, so a handler that runs mid
 * sequence would clobber whatever function it interrupted. Real hardware
 * avoids this by banking r0-r7 on interrupt entry. We save and restore the
 * whole architectural register file, which covers the banked half plus r8-r15
 * and the FP state.
 *
 * Deliberately NOT saved: dmac_regs, tmu_regs, sq/qacr, cycles and running.
 * Those are hardware side effects the handler is supposed to leave behind.
 */
typedef struct {
    uint32_t r[16];
    uint32_t pr, sr, gbr, mach, macl, fpul, fpscr;
    float fr[16], xf[16];
} SH4IrqFrame;

static void (*g_irq_handler)(SH4CPU *cpu) = NULL;
static uint64_t g_last_vblank_ms = 0;
static bool g_in_irq = false;
static uint64_t g_irq_delivered = 0;
static uint64_t g_irq_reentrant = 0;   /* skipped because a handler was running */
static uint64_t g_irq_masked = 0;      /* skipped because SR.BL or SR.IMASK blocked it */
static unsigned g_irq_repeats = 0;     /* deliveries since the pending set last emptied */


void sh4_set_irq_handler(void (*handler)(SH4CPU *cpu)) {
    g_irq_handler = handler;
    printf("[IRQ] handler %s\n", handler ? "registered" : "cleared");
}

/* Which IRL level Holly is routing VBlank-IN to, per SB_IML6/4/2NRM.
 * Zero means the game has not routed it anywhere, so nothing should fire. */
static int vblank_irl_level(void) {
    const uint32_t VBL_IN = 1u << 3;
    uint32_t pend = dc_hw_get_istnrm(g_hardware);
    if (!pend)
        return 0;

    /* Whatever the game has routed, at the level it routed it to. VBlank is
     * not special - a DMA completion or an end-of-list the game is waiting on
     * has to reach it too, and waiting for the next VBlank to deliver one is
     * how a transfer that finished instantly still takes a frame. */
    if (pend & dc_hw_read32(g_hardware, SB_IML6NRM)) return 13;
    if (pend & dc_hw_read32(g_hardware, SB_IML4NRM)) return 11;
    if (pend & dc_hw_read32(g_hardware, SB_IML2NRM)) return 9;

    /* External sources - GD-ROM, AICA, modem - have their own status and mask
     * registers and are just as able to be what a game is waiting on. */
    { uint32_t ext = dc_hw_get_istext(g_hardware);
      if (ext) {
          if (ext & dc_hw_read32(g_hardware, SB_IML6EXT)) return 13;
          if (ext & dc_hw_read32(g_hardware, SB_IML4EXT)) return 11;
          if (ext & dc_hw_read32(g_hardware, SB_IML2EXT)) return 9;
      } }
    if (!(pend & VBL_IN))
        return 0;
    /* Holly's three interrupt levels are wired to SH-4 IRL 13, 11 and 9 - not
     * to 6, 4 and 2, which are just what the registers are named after. Getting
     * this wrong matters: a game that masks to IMASK=8 around video setup still
     * expects VBlank, because IRL 13 is above the mask. Treating it as level 6
     * blocked it, and the vblank wait in the middle of the mode change never
     * finished. */
    if (dc_hw_read32(g_hardware, SB_IML6NRM) & VBL_IN) return 13;
    if (dc_hw_read32(g_hardware, SB_IML4NRM) & VBL_IN) return 11;
    if (dc_hw_read32(g_hardware, SB_IML2NRM) & VBL_IN) return 9;

    /* Nothing routed. On hardware the BIOS programs these before handing
     * control to the game, and we bypass the BIOS - Crazy Taxi happens to set
     * them itself, Mushiking never touches them. Treating unrouted as "never
     * deliver" would mean such a game gets no interrupts at all, so assume the
     * mid IRL. The SR.BL and SR.IMASK checks below still do the real work of
     * keeping us out of critical sections. */
    return 11;
}

/* Frames the game is owed for time a device spent that we did not, and the
 * remainder that has not yet added up to one. */
static unsigned g_owed_vblanks = 0;
static uint32_t g_owed_ms = 0;

void sh4_credit_elapsed_ms(uint32_t ms) {
    g_owed_ms += ms;
    g_owed_vblanks += g_owed_ms / 16;
    g_owed_ms %= 16;
    /* A second's worth is more than any single wait needs, and capping keeps a
     * bad length from parking us in the poll loop. */
    if (g_owed_vblanks > 60)
        g_owed_vblanks = 60;
}

void sh4_poll_irq(SH4CPU *cpu) {
    if (!g_hardware) return;
    /* ponytail: no nested interrupts. Hardware allows a handler that lowers
     * SR.IMASK to be interrupted again; we just drop the second one, which
     * costs a frame at worst. If a handler ever needs to wait on a later
     * interrupt, this guard is what deadlocks and this is where to fix it. */
    if (g_in_irq) { g_irq_reentrant++; return; }

    /* Raising is on the clock; delivering is not. With nothing pending the
     * only question is whether 16ms have passed, and asking costs a clock read
     * - so ask at the old rate. With something pending the question is whether
     * the game has unmasked, which needs no clock and wants asking every time:
     * a game masks and unmasks around every list it shares with its handler,
     * so it is unmasked for only a small part of the time and a coarse check
     * lands inside a critical section almost every time. */
    /* Raising VBlank is on the clock, and the clock is the expensive part, so
     * ask at the old rate. It has to be asked unconditionally: we raise bits a
     * game may never route or clear - a DMA completion it does not use, say -
     * and gating the clock on "nothing pending" would let one of those stop
     * VBlank for good. */
    dc_g2_retire_finished(g_hardware);

    { static unsigned tick = 0;
      if ((++tick & 0x1F) == 0) {
          uint64_t now = platform_get_ticks_ms();
          if (g_last_vblank_ms == 0) g_last_vblank_ms = now;
          if (now - g_last_vblank_ms >= 16) {  /* ~60Hz */
              g_last_vblank_ms = now;
              dc_pvr_wait_vblank(g_hardware);  /* raises SB_ISTNRM VBlank-IN */
          } else if (g_owed_vblanks) {
              g_owed_vblanks--;
              dc_pvr_wait_vblank(g_hardware);
          }
      } }
    if (!g_irq_handler) return;

    /* Respect the CPU's own masking. Delivering into a critical section the
     * game had closed leaves it re-entered halfway through its own bookkeeping,
     * which is exactly as broken as it sounds. SR.BL blocks everything; SR.IMASK
     * blocks any level at or below it. The interrupt stays pending and gets
     * delivered on a later poll. */
    /* Delivering is free - no clock - so it happens on every check. What we
     * are usually waiting for is not time passing but the game leaving a
     * critical section, and it enters and leaves those constantly. */
    int level = vblank_irl_level();
    if (level == 0) { g_irq_masked++; g_irq_repeats = 0; return; }
    if (cpu->sr & SR_BL) { g_irq_masked++; return; }
    if ((int)((cpu->sr & SR_IMASK) >> 4) >= level) {
        g_irq_masked++;
        if (!getenv("DCRECOMP_IGNORE_IMASK")) return;
    }

    /* A bit nothing ever clears would otherwise be delivered on every check
     * forever. Count deliveries that leave something still pending and back
     * off once there have been a few; the counter is cleared the moment the
     * pending set empties, so a handler that is servicing what it is given
     * never runs into this. */
    if (g_irq_repeats > 8 && (g_irq_repeats & 0x3F))
        { g_irq_repeats++; return; }
    g_irq_repeats++;

    SH4IrqFrame f;
    memcpy(f.r, cpu->r, sizeof f.r);
    f.pr = cpu->pr; f.sr = cpu->sr; f.gbr = cpu->gbr;
    f.mach = cpu->mach; f.macl = cpu->macl;
    f.fpul = cpu->fpul; f.fpscr = cpu->fpscr;
    memcpy(f.fr, cpu->fr, sizeof f.fr);
    memcpy(f.xf, cpu->xf, sizeof f.xf);

    /* Where is the game actually spending its time? Set DCRECOMP_STACKEVERY=N
     * and every Nth interrupt prints the recompiled call chain it interrupted.
     * A game that has stopped making progress shows the same chain every time. */
    { static int every = -1, n = 0;
      if (every < 0) { const char *e = getenv("DCRECOMP_STACKEVERY");
                       every = e ? atoi(e) : 0; }
      if (every > 0 && (++n % every) == 0) sh4_dump_native_stack("irq"); }

    g_in_irq = true;
    g_irq_delivered++;
    g_irq_handler(cpu);
    g_in_irq = false;

    memcpy(cpu->r, f.r, sizeof f.r);
    cpu->pr = f.pr; cpu->sr = f.sr; cpu->gbr = f.gbr;
    cpu->mach = f.mach; cpu->macl = f.macl;
    cpu->fpul = f.fpul; cpu->fpscr = f.fpscr;
    memcpy(cpu->fr, f.fr, sizeof f.fr);
    memcpy(cpu->xf, f.xf, sizeof f.xf);
}

DCHardware *sh4_get_hardware(void) {
    return g_hardware;
}

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

uint8_t *sh4_get_aica_ram_ptr(void) {
    return g_cpu_ref ? g_cpu_ref->aica_ram : NULL;
}

/* Words the sound driver would have published. Overriding the read rather than
 * writing sound RAM keeps the value out of reach of the DMA that uploads the
 * driver, which lands on top of it. */
#define AICA_PUBLISHED_MAX 8
static struct { uint32_t offset, value, after_ms; } g_aica_published[AICA_PUBLISHED_MAX];
static int g_aica_published_n = 0;

/* When the sound processor was let go, and how long the driver takes to have
 * something to say. A hundred milliseconds is six frames: long enough for the
 * game to go round its wait loop with interrupts open, which is what it is
 * really there for, and short enough that nobody notices. */
static uint64_t g_aica_arm_released_ms = 0;

#define STUBBED_MAX 16
static struct { uint32_t addr, value; } g_stubbed[STUBBED_MAX];
static int g_stubbed_n = 0;

void sh4_stub_function(uint32_t addr, uint32_t value) {
    if (g_stubbed_n >= STUBBED_MAX)
        return;
    g_stubbed[g_stubbed_n].addr = addr & 0x1FFFFFFF;
    g_stubbed[g_stubbed_n].value = value;
    g_stubbed_n++;
    printf("[STUB] func_%08X answered with %u instead of run\n", addr, value);
}

bool sh4_stubbed_function(uint32_t addr, uint32_t *value) {
    addr &= 0x1FFFFFFF;
    for (int i = 0; i < g_stubbed_n; i++)
        if (g_stubbed[i].addr == addr) {
            *value = g_stubbed[i].value;
            return true;
        }
    return false;
}

/* The pair a register number names: even is this bank, odd is the other. */
static float *fmov_pair(SH4CPU *cpu, int n) {
    return (n & 1) ? &cpu->xf[n & 0xE] : &cpu->fr[n & 0xE];
}

void sh4_fmov_reg(SH4CPU *cpu, int n, int m) {
    if (cpu->fpscr & FPSCR_SZ) {
        float *d = fmov_pair(cpu, n), *s = fmov_pair(cpu, m);
        d[0] = s[0];
        d[1] = s[1];
    } else {
        cpu->fr[n] = cpu->fr[m];
    }
}

void sh4_fmov_load(SH4CPU *cpu, int n, uint32_t addr) {
    if (cpu->fpscr & FPSCR_SZ) {
        float *d = fmov_pair(cpu, n);
        d[0] = sh4_read_float(cpu, addr);
        d[1] = sh4_read_float(cpu, addr + 4);
    } else {
        cpu->fr[n] = sh4_read_float(cpu, addr);
    }
}

void sh4_fmov_store(SH4CPU *cpu, int m, uint32_t addr) {
    if (cpu->fpscr & FPSCR_SZ) {
        float *s = fmov_pair(cpu, m);
        sh4_write_float(cpu, addr, s[0]);
        sh4_write_float(cpu, addr + 4, s[1]);
    } else {
        sh4_write_float(cpu, addr, cpu->fr[m]);
    }
}

void sh4_fmov_load_inc(SH4CPU *cpu, int n, int m) {
    uint32_t addr = cpu->r[m];
    sh4_fmov_load(cpu, n, addr);
    cpu->r[m] += (cpu->fpscr & FPSCR_SZ) ? 8 : 4;
}

void sh4_fmov_store_dec(SH4CPU *cpu, int m, int n) {
    cpu->r[n] -= (cpu->fpscr & FPSCR_SZ) ? 8 : 4;
    sh4_fmov_store(cpu, m, cpu->r[n]);
}

/* DIV1 Rm,Rn, straight out of the SH-4 manual. Q and M live in SR; each step
 * shifts the dividend left through T, then adds or subtracts the divisor
 * depending on the previous Q and on M, and recomputes Q from whether that
 * carried. T ends up as (Q == M), which is what the next step shifts in. */
void sh4_div1(SH4CPU *cpu, int n, int m) {
    uint32_t divisor = cpu->r[m];
    unsigned old_q = (cpu->sr & SR_Q) ? 1u : 0u;
    unsigned mbit  = (cpu->sr & SR_M) ? 1u : 0u;
    unsigned q     = (cpu->r[n] & 0x80000000u) ? 1u : 0u;
    unsigned carry;
    uint32_t before;

    cpu->r[n] = (cpu->r[n] << 1) | ((cpu->sr & SR_T) ? 1u : 0u);

    before = cpu->r[n];
    if (old_q == mbit) {
        cpu->r[n] -= divisor;
        carry = (cpu->r[n] > before) ? 1u : 0u;
    } else {
        cpu->r[n] += divisor;
        carry = (cpu->r[n] < before) ? 1u : 0u;
    }

    /* The manual writes the new Q as four cases on (old_q, M, Q). Written out,
     * the dependence on old_q cancels and what is left is Q ^ carry ^ M. */
    q = q ^ carry ^ mbit;

    if (q) cpu->sr |= SR_Q; else cpu->sr &= ~SR_Q;
    if (q == mbit) cpu->sr |= SR_T; else cpu->sr &= ~SR_T;
}

void sh4_frchg(SH4CPU *cpu) {
    for (int i = 0; i < 16; i++) {
        float t = cpu->fr[i];
        cpu->fr[i] = cpu->xf[i];
        cpu->xf[i] = t;
    }
    cpu->fpscr ^= FPSCR_FR;
}

void sh4_aica_arm_released(void) {
    g_aica_arm_released_ms = platform_get_ticks_ms();
    if (!g_aica_arm_released_ms)
        g_aica_arm_released_ms = 1;
}

static int aica_driver_up_for(uint32_t ms) {
    return g_aica_arm_released_ms &&
           platform_get_ticks_ms() - g_aica_arm_released_ms >= ms;
}

void sh4_aica_publish(uint32_t offset, uint32_t value, uint32_t after_ms) {
    if (g_aica_published_n >= AICA_PUBLISHED_MAX)
        return;
    g_aica_published[g_aica_published_n].offset = offset;
    g_aica_published[g_aica_published_n].value = value;
    g_aica_published[g_aica_published_n].after_ms = after_ms;
    g_aica_published_n++;
    printf("[AICA] sound RAM +0x%X reads as 0x%08X, %ums after the processor "
           "starts (driver stub)\n", offset, value, after_ms);
}

uint32_t sh4_get_ram_size(void) {
    return g_cpu_ref ? g_cpu_ref->ram_size : 0;
}

uint32_t sh4_get_ram_mask(void) {
    return g_cpu_ref ? g_cpu_ref->ram_mask : 0;
}

uint32_t sh4_get_dmac_reg(int idx) {
    if (!g_cpu_ref || idx < 0 || idx >= 17) return 0;
    return g_cpu_ref->dmac_regs[idx];
}

void sh4_init_ex(SH4CPU *cpu, uint32_t ram_size) {
    sh4_install_crash_handler();
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

static uint32_t g_read_seq = 0;

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
            /* TMU registers:
             * 0xFFD80000: TOCR, 0xFFD80004: TSTR
             * 0xFFD80008: TCOR0, 0xFFD8000C: TCNT0, 0xFFD80010: TCR0
             * 0xFFD80014: TCOR1, 0xFFD80018: TCNT1, 0xFFD8001C: TCR1
             * 0xFFD80020: TCOR2, 0xFFD80024: TCNT2, 0xFFD80028: TCR2 */
            uint32_t idx = (addr - 0xFFD80000) / 4;
            if (idx < 12) {
                /* For TCNT0/1/2 (indices 3, 6, 9): simulate countdown.
                 * Naomi Pclk = 50MHz. With prescaler /4 (TCR default),
                 * timer ticks at 12.5MHz = 12500 ticks/ms. */
                if (idx == 3 || idx == 6 || idx == 9) {
                    int ch = (idx == 3) ? 0 : (idx == 6) ? 1 : 2;
                    uint64_t now = platform_get_ticks_ms();
                    uint64_t base = tmu_write_time_ms[ch];
                    if (base == 0) base = now; /* first read before any write */
                    uint64_t elapsed_ms = now - base;
                    /* Decrement by elapsed ticks (12500 per ms for ~50MHz/4) */
                    uint64_t ticks64 = elapsed_ms * 12500;
                    uint32_t val = cpu->tmu_regs[idx];
                    if (ticks64 >= (uint64_t)val) {
                        /* Timer underflowed - reload and reset time base */
                        uint32_t reload = cpu->tmu_regs[idx - 1]; /* TCORn */
                        if (reload == 0) reload = 0xFFFFFFFF;
                        cpu->tmu_regs[idx] = reload;
                        tmu_write_time_ms[ch] = now;
                    }
                    /* Return the stored value minus elapsed (computed on the fly) */
                    uint32_t result = cpu->tmu_regs[idx];
                    if (ticks64 < (uint64_t)result) {
                        result -= (uint32_t)ticks64;
                    }
                    return result;
                }
                return cpu->tmu_regs[idx];
            }
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

    g_read_seq++;
    /* Check often. sh4_poll_irq consults the clock only when it has to, so
     * the cost of a check with nothing pending is a load and a branch - and
     * the thing we are usually waiting for is not the clock but the game
     * leaving a critical section, which it does and undoes constantly. */
    if ((g_read_seq & 0xFF) == 0) sh4_poll_irq(cpu);
    if ((g_read_seq & 0x3FFFFF) == 0) {
        printf("[HEARTBEAT] read32 #%uM addr=0x%08X pc=0x%08X pr=0x%08X sr=0x%08X irq=%llu reent=%llu masked=%llu\n",
               g_read_seq >> 20, addr, cpu->pc, cpu->pr, cpu->sr,
               (unsigned long long)g_irq_delivered,
               (unsigned long long)g_irq_reentrant,
               (unsigned long long)g_irq_masked);
        /* Set DCRECOMP_STACK_TRACE=1 to get the recompiled call chain with
         * each heartbeat. Symbol resolution is slow enough to distort timing,
         * so it stays off unless asked for. */
        static int trace = -1;
        if (trace < 0) trace = getenv("DCRECOMP_STACK_TRACE") ? 1 : 0;
        if (trace) sh4_dump_native_stack("heartbeat");
    }

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
        uint32_t off = phys - DC_AICA_BASE;
        uint32_t v = *(uint32_t *)(cpu->aica_ram + off);
        /* Only answer while nothing has written the word - if the game puts
         * its own value there, that is the real state and it wins - and only
         * once the driver would have been up to write it. */
        /* Answer once the driver would have got round to this one. The
         * delays are what make it work: a driver clears the requests already
         * queued for it as it starts and only then reports itself ready, so
         * the game's wait for "ready" is time its own handler gets to notice
         * those requests finishing. Announce everything at once and the game
         * walks out of that wait with the work still outstanding. */
        for (int i = 0; i < g_aica_published_n; i++)
            if (g_aica_published[i].offset == off &&
                aica_driver_up_for(g_aica_published[i].after_ms))
                return g_aica_published[i].value;
        /* A word in sound RAM read over and over is the CPU waiting on the
         * sound driver, which runs on a processor we do not have. Naming the
         * address and the value it is stuck on is the first thing anyone
         * needs. Set DCRECOMP_AICAPOLL to see them. */
        { static int dl = -1; if (dl < 0) dl = getenv("DCRECOMP_AICAREADS") ? 1 : 0;
          if (dl && g_in_irq) { static uint32_t seen[64]; static int ns = 0; int k;
            for (k = 0; k < ns; k++) if (seen[k] == off) break;
            if (k == ns && ns < 64) { seen[ns++] = off;
                printf("[AICARD] handler reads +0x%X = 0x%08X\n", off, v); } } }
        static int aicalog = -1;
        if (aicalog < 0) aicalog = getenv("DCRECOMP_AICAPOLL") ? 1 : 0;
        if (aicalog) {
            static uint32_t last = 0; static unsigned n = 0; static int said = 0;
            if (phys == last) {
                if (++n == 20000 && said < 12) {
                    said++;
                    printf("[AICA] 0x%08X read %u times, stuck on 0x%08X\n", phys, n, v);
                }
            } else { last = phys; n = 1; }
        }
        return v;
    }
    if (phys >= 0x00700000 && phys < 0x00710000) {
        uint32_t v = dc_aica_reg_read(phys - 0x00700000);
        { static int dl = -1; if (dl < 0) dl = getenv("DCRECOMP_AICAREADS") ? 1 : 0;
          if (dl) { static uint32_t seen[64]; static int ns = 0; int k;
            for (k = 0; k < ns; k++) if (seen[k] == phys) break;
            if (k == ns && ns < 64) { seen[ns++] = phys;
                printf("[AICAREG] read 0x%08X = 0x%08X%s\n", phys, v,
                       g_in_irq ? "  (in handler)" : ""); } } }
        return v;
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
    watch_check(addr, (uint32_t)val);
    uint32_t phys = translate_addr(addr);

    if (phys >= DC_RAM_BASE && phys < DC_RAM_BASE + cpu->ram_size) {
        /* Protect allocator vtable data from byte writes */
        if (phys >= 0x0C1733A4 && phys <= 0x0C1733B3) return;
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
    watch_check(addr, (uint32_t)val);
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
    watch_check(addr, val);
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
                if (idx < 12) {
                    cpu->tmu_regs[idx] = val;
                    /* Reset time base when TCNTn is written */
                    if (idx == 3 || idx == 6 || idx == 9) {
                        int ch = (idx == 3) ? 0 : (idx == 6) ? 1 : 2;
                        tmu_write_time_ms[ch] = platform_get_ticks_ms();
                    }
                }
                return;
            }
            return;
        }
        return;
    }

    /* Normal address translation */
    uint32_t phys = translate_addr(addr);

    if (phys >= DC_RAM_BASE && phys < DC_RAM_BASE + cpu->ram_size) {
        /* Protect allocator vtable data (4 function pointers at 0x0C1733A4-0x0C1733B0)
         * These get corrupted by memset/memfill operations during init.
         * The ROM values are the correct function pointers. */
        if (phys >= 0x0C1733A4 && phys <= 0x0C1733B0) {
            return; /* Block all writes to vtable data region */
        }
        /* Protect allocator vtable pointer */
        if (phys == 0x0C2FB7A4) {
            uint32_t cur = *(uint32_t *)(cpu->ram + (phys & cpu->ram_mask));
            if (cur == 0x8C1733A4 && val != 0x8C1733A4 && val != 0 && val != 0xFFFFFFFF) {
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
    if (phys >= 0x00700000 && phys < 0x00710000) {
        dc_aica_reg_write(phys - 0x00700000, val);
        return;
    }
    if (is_hw_register(phys) && g_hardware) {
        /* Log the first write to each distinct register. "Which hardware has
         * the game actually programmed" is otherwise invisible - and a game
         * that shows nothing has usually not programmed the part you assume. */
        static int hwlog = -1;
        if (hwlog < 0) hwlog = getenv("DCRECOMP_HWREG") ? 1 : 0;
        if (hwlog) { static uint32_t seen[512]; static int nseen = 0; int i;
          for (i = 0; i < nseen; i++) if (seen[i] == phys) break;
          if (i == nseen && nseen < 512) {
              seen[nseen++] = phys;
              printf("[HWREG] first write 0x%08X = 0x%08X\n", phys, val);
          } }
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

    /* A store queue burst goes wherever QACR says, and QACR is a register the
     * game sets. Zero sends the whole burst to the bottom of area 0, which is
     * nothing - and since the store queue is how a game feeds the tile
     * accelerator, a game whose QACR is wrong renders nothing and looks like a
     * dozen other faults. Say so once. */
    { static unsigned long n = 0, lost = 0; static int said = 0;
      uint32_t q = cpu->qacr[(addr >> 5) & 1];
      uint32_t d = (((q >> 2) & 7) << 26) | (addr & 0x03FFFFE0);
      n++;
      if (d < 0x00100000) lost++;
      if (!said && n >= 2000 && lost * 2 > n) {
          said = 1;
          printf("[SQ] %lu of %lu store queue bursts are going nowhere: QACR "
                 "is 0x%X/0x%X, so the destination computes to 0x%08X. "
                 "Geometry written to the store queue is being discarded.\n",
                 lost, n, cpu->qacr[0], cpu->qacr[1], d);
      } }

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
