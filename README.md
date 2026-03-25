# dcrecomp - Dreamcast/Naomi Static Recompilation Framework

A framework for statically recompiling Sega Dreamcast and Naomi arcade games from SH-4 binary code to native C. This is **not** an emulator - the original game code is translated ahead-of-time into C functions that compile natively on modern hardware.

## Architecture

The Dreamcast and Naomi arcade boards share the same core hardware:
- **CPU**: Hitachi SH-4 (SH7091) @ 200MHz
- **GPU**: PowerVR2 (Holly) with Tile Accelerator
- **Sound**: Yamaha AICA (ARM7 + 64-channel DSP)
- **RAM**: 16MB (Dreamcast) / 32MB (Naomi)

### Components

```
dcrecomp/
├── include/
│   ├── recompiler/sh4_cpu.h    # SH-4 CPU state (registers, memory, MMU)
│   ├── hal/
│   │   ├── dc_hardware.h       # Hardware registers (lightweight/legacy HAL)
│   │   ├── pvr2.h              # PowerVR2 TA + renderer API (lightweight)
│   │   ├── naomi_io.h          # Naomi JVS I/O (lightweight)
│   │   └── flycast_adapter.h   # C API bridging to Flycast C++ subsystems
│   └── platform/platform.h     # SDL2/Win32 platform abstraction
├── flycast/                     # Extracted Flycast hardware emulation (GPLv2)
│   ├── pvr/                    # PowerVR2 GPU (TA, textures, OpenGL/Vulkan/DX)
│   ├── aica/                   # AICA sound (64-ch, ADPCM, ARM7 DSP)
│   ├── maple/                  # Maple bus + full JVS protocol
│   ├── naomi/                  # ROM boards, card readers, hoppers, EEPROMs
│   ├── holly/                  # System bus, interrupt controller, DMA
│   ├── mem/                    # Address space routing + handler registration
│   └── README.md               # Integration architecture docs
├── src/
│   ├── recompiler/sh4_cpu.c    # CPU state, memory access, address translation
│   ├── hal/
│   │   ├── dc_hardware.c       # Register read/write, VBlank, DMA, interrupts
│   │   ├── pvr2_ta.c           # TA FIFO parser (32-byte packets, strip conversion)
│   │   ├── pvr2_render.c       # OpenGL 3.3 rendering backend
│   │   └── naomi_io.c          # JVS master, button/coin/card reader emulation
│   └── platform/platform_sdl2.c
├── tools/
│   ├── sh4_disasm.py           # SH-4 disassembler & function finder
│   ├── static_recompile.py     # SH-4 to C translator (core recompiler)
│   ├── generate_stubs.py       # Stub generator for undefined references
│   ├── extract_gdi.py          # GD-ROM disc extractor (Dreamcast)
│   ├── extract_naomi_rom.py    # Naomi ROM board extractor (ic8/ic9 flash chips)
│   └── naomi_m4_decrypt.py     # Naomi M4 board decryption (317-xxxx security chips)
└── CMakeLists.txt              # Builds dcrecomp_core static library
```

### Recompilation Pipeline

```
ROM/Disc Image
    │
    ├─ extract_gdi.py          (Dreamcast GD-ROM)
    ├─ extract_naomi_rom.py    (Naomi ROM boards)
    └─ naomi_m4_decrypt.py     (Naomi M4 encrypted ROMs)
    │
    ▼
1ST_READ.BIN (game executable)
    │
    ├─ sh4_disasm.py           Find functions (prologue detection + call graph)
    │
    ▼
function_map.json
    │
    ├─ static_recompile.py     Translate SH-4 → C (instruction-by-instruction)
    │
    ▼
game_code_*.c + game_functions.h + dispatch_table.c
    │
    ├─ generate_stubs.py       Fill in undefined references
    │
    ▼
Compile with CMake → Native executable
```

### Supported Hardware Features

| Feature | Status |
|---------|--------|
| SH-4 integer instructions | Full |
| SH-4 floating point (single/double) | Full |
| SH-4 delay slots | Full |
| Memory-mapped I/O | Full |
| MMU / UTLB | Full |
| Store Queues (SQ DMA) | Full |
| PowerVR2 Tile Accelerator | Full (18 vertex types) |
| OpenGL 3.3 rendering | Basic (colored triangles, blending) |
| PVR DMA / Sort DMA | Full |
| AICA sound | Stub |
| Maple Bus (DC controllers) | Full |
| Naomi JVS I/O | Basic (buttons, coins, card reader) |
| Naomi M4 decryption | Full |
| Flycast PVR2 (textures, fog, all vertex types) | Extracted, adapter in progress |
| Flycast AICA (full sound) | Extracted, not yet wired |
| Flycast JVS (complete arcade I/O) | Extracted, not yet wired |
| Flycast Naomi (card readers, hoppers, ROMs) | Extracted, not yet wired |

## Usage

### As a submodule in a game project

```cmake
add_subdirectory(dcrecomp)
target_link_libraries(my_game dcrecomp_core)
```

### Running the tools

```bash
# Dreamcast: extract GD-ROM
python tools/extract_gdi.py "Track 3.bin" disc_extract/

# Naomi: decrypt and extract ROM
python tools/naomi_m4_decrypt.py --zip game.zip decrypted.bin
python tools/extract_naomi_rom.py decrypted.bin output/

# Disassemble (use --base for non-standard load addresses)
python tools/sh4_disasm.py 1ST_READ.BIN --base 0x8C020000

# Recompile to C
python tools/static_recompile.py 1ST_READ.BIN src/game include/game --base 0x8C020000

# Generate stubs
python tools/generate_stubs.py
```

## Building

Requires CMake 3.16+, C11 compiler. Optional: SDL2, OpenGL 3.3 + GLEW.

```bash
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

## Hardware Backends

dcrecomp supports two hardware emulation backends:

### Lightweight HAL (legacy)
The original homespun hardware abstraction (`src/hal/`). Simple register stubs
suitable for initial bring-up and headless testing. Limited accuracy.

### Flycast Backend (recommended)
Battle-tested hardware emulation extracted from [Flycast](https://github.com/flyinghead/flycast)
(`flycast/`). Provides accurate PVR2 rendering (with textures, fog, modifier volumes),
full AICA sound, complete JVS arcade I/O protocol, Naomi card readers, and more.

The Flycast backend follows the same pattern used by other recomp projects
(e.g., Xenia → Xenon recomp): take a mature emulator, strip the CPU
interpreter/JIT, and link the hardware subsystems as libraries against
statically recompiled game code.

See `flycast/README.md` and `docs/flycast-extraction.md` for details.

## Current Projects

- **Crazy Taxi** (Dreamcast) - 11,561 functions, first link achieved
- **Mushiking: King of Beetles** (Naomi) - 26,004 functions, SDL2+OpenGL build running

## Contributing

This framework is designed to make Dreamcast/Naomi static recompilation accessible.
To recomp a new game:

1. Obtain the ROM/disc image
2. Run the extraction pipeline (decrypt if Naomi M4, extract filesystem)
3. Disassemble with `sh4_disasm.py`
4. Recompile with `static_recompile.py`
5. Create a game project with dcrecomp as a submodule
6. Write a `main.c` bootstrap (set entry point, RAM size, BIOS state)
7. Build and iterate on hardware interactions

See Mushiking (kingofbeetle) as a reference implementation.

## License

- dcrecomp core: Private repository
- Flycast subsystems (`flycast/`): GPLv2 (see flycast/README.md)
