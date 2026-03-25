#!/usr/bin/env python3
"""Generate stub functions for undefined func_XXXXXXXX references.

Scans all game_code_*.c files for func_XXXXXXXX calls, compares against
the declarations in game_functions.h, and generates:
  - src/game/game_stubs.c  (stub function bodies)
  - Appends declarations to include/game/game_functions.h
"""

import re
import os
import sys

PROJ_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_DIR = os.path.join(PROJ_ROOT, "src", "game")
HEADER = os.path.join(PROJ_ROOT, "include", "game", "game_functions.h")
STUBS_C = os.path.join(GAME_DIR, "game_stubs.c")

FUNC_RE = re.compile(r'\bfunc_(8C[0-9A-Fa-f]{6})\b')


def find_declared_functions():
    """Parse game_functions.h for declared func_XXXXXXXX names."""
    declared = set()
    with open(HEADER, 'r') as f:
        for line in f:
            m = FUNC_RE.search(line)
            if m:
                declared.add("func_" + m.group(1).upper())
    return declared


def find_referenced_functions():
    """Scan all game_code_*.c files for func_XXXXXXXX references."""
    referenced = set()
    for fname in os.listdir(GAME_DIR):
        if fname.startswith("game_code_") and fname.endswith(".c"):
            path = os.path.join(GAME_DIR, fname)
            with open(path, 'r') as f:
                for line in f:
                    for m in FUNC_RE.finditer(line):
                        referenced.add("func_" + m.group(1).upper())
    # Also check dispatch_table.c
    dt_path = os.path.join(GAME_DIR, "dispatch_table.c")
    if os.path.exists(dt_path):
        with open(dt_path, 'r') as f:
            for line in f:
                for m in FUNC_RE.finditer(line):
                    referenced.add("func_" + m.group(1).upper())
    return referenced


def find_defined_functions():
    """Scan all game_code_*.c files for func_XXXXXXXX definitions (function bodies)."""
    defined = set()
    for fname in os.listdir(GAME_DIR):
        if fname.startswith("game_code_") and fname.endswith(".c"):
            path = os.path.join(GAME_DIR, fname)
            with open(path, 'r') as f:
                for line in f:
                    # Match function definition: void func_XXXXXXXX(SH4CPU *cpu) {
                    if line.startswith("void func_"):
                        m = FUNC_RE.search(line)
                        if m and '{' in line:
                            defined.add("func_" + m.group(1).upper())
    return defined


def main():
    print("Scanning for undefined function references...")

    declared = find_declared_functions()
    referenced = find_referenced_functions()
    defined = find_defined_functions()

    print(f"  Declared in header:  {len(declared)}")
    print(f"  Referenced in code:  {len(referenced)}")
    print(f"  Defined (have body): {len(defined)}")

    # Undefined = referenced but not defined
    undefined = sorted(referenced - defined)
    # Undeclared = referenced but not declared
    undeclared = sorted(referenced - declared)

    print(f"  Undefined (need stubs): {len(undefined)}")
    print(f"  Undeclared (need header): {len(undeclared)}")

    if not undefined:
        print("No stubs needed!")
        return

    # Generate stub source file
    print(f"\nGenerating {STUBS_C}...")
    with open(STUBS_C, 'w') as f:
        f.write("/* Auto-generated stub functions for undefined references */\n")
        f.write("/* These are BSR targets that land in data regions or */\n")
        f.write("/* branch targets outside known function boundaries. */\n")
        f.write('#include "recompiler/sh4_cpu.h"\n')
        f.write('#include "game/game_functions.h"\n\n')
        f.write(f"/* {len(undefined)} stub functions */\n\n")

        for func_name in undefined:
            addr = func_name.replace("func_", "0x")
            f.write(f"void {func_name}(SH4CPU *cpu) {{\n")
            f.write(f'    /* Stub: no code at {addr} (data region or misaligned branch) */\n')
            f.write(f"    (void)cpu;\n")
            f.write(f"}}\n\n")

    print(f"  Wrote {len(undefined)} stub functions")

    # Add undeclared functions to header
    if undeclared:
        print(f"\nAdding {len(undeclared)} declarations to {HEADER}...")
        with open(HEADER, 'r') as f:
            header_content = f.read()

        # Insert before the #endif
        insert_point = header_content.rfind("#endif")
        if insert_point == -1:
            print("ERROR: Could not find #endif in header!")
            return

        decl_block = f"\n/* {len(undeclared)} stub function declarations */\n"
        for func_name in undeclared:
            decl_block += f"void {func_name}(SH4CPU *cpu);\n"
        decl_block += "\n"

        header_content = header_content[:insert_point] + decl_block + header_content[insert_point:]

        with open(HEADER, 'w') as f:
            f.write(header_content)

        print(f"  Added {len(undeclared)} declarations")

    print("\nDone! Add src/game/game_stubs.c to your CMakeLists.txt")


if __name__ == "__main__":
    main()
