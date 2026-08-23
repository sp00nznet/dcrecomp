#!/usr/bin/env python3
"""
SH-4 Static Recompiler - Translates SH-4 binary code to C source code.

Reads the 1ST_READ.BIN executable and produces C functions that perform
equivalent operations using the SH4CPU state structure. Each SH-4 function
is translated to a C function that operates on the CPU state.

This is the core of the static recompilation approach - instead of
interpreting or dynamically recompiling SH-4 instructions at runtime,
we translate them ahead-of-time to native C code that can be compiled
with an optimizing C compiler (gcc/clang/MSVC).
"""

import struct
import sys
import json
import os
from collections import defaultdict

LOAD_ADDR = 0x8C010000

def sext8(v):
    return v if v < 128 else v - 256

def sext12(v):
    return v if v < 2048 else v - 4096

class SH4Recompiler:
    """Translates SH-4 binary functions to C code."""

    def __init__(self, binary_data, base_addr=LOAD_ADDR):
        self.data = binary_data
        self.entry_hints = set()
        self.base = base_addr
        self.size = len(binary_data)
        self.functions = {}
        self.call_targets = set()
        self.labels = set()        # Branch targets within functions
        self.string_refs = {}      # PC-relative data references
        self.indirect_calls = []   # Addresses with indirect JSR/JMP

    def read_u16(self, offset):
        return struct.unpack_from('<H', self.data, offset)[0]

    def read_u32(self, offset):
        if offset + 4 <= self.size:
            return struct.unpack_from('<I', self.data, offset)[0]
        return 0

    def addr_to_offset(self, addr):
        return addr - self.base

    def offset_to_addr(self, offset):
        return self.base + offset

    def find_functions(self):
        """Identify function boundaries using heuristic analysis."""
        call_targets = set()
        prologue_addrs = set()

        # Pass 1: Find BSR targets and function prologues
        for offset in range(0, self.size - 1, 2):
            opcode = self.read_u16(offset)
            pc = self.offset_to_addr(offset)

            # BSR - direct subroutine call
            if (opcode & 0xF000) == 0xB000:
                d12 = opcode & 0xFFF
                disp = sext12(d12) * 2
                target = pc + 4 + disp
                call_targets.add(target)

            # STS.L PR, @-R15 (function prologue)
            if opcode == 0x4F22:
                prologue_addrs.add(pc)

        self.call_targets = call_targets

        # Forced entry points (code not detected by prologue/BSR analysis)
        # These are reached via indirect jumps (JMP @Rn) or from BIOS dispatch
        forced_entries = {
            self.base + 0x100,    # 0x8C010100: relocated bootstrap entry point
            0x8C148990,           # Main loop entry (BRA target from init)
            0x8C14ACEE,           # Init function (JMP target from func_8C148988)
            0x8C14AD46,           # Main game loop (JMP target from func_8C148990)
            0x8C02A808,           # Indirect JSR target from game init
            0x8C02A698,           # Game main() - dead code entry in func_8C02A680
            0x8C02A6EC,           # Game main loop body (BRA target from func_8C02A760)
            # Functions detected as unresolved indirect call targets at runtime:
            0x8C080BA4,           # memcmp (CRT0 runtime)
            0x8C07A870,           # Hardware init (writes constants)
            0x8C07A570,           # Veneer (BRA to 0x8C07A4A0 with r4=0)
            0x8C07A580,           # Setter (writes to TA FIFO base)
            0x8C07A590,           # Setter (writes r5 to memory, returns 1)
            0x8C073B10,           # Veneer (BRA to 0x8C073910 with r7=0)
            0x8C073910,           # Target of 0x8C073B10 veneer
            0x8C07A4A0,           # Target of 0x8C07A570 veneer
            0x8C15ED94,           # SDK runtime function (called ~80x per frame)
            0x8C15EA50,           # SDK runtime function
            0x8C15EC28,           # SDK runtime function
            0x8C14D214,           # Init function
            0x8C15F258,           # SDK runtime function
            0x8C15E56C,           # SDK runtime function
            0x8C15EEDC,           # SDK runtime function
            0x8C07A5A0,           # Setter function (near 0x8C07A590)
            0x8C14D240,           # Cache flush continuation (P2 jump target from func_8C14D214)
            # Session 3: unresolved targets discovered at runtime
            0x8C087E9C,           # Init helper (called frequently from 0x8C0875F4)
            0x8C1566A0,           # SDK/init function
            0x8C15B282,           # SDK function (called via jsr @r3)
            0x8C15B27C,           # SDK function (called via jsr @r3)
            0x8C16C138,           # SDK runtime function
            0x8C07A5F0,           # Setter function (near 0x8C07A5A0)
            0x8C07A600,           # Setter function
            0x8C07A610,           # Setter function (called ~9 times)
            0x8C07A630,           # Setter function
            0x8C1587BA,           # SDK function
            0x8C15645C,           # SDK function
            0x8C14B434,           # Init function
            0x8C14BB4C,           # Init function
            0x8C16C300,           # SDK runtime function
            0x8C16F5FC,           # SDK runtime function
            0x8C1564C6,           # PVR register write helper
            # Session 3 round 2: more unresolved targets
            0x8C153160,           # SDK function
            0x8C156080,           # SDK function
            0x8C15B368,           # SDK function
            0x8C16DF10,           # SDK function
            0x8C150246,           # Init function
            0x8C080B2E,           # CRT function
            0x8C073EB0,           # Game init function
            0x8C074050,           # Game init function
            0x8C075820,           # Game function
            0x8C086BD8,           # Game function
            0x8C156406,           # Rendering callback registration?
        }

        # Also scan for mov.l @(disp,PC) + JMP @Rn patterns to find indirect targets
        for i in range(0, len(self.data) - 4, 2):
            pc = self.base + i
            op = self.data[i] | (self.data[i+1] << 8)
            # Look for JMP @Rn (0100nnnn00101011) or JSR @Rn (0100nnnn00001011)
            if (op & 0xF0FF) == 0x400B or (op & 0xF0FF) == 0x402B:
                rn = (op >> 8) & 0xF
                # Look back for mov.l @(disp,PC), Rn to find the target.
                # Nearest first: a run of consecutive load/call pairs is the
                # normal shape of an SDK init sequence, and scanning forwards
                # matched an earlier pair's load every time - which silently
                # dropped the last target in every such run.
                for j in range(i - 2, max(-2, i - 22), -2):
                    if j < 0:
                        break
                    prev = self.data[j] | (self.data[j+1] << 8)
                    if (prev & 0xF000) == 0xD000 and ((prev >> 8) & 0xF) == rn:
                        disp = prev & 0xFF
                        lit_addr = ((self.base + j) & 0xFFFFFFFC) + 4 + disp * 4
                        lit_off = lit_addr - self.base
                        if 0 <= lit_off < len(self.data) - 3:
                            target = (self.data[lit_off] | (self.data[lit_off+1] << 8) |
                                     (self.data[lit_off+2] << 16) | (self.data[lit_off+3] << 24))
                            # Convert physical to cached address
                            if 0x0C000000 <= target < 0x0D000000:
                                target = target | 0x80000000
                            if self.base <= target < self.base + self.size:
                                forced_entries.add(target)
                        break
        for addr in forced_entries:
            if self.base <= addr < self.base + self.size:
                print(f"  Forced entry point: 0x{addr:08X}")

        # Combine entry points
        all_entries = sorted(set([self.base]) | call_targets | prologue_addrs | forced_entries)

        # Filter to valid range
        all_entries = [a for a in all_entries if self.base <= a < self.base + self.size]

        # Build function map
        for i, entry in enumerate(all_entries):
            end = all_entries[i + 1] if i + 1 < len(all_entries) else self.base + self.size
            self.functions[entry] = {
                'addr': entry,
                'end': end,
                'size': end - entry,
                'has_prologue': entry in prologue_addrs,
                'is_call_target': entry in call_targets,
            }

        print(f"Found {len(self.functions)} functions")

        # Addresses reached by a direct jump. Used to keep the instruction
        # walk in step when a literal pool decodes as a branch.
        self.entry_hints = (self._mova_targets()
                            | self._external_branch_targets()
                            | self._jump_table_targets())

        self._resolve_mid_entries()

    def _resolve_mid_entries(self):
        """Find entry points that land inside an existing function.

        Indirect calls through function pointers stored in RAM (vtables,
        callback tables, jump tables) cannot be resolved by looking at the code
        around the JSR - the target is only a value at runtime. But the value
        usually comes from a pointer table that is in the binary already, so
        scanning the data for words that look like code addresses finds most of
        them.

        These are NOT added as function boundaries. Splitting a function at an
        arbitrary address breaks any loop that spans the split, because a
        backward branch across the new boundary stops being a local goto. They
        are recorded per containing function instead, and recompile_function
        emits a shared implementation with a goto dispatch at the top.

        False positives are cheap here: an address that is not a real
        instruction boundary is dropped below, and one that is costs a switch
        case and a one-line wrapper that nothing ever calls.
        """
        import bisect

        candidates = set()
        for off in range(0, self.size - 3, 4):
            v = (self.data[off] | (self.data[off + 1] << 8) |
                 (self.data[off + 2] << 16) | (self.data[off + 3] << 24))
            if 0x0C000000 <= v < 0x0D000000:
                v |= 0x80000000
            if self.base <= v < self.base + self.size and (v & 1) == 0:
                candidates.add(v)
        candidates |= self.entry_hints
        candidates -= set(self.functions)

        starts = sorted(self.functions)
        by_func = {}
        for addr in candidates:
            i = bisect.bisect_right(starts, addr) - 1
            if i < 0:
                continue
            owner = starts[i]
            if addr < self.functions[owner]['end']:
                by_func.setdefault(owner, []).append(addr)

        self.mid_entries = {}
        for owner, addrs in by_func.items():
            emittable = self._emittable_addrs(owner, self.functions[owner]['end'])
            keep = sorted(a for a in addrs if a in emittable)
            if keep:
                self.mid_entries[owner] = keep

        self.mid_entry_addrs = {a for v in self.mid_entries.values() for a in v}
        print(f"Found {len(self.mid_entry_addrs)} mid-function entry points "
              f"in {len(self.mid_entries)} functions "
              f"({len(candidates)} candidates scanned)")

    def _external_branch_targets(self):
        """Branch targets that leave the function they were emitted in.

        recompile_function turns a branch it cannot resolve locally into
        `func_<target>(cpu); return;`. If <target> is a label inside some other
        function rather than a function start, no such function exists and
        generate_stubs.py fills it with a silent no-op - so the branch returns
        without running the epilogue it was heading for, and every callee-saved
        register the function had pushed stays clobbered. That was 5642 stubs
        quietly corrupting registers.

        Treating these as mid-function entries makes the branch land where it
        was supposed to.
        """
        targets = set()
        for addr, info in self.functions.items():
            offset = self.addr_to_offset(addr)
            end = self.addr_to_offset(info['end'])
            if offset < 0 or end > self.size:
                continue
            while offset < end - 1:
                pc = self.offset_to_addr(offset)
                opcode = self.read_u16(offset)
                _, is_branch, target, has_delay = self.recompile_instruction(opcode, pc)
                if is_branch and isinstance(target, int):
                    if not (addr <= target < info['end']):
                        targets.add(target)
                offset += 4 if self._takes_delay_slot(offset, has_delay) else 2
        return targets

    def _takes_delay_slot(self, offset, has_delay):
        """Whether the word after a delay-slot branch really is its delay slot.

        False when that address is something the program jumps to directly - a
        known entry point cannot also be a delay slot, and treating it as one
        desynchronises the walk through a mis-decoded literal pool.
        """
        if not has_delay:
            return False
        return self.offset_to_addr(offset + 2) not in self.entry_hints

    def _jump_table_targets(self):
        """Targets of `braf`/`bsrf` fed from a mova'd displacement table.

        The GCC SH idiom is:

            shll   r0                  ; index *= 2
            mov    r0, r1
            mova   @(disp,PC), r0      ; r0 = table
            mov.w  @(r0,r1), r0        ; r0 = table[index]
            braf   r0                  ; PC = braf + 4 + r0

        so each entry is a forward displacement from just past the branch. The
        table has no length marker; it is followed immediately by code, so read
        entries while they still land on an even address a sane distance ahead
        and stop at the first one that does not.
        """
        WINDOW = 0x4000          # switch arms live close to the branch
        MAX_ENTRIES = 256
        targets = set()

        for offset in range(0, self.size - 1, 2):
            opcode = self.read_u16(offset)
            if (opcode & 0xF0FF) not in (0x0023, 0x0003):   # braf / bsrf
                continue
            reg = (opcode >> 8) & 0xF
            braf_pc = self.offset_to_addr(offset)

            # Walk back for the mova that loaded the table, and the load that
            # indexes it - the load width tells us the entry size.
            table = None
            entry_size = 2
            for back in range(2, 20, 2):
                if offset - back < 0:
                    break
                prev = self.read_u16(offset - back)
                if (prev & 0xFF00) == 0xC700:               # mova @(disp,PC),r0
                    pc = self.offset_to_addr(offset - back)
                    table = (((pc + 4) & 0xFFFFFFFC) + (prev & 0xFF) * 4)
                    break
                if (prev & 0xF00F) == 0x000C and ((prev >> 8) & 0xF) == reg:
                    entry_size = 1                          # mov.b @(R0,Rm),Rn
            if table is None:
                continue

            base = braf_pc + 4
            for i in range(MAX_ENTRIES):
                off = self.addr_to_offset(table) + i * entry_size
                if off < 0 or off + entry_size > self.size:
                    break
                if entry_size == 2:
                    disp = self.read_u16(off)
                else:
                    disp = self.data[off] * 2
                target = base + disp
                if (target & 1) or not (base <= target < base + WINDOW):
                    break
                if not (self.base <= target < self.base + self.size):
                    break
                targets.add(target)
        return targets

    def _mova_targets(self):
        """Addresses produced by MOVA @(disp,PC),R0.

        SDK code reaches its cache and store-queue routines through a P1->P2
        trampoline: MOVA to get the address of the following code, OR in
        0xA0000000 to make it uncached, then JMP. The target is computed at
        runtime from a PC-relative displacement, so neither the literal-pool
        scan nor the branch scan sees it - the jump lands on an address with no
        entry point, gets skipped, and the routine's epilogue never runs. One
        of those leaked four bytes of stack per call.

        MOVA is also used to address data (jump tables, string literals). Those
        candidates are dropped by the emittable filter or, if they survive it,
        cost one switch case that nothing ever calls.
        """
        targets = set()
        for offset in range(0, self.size - 1, 2):
            opcode = self.read_u16(offset)
            if (opcode & 0xFF00) == 0xC700:      # MOVA @(disp,PC), R0
                pc = self.offset_to_addr(offset)
                targets.add((((pc + 4) & 0xFFFFFFFC) + (opcode & 0xFF) * 4))
        return targets

    def _emittable_addrs(self, start_addr, end_addr):
        """Addresses inside [start,end) that begin an emitted instruction.

        Walks the same way recompile_function does, so delay slots and the
        insides of literal pools are excluded.
        """
        out = set()
        offset = self.addr_to_offset(start_addr)
        end = self.addr_to_offset(end_addr)
        if offset < 0 or end > self.size:
            return out
        while offset < end - 1:
            out.add(self.offset_to_addr(offset))
            opcode = self.read_u16(offset)
            _, _, _, has_delay = self.recompile_instruction(opcode,
                                                           self.offset_to_addr(offset))
            offset += 4 if self._takes_delay_slot(offset, has_delay) else 2
        return out

    def recompile_instruction(self, opcode, pc):
        """
        Translate a single SH-4 instruction to C code.
        Returns (c_code, is_branch, branch_target, is_delay_slot_insn).
        """
        n = (opcode >> 8) & 0xF
        m = (opcode >> 4) & 0xF
        d = opcode & 0xF
        i = opcode & 0xFF
        d8 = opcode & 0xFF
        d12 = opcode & 0xFFF

        # ---- NOP ----
        if opcode == 0x0009:
            return "/* nop */", False, None, False

        # ---- RTS ----
        if opcode == 0x000B:
            return "/* rts - return after delay slot */", True, "rts", True

        # ---- RTE ----
        if opcode == 0x002B:
            return "/* rte */\n    cpu->sr = cpu->ssr; cpu->pc = cpu->spc;", True, "rte", True

        # ---- CLRT ----
        if opcode == 0x0008:
            return "cpu->sr &= ~SR_T;", False, None, False

        # ---- SETT ----
        if opcode == 0x0018:
            return "cpu->sr |= SR_T;", False, None, False

        # ---- CLRMAC ----
        if opcode == 0x0028:
            return "cpu->mach = 0; cpu->macl = 0;", False, None, False

        # ---- MOV Rm, Rn ----
        if (opcode & 0xF00F) == 0x6003:
            return f"cpu->r[{n}] = cpu->r[{m}];", False, None, False

        # ---- MOV #imm, Rn ----
        if (opcode & 0xF000) == 0xE000:
            imm = sext8(i)
            if imm >= 0:
                return f"cpu->r[{n}] = 0x{imm:X}u;", False, None, False
            else:
                return f"cpu->r[{n}] = 0x{imm & 0xFFFFFFFF:08X}u; /* {imm} */", False, None, False

        # ---- MOV.L @(disp,PC), Rn ----
        if (opcode & 0xF000) == 0xD000:
            disp = d8 * 4
            addr = (pc & 0xFFFFFFFC) + 4 + disp
            # Read the literal from the binary
            off = self.addr_to_offset(addr)
            if 0 <= off < self.size - 3:
                val = self.read_u32(off)
                return f"cpu->r[{n}] = 0x{val:08X}u; /* @0x{addr:08X} */", False, None, False
            return f"cpu->r[{n}] = sh4_read32(cpu, 0x{addr:08X}u); /* PC-rel */", False, None, False

        # ---- MOV.W @(disp,PC), Rn ----
        if (opcode & 0xF000) == 0x9000:
            disp = d8 * 2
            addr = pc + 4 + disp
            off = self.addr_to_offset(addr)
            if 0 <= off < self.size - 1:
                val = struct.unpack_from('<H', self.data, off)[0]
                sval = val if val < 0x8000 else val - 0x10000
                return f"cpu->r[{n}] = (int32_t)(int16_t)0x{val:04X}u; /* {sval} @0x{addr:08X} */", False, None, False
            return f"cpu->r[{n}] = (int32_t)(int16_t)sh4_read16(cpu, 0x{addr:08X}u);", False, None, False

        # ---- MOV.L Rm, @-Rn ---- (0010nnnnmmmm0110)
        if (opcode & 0xF00F) == 0x2006:
            return f"cpu->r[{n}] -= 4; sh4_write32(cpu, cpu->r[{n}], cpu->r[{m}]);", False, None, False

        # ---- MOV.L @Rm, Rn ----
        if (opcode & 0xF00F) == 0x6002:
            return f"cpu->r[{n}] = sh4_read32(cpu, cpu->r[{m}]);", False, None, False

        # ---- MOV.L @Rm+, Rn ----
        if (opcode & 0xF00F) == 0x6006:
            if n == m:
                return f"cpu->r[{n}] = sh4_read32(cpu, cpu->r[{m}]); cpu->r[{n}] += 4;", False, None, False
            return f"cpu->r[{n}] = sh4_read32(cpu, cpu->r[{m}]); cpu->r[{m}] += 4;", False, None, False

        # ---- MOV.L Rm, @(disp,Rn) ----
        if (opcode & 0xF000) == 0x1000:
            disp = d * 4
            return f"sh4_write32(cpu, cpu->r[{n}] + {disp}, cpu->r[{m}]);", False, None, False

        # ---- MOV.L @(disp,Rm), Rn ----
        if (opcode & 0xF000) == 0x5000:
            disp = d * 4
            return f"cpu->r[{n}] = sh4_read32(cpu, cpu->r[{m}] + {disp});", False, None, False

        # ---- MOV.B Rm, @Rn ----
        if (opcode & 0xF00F) == 0x2000:
            return f"sh4_write8(cpu, cpu->r[{n}], (uint8_t)cpu->r[{m}]);", False, None, False

        # ---- MOV.B @Rm, Rn (sign extend) ----
        if (opcode & 0xF00F) == 0x6000:
            return f"cpu->r[{n}] = (int32_t)(int8_t)sh4_read8(cpu, cpu->r[{m}]);", False, None, False

        # ---- MOV.W Rm, @Rn ----
        if (opcode & 0xF00F) == 0x2001:
            return f"sh4_write16(cpu, cpu->r[{n}], (uint16_t)cpu->r[{m}]);", False, None, False

        # ---- MOV.W @Rm, Rn (sign extend) ----
        if (opcode & 0xF00F) == 0x6001:
            return f"cpu->r[{n}] = (int32_t)(int16_t)sh4_read16(cpu, cpu->r[{m}]);", False, None, False

        # ---- MOV.B @Rm+, Rn ----
        if (opcode & 0xF00F) == 0x6004:
            if n == m:
                return f"cpu->r[{n}] = (int32_t)(int8_t)sh4_read8(cpu, cpu->r[{m}]); /* r{n}==r{m} */", False, None, False
            return f"cpu->r[{n}] = (int32_t)(int8_t)sh4_read8(cpu, cpu->r[{m}]); cpu->r[{m}] += 1;", False, None, False

        # ---- MOV.W @Rm+, Rn ----
        if (opcode & 0xF00F) == 0x6005:
            if n == m:
                return f"cpu->r[{n}] = (int32_t)(int16_t)sh4_read16(cpu, cpu->r[{m}]);", False, None, False
            return f"cpu->r[{n}] = (int32_t)(int16_t)sh4_read16(cpu, cpu->r[{m}]); cpu->r[{m}] += 2;", False, None, False

        # ---- MOV.B Rm, @-Rn ----
        if (opcode & 0xF00F) == 0x2004:
            return f"cpu->r[{n}] -= 1; sh4_write8(cpu, cpu->r[{n}], (uint8_t)cpu->r[{m}]);", False, None, False

        # ---- MOV.W Rm, @-Rn ----
        if (opcode & 0xF00F) == 0x2005:
            return f"cpu->r[{n}] -= 2; sh4_write16(cpu, cpu->r[{n}], (uint16_t)cpu->r[{m}]);", False, None, False

        # ---- MOV.B R0, @(disp,Rn) ----
        if (opcode & 0xFF00) == 0x8000:
            disp = opcode & 0xF
            rn = (opcode >> 4) & 0xF
            return f"sh4_write8(cpu, cpu->r[{rn}] + {disp}, (uint8_t)cpu->r[0]);", False, None, False

        # ---- MOV.W R0, @(disp,Rn) ----
        if (opcode & 0xFF00) == 0x8100:
            disp = (opcode & 0xF) * 2
            rn = (opcode >> 4) & 0xF
            return f"sh4_write16(cpu, cpu->r[{rn}] + {disp}, (uint16_t)cpu->r[0]);", False, None, False

        # ---- MOV.B @(disp,Rm), R0 ----
        if (opcode & 0xFF00) == 0x8400:
            disp = opcode & 0xF
            rm = (opcode >> 4) & 0xF
            return f"cpu->r[0] = (int32_t)(int8_t)sh4_read8(cpu, cpu->r[{rm}] + {disp});", False, None, False

        # ---- MOV.W @(disp,Rm), R0 ----
        if (opcode & 0xFF00) == 0x8500:
            disp = (opcode & 0xF) * 2
            rm = (opcode >> 4) & 0xF
            return f"cpu->r[0] = (int32_t)(int16_t)sh4_read16(cpu, cpu->r[{rm}] + {disp});", False, None, False

        # ---- MOV.B/W/L @(R0,Rm), Rn ----
        if (opcode & 0xF00F) == 0x000C:
            return f"cpu->r[{n}] = (int32_t)(int8_t)sh4_read8(cpu, cpu->r[0] + cpu->r[{m}]);", False, None, False
        if (opcode & 0xF00F) == 0x000D:
            return f"cpu->r[{n}] = (int32_t)(int16_t)sh4_read16(cpu, cpu->r[0] + cpu->r[{m}]);", False, None, False
        if (opcode & 0xF00F) == 0x000E:
            return f"cpu->r[{n}] = sh4_read32(cpu, cpu->r[0] + cpu->r[{m}]);", False, None, False

        # ---- MOV.B/W/L Rm, @(R0,Rn) ----
        if (opcode & 0xF00F) == 0x0004:
            return f"sh4_write8(cpu, cpu->r[0] + cpu->r[{n}], (uint8_t)cpu->r[{m}]);", False, None, False
        if (opcode & 0xF00F) == 0x0005:
            return f"sh4_write16(cpu, cpu->r[0] + cpu->r[{n}], (uint16_t)cpu->r[{m}]);", False, None, False
        if (opcode & 0xF00F) == 0x0006:
            return f"sh4_write32(cpu, cpu->r[0] + cpu->r[{n}], cpu->r[{m}]);", False, None, False

        # ---- MOV.L Rm, @Rn ---- (0010nnnnmmmm0010)
        if (opcode & 0xF00F) == 0x2002:
            return f"sh4_write32(cpu, cpu->r[{n}], cpu->r[{m}]);", False, None, False

        # ---- MOV.L @Rm, Rn ---- (0110nnnnmmmm0010) - already handled

        # ---- MOVA @(disp,PC), R0 ----
        if (opcode & 0xFF00) == 0xC700:
            disp = d8 * 4
            addr = (pc & 0xFFFFFFFC) + 4 + disp
            return f"cpu->r[0] = 0x{addr:08X}u; /* mova */", False, None, False

        # ---- MOVT Rn ----
        if (opcode & 0xF0FF) == 0x0029:
            return f"cpu->r[{n}] = (cpu->sr & SR_T) ? 1 : 0;", False, None, False

        # ---- ADD Rm, Rn ----
        if (opcode & 0xF00F) == 0x300C:
            return f"cpu->r[{n}] += cpu->r[{m}];", False, None, False

        # ---- ADD #imm, Rn ----
        if (opcode & 0xF000) == 0x7000:
            imm = sext8(i)
            if imm >= 0:
                return f"cpu->r[{n}] += {imm};", False, None, False
            else:
                return f"cpu->r[{n}] += (int32_t){imm};", False, None, False

        # ---- SUB Rm, Rn ----
        if (opcode & 0xF00F) == 0x3008:
            return f"cpu->r[{n}] -= cpu->r[{m}];", False, None, False

        # ---- CMP/EQ Rm, Rn ----
        if (opcode & 0xF00F) == 0x3000:
            return f"sh4_set_t(cpu, cpu->r[{n}] == cpu->r[{m}]);", False, None, False

        # ---- CMP/EQ #imm, R0 ----
        if (opcode & 0xFF00) == 0x8800:
            imm = sext8(i)
            return f"sh4_set_t(cpu, (int32_t)cpu->r[0] == {imm});", False, None, False

        # ---- CMP/GT Rm, Rn ----
        if (opcode & 0xF00F) == 0x3007:
            return f"sh4_set_t(cpu, (int32_t)cpu->r[{n}] > (int32_t)cpu->r[{m}]);", False, None, False

        # ---- CMP/GE Rm, Rn ----
        if (opcode & 0xF00F) == 0x3003:
            return f"sh4_set_t(cpu, (int32_t)cpu->r[{n}] >= (int32_t)cpu->r[{m}]);", False, None, False

        # ---- CMP/HI Rm, Rn (unsigned >) ----
        if (opcode & 0xF00F) == 0x3006:
            return f"sh4_set_t(cpu, cpu->r[{n}] > cpu->r[{m}]);", False, None, False

        # ---- CMP/HS Rm, Rn (unsigned >=) ----
        if (opcode & 0xF00F) == 0x3002:
            return f"sh4_set_t(cpu, cpu->r[{n}] >= cpu->r[{m}]);", False, None, False

        # ---- CMP/PL Rn (> 0) ----
        if (opcode & 0xF0FF) == 0x4015:
            return f"sh4_set_t(cpu, (int32_t)cpu->r[{n}] > 0);", False, None, False

        # ---- CMP/PZ Rn (>= 0) ----
        if (opcode & 0xF0FF) == 0x4011:
            return f"sh4_set_t(cpu, (int32_t)cpu->r[{n}] >= 0);", False, None, False

        # ---- TST Rm, Rn ----
        if (opcode & 0xF00F) == 0x2008:
            return f"sh4_set_t(cpu, (cpu->r[{n}] & cpu->r[{m}]) == 0);", False, None, False

        # ---- TST #imm, R0 ----
        if (opcode & 0xFF00) == 0xC800:
            return f"sh4_set_t(cpu, (cpu->r[0] & 0x{i:02X}u) == 0);", False, None, False

        # ---- AND Rm, Rn ----
        if (opcode & 0xF00F) == 0x2009:
            return f"cpu->r[{n}] &= cpu->r[{m}];", False, None, False

        # ---- AND #imm, R0 ----
        if (opcode & 0xFF00) == 0xC900:
            return f"cpu->r[0] &= 0x{i:02X}u;", False, None, False

        # ---- OR Rm, Rn ----
        if (opcode & 0xF00F) == 0x200B:
            return f"cpu->r[{n}] |= cpu->r[{m}];", False, None, False

        # ---- OR #imm, R0 ----
        if (opcode & 0xFF00) == 0xCB00:
            return f"cpu->r[0] |= 0x{i:02X}u;", False, None, False

        # ---- XOR Rm, Rn ----
        if (opcode & 0xF00F) == 0x200A:
            return f"cpu->r[{n}] ^= cpu->r[{m}];", False, None, False

        # ---- NOT Rm, Rn ----
        if (opcode & 0xF00F) == 0x6007:
            return f"cpu->r[{n}] = ~cpu->r[{m}];", False, None, False

        # ---- NEG Rm, Rn ----
        if (opcode & 0xF00F) == 0x600B:
            return f"cpu->r[{n}] = (uint32_t)(-(int32_t)cpu->r[{m}]);", False, None, False

        # ---- Shift operations ----
        if (opcode & 0xF0FF) == 0x4000:  # SHLL
            return f"sh4_set_t(cpu, (cpu->r[{n}] >> 31) & 1); cpu->r[{n}] <<= 1;", False, None, False
        if (opcode & 0xF0FF) == 0x4001:  # SHLR
            return f"sh4_set_t(cpu, cpu->r[{n}] & 1); cpu->r[{n}] >>= 1;", False, None, False
        if (opcode & 0xF0FF) == 0x4008:  # SHLL2
            return f"cpu->r[{n}] <<= 2;", False, None, False
        if (opcode & 0xF0FF) == 0x4009:  # SHLR2
            return f"cpu->r[{n}] >>= 2;", False, None, False
        if (opcode & 0xF0FF) == 0x4018:  # SHLL8
            return f"cpu->r[{n}] <<= 8;", False, None, False
        if (opcode & 0xF0FF) == 0x4019:  # SHLR8
            return f"cpu->r[{n}] >>= 8;", False, None, False
        if (opcode & 0xF0FF) == 0x4028:  # SHLL16
            return f"cpu->r[{n}] <<= 16;", False, None, False
        if (opcode & 0xF0FF) == 0x4029:  # SHLR16
            return f"cpu->r[{n}] >>= 16;", False, None, False
        if (opcode & 0xF0FF) == 0x4020:  # SHAL
            return f"sh4_set_t(cpu, (cpu->r[{n}] >> 31) & 1); cpu->r[{n}] <<= 1;", False, None, False
        if (opcode & 0xF0FF) == 0x4021:  # SHAR
            return f"sh4_set_t(cpu, cpu->r[{n}] & 1); cpu->r[{n}] = (uint32_t)((int32_t)cpu->r[{n}] >> 1);", False, None, False

        # ---- SHAD Rm, Rn ----
        if (opcode & 0xF00F) == 0x400C:
            return (f"{{\n"
                    f"        int32_t shift = (int32_t)cpu->r[{m}];\n"
                    f"        if (shift >= 0) cpu->r[{n}] <<= (shift & 31);\n"
                    f"        else if (shift > -32) cpu->r[{n}] = (uint32_t)((int32_t)cpu->r[{n}] >> (-shift));\n"
                    f"        else cpu->r[{n}] = ((int32_t)cpu->r[{n}] < 0) ? 0xFFFFFFFFu : 0;\n"
                    f"    }}"), False, None, False

        # ---- SHLD Rm, Rn ----
        if (opcode & 0xF00F) == 0x400D:
            return (f"{{\n"
                    f"        int32_t shift = (int32_t)cpu->r[{m}];\n"
                    f"        if (shift >= 0) cpu->r[{n}] <<= (shift & 31);\n"
                    f"        else if (shift > -32) cpu->r[{n}] >>= (-shift);\n"
                    f"        else cpu->r[{n}] = 0;\n"
                    f"    }}"), False, None, False

        # ---- EXTS/EXTU ----
        if (opcode & 0xF00F) == 0x600E:  # EXTS.B
            return f"cpu->r[{n}] = (uint32_t)(int32_t)(int8_t)(uint8_t)cpu->r[{m}];", False, None, False
        if (opcode & 0xF00F) == 0x600F:  # EXTS.W
            return f"cpu->r[{n}] = (uint32_t)(int32_t)(int16_t)(uint16_t)cpu->r[{m}];", False, None, False
        if (opcode & 0xF00F) == 0x600C:  # EXTU.B
            return f"cpu->r[{n}] = cpu->r[{m}] & 0xFF;", False, None, False
        if (opcode & 0xF00F) == 0x600D:  # EXTU.W
            return f"cpu->r[{n}] = cpu->r[{m}] & 0xFFFF;", False, None, False

        # ---- SWAP ----
        if (opcode & 0xF00F) == 0x6008:  # SWAP.B
            return f"cpu->r[{n}] = (cpu->r[{m}] & 0xFFFF0000u) | ((cpu->r[{m}] & 0xFF) << 8) | ((cpu->r[{m}] >> 8) & 0xFF);", False, None, False
        if (opcode & 0xF00F) == 0x6009:  # SWAP.W
            return f"cpu->r[{n}] = (cpu->r[{m}] << 16) | (cpu->r[{m}] >> 16);", False, None, False

        # ---- Multiply ----
        if (opcode & 0xF00F) == 0x200E:  # MULU.W
            return f"cpu->macl = (uint32_t)(uint16_t)cpu->r[{n}] * (uint32_t)(uint16_t)cpu->r[{m}];", False, None, False
        if (opcode & 0xF00F) == 0x200F:  # MULS.W
            return f"cpu->macl = (uint32_t)((int32_t)(int16_t)cpu->r[{n}] * (int32_t)(int16_t)cpu->r[{m}]);", False, None, False
        if (opcode & 0xF00F) == 0x0007:  # MUL.L
            return f"cpu->macl = cpu->r[{n}] * cpu->r[{m}];", False, None, False
        if (opcode & 0xF00F) == 0x3005:  # DMULU.L
            return (f"{{ uint64_t result = (uint64_t)cpu->r[{n}] * (uint64_t)cpu->r[{m}];\n"
                    f"    cpu->macl = (uint32_t)result; cpu->mach = (uint32_t)(result >> 32); }}")  , False, None, False
        if (opcode & 0xF00F) == 0x300D:  # DMULS.L
            return (f"{{ int64_t result = (int64_t)(int32_t)cpu->r[{n}] * (int64_t)(int32_t)cpu->r[{m}];\n"
                    f"    cpu->macl = (uint32_t)result; cpu->mach = (uint32_t)(result >> 32); }}"), False, None, False

        # ---- STS/LDS (system registers) ----
        if (opcode & 0xF0FF) == 0x001A:  # STS MACL, Rn
            return f"cpu->r[{n}] = cpu->macl;", False, None, False
        if (opcode & 0xF0FF) == 0x000A:  # STS MACH, Rn
            return f"cpu->r[{n}] = cpu->mach;", False, None, False
        if (opcode & 0xF0FF) == 0x002A:  # STS PR, Rn
            return f"cpu->r[{n}] = cpu->pr;", False, None, False
        if (opcode & 0xF0FF) == 0x005A:  # STS FPUL, Rn
            return f"cpu->r[{n}] = cpu->fpul;", False, None, False
        if (opcode & 0xF0FF) == 0x006A:  # STS FPSCR, Rn
            return f"cpu->r[{n}] = cpu->fpscr;", False, None, False

        if (opcode & 0xF0FF) == 0x402A:  # LDS Rm, PR
            return f"cpu->pr = cpu->r[{n}];", False, None, False
        if (opcode & 0xF0FF) == 0x401A:  # LDS Rm, MACL
            return f"cpu->macl = cpu->r[{n}];", False, None, False
        if (opcode & 0xF0FF) == 0x400A:  # LDS Rm, MACH
            return f"cpu->mach = cpu->r[{n}];", False, None, False
        if (opcode & 0xF0FF) == 0x405A:  # LDS Rm, FPUL
            return f"cpu->fpul = cpu->r[{n}];", False, None, False
        if (opcode & 0xF0FF) == 0x406A:  # LDS Rm, FPSCR
            return f"cpu->fpscr = cpu->r[{n}];", False, None, False

        # ---- STS.L/LDS.L (push/pop system registers) ----
        if (opcode & 0xF0FF) == 0x4022:  # STS.L PR, @-Rn
            return f"cpu->r[{n}] -= 4; sh4_write32(cpu, cpu->r[{n}], cpu->pr);", False, None, False
        if (opcode & 0xF0FF) == 0x4026:  # LDS.L @Rm+, PR
            return f"cpu->pr = sh4_read32(cpu, cpu->r[{n}]); cpu->r[{n}] += 4;", False, None, False
        if (opcode & 0xF0FF) == 0x4012:  # STS.L MACL, @-Rn
            return f"cpu->r[{n}] -= 4; sh4_write32(cpu, cpu->r[{n}], cpu->macl);", False, None, False
        if (opcode & 0xF0FF) == 0x4016:  # LDS.L @Rm+, MACL
            return f"cpu->macl = sh4_read32(cpu, cpu->r[{n}]); cpu->r[{n}] += 4;", False, None, False
        if (opcode & 0xF0FF) == 0x4002:  # STS.L MACH, @-Rn
            return f"cpu->r[{n}] -= 4; sh4_write32(cpu, cpu->r[{n}], cpu->mach);", False, None, False
        if (opcode & 0xF0FF) == 0x4006:  # LDS.L @Rm+, MACH
            return f"cpu->mach = sh4_read32(cpu, cpu->r[{n}]); cpu->r[{n}] += 4;", False, None, False
        if (opcode & 0xF0FF) == 0x4052:  # STS.L FPUL, @-Rn
            return f"cpu->r[{n}] -= 4; sh4_write32(cpu, cpu->r[{n}], cpu->fpul);", False, None, False
        if (opcode & 0xF0FF) == 0x4056:  # LDS.L @Rm+, FPUL
            return f"cpu->fpul = sh4_read32(cpu, cpu->r[{n}]); cpu->r[{n}] += 4;", False, None, False
        if (opcode & 0xF0FF) == 0x4062:  # STS.L FPSCR, @-Rn
            return f"cpu->r[{n}] -= 4; sh4_write32(cpu, cpu->r[{n}], cpu->fpscr);", False, None, False
        if (opcode & 0xF0FF) == 0x4066:  # LDS.L @Rm+, FPSCR
            return f"cpu->fpscr = sh4_read32(cpu, cpu->r[{n}]); cpu->r[{n}] += 4;", False, None, False

        # ---- STC/LDC (control registers) ----
        if (opcode & 0xF0FF) == 0x0002:  # STC SR, Rn
            return f"cpu->r[{n}] = cpu->sr;", False, None, False
        if (opcode & 0xF0FF) == 0x0012:  # STC GBR, Rn
            return f"cpu->r[{n}] = cpu->gbr;", False, None, False
        if (opcode & 0xF0FF) == 0x0022:  # STC VBR, Rn
            return f"cpu->r[{n}] = cpu->vbr;", False, None, False
        if (opcode & 0xF0FF) == 0x400E:  # LDC Rm, SR
            return f"cpu->sr = cpu->r[{n}];", False, None, False
        if (opcode & 0xF0FF) == 0x401E:  # LDC Rm, GBR
            return f"cpu->gbr = cpu->r[{n}];", False, None, False
        if (opcode & 0xF0FF) == 0x402E:  # LDC Rm, VBR
            return f"cpu->vbr = cpu->r[{n}];", False, None, False

        # ---- DT Rn (decrement and test) ----
        if (opcode & 0xF0FF) == 0x4010:
            return f"cpu->r[{n}]--; sh4_set_t(cpu, cpu->r[{n}] == 0);", False, None, False

        # ---- Branch instructions ----

        # BRA
        if (opcode & 0xF000) == 0xA000:
            disp = sext12(d12) * 2
            target = pc + 4 + disp
            return f"/* bra 0x{target:08X} */", True, target, True

        # BSR
        if (opcode & 0xF000) == 0xB000:
            disp = sext12(d12) * 2
            target = pc + 4 + disp
            return f"cpu->pr = 0x{pc + 4:08X}u; /* bsr 0x{target:08X} */", True, target, True

        # BT
        if (opcode & 0xFF00) == 0x8900:
            disp = sext8(d8) * 2
            target = pc + 4 + disp
            return f"/* bt 0x{target:08X} */", True, target, False

        # BF
        if (opcode & 0xFF00) == 0x8B00:
            disp = sext8(d8) * 2
            target = pc + 4 + disp
            return f"/* bf 0x{target:08X} */", True, target, False

        # BT/S
        if (opcode & 0xFF00) == 0x8D00:
            disp = sext8(d8) * 2
            target = pc + 4 + disp
            return f"/* bt/s 0x{target:08X} */", True, target, True

        # BF/S
        if (opcode & 0xFF00) == 0x8F00:
            disp = sext8(d8) * 2
            target = pc + 4 + disp
            return f"/* bf/s 0x{target:08X} */", True, target, True

        # JMP @Rm
        if (opcode & 0xF0FF) == 0x402B:
            return f"cpu->pc = cpu->r[{n}]; /* jmp @r{n} */", True, "indirect", True

        # JSR @Rm
        if (opcode & 0xF0FF) == 0x400B:
            return f"cpu->pr = 0x{pc + 4:08X}u; cpu->pc = cpu->r[{n}]; /* jsr @r{n} */", True, "indirect", True

        # BRAF Rm
        if (opcode & 0xF0FF) == 0x0023:
            return f"cpu->pc = 0x{pc + 4:08X}u + cpu->r[{n}]; /* braf r{n} */", True, "indirect", True

        # BSRF Rm
        if (opcode & 0xF0FF) == 0x0003:
            return f"cpu->pr = 0x{pc + 4:08X}u; cpu->pc = 0x{pc + 4:08X}u + cpu->r[{n}]; /* bsrf r{n} */", True, "indirect", True

        # ---- Floating point ----

        if (opcode & 0xF00F) == 0xF00C:  # FMOV
            return f"cpu->fr[{n}] = cpu->fr[{m}];", False, None, False
        if (opcode & 0xF00F) == 0xF008:  # FMOV.S @Rm, FRn
            return f"cpu->fr[{n}] = sh4_read_float(cpu, cpu->r[{m}]);", False, None, False
        if (opcode & 0xF00F) == 0xF00A:  # FMOV.S FRm, @Rn
            return f"sh4_write_float(cpu, cpu->r[{n}], cpu->fr[{m}]);", False, None, False
        if (opcode & 0xF00F) == 0xF009:  # FMOV.S @Rm+, FRn
            return f"cpu->fr[{n}] = sh4_read_float(cpu, cpu->r[{m}]); cpu->r[{m}] += 4;", False, None, False
        if (opcode & 0xF00F) == 0xF00B:  # FMOV.S FRm, @-Rn
            return f"cpu->r[{n}] -= 4; sh4_write_float(cpu, cpu->r[{n}], cpu->fr[{m}]);", False, None, False
        if (opcode & 0xF00F) == 0xF006:  # FMOV.S @(R0,Rm), FRn
            return f"cpu->fr[{n}] = sh4_read_float(cpu, cpu->r[0] + cpu->r[{m}]);", False, None, False
        if (opcode & 0xF00F) == 0xF007:  # FMOV.S FRm, @(R0,Rn)
            return f"sh4_write_float(cpu, cpu->r[0] + cpu->r[{n}], cpu->fr[{m}]);", False, None, False

        if (opcode & 0xF00F) == 0xF000:  # FADD
            return f"cpu->fr[{n}] += cpu->fr[{m}];", False, None, False
        if (opcode & 0xF00F) == 0xF001:  # FSUB
            return f"cpu->fr[{n}] -= cpu->fr[{m}];", False, None, False
        if (opcode & 0xF00F) == 0xF002:  # FMUL
            return f"cpu->fr[{n}] *= cpu->fr[{m}];", False, None, False
        if (opcode & 0xF00F) == 0xF003:  # FDIV
            return f"cpu->fr[{n}] /= cpu->fr[{m}];", False, None, False
        if (opcode & 0xF00F) == 0xF004:  # FCMP/EQ
            return f"sh4_set_t(cpu, cpu->fr[{n}] == cpu->fr[{m}]);", False, None, False
        if (opcode & 0xF00F) == 0xF005:  # FCMP/GT
            return f"sh4_set_t(cpu, cpu->fr[{n}] > cpu->fr[{m}]);", False, None, False

        if (opcode & 0xF0FF) == 0xF02D:  # FLOAT FPUL, FRn
            return (f"{{ union {{ uint32_t u; float f; }} conv; conv.u = cpu->fpul;\n"
                    f"    cpu->fr[{n}] = (float)(int32_t)cpu->fpul; }}"), False, None, False
        if (opcode & 0xF0FF) == 0xF03D:  # FTRC FRm, FPUL
            return f"cpu->fpul = (uint32_t)(int32_t)cpu->fr[{n}]; /* ftrc */", False, None, False
        if (opcode & 0xF0FF) == 0xF04D:  # FNEG
            return f"cpu->fr[{n}] = -cpu->fr[{n}];", False, None, False
        if (opcode & 0xF0FF) == 0xF05D:  # FABS
            return f"cpu->fr[{n}] = fabsf(cpu->fr[{n}]);", False, None, False
        if (opcode & 0xF0FF) == 0xF06D:  # FSQRT
            return f"cpu->fr[{n}] = sqrtf(cpu->fr[{n}]);", False, None, False
        if (opcode & 0xF0FF) == 0xF01D:  # FLDS FRm, FPUL
            return f"{{ union {{ float f; uint32_t u; }} c; c.f = cpu->fr[{n}]; cpu->fpul = c.u; }}", False, None, False
        if (opcode & 0xF0FF) == 0xF00D:  # FSTS FPUL, FRn
            return f"{{ union {{ uint32_t u; float f; }} c; c.u = cpu->fpul; cpu->fr[{n}] = c.f; }}", False, None, False

        # FIPR (dot product)
        if (opcode & 0xF0FF) == 0xF0ED:
            vn = (n >> 2) & 3
            vm = n & 3
            vn_base = vn * 4
            vm_base = vm * 4
            return (f"cpu->fr[{vn_base + 3}] = "
                    f"cpu->fr[{vm_base}]*cpu->fr[{vn_base}] + "
                    f"cpu->fr[{vm_base+1}]*cpu->fr[{vn_base+1}] + "
                    f"cpu->fr[{vm_base+2}]*cpu->fr[{vn_base+2}] + "
                    f"cpu->fr[{vm_base+3}]*cpu->fr[{vn_base+3}]; /* fipr */"), False, None, False

        # FMAC FR0, FRm, FRn
        if (opcode & 0xF00F) == 0xF00E:
            return f"cpu->fr[{n}] += cpu->fr[0] * cpu->fr[{m}];", False, None, False

        # FRCHG
        if opcode == 0xFBFD:
            return "cpu->fpscr ^= FPSCR_FR; /* frchg */", False, None, False

        # FSCHG
        if opcode == 0xF3FD:
            return "cpu->fpscr ^= FPSCR_SZ; /* fschg */", False, None, False

        # ---- ADDC/SUBC ----
        if (opcode & 0xF00F) == 0x300E:  # ADDC
            return (f"{{ uint32_t tmp = cpu->r[{n}]; uint32_t t = sh4_get_t(cpu) ? 1 : 0;\n"
                    f"    cpu->r[{n}] += cpu->r[{m}] + t;\n"
                    f"    sh4_set_t(cpu, (cpu->r[{n}] < tmp) || (t && cpu->r[{n}] == tmp)); }}"), False, None, False
        if (opcode & 0xF00F) == 0x300A:  # SUBC
            return (f"{{ uint32_t tmp = cpu->r[{n}]; uint32_t t = sh4_get_t(cpu) ? 1 : 0;\n"
                    f"    cpu->r[{n}] -= cpu->r[{m}] + t;\n"
                    f"    sh4_set_t(cpu, (tmp < cpu->r[{n}]) || (t && tmp == cpu->r[{n}])); }}"), False, None, False

        # ---- ROTL/ROTR ----
        if (opcode & 0xF0FF) == 0x4004:  # ROTL
            return f"sh4_set_t(cpu, (cpu->r[{n}] >> 31) & 1); cpu->r[{n}] = (cpu->r[{n}] << 1) | ((cpu->r[{n}] >> 31) & 1);", False, None, False
        if (opcode & 0xF0FF) == 0x4005:  # ROTR
            return f"sh4_set_t(cpu, cpu->r[{n}] & 1); cpu->r[{n}] = (cpu->r[{n}] >> 1) | ((cpu->r[{n}] & 1) << 31);", False, None, False
        if (opcode & 0xF0FF) == 0x4024:  # ROTCL
            return (f"{{ uint32_t old_t = sh4_get_t(cpu) ? 1 : 0;\n"
                    f"    sh4_set_t(cpu, (cpu->r[{n}] >> 31) & 1);\n"
                    f"    cpu->r[{n}] = (cpu->r[{n}] << 1) | old_t; }}"), False, None, False
        if (opcode & 0xF0FF) == 0x4025:  # ROTCR
            return (f"{{ uint32_t old_t = sh4_get_t(cpu) ? 1 : 0;\n"
                    f"    sh4_set_t(cpu, cpu->r[{n}] & 1);\n"
                    f"    cpu->r[{n}] = (cpu->r[{n}] >> 1) | (old_t << 31); }}"), False, None, False

        # ---- PREF/OCBI/OCBP/OCBWB (cache ops - mostly NOPs in recompilation) ----
        if (opcode & 0xF0FF) == 0x0083:  # PREF
            return f"sh4_sq_prefetch(cpu, cpu->r[{n}]);", False, None, False
        if (opcode & 0xF0FF) == 0x0093:  # OCBI
            return f"/* ocbi @r{n} (nop) */", False, None, False
        if (opcode & 0xF0FF) == 0x00A3:  # OCBP
            return f"/* ocbp @r{n} (nop) */", False, None, False
        if (opcode & 0xF0FF) == 0x00B3:  # OCBWB
            return f"/* ocbwb @r{n} (nop) */", False, None, False

        # ---- XTRCT ----
        if (opcode & 0xF00F) == 0x200D:
            return f"cpu->r[{n}] = (cpu->r[{n}] >> 16) | (cpu->r[{m}] << 16);", False, None, False

        # ---- NEGC ----
        if (opcode & 0xF00F) == 0x600A:
            return (f"{{ uint32_t t = sh4_get_t(cpu) ? 1 : 0;\n"
                    f"    cpu->r[{n}] = (uint32_t)(-(int32_t)cpu->r[{m}]) - t;\n"
                    f"    sh4_set_t(cpu, cpu->r[{m}] != 0 || t != 0); }}"), False, None, False

        # ---- CMP/STR ----
        if (opcode & 0xF00F) == 0x200C:
            return (f"{{ uint32_t tmp = cpu->r[{n}] ^ cpu->r[{m}];\n"
                    f"    sh4_set_t(cpu, !(tmp & 0xFF) || !((tmp>>8)&0xFF) || !((tmp>>16)&0xFF) || !((tmp>>24)&0xFF)); }}"), False, None, False

        # ---- MAC.L @Rm+, @Rn+ ----
        if (opcode & 0xF00F) == 0x000F:
            return (f"{{ int64_t mac = ((int64_t)cpu->mach << 32) | cpu->macl;\n"
                    f"    int32_t a = (int32_t)sh4_read32(cpu, cpu->r[{n}]);\n"
                    f"    int32_t b = (int32_t)sh4_read32(cpu, cpu->r[{m}]);\n"
                    f"    mac += (int64_t)a * (int64_t)b;\n"
                    f"    cpu->mach = (uint32_t)(mac >> 32); cpu->macl = (uint32_t)mac;\n"
                    f"    cpu->r[{n}] += 4; cpu->r[{m}] += 4; }}"), False, None, False

        # ---- TRAPA ----
        if (opcode & 0xFF00) == 0xC300:
            return f"/* trapa #0x{i:02X} - system call */", True, "trap", False

        # ---- GBR-relative MOV ----
        if (opcode & 0xFF00) == 0xC000:
            return f"sh4_write8(cpu, cpu->gbr + {d8}, (uint8_t)cpu->r[0]);", False, None, False
        if (opcode & 0xFF00) == 0xC100:
            return f"sh4_write16(cpu, cpu->gbr + {d8*2}, (uint16_t)cpu->r[0]);", False, None, False
        if (opcode & 0xFF00) == 0xC200:
            return f"sh4_write32(cpu, cpu->gbr + {d8*4}, cpu->r[0]);", False, None, False
        if (opcode & 0xFF00) == 0xC400:
            return f"cpu->r[0] = (int32_t)(int8_t)sh4_read8(cpu, cpu->gbr + {d8});", False, None, False
        if (opcode & 0xFF00) == 0xC500:
            return f"cpu->r[0] = (int32_t)(int16_t)sh4_read16(cpu, cpu->gbr + {d8*2});", False, None, False
        if (opcode & 0xFF00) == 0xC600:
            return f"cpu->r[0] = sh4_read32(cpu, cpu->gbr + {d8*4});", False, None, False

        # ---- ADDV/SUBV ----
        if (opcode & 0xF00F) == 0x300F:  # ADDV
            return (f"{{ int32_t a = (int32_t)cpu->r[{n}], b = (int32_t)cpu->r[{m}];\n"
                    f"    int32_t result = a + b;\n"
                    f"    sh4_set_t(cpu, ((a ^ result) & (b ^ result)) < 0);\n"
                    f"    cpu->r[{n}] = (uint32_t)result; }}"), False, None, False
        if (opcode & 0xF00F) == 0x300B:  # SUBV
            return (f"{{ int32_t a = (int32_t)cpu->r[{n}], b = (int32_t)cpu->r[{m}];\n"
                    f"    int32_t result = a - b;\n"
                    f"    sh4_set_t(cpu, ((a ^ b) & (a ^ result)) < 0);\n"
                    f"    cpu->r[{n}] = (uint32_t)result; }}"), False, None, False

        # ---- MAC.W ----
        if (opcode & 0xF00F) == 0x400F:
            return (f"{{ int32_t a = (int32_t)(int16_t)sh4_read16(cpu, cpu->r[{n}]);\n"
                    f"    int32_t b = (int32_t)(int16_t)sh4_read16(cpu, cpu->r[{m}]);\n"
                    f"    int64_t mac = ((int64_t)cpu->mach << 32) | cpu->macl;\n"
                    f"    mac += (int64_t)a * (int64_t)b;\n"
                    f"    cpu->mach = (uint32_t)(mac >> 32); cpu->macl = (uint32_t)mac;\n"
                    f"    cpu->r[{n}] += 2; cpu->r[{m}] += 2; }}"), False, None, False

        # ---- XOR #imm, R0 ----
        if (opcode & 0xFF00) == 0xCA00:
            return f"cpu->r[0] ^= 0x{i:02X}u;", False, None, False

        # ---- Default: unknown instruction ----
        return f"/* UNKNOWN: 0x{opcode:04X} at 0x{pc:08X} */", False, None, False

    def recompile_function(self, func_addr, func_info, next_func_addr=None):
        """Recompile a single function to C code."""
        func_start = self.addr_to_offset(func_addr)
        func_end = self.addr_to_offset(func_info['end'])

        if func_start < 0 or func_end > self.size:
            return None

        # First pass: collect branch targets and emittable addresses
        branch_targets = set()
        emittable_addrs = set()
        offset = func_start
        while offset < func_end - 1:
            pc = self.offset_to_addr(offset)
            emittable_addrs.add(pc)
            opcode = self.read_u16(offset)
            _, is_branch, target, has_delay = self.recompile_instruction(opcode, pc)
            if is_branch and isinstance(target, int):
                if func_addr <= target < func_info['end']:
                    branch_targets.add(target)
            if self._takes_delay_slot(offset, has_delay):
                # Delay slot instruction is also emittable
                if offset + 2 < func_end:
                    emittable_addrs.add(self.offset_to_addr(offset + 2))
                offset += 4
            else:
                offset += 2

        # Only label addresses that are both branch targets AND emittable
        local_labels = branch_targets & emittable_addrs

        # Generate function name
        func_name = f"func_{func_addr:08X}"

        # Always include function's own start address as a local label
        # (needed for self-tail-call → goto conversion to avoid stack overflow)
        local_labels.add(func_addr)

        # Mid-function entry points (see _resolve_mid_entries). Rather than
        # splitting the function - which would turn backward branches across the
        # split into calls that re-enter from the top - the body is emitted once
        # and each entry gets a wrapper that jumps straight to its label.
        mids = [a for a in getattr(self, 'mid_entries', {}).get(func_addr, ())
                if a in emittable_addrs and a != func_addr]
        local_labels |= set(mids)

        # Second pass: generate C code
        lines = []
        if mids:
            lines.append(f"static void impl_{func_addr:08X}(SH4CPU *cpu, uint32_t entry) {{")
            lines.append("    switch (entry) {")
            for a in mids:
                lines.append(f"    case 0x{a:08X}u: goto L_{a:08X};")
            lines.append("    default: break;")
            lines.append("    }")
        else:
            lines.append(f"void {func_name}(SH4CPU *cpu) {{")

        # Track which labels we've actually emitted
        emitted_labels = set()

        offset = func_start
        while offset < func_end - 1:
            opcode = self.read_u16(offset)
            pc = self.offset_to_addr(offset)

            # Insert label if this is a branch target
            if pc in local_labels:
                lines.append(f"L_{pc:08X}:;")
                emitted_labels.add(pc)

            code, is_branch, target, has_delay = self.recompile_instruction(opcode, pc)

            # A target is "local" only if it has a label AND we can emit it
            is_local = isinstance(target, int) and target in local_labels
            is_external_func = isinstance(target, int) and not is_local

            # Handle non-delay-slot branches first (BT, BF without /S)
            if is_branch and not has_delay and isinstance(target, int):
                if is_local:
                    if "bt" in code:
                        lines.append(f"    if (cpu->sr & SR_T) goto L_{target:08X}; /* bt */")
                    elif "bf" in code:
                        lines.append(f"    if (!(cpu->sr & SR_T)) goto L_{target:08X}; /* bf */")
                else:
                    # Branch target outside function - treat as conditional tail call
                    if "bt" in code:
                        lines.append(f"    if (cpu->sr & SR_T) {{ func_{target:08X}(cpu); return; }} /* bt tail */")
                    elif "bf" in code:
                        lines.append(f"    if (!(cpu->sr & SR_T)) {{ func_{target:08X}(cpu); return; }} /* bf tail */")
                offset += 2
                continue

            # An rts whose delay slot is the first instruction of the next
            # function still has to execute it and return. Without this the rts
            # degrades to a comment and control falls through into that
            # function on someone else's stack frame.
            if (target == "rts" and has_delay
                    and offset + 2 >= func_end and offset + 2 < self.size):
                delay_pc = self.offset_to_addr(offset + 2)
                delay_code, _, _, _ = self.recompile_instruction(
                    self.read_u16(offset + 2), delay_pc)
                lines.append(f"    {delay_code} /* delay slot, across boundary */")
                lines.append(f"    return; /* rts */")
                offset += 4
                continue

            # Handle delay slots for branch instructions
            if has_delay and offset + 2 < func_end:
                delay_opcode = self.read_u16(offset + 2)
                delay_pc = self.offset_to_addr(offset + 2)
                delay_code, _, _, _ = self.recompile_instruction(delay_opcode, delay_pc)

                # The slot can also be somewhere the program jumps to directly.
                # It is still this branch's delay slot - it just cannot be
                # *consumed* here, or the entry point loses its label and the
                # code it starts is never emitted. Inline a copy of the slot to
                # finish the branch, then let the walk re-emit it under its
                # label. Skipping the branch instead drops the `return` off an
                # rts and falls through into the next state on the caller's
                # frame, which is how a scene that should run once loops.
                slot_is_entry = not self._takes_delay_slot(offset, has_delay)
                if slot_is_entry and not (target == "rts" or "jmp" in code
                                          or "braf" in code or "rte" in code
                                          or ("bra" in code and "bsr" not in code)):
                    # Control comes back here - bsr, bt/s, bf/s. An inline copy
                    # plus the labelled one would run the slot twice, which is
                    # worse than the branch this would have fixed. Leave those
                    # alone until they are worth a jump around the label.
                    lines.append(f"    {code}")
                    offset += 2
                    continue

                # Emit label at delay-slot position if needed
                if delay_pc in local_labels and not slot_is_entry:
                    lines.append(f"L_{delay_pc:08X}:;")
                    emitted_labels.add(delay_pc)

                if target == "rts":
                    lines.append(f"    {delay_code} /* delay slot */")
                    lines.append(f"    return; /* rts */")
                elif isinstance(target, int) and "bsr" in code:
                    # BSR - a real call, so control returns here afterwards.
                    # Classify by the instruction, never by whether the target
                    # happens to be BSR'd from somewhere else in the binary: a
                    # BRA to a shared helper is a tail jump, and emitting it as
                    # a call makes execution fall through into whatever the
                    # assembler happened to place next.
                    lines.append(f"    {delay_code} /* delay slot */")
                    lines.append(f"    func_{target:08X}(cpu); /* bsr */")
                elif is_local:
                    # Local branch (BRA, BT/S, BF/S within function)
                    if "bt/s" in code:
                        lines.append(f"    {{ int t = cpu->sr & SR_T;")
                        lines.append(f"    {delay_code} /* delay slot */")
                        lines.append(f"    if (t) goto L_{target:08X}; }}")
                    elif "bf/s" in code:
                        lines.append(f"    {{ int t = cpu->sr & SR_T;")
                        lines.append(f"    {delay_code} /* delay slot */")
                        lines.append(f"    if (!t) goto L_{target:08X}; }}")
                    else:
                        # BRA local
                        lines.append(f"    {delay_code} /* delay slot */")
                        lines.append(f"    goto L_{target:08X};")
                elif is_external_func:
                    # External branch/call
                    if "bt/s" in code:
                        lines.append(f"    {{ int t = cpu->sr & SR_T;")
                        lines.append(f"    {delay_code} /* delay slot */")
                        lines.append(f"    if (t) {{ func_{target:08X}(cpu); return; }} }} /* bt/s tail */")
                    elif "bf/s" in code:
                        lines.append(f"    {{ int t = cpu->sr & SR_T;")
                        lines.append(f"    {delay_code} /* delay slot */")
                        lines.append(f"    if (!t) {{ func_{target:08X}(cpu); return; }} }} /* bf/s tail */")
                    elif "bsr" in code or "bra" not in code:
                        # BSR external
                        lines.append(f"    {delay_code} /* delay slot */")
                        lines.append(f"    func_{target:08X}(cpu);")
                    else:
                        # BRA external (tail call)
                        lines.append(f"    {delay_code} /* delay slot */")
                        lines.append(f"    func_{target:08X}(cpu); return; /* bra tail */")
                elif target == "indirect":
                    # JSR/JMP @Rn
                    if "jsr" in code or "bsrf" in code:
                        lines.append(f"    {code}")
                        lines.append(f"    {delay_code} /* delay slot */")
                        lines.append(f"    sh4_call_indirect(cpu);")
                    elif "jmp" in code or "braf" in code:
                        lines.append(f"    {delay_code} /* delay slot */")
                        lines.append(f"    {code}")
                        lines.append(f"    sh4_jump_indirect(cpu);")
                        lines.append(f"    return;")
                    else:
                        lines.append(f"    {code}")
                        lines.append(f"    {delay_code} /* delay slot */")
                else:
                    lines.append(f"    {code}")
                    lines.append(f"    {delay_code} /* delay slot */")

                offset += 2 if slot_is_entry else 4
                continue

            lines.append(f"    {code}")
            offset += 2

        # Check if function needs fall-through to next function
        # Find last non-empty code line
        if next_func_addr is not None:
            last_code = ""
            for l in reversed(lines):
                stripped = l.strip()
                if stripped and not stripped.startswith("L_") and stripped != "{":
                    last_code = stripped
                    break
            # If last line is not a terminator, add fall-through call
            if (last_code and
                not last_code.endswith("return;") and
                not last_code.endswith("return; }") and
                not last_code.startswith("goto ") and
                not last_code.startswith("return;")):
                lines.append(f"    func_{next_func_addr:08X}(cpu); /* fall-through */")

        lines.append("}")

        if mids:
            lines.append("")
            lines.append(f"void {func_name}(SH4CPU *cpu) {{ impl_{func_addr:08X}(cpu, 0x{func_addr:08X}u); }}")
            for a in mids:
                lines.append(f"void func_{a:08X}(SH4CPU *cpu) {{ impl_{func_addr:08X}(cpu, 0x{a:08X}u); }}")

        # Post-process: replace any goto to non-existent labels with function calls
        import re
        result_lines = []
        for line in lines:
            m = re.search(r'goto L_([0-9A-Fa-f]{8})', line)
            if m:
                label_addr = int(m.group(1), 16)
                if label_addr not in emitted_labels:
                    # Replace goto with tail call
                    line = re.sub(
                        r'goto L_([0-9A-Fa-f]{8});',
                        lambda match: f'{{ func_{match.group(1)}(cpu); return; }}',
                        line
                    )
            result_lines.append(line)

        return '\n'.join(result_lines)

    def generate_header(self, output_path):
        """Generate the function declarations header."""
        with open(output_path, 'w') as f:
            f.write("/* Auto-generated SH-4 recompiled function declarations */\n")
            f.write("#ifndef GAME_FUNCTIONS_H\n")
            f.write("#define GAME_FUNCTIONS_H\n\n")
            f.write("#include \"recompiler/sh4_cpu.h\"\n\n")
            f.write("/* Indirect call/jump dispatch */\n")
            f.write("void sh4_call_indirect(SH4CPU *cpu);\n")
            f.write("void sh4_jump_indirect(SH4CPU *cpu);\n\n")
            f.write(f"/* {len(self.functions)} recompiled functions */\n")

            for addr in sorted(set(self.functions) | getattr(self, "mid_entry_addrs", set())):
                f.write(f"void func_{addr:08X}(SH4CPU *cpu);\n")

            f.write("\n#endif /* GAME_FUNCTIONS_H */\n")

    def generate_dispatch_table(self, output_path):
        """Generate the address-to-function dispatch table."""
        with open(output_path, 'w') as f:
            f.write("/* Auto-generated function dispatch table */\n")
            f.write("#include \"game/game_functions.h\"\n")
            f.write("#include \"hal/dc_bios.h\"\n")
            f.write("#include <stddef.h>\n\n")

            f.write("typedef struct {\n")
            f.write("    uint32_t addr;\n")
            f.write("    void (*func)(SH4CPU *cpu);\n")
            f.write("} FuncEntry;\n\n")

            f.write(f"static const FuncEntry func_table[] = {{\n")
            for addr in sorted(set(self.functions) | getattr(self, "mid_entry_addrs", set())):
                f.write(f"    {{ 0x{addr:08X}u, func_{addr:08X} }},\n")
            f.write(f"    {{ 0, NULL }}\n")
            f.write("};\n\n")

            f.write(f"#define FUNC_TABLE_SIZE {len(set(self.functions) | getattr(self, 'mid_entry_addrs', set()))}\n\n")

            # Binary search dispatch
            f.write("static void (*find_function(uint32_t addr))(SH4CPU *cpu) {\n")
            f.write("    int lo = 0, hi = FUNC_TABLE_SIZE - 1;\n")
            f.write("    while (lo <= hi) {\n")
            f.write("        int mid = (lo + hi) / 2;\n")
            f.write("        if (func_table[mid].addr == addr) return func_table[mid].func;\n")
            f.write("        if (func_table[mid].addr < addr) lo = mid + 1;\n")
            f.write("        else hi = mid - 1;\n")
            f.write("    }\n")
            f.write("    return NULL;\n")
            f.write("}\n\n")

            f.write("#include <stdio.h>\n")
            f.write("static int unresolved_log = 0;\n\n")
            f.write("void sh4_call_indirect(SH4CPU *cpu) {\n")
            f.write("    /* Normalize: strip P1/P2/P3 area bits to P1 cached form */\n")
            f.write("    uint32_t phys = cpu->pc & 0x1FFFFFFF;\n")
            f.write("    /* A call through the BIOS vector table is a syscall, not\n")
            f.write("     * a function in this binary. */\n")
            f.write("    if (sh4_bios_syscall(cpu)) return;\n")
            f.write("    uint32_t lookup = phys | 0x80000000;\n")
            f.write("    void (*fn)(SH4CPU *cpu) = find_function(lookup);\n")
            f.write("    if (!fn) {\n")
            f.write("        /* Handle relocated bootstrap: 0x0C004000-0x0C008000 -> +0xC100 */\n")
            f.write("        if (phys >= 0x0C004000 && phys < 0x0C008000) {\n")
            f.write("            fn = find_function((phys + 0xC100) | 0x80000000);\n")
            f.write("        }\n")
            f.write("    }\n")
            f.write("#ifdef DCRECOMP_ABI_CHECK\n")
            f.write("    /* SH-4 keeps r8-r14 callee-saved. A recompiled function\n")
            f.write("     * that returns with one changed is a recompiler bug - usually\n")
            f.write("     * a branch target with no entry point, stubbed out so the\n")
            f.write("     * epilogue that would have popped them never runs. */\n")
            f.write("    if (fn) {\n")
            f.write("        uint32_t saved[8];\n")
            f.write("        for (int i = 8; i <= 15; i++) saved[i - 8] = cpu->r[i];\n")
            f.write("        uint32_t target = cpu->pc, from = cpu->pr;\n")
            f.write("        fn(cpu);\n")
            f.write("        static int abi_log = 0;\n")
            f.write("        if (cpu->r[15] != saved[7] && abi_log < 50) {\n")
            f.write("            abi_log++;\n")
            f.write("            printf(\"[ABI] func_%08X (from 0x%08X) left the stack unbalanced: SP %08X -> %08X\\n\",\n")
            f.write("                   target | 0x80000000u, from, saved[7], cpu->r[15]);\n")
            f.write("        }\n")
            f.write("        for (int i = 8; i <= 14 && abi_log < 50; i++) {\n")
            f.write("            if (cpu->r[i] != saved[i - 8]) {\n")
            f.write("                abi_log++;\n")
            f.write("                printf(\"[ABI] func_%08X (from 0x%08X) clobbered r%d: %08X -> %08X\\n\",\n")
            f.write("                       target | 0x80000000u, from, i, saved[i - 8], cpu->r[i]);\n")
            f.write("                break;\n")
            f.write("            }\n")
            f.write("        }\n")
            f.write("    }\n")
            f.write("#else\n")
            f.write("    if (fn) { fn(cpu); }\n")
            f.write("#endif\n")
            f.write("    else {\n")
            f.write("        /* BIOS/Boot ROM calls and null pointers are a no-op, but still\n")
            f.write("         * log them: a silently skipped callback is the classic cause of\n")
            f.write("         * a spin loop it was supposed to break. */\n")
            f.write("        if (unresolved_log < 100) {\n")
            f.write("            unresolved_log++;\n")
            f.write("            printf(\"[DISPATCH] unresolved call to 0x%08X (pr=0x%08X)\\n\",\n")
            f.write("                   cpu->pc, cpu->pr);\n")
            f.write("        }\n")
            f.write("    }\n")
            f.write("}\n\n")

            f.write("void sh4_jump_indirect(SH4CPU *cpu) {\n")
            f.write("    sh4_call_indirect(cpu);\n")
            f.write("}\n")

    def recompile_all(self, output_dir, batch_size=500):
        """Recompile all functions, split into multiple source files."""
        os.makedirs(output_dir, exist_ok=True)

        sorted_funcs = sorted(self.functions.items())
        file_idx = 0
        func_idx = 0
        total_funcs = len(sorted_funcs)

        while func_idx < total_funcs:
            batch = sorted_funcs[func_idx:func_idx + batch_size]
            filepath = os.path.join(output_dir, f"game_code_{file_idx:03d}.c")

            with open(filepath, 'w') as f:
                f.write(f"/* Auto-generated - DO NOT EDIT */\n")
                f.write(f"/* Functions 0x{batch[0][0]:08X} - 0x{batch[-1][0]:08X} */\n")
                f.write(f"#include \"recompiler/sh4_cpu.h\"\n")
                f.write(f"#include \"game/game_functions.h\"\n")
                f.write(f"#include <math.h>\n\n")

                for batch_idx, (addr, info) in enumerate(batch):
                    # Determine next function address for fall-through
                    global_idx = func_idx + batch_idx
                    next_addr = None
                    if global_idx + 1 < total_funcs:
                        next_addr = sorted_funcs[global_idx + 1][0]
                    code = self.recompile_function(addr, info, next_func_addr=next_addr)
                    if code:
                        f.write(code)
                        f.write("\n\n")

            print(f"  Generated {filepath} ({len(batch)} functions)")
            func_idx += batch_size
            file_idx += 1

        print(f"Total: {file_idx} source files, {total_funcs} functions")
        return file_idx


def main():
    # Parse arguments
    positional = []
    base_addr = LOAD_ADDR
    for i, arg in enumerate(sys.argv[1:], 1):
        if arg == '--base' and i + 1 <= len(sys.argv) - 1:
            base_addr = int(sys.argv[i + 1], 0)
        elif i > 1 and sys.argv[i - 1] == '--base':
            continue  # skip value after --base
        else:
            positional.append(arg)

    binary_path = positional[0] if len(positional) > 0 else "disc_extract/1ST_READ.BIN"
    output_dir = positional[1] if len(positional) > 1 else "src/game"
    header_dir = positional[2] if len(positional) > 2 else "include/game"

    print(f"Loading {binary_path}...")
    with open(binary_path, 'rb') as f:
        data = f.read()

    print(f"Binary: {len(data):,} bytes")
    print(f"Base address: 0x{base_addr:08X}")

    recompiler = SH4Recompiler(data, base_addr)

    print("Finding functions...")
    recompiler.find_functions()

    print("Generating function header...")
    os.makedirs(header_dir, exist_ok=True)
    recompiler.generate_header(os.path.join(header_dir, "game_functions.h"))

    print("Generating dispatch table...")
    recompiler.generate_dispatch_table(os.path.join(output_dir, "dispatch_table.c"))

    print("Recompiling functions to C...")
    num_files = recompiler.recompile_all(output_dir)

    print(f"\nStatic recompilation complete!")
    print(f"  Functions: {len(recompiler.functions)}")
    print(f"  Mid-function entries: {len(getattr(recompiler, 'mid_entry_addrs', ()))}")
    print(f"  Source files: {num_files}")
    print(f"  Output: {output_dir}/")


if __name__ == '__main__':
    main()
