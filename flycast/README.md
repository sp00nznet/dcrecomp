# Flycast Hardware Subsystems

This directory contains hardware emulation subsystems extracted from
[Flycast](https://github.com/flyinghead/flycast), the Dreamcast/Naomi emulator.

## What's Here

These are the battle-tested hardware emulation components that replace dcrecomp's
homespun HAL. Instead of our simple register stubs, recompiled game code calls into
Flycast's accurate emulation of:

| Directory | Subsystem | Description |
|-----------|-----------|-------------|
| `pvr/`    | PowerVR2 GPU | Tile Accelerator, vertex processing, OpenGL/Vulkan/DX rendering |
| `aica/`   | AICA Sound | 64-channel sound, ADPCM, ARM7 DSP, audio mixing |
| `maple/`  | Maple / JVS | Controller bus, JVS arcade I/O protocol, button/coin/analog |
| `naomi/`  | Naomi Board | ROM boards (M1/M2/M4), card readers, hoppers, EEPROMs |
| `holly/`  | Holly SB | System bus registers, interrupt controller (47 IRQ types), DMA |
| `mem/`    | Memory | Address space routing, memory-mapped I/O handler registration |

Plus scheduler (`sh4_sched.*`), interrupt handling, and MMR (memory-mapped registers).

## How It Connects to Recompiled Code

```
Recompiled C function (from static_recompile.py)
  │
  ├── sh4_read32(cpu, addr)     ← our CPU state wrapper
  │     │
  │     └── addrspace::read32(phys_addr)    ← Flycast address space
  │           │
  │           ├── RAM access (direct pointer)
  │           ├── pvr_ReadReg()             ← PVR2 register read
  │           ├── sb_ReadMem()              ← System bus register
  │           ├── ReadMem_naomi()           ← Naomi cart/board
  │           └── aica::readReg()           ← AICA sound register
  │
  └── sh4_write32(cpu, addr, val)
        │
        └── addrspace::write32(phys_addr, val)
              │
              ├── RAM write (direct pointer)
              ├── pvr_WriteReg()            ← triggers rendering
              ├── sb_WriteMem()             ← triggers DMA, interrupts
              ├── WriteMem_naomi()          ← ROM board commands
              └── maple DMA                 ← controller polling
```

## Adapting for Recompilation

The key difference from normal Flycast operation:

- **Normal Flycast**: SH4 interpreter/JIT executes instructions → reads/writes memory
- **dcrecomp**: Statically recompiled C functions call `sh4_read/write()` → same memory routing

We strip the SH4 interpreter/JIT and replace it with our recompiled function calls.
The hardware subsystems don't care whether the memory access comes from an interpreter
or from native C code - they just see register reads and writes.

## Scheduler Adaptation

Flycast uses cycle-counting to synchronize subsystems. In a recomp, we don't have
exact cycle counts, so the scheduler runs on wall-clock time or frame boundaries:

- VBlank fires every ~16.67ms (60Hz)
- AICA generates samples at 44.1kHz
- DMA completions are instantaneous (no cycle-accurate timing needed)

## License

Flycast is licensed under **GPLv2**. All code in this directory is subject to GPLv2 terms.
See https://github.com/flyinghead/flycast/blob/master/LICENSE for details.

Projects using these subsystems must:
- Make their source code available
- License derivatives under GPLv2
- Include the original copyright notices
