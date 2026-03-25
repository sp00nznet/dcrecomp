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
│   │   ├── dc_hardware.h       # Hardware registers (PVR, AICA, Maple, SB)
│   │   ├── pvr2.h              # PowerVR2 Tile Accelerator + renderer API
│   │   └── naomi_io.h          # Naomi JVS arcade I/O (buttons, coins, card readers)
│   └── platform/platform.h     # SDL2/Win32 platform abstraction
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

## Current Projects

- **Crazy Taxi** (Dreamcast) - 11,561 functions, first link achieved
- **Mushiking: King of Beetles** (Naomi) - 26,004 functions, builds and runs in headless mode

## License

Private repository.
