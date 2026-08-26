/**
 * ARM7DI - the processor inside the AICA.
 *
 * The Dreamcast's sound hardware has its own CPU, and the sound driver runs on
 * it. The SH-4 uploads that driver into sound RAM, releases the ARM from reset,
 * and then talks to it through a mailbox: it leaves a request, the driver
 * services it and writes back. A game that gets no reply concludes its sound
 * hardware is broken - ChuChu Rocket treats that as fatal and exits to the BIOS
 * without ever entering its main loop.
 *
 * This is ARMv3: ARM state only, no Thumb, no MMU, no long multiplies. Written
 * from the ARM Architecture Reference. Deliberately not derived from any
 * emulator - Flycast is the obvious reference for how this hardware behaves,
 * and it is GPLv2, so nothing here is taken from it.
 *
 * The ARM sees sound RAM at 0x00000000 and the AICA's registers at 0x00800000.
 */

#ifndef DCRECOMP_ARM7_H
#define DCRECOMP_ARM7_H

#include <stdint.h>
#include <stdbool.h>

/* Processor modes, as they appear in the low five bits of CPSR. */
#define ARM7_MODE_USR  0x10
#define ARM7_MODE_FIQ  0x11
#define ARM7_MODE_IRQ  0x12
#define ARM7_MODE_SVC  0x13
#define ARM7_MODE_ABT  0x17
#define ARM7_MODE_UND  0x1B

/* CPSR bits. */
#define ARM7_N  (1u << 31)
#define ARM7_Z  (1u << 30)
#define ARM7_C  (1u << 29)
#define ARM7_V  (1u << 28)
#define ARM7_I  (1u << 7)    /* IRQ disable */
#define ARM7_F  (1u << 6)    /* FIQ disable */

typedef struct {
    /* r[15] is the PC. Reading it yields the instruction address plus eight,
     * because the ARM7 fetches two instructions ahead; the interpreter keeps
     * r[15] at that value while an instruction executes. */
    uint32_t r[16];
    uint32_t cpsr;
    uint32_t spsr;              /* of the current mode; meaningless in user */

    /* Banked registers. The visible set above is swapped with these on a mode
     * change. FIQ banks r8-r14, the rest bank only r13 and r14. */
    uint32_t bank_usr[7];       /* r8..r14 */
    uint32_t bank_fiq[7];
    uint32_t bank_svc[2], bank_abt[2], bank_irq[2], bank_und[2];
    uint32_t spsr_fiq, spsr_svc, spsr_abt, spsr_irq, spsr_und;

    bool     running;           /* false while held in reset */
    uint64_t instructions;
} ARM7;

/* Hand the core its sound RAM. Held in reset until arm7_set_reset(cpu, false). */
void arm7_init(ARM7 *cpu, uint8_t *sound_ram, uint32_t sound_ram_size);

/* The SH-4 drives this through bit 0 of the AICA register at 0x2C00. Releasing
 * reset restarts the core from the vector at address 0. */
void arm7_set_reset(ARM7 *cpu, bool held);

/* Run up to `budget` instructions. Returns how many it ran; zero while the
 * core is in reset. */
int arm7_run(ARM7 *cpu, int budget);

/* Raise or clear the FIQ line. The AICA wires its interrupts to FIQ, and the
 * driver's whole job is servicing them. */
void arm7_set_fiq(ARM7 *cpu, bool asserted);

uint64_t arm7_instruction_count(const ARM7 *cpu);

/* Debug: the registers the driver reads most, then reset the counts. */
void arm7_dump_polled_regs(void);
void arm7_dump_polled_ram(void);
uint64_t arm7_fiq_count(const ARM7 *cpu);

#endif /* DCRECOMP_ARM7_H */
