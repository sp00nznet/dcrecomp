# Flycast Extraction Guide

## Overview

This document describes how dcrecomp extracts and adapts Flycast's hardware emulation
subsystems for use as libraries in static recompilation projects. The goal is to replace
dcrecomp's homespun HAL with battle-tested emulation code from Flycast, while stripping
out the CPU interpreter/JIT that we don't need (recompiled code runs natively).

## Why Flycast?

- Supports both Dreamcast and Naomi arcade hardware
- Mature, accurate hardware emulation (PVR2, AICA, Maple, JVS)
- Already has Mushiking barcode card reader support
- Clean subsystem interfaces with well-defined boundaries
- Active open-source project (GPLv2)
- Same pattern used by other recomp projects (e.g., Xenia → Xenon)

## Architecture

Flycast's hardware is organized as independent subsystems connected via:

1. **Address Space** (`addrspace.h`) - Memory-mapped I/O routing
2. **Scheduler** (`sh4_sched.h`) - Cycle-accurate timing callbacks
3. **Holly Interrupt Controller** (`holly_intc.h`) - Inter-subsystem signaling

### Subsystem Extraction Map

```
Flycast Source                    → dcrecomp Library
─────────────────────────────────────────────────────
core/hw/pvr/                     → libpvr2     (GPU rendering)
core/hw/aica/                    → libaica     (sound processor)
core/hw/maple/                   → libmaple    (controllers, JVS I/O)
core/hw/naomi/                   → libnaomi    (ROM boards, card readers)
core/hw/holly/                   → libholly    (system bus, interrupts)
core/hw/mem/addrspace.h          → libmemory   (address space routing)
core/hw/sh4/sh4_sched.h          → libsched    (timing/scheduler)
```

### What We Keep vs Strip

**Keep (hardware emulation):**
- PVR2 Tile Accelerator + all renderers (OpenGL, Vulkan, DirectX)
- AICA sound processor + ARM7 core
- Maple bus protocol + all device types
- JVS I/O board emulation (full protocol)
- Card reader implementations (Sanwa, barcode, etc.)
- Naomi ROM board types (M1, M2, M4, GD-ROM)
- Holly system bus + interrupt controller
- Address space mapping system
- Scheduler (adapted for recompiled code)

**Strip (CPU emulation - we don't need it):**
- SH4 interpreter (`core/hw/sh4/interpr/`)
- SH4 dynamic recompiler (`core/rec-*/`)
- SH4 JIT infrastructure
- Dynarec memory management

### Integration With Recompiled Code

Instead of the SH4 interpreter calling into hardware via memory-mapped I/O,
our recompiled C functions call `sh4_read32()`/`sh4_write32()` which route
through Flycast's `addrspace` handlers:

```
Recompiled game function
  → sh4_write32(cpu, 0xA05F8014, val)     // Write to PVR STARTRENDER
  → addrspace::write32(0x005F8014, val)    // Routes to PVR handler
  → pvr_WriteReg(0x8014, val)             // Flycast PVR processes it
  → renderer->Render()                     // OpenGL/Vulkan renders frame
```

## Subsystem Details

### PVR2 GPU (core/hw/pvr/)

**Key interface:** `Renderer_if.h`
- `Renderer` abstract class with `Init()`, `Process()`, `Render()`, `Term()`
- Multiple backends: OpenGL (`rend/gl4/`), Vulkan (`rend/vulkan/`), DirectX (`rend/dx*/`)
- TA (Tile Accelerator) processes 32-byte geometry packets
- Supports all 18 PVR2 vertex types, textures, fog, modifier volumes

### AICA Sound (core/hw/aica/)

**Key interface:** `aica_if.h`
- `aica::init()`, `aica::reset()`, `aica::term()`
- `aica::timeStep()` - called per audio sample (44.1kHz)
- ARM7 coprocessor for DSP programs
- 64 sound channels with ADPCM support

### Maple / JVS (core/hw/maple/)

**Key interface:** `maple_if.h`
- `maple_Init()`, `maple_Reset()`, `maple_vblank()`
- Device model: `MapleDevices[4][6]` (4 ports, 6 sub-devices each)
- Full JVS protocol in `maple_jvs.cpp` (buttons, coins, analog, lightguns)
- Button mapping via `naomi_button_mapping[32]`

### Naomi Hardware (core/hw/naomi/)

**Key interface:** `naomi_cart.h`
- `Cartridge` base class: `ReadMem()`, `WriteMem()`, `GetBootId()`
- M4 encryption with PIC security chip
- Card reader: `card_reader::barcodeInit()` for Mushiking
- Hopper/coin system: `hopper::init()`
- Game database: 1500+ games in `naomi_roms.cpp`

### Holly System Bus (core/hw/holly/)

**Key interface:** `sb.h`, `holly_intc.h`
- `sb_ReadMem()`, `sb_WriteMem()` - register access
- `asic_RaiseInterrupt()`, `asic_CancelInterrupt()` - 47 interrupt types
- DMA controllers: CH2, Sort, Maple, AICA, GD-ROM, PVR

### Address Space (core/hw/mem/)

**Key interface:** `addrspace.h`
- `registerHandler()` - subsystems register read/write callbacks
- `mapHandler()` - map handlers to address ranges
- `mapBlock()` - map raw memory buffers
- Replaces our simple `if/else` chain in `sh4_cpu.c`

### Scheduler (core/hw/sh4/)

**Key interface:** `sh4_sched.h`
- `sh4_sched_register()` - register timed callback
- `sh4_sched_request()` - schedule callback N cycles from now
- `sh4_sched_now64()` - current cycle count
- Drives VBlank, audio mixing, DMA completion timing

## Build Integration

The extracted Flycast subsystems build as a static library (`libflycast_hw.a`)
that dcrecomp links against instead of the homespun HAL:

```cmake
# In dcrecomp CMakeLists.txt
add_library(flycast_hw STATIC
    flycast/core/hw/pvr/*.cpp
    flycast/core/hw/aica/*.cpp
    flycast/core/hw/maple/*.cpp
    flycast/core/hw/naomi/*.cpp
    flycast/core/hw/holly/*.cpp
    flycast/core/hw/mem/*.cpp
    flycast/core/hw/sh4/sh4_sched.cpp
    flycast/core/hw/sh4/sh4_mmr.cpp
)

target_link_libraries(dcrecomp_core PUBLIC flycast_hw)
```

## License

Flycast is licensed under GPLv2. Projects using the extracted subsystems must
comply with GPLv2 terms (source code availability, same license for derivatives).
