/* The translated IRQ callback must not replace the interrupted dispatch PC. */
#include "hal/dc_hardware.h"
#include "recompiler/sh4_cpu.h"
#include <string.h>
#include <stdio.h>
#ifndef _WIN32
/* Upstream defines the crash-handler installer only on Windows. */
void sh4_install_crash_handler(void) {}
#endif
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    return 1; \
} } while (0)

static int deliveries;
static void handler(SH4CPU *cpu) {
    deliveries++;
    cpu->pc = 0x8C001234;
    cpu->pr = 0x8C005678;
    cpu->r[8] = 0;
    cpu->r[15] -= 16;
    /* Also exercise the existing re-entrancy guard. */
    sh4_poll_irq(cpu);
    dc_hw_write32(sh4_get_hardware(), SB_ISTNRM, 1u << 3);
}
int main(void) {
    SH4CPU cpu;
    sh4_init(&cpu);
    sh4_set_cpu_ref(&cpu);
    DCHardware *hw = dc_hw_init();
    CHECK(hw != NULL);
    sh4_set_hardware(hw);
    sh4_set_irq_handler(handler);
    dc_hw_write32(hw, SB_IML6NRM, 1u << 3);
    dc_pvr_wait_vblank(hw);
    cpu.pc = 0x8C020000; cpu.pr = 0x8C030000;
    cpu.r[8] = 0x12345678; cpu.r[15] = 0x8C00FFF0;
    cpu.sr = 0;
    sh4_poll_irq(&cpu);
    CHECK(deliveries == 1);
    CHECK(cpu.pc == 0x8C020000 && cpu.pr == 0x8C030000);
    CHECK(cpu.r[8] == 0x12345678 && cpu.r[15] == 0x8C00FFF0);
    CHECK(!(dc_hw_get_istnrm(hw) & (1u << 3)));
    /* A second interrupt preserves a different pending dispatch target. */
    dc_pvr_wait_vblank(hw); cpu.pc = 0x0C040000;
    sh4_poll_irq(&cpu);
    CHECK(deliveries == 2 && cpu.pc == 0x0C040000);
    sh4_set_irq_handler(NULL); sh4_set_hardware(NULL);
    dc_hw_destroy(hw); sh4_set_cpu_ref(NULL); sh4_destroy(&cpu);
    puts("IRQ bridge: dispatch PC, preserved registers, acknowledgement and re-entrancy passed");
    return 0;
}
