# Roadmap

## Next
- **Conformance harness.** Run SH-4 instruction tests against a fixed corpus,
  report a pass/fail count, fail CI on regression. tbg-decomp's SH-4 object
  simulator is the likely corpus source (check its licence first).
- **CI** on push and PR to `main`: build the C core, run `tests/arm7_selftest.c`
  and the harness.
- **Headless mode**, `--headless --record out.mp4`, shared with the other recomps.
- **Quick start** (`Setup.cmd` / `setup.sh`) and a step-by-step Getting Started
  from a clean machine.
- **Move the remaining title-specific code out of the generic runtime**: the
  dispatcher's relocated-bootstrap remap (`0x0C004000-0x0C008000 -> +0xC100`)
  is Crazy Taxi's.
- **Record unresolved dispatch targets to a file** in `--entries` format,
  instead of a stdout log capped at 100 lines (#1).

## Later
- **Accurate hardware backend**: link Flycast's PVR2, AICA, Maple/JVS, Naomi
  board and Holly subsystems against recompiled code, with the CPU core
  stripped. Recompiled code would reach them through `sh4_read32`/`sh4_write32`
  -> Flycast's `addrspace` handlers. dcrecomp being GPL-2.0 makes this
  licence-compatible; it needs its files listed in `NOTICE`.
- **AICA sound** past the timer and ARM7 core.
- **Full PVR2**: textures, fog, modifier volumes.

## Out of scope
- Distributing recompiled game code, disc images, ROMs or BIOS files.
- An interpreter or JIT: dcrecomp is ahead-of-time only.
