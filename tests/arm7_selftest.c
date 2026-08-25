/* Self-check for the ARM7DI core. Covers the parts that are easy to get wrong
 * and silent when they are: the barrel shifter's zero-length forms, carry and
 * overflow on the arithmetic, register banking across a mode change, and FIQ
 * entry and return.
 *
 * Includes the implementation rather than linking it, so it can drive step()
 * one instruction at a time instead of assembling whole programs.
 *
 *   cl /nologo /I include tests\arm7_selftest.c && arm7_selftest.exe
 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>

/* arm7.c reaches for these; the core is what is under test, not the AICA. */
static uint32_t g_fake_regs[0x8000 / 4];
uint32_t dc_aica_reg_read(uint32_t off) { return g_fake_regs[(off & 0x7FFF) / 4]; }
void dc_aica_reg_write(uint32_t off, uint32_t val) { g_fake_regs[(off & 0x7FFF) / 4] = val; }

#include "../src/hal/arm7.c"

static uint8_t ram[0x10000];
static ARM7 cpu;

static void boot(void) {
    arm7_init(&cpu, ram, sizeof ram);
    arm7_set_reset(&cpu, false);
    cpu.cpsr = ARM7_MODE_SVC;           /* interrupts unmasked for the tests */
}

/* Execute one instruction placed at 0x100. */
static void run1(uint32_t op) {
    cpu.r[15] = 0x100;
    arm7_write32(0x100, op);
    step(&cpu);
}

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

int main(void) {
    int fails = 0;

    /* ---- the PC reads eight ahead ---- */
    boot();
    run1(0xE1A0000F);                       /* mov r0, r15 */
    CHECK(cpu.r[0] == 0x108);

    /* ---- LSR #0 means LSR #32: result zero, carry from bit 31 ---- */
    boot();
    cpu.r[0] = 0x80000000u;
    run1(0xE1B00020);                       /* movs r0, r0, lsr #32 */
    CHECK(cpu.r[0] == 0);
    CHECK(cpu.cpsr & ARM7_C);
    CHECK(cpu.cpsr & ARM7_Z);

    /* ---- ASR #0 means ASR #32: all sign bits ---- */
    boot();
    cpu.r[0] = 0x80000000u;
    run1(0xE1B00040);                       /* movs r0, r0, asr #32 */
    CHECK(cpu.r[0] == 0xFFFFFFFFu);
    CHECK(cpu.cpsr & ARM7_C);

    /* ---- ROR #0 is RRX: carry in at the top, bit 0 out ---- */
    boot();
    cpu.r[0] = 0x00000003u;
    cpu.cpsr |= ARM7_C;
    run1(0xE1B00060);                       /* movs r0, r0, rrx */
    CHECK(cpu.r[0] == 0x80000001u);
    CHECK(cpu.cpsr & ARM7_C);               /* bit 0 was set */

    /* ---- a register-specified shift of zero leaves carry alone ---- */
    boot();
    cpu.r[0] = 0xFFFFFFFFu;
    cpu.r[1] = 0;
    cpu.cpsr &= ~ARM7_C;
    run1(0xE1B00110);                       /* movs r0, r0, lsl r1 */
    CHECK(cpu.r[0] == 0xFFFFFFFFu);
    CHECK(!(cpu.cpsr & ARM7_C));

    /* ---- subtraction: carry set means no borrow ---- */
    boot();
    cpu.r[0] = 5; cpu.r[1] = 3;
    run1(0xE0500001);                       /* subs r0, r0, r1 */
    CHECK(cpu.r[0] == 2);
    CHECK(cpu.cpsr & ARM7_C);
    CHECK(!(cpu.cpsr & ARM7_V));

    boot();
    cpu.r[0] = 3; cpu.r[1] = 5;
    run1(0xE0500001);                       /* subs r0, r0, r1 -> borrow */
    CHECK(cpu.r[0] == (uint32_t)-2);
    CHECK(!(cpu.cpsr & ARM7_C));
    CHECK(cpu.cpsr & ARM7_N);

    /* ---- signed overflow ---- */
    boot();
    cpu.r[0] = 0x7FFFFFFFu; cpu.r[1] = 1;
    run1(0xE0900001);                       /* adds r0, r0, r1 */
    CHECK(cpu.r[0] == 0x80000000u);
    CHECK(cpu.cpsr & ARM7_V);
    CHECK(cpu.cpsr & ARM7_N);

    /* ---- a failed condition still advances the PC ---- */
    boot();
    cpu.cpsr &= ~ARM7_Z;
    run1(0x03A00001);                       /* moveq r0, #1 */
    CHECK(cpu.r[0] == 0);
    CHECK(cpu.r[15] == 0x104);

    /* ---- branch with link ---- */
    boot();
    run1(0xEB00000E);                       /* bl +0x38 */
    CHECK(cpu.r[14] == 0x104);
    CHECK(cpu.r[15] == 0x140);

    /* ---- store/load round trip, post-indexed with writeback ---- */
    boot();
    cpu.r[0] = 0xDEADBEEFu; cpu.r[1] = 0x200;
    run1(0xE4810004);                       /* str r0, [r1], #4 */
    CHECK(cpu.r[1] == 0x204);
    CHECK(arm7_read32(0x200) == 0xDEADBEEFu);
    cpu.r[2] = 0;
    cpu.r[1] = 0x200;
    run1(0xE5912000);                       /* ldr r2, [r1] */
    CHECK(cpu.r[2] == 0xDEADBEEFu);

    /* ---- byte access does not disturb its neighbours ---- */
    boot();
    arm7_write32(0x300, 0x11223344u);
    cpu.r[0] = 0xFF; cpu.r[1] = 0x301;
    run1(0xE5C10000);                       /* strb r0, [r1] */
    CHECK(arm7_read32(0x300) == 0x1122FF44u);

    /* ---- STM then LDM, full descending, with writeback ---- */
    boot();
    cpu.r[0] = 0xAAAA; cpu.r[1] = 0xBBBB; cpu.r[13] = 0x400;
    run1(0xE92D0003);                       /* stmfd sp!, {r0, r1} */
    CHECK(cpu.r[13] == 0x3F8);
    CHECK(arm7_read32(0x3F8) == 0xAAAA);    /* lowest register, lowest address */
    CHECK(arm7_read32(0x3FC) == 0xBBBB);
    cpu.r[0] = cpu.r[1] = 0;
    run1(0xE8BD0003);                       /* ldmfd sp!, {r0, r1} */
    CHECK(cpu.r[0] == 0xAAAA);
    CHECK(cpu.r[1] == 0xBBBB);
    CHECK(cpu.r[13] == 0x400);

    /* ---- FIQ banks r8-r14, and the old set survives ---- */
    boot();
    cpu.r[8] = 0x1111; cpu.r[13] = 0x2222;
    cpu.r[0] = ARM7_MODE_FIQ;
    run1(0xE129F000);                       /* msr cpsr, r0 */
    CHECK((cpu.cpsr & 0x1F) == ARM7_MODE_FIQ);
    CHECK(cpu.r[8] != 0x1111);              /* banked out */
    cpu.r[8] = 0x3333;
    cpu.r[0] = ARM7_MODE_SVC;
    run1(0xE129F000);
    CHECK(cpu.r[8] == 0x1111);              /* and back */
    CHECK(cpu.r[13] == 0x2222);

    /* ---- MRS reads what MSR wrote ---- */
    boot();
    run1(0xE10F0000);                       /* mrs r0, cpsr */
    CHECK((cpu.r[0] & 0x1F) == ARM7_MODE_SVC);

    /* ---- FIQ entry, and the standard return ---- */
    boot();
    cpu.cpsr = ARM7_MODE_SVC;               /* F clear: FIQ enabled */
    cpu.r[15] = 0x100;
    arm7_set_fiq(&cpu, true);
    step(&cpu);
    CHECK(cpu.r[15] == 0x1C);
    CHECK((cpu.cpsr & 0x1F) == ARM7_MODE_FIQ);
    CHECK(cpu.cpsr & ARM7_F);               /* masked on entry */
    CHECK(cpu.r[14] == 0x104);              /* return address + 4 */
    CHECK((cpu.spsr & 0x1F) == ARM7_MODE_SVC);

    arm7_set_fiq(&cpu, false);
    cpu.r[15] = 0x1C;
    arm7_write32(0x1C, 0xE25EF004);         /* subs pc, lr, #4 */
    step(&cpu);
    CHECK(cpu.r[15] == 0x100);              /* resumes where it was */
    CHECK((cpu.cpsr & 0x1F) == ARM7_MODE_SVC);
    CHECK(!(cpu.cpsr & ARM7_F));            /* and unmasks again */

    /* ---- a masked FIQ is not taken ---- */
    boot();
    cpu.cpsr = ARM7_MODE_SVC | ARM7_F;
    arm7_set_fiq(&cpu, true);
    run1(0xE3A00001);                       /* mov r0, #1 */
    CHECK(cpu.r[0] == 1);
    CHECK(cpu.r[15] == 0x104);

    /* ---- multiply ---- */
    boot();
    cpu.r[1] = 7; cpu.r[2] = 6;
    run1(0xE0000291);                       /* mul r0, r1, r2 */
    CHECK(cpu.r[0] == 42);

    if (fails == 0)
        printf("arm7 self-check: all checks passed\n");
    else
        printf("arm7 self-check: %d FAILED\n", fails);
    return fails != 0;
}
