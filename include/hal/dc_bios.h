/**
 * Dreamcast BIOS syscall emulation.
 *
 * The BIOS leaves a small table of function pointers in low RAM and games call
 * through it for anything it owns - system settings in flash, the ROM font, the
 * GD-ROM drive. A game reaches one via a thunk like:
 *
 *     mov.l  @(sel), r7      ; function selector
 *     mov.l  @(vec), r0      ; r0 = 0x8C0000B8, say
 *     mov.l  @r0, r0         ; load the BIOS entry point
 *     jmp    @r0
 *
 * We boot without a BIOS, so that table reads back as zeros and the jump lands
 * on address 0. Installing recognisable values and intercepting calls to them
 * lets the syscalls be serviced natively instead.
 *
 * Arguments follow the BIOS convention: r4-r6 are parameters, r7 selects the
 * function within a vector, and r0 carries the result.
 */

#ifndef DC_BIOS_H
#define DC_BIOS_H

#include "recompiler/sh4_cpu.h"
#include <stdio.h>

/* The vector table the BIOS leaves in low RAM. */
#define BIOS_VEC_SYSINFO   0x8C0000B0u
#define BIOS_VEC_ROMFONT   0x8C0000B4u
#define BIOS_VEC_FLASHROM  0x8C0000B8u
#define BIOS_VEC_GDROM     0x8C0000BCu

#define BIOS_VEC_FIRST     BIOS_VEC_SYSINFO
#define BIOS_VEC_LAST      BIOS_VEC_GDROM

/* Point each vector at itself, so a call through the table arrives at an
 * address sh4_bios_syscall() recognises. Call once after loading the game. */
void sh4_bios_install_vectors(SH4CPU *cpu);

/* Service a call that landed on a vector address. Returns true if it handled
 * it, false if the address was not a BIOS vector - in which case the caller
 * should carry on with normal dispatch. */
bool sh4_bios_syscall(SH4CPU *cpu);

/* Where flashrom reads come from. Without this, reads return zeros, which most
 * games treat as "settings not initialised" rather than as an error. */
void sh4_bios_set_flashrom(const void *data, uint32_t size);

/* Serve GD-ROM sector reads from a disc data track. `start_lba` is the absolute
 * LBA the track begins at, which for a Dreamcast GD-ROM is not 0 - the
 * high-density area starts at 45000 and later tracks start higher still. */
void sh4_bios_set_gdrom_track(const char *path, uint32_t start_lba);

#endif /* DC_BIOS_H */
