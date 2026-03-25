#!/usr/bin/env python3
"""
SH-4 Disassembler for Dreamcast 1ST_READ.BIN
Decodes SH-4 instructions and builds a function map.

The Dreamcast loads 1ST_READ.BIN at address 0x8C010000 in SH-4 memory space.
"""

import struct
import sys
import json
from collections import defaultdict

# Dreamcast 1ST_READ.BIN load address
LOAD_ADDR = 0x8C010000

def decode_sh4(opcode, pc):
    """Decode a single SH-4 16-bit instruction. Returns (mnemonic, operands, is_branch, branch_target)."""
    n = (opcode >> 8) & 0xF
    m = (opcode >> 4) & 0xF
    d = opcode & 0xF
    i = opcode & 0xFF
    d8 = opcode & 0xFF
    d12 = opcode & 0xFFF

    # Sign extend helpers
    def sext8(v):
        return v if v < 128 else v - 256

    def sext12(v):
        return v if v < 2048 else v - 4096

    # NOP
    if opcode == 0x0009:
        return "nop", "", False, None

    # RTS
    if opcode == 0x000B:
        return "rts", "", True, "rts"

    # RTE
    if opcode == 0x002B:
        return "rte", "", True, "rte"

    # CLRT
    if opcode == 0x0008:
        return "clrt", "", False, None

    # SETT
    if opcode == 0x0018:
        return "sett", "", False, None

    # CLRMAC
    if opcode == 0x0028:
        return "clrmac", "", False, None

    # SLEEP
    if opcode == 0x001B:
        return "sleep", "", False, None

    # MOV Rm, Rn (0110nnnnmmmm0011)
    if (opcode & 0xF00F) == 0x6003:
        return "mov", f"r{m}, r{n}", False, None

    # MOV #imm, Rn (1110nnnniiiiiiii)
    if (opcode & 0xF000) == 0xE000:
        imm = sext8(i)
        return "mov", f"#{imm}, r{n}", False, None

    # MOV.L @(disp,PC), Rn (1101nnnndddddddd)
    if (opcode & 0xF000) == 0xD000:
        disp = d8 * 4
        addr = (pc & 0xFFFFFFFC) + 4 + disp
        return "mov.l", f"@(0x{addr:08X}), r{n}", False, None

    # MOV.W @(disp,PC), Rn (1001nnnndddddddd)
    if (opcode & 0xF000) == 0x9000:
        disp = d8 * 2
        addr = pc + 4 + disp
        return "mov.w", f"@(0x{addr:08X}), r{n}", False, None

    # MOV.L Rm, @Rn (0010nnnnmmmm0110)
    if (opcode & 0xF00F) == 0x2006:
        return "mov.l", f"r{m}, @r{n}", False, None

    # MOV.L @Rm, Rn (0110nnnnmmmm0010)
    if (opcode & 0xF00F) == 0x6002:
        return "mov.l", f"@r{m}, r{n}", False, None

    # MOV.L Rm, @-Rn (0010nnnnmmmm0110) -- actually 0010nnnnmmmm0110 is above
    # MOV.L Rm, @-Rn (0010nnnnmmmm0110) -- 0010nnnnmmmm0110
    # Let me be more precise:

    # MOV.L Rm, @-Rn (0010nnnnmmmm0110) is already handled
    # MOV.L @Rm+, Rn (0110nnnnmmmm0110)
    if (opcode & 0xF00F) == 0x6006:
        return "mov.l", f"@r{m}+, r{n}", False, None

    # MOV.L Rm, @(disp,Rn) (0001nnnnmmmmdddd)
    if (opcode & 0xF000) == 0x1000:
        disp = d * 4
        return "mov.l", f"r{m}, @({disp}, r{n})", False, None

    # MOV.L @(disp,Rm), Rn (0101nnnnmmmmdddd)
    if (opcode & 0xF000) == 0x5000:
        disp = d * 4
        return "mov.l", f"@({disp}, r{m}), r{n}", False, None

    # MOV.B Rm, @Rn (0010nnnnmmmm0000)
    if (opcode & 0xF00F) == 0x2000:
        return "mov.b", f"r{m}, @r{n}", False, None

    # MOV.B @Rm, Rn (0110nnnnmmmm0000)
    if (opcode & 0xF00F) == 0x6000:
        return "mov.b", f"@r{m}, r{n}", False, None

    # MOV.W Rm, @Rn (0010nnnnmmmm0001)
    if (opcode & 0xF00F) == 0x2001:
        return "mov.w", f"r{m}, @r{n}", False, None

    # MOV.W @Rm, Rn (0110nnnnmmmm0001)
    if (opcode & 0xF00F) == 0x6001:
        return "mov.w", f"@r{m}, r{n}", False, None

    # MOV.L Rm, @-Rn (0010nnnnmmmm0110) - duplicate, remove
    # MOV.B @Rm+, Rn (0110nnnnmmmm0100)
    if (opcode & 0xF00F) == 0x6004:
        return "mov.b", f"@r{m}+, r{n}", False, None

    # MOV.W @Rm+, Rn (0110nnnnmmmm0101)
    if (opcode & 0xF00F) == 0x6005:
        return "mov.w", f"@r{m}+, r{n}", False, None

    # MOV.B Rm, @-Rn (0010nnnnmmmm0100)
    if (opcode & 0xF00F) == 0x2004:
        return "mov.b", f"r{m}, @-r{n}", False, None

    # MOV.W Rm, @-Rn (0010nnnnmmmm0101)
    if (opcode & 0xF00F) == 0x2005:
        return "mov.w", f"r{m}, @-r{n}", False, None

    # MOV.L Rm, @-Rn (0010nnnnmmmm0110) handled above

    # ADD Rm, Rn (0011nnnnmmmm1100)
    if (opcode & 0xF00F) == 0x300C:
        return "add", f"r{m}, r{n}", False, None

    # ADD #imm, Rn (0111nnnniiiiiiii)
    if (opcode & 0xF000) == 0x7000:
        imm = sext8(i)
        return "add", f"#{imm}, r{n}", False, None

    # SUB Rm, Rn (0011nnnnmmmm1000)
    if (opcode & 0xF00F) == 0x3008:
        return "sub", f"r{m}, r{n}", False, None

    # CMP/EQ Rm, Rn (0011nnnnmmmm0000)
    if (opcode & 0xF00F) == 0x3000:
        return "cmp/eq", f"r{m}, r{n}", False, None

    # CMP/EQ #imm, R0 (10001000iiiiiiii)
    if (opcode & 0xFF00) == 0x8800:
        imm = sext8(i)
        return "cmp/eq", f"#{imm}, r0", False, None

    # CMP/GT Rm, Rn (0011nnnnmmmm0111)
    if (opcode & 0xF00F) == 0x3007:
        return "cmp/gt", f"r{m}, r{n}", False, None

    # CMP/GE Rm, Rn (0011nnnnmmmm0011)
    if (opcode & 0xF00F) == 0x3003:
        return "cmp/ge", f"r{m}, r{n}", False, None

    # CMP/HI Rm, Rn (0011nnnnmmmm0110)
    if (opcode & 0xF00F) == 0x3006:
        return "cmp/hi", f"r{m}, r{n}", False, None

    # CMP/HS Rm, Rn (0011nnnnmmmm0010)
    if (opcode & 0xF00F) == 0x3002:
        return "cmp/hs", f"r{m}, r{n}", False, None

    # CMP/PL Rn (0100nnnn00010101)
    if (opcode & 0xF0FF) == 0x4015:
        return "cmp/pl", f"r{n}", False, None

    # CMP/PZ Rn (0100nnnn00010001)
    if (opcode & 0xF0FF) == 0x4011:
        return "cmp/pz", f"r{n}", False, None

    # CMP/STR Rm, Rn (0010nnnnmmmm1100)
    if (opcode & 0xF00F) == 0x200C:
        return "cmp/str", f"r{m}, r{n}", False, None

    # TST Rm, Rn (0010nnnnmmmm1000)
    if (opcode & 0xF00F) == 0x2008:
        return "tst", f"r{m}, r{n}", False, None

    # TST #imm, R0 (11001000iiiiiiii)
    if (opcode & 0xFF00) == 0xC800:
        return "tst", f"#0x{i:02X}, r0", False, None

    # AND Rm, Rn (0010nnnnmmmm1001)
    if (opcode & 0xF00F) == 0x2009:
        return "and", f"r{m}, r{n}", False, None

    # AND #imm, R0 (11001001iiiiiiii)
    if (opcode & 0xFF00) == 0xC900:
        return "and", f"#0x{i:02X}, r0", False, None

    # OR Rm, Rn (0010nnnnmmmm1011)
    if (opcode & 0xF00F) == 0x200B:
        return "or", f"r{m}, r{n}", False, None

    # OR #imm, R0 (11001011iiiiiiii)
    if (opcode & 0xFF00) == 0xCB00:
        return "or", f"#0x{i:02X}, r0", False, None

    # XOR Rm, Rn (0010nnnnmmmm1010)
    if (opcode & 0xF00F) == 0x200A:
        return "xor", f"r{m}, r{n}", False, None

    # NOT Rm, Rn (0110nnnnmmmm0111)
    if (opcode & 0xF00F) == 0x6007:
        return "not", f"r{m}, r{n}", False, None

    # NEG Rm, Rn (0110nnnnmmmm1011)
    if (opcode & 0xF00F) == 0x600B:
        return "neg", f"r{m}, r{n}", False, None

    # SHLL Rn (0100nnnn00000000)
    if (opcode & 0xF0FF) == 0x4000:
        return "shll", f"r{n}", False, None

    # SHLR Rn (0100nnnn00000001)
    if (opcode & 0xF0FF) == 0x4001:
        return "shlr", f"r{n}", False, None

    # SHLL2 Rn (0100nnnn00001000)
    if (opcode & 0xF0FF) == 0x4008:
        return "shll2", f"r{n}", False, None

    # SHLR2 Rn (0100nnnn00001001)
    if (opcode & 0xF0FF) == 0x4009:
        return "shlr2", f"r{n}", False, None

    # SHLL8 Rn (0100nnnn00011000)
    if (opcode & 0xF0FF) == 0x4018:
        return "shll8", f"r{n}", False, None

    # SHLR8 Rn (0100nnnn00011001)
    if (opcode & 0xF0FF) == 0x4019:
        return "shlr8", f"r{n}", False, None

    # SHLL16 Rn (0100nnnn00101000)
    if (opcode & 0xF0FF) == 0x4028:
        return "shll16", f"r{n}", False, None

    # SHLR16 Rn (0100nnnn00101001)
    if (opcode & 0xF0FF) == 0x4029:
        return "shlr16", f"r{n}", False, None

    # SHAL Rn (0100nnnn00100000)
    if (opcode & 0xF0FF) == 0x4020:
        return "shal", f"r{n}", False, None

    # SHAR Rn (0100nnnn00100001)
    if (opcode & 0xF0FF) == 0x4021:
        return "shar", f"r{n}", False, None

    # ROTL Rn (0100nnnn00000100)
    if (opcode & 0xF0FF) == 0x4004:
        return "rotl", f"r{n}", False, None

    # ROTR Rn (0100nnnn00000101)
    if (opcode & 0xF0FF) == 0x4005:
        return "rotr", f"r{n}", False, None

    # SHAD Rm, Rn (0100nnnnmmmm1100)
    if (opcode & 0xF00F) == 0x400C:
        return "shad", f"r{m}, r{n}", False, None

    # SHLD Rm, Rn (0100nnnnmmmm1101)
    if (opcode & 0xF00F) == 0x400D:
        return "shld", f"r{m}, r{n}", False, None

    # EXTS.B Rm, Rn (0110nnnnmmmm1110)
    if (opcode & 0xF00F) == 0x600E:
        return "exts.b", f"r{m}, r{n}", False, None

    # EXTS.W Rm, Rn (0110nnnnmmmm1111)
    if (opcode & 0xF00F) == 0x600F:
        return "exts.w", f"r{m}, r{n}", False, None

    # EXTU.B Rm, Rn (0110nnnnmmmm1100)
    if (opcode & 0xF00F) == 0x600C:
        return "extu.b", f"r{m}, r{n}", False, None

    # EXTU.W Rm, Rn (0110nnnnmmmm1101)
    if (opcode & 0xF00F) == 0x600D:
        return "extu.w", f"r{m}, r{n}", False, None

    # SWAP.B Rm, Rn (0110nnnnmmmm1000)
    if (opcode & 0xF00F) == 0x6008:
        return "swap.b", f"r{m}, r{n}", False, None

    # SWAP.W Rm, Rn (0110nnnnmmmm1001)
    if (opcode & 0xF00F) == 0x6009:
        return "swap.w", f"r{m}, r{n}", False, None

    # MULU.W Rm, Rn (0010nnnnmmmm1110)
    if (opcode & 0xF00F) == 0x200E:
        return "mulu.w", f"r{m}, r{n}", False, None

    # MULS.W Rm, Rn (0010nnnnmmmm1111)
    if (opcode & 0xF00F) == 0x200F:
        return "muls.w", f"r{m}, r{n}", False, None

    # MUL.L Rm, Rn (0000nnnnmmmm0111)
    if (opcode & 0xF00F) == 0x0007:
        return "mul.l", f"r{m}, r{n}", False, None

    # DMULU.L Rm, Rn (0011nnnnmmmm0101)
    if (opcode & 0xF00F) == 0x3005:
        return "dmulu.l", f"r{m}, r{n}", False, None

    # DMULS.L Rm, Rn (0011nnnnmmmm1101)
    if (opcode & 0xF00F) == 0x300D:
        return "dmuls.l", f"r{m}, r{n}", False, None

    # MAC.L @Rm+, @Rn+ (0000nnnnmmmm1111)
    if (opcode & 0xF00F) == 0x000F:
        return "mac.l", f"@r{m}+, @r{n}+", False, None

    # MAC.W @Rm+, @Rn+ (0100nnnnmmmm1111)
    if (opcode & 0xF00F) == 0x400F:
        return "mac.w", f"@r{m}+, @r{n}+", False, None

    # STS MACL, Rn (0000nnnn00011010)
    if (opcode & 0xF0FF) == 0x001A:
        return "sts", f"macl, r{n}", False, None

    # STS MACH, Rn (0000nnnn00001010)
    if (opcode & 0xF0FF) == 0x000A:
        return "sts", f"mach, r{n}", False, None

    # STS PR, Rn (0000nnnn00101010)
    if (opcode & 0xF0FF) == 0x002A:
        return "sts", f"pr, r{n}", False, None

    # STS FPSCR, Rn (0000nnnn01101010)
    if (opcode & 0xF0FF) == 0x006A:
        return "sts", f"fpscr, r{n}", False, None

    # STS FPUL, Rn (0000nnnn01011010)
    if (opcode & 0xF0FF) == 0x005A:
        return "sts", f"fpul, r{n}", False, None

    # LDS Rm, PR (0100mmmm00101010)
    if (opcode & 0xF0FF) == 0x402A:
        return "lds", f"r{n}, pr", False, None

    # LDS Rm, MACL (0100mmmm00011010)
    if (opcode & 0xF0FF) == 0x401A:
        return "lds", f"r{n}, macl", False, None

    # LDS Rm, MACH (0100mmmm00001010)
    if (opcode & 0xF0FF) == 0x400A:
        return "lds", f"r{n}, mach", False, None

    # LDS Rm, FPSCR (0100mmmm01101010)
    if (opcode & 0xF0FF) == 0x406A:
        return "lds", f"r{n}, fpscr", False, None

    # LDS Rm, FPUL (0100mmmm01011010)
    if (opcode & 0xF0FF) == 0x405A:
        return "lds", f"r{n}, fpul", False, None

    # STS.L PR, @-Rn (0100nnnn00100010)
    if (opcode & 0xF0FF) == 0x4022:
        return "sts.l", f"pr, @-r{n}", False, None

    # LDS.L @Rm+, PR (0100mmmm00100110)
    if (opcode & 0xF0FF) == 0x4026:
        return "lds.l", f"@r{n}+, pr", False, None

    # STS.L MACL, @-Rn (0100nnnn00010010)
    if (opcode & 0xF0FF) == 0x4012:
        return "sts.l", f"macl, @-r{n}", False, None

    # STS.L MACH, @-Rn (0100nnnn00000010)
    if (opcode & 0xF0FF) == 0x4002:
        return "sts.l", f"mach, @-r{n}", False, None

    # LDS.L @Rm+, MACL (0100mmmm00010110)
    if (opcode & 0xF0FF) == 0x4016:
        return "lds.l", f"@r{n}+, macl", False, None

    # LDS.L @Rm+, MACH (0100mmmm00000110)
    if (opcode & 0xF0FF) == 0x4006:
        return "lds.l", f"@r{n}+, mach", False, None

    # STS.L FPSCR, @-Rn (0100nnnn01100010)
    if (opcode & 0xF0FF) == 0x4062:
        return "sts.l", f"fpscr, @-r{n}", False, None

    # LDS.L @Rm+, FPSCR (0100mmmm01100110)
    if (opcode & 0xF0FF) == 0x4066:
        return "lds.l", f"@r{n}+, fpscr", False, None

    # STS.L FPUL, @-Rn (0100nnnn01010010)
    if (opcode & 0xF0FF) == 0x4052:
        return "sts.l", f"fpul, @-r{n}", False, None

    # LDS.L @Rm+, FPUL (0100mmmm01010110)
    if (opcode & 0xF0FF) == 0x4056:
        return "lds.l", f"@r{n}+, fpul", False, None

    # STC SR, Rn (0000nnnn00000010)
    if (opcode & 0xF0FF) == 0x0002:
        return "stc", f"sr, r{n}", False, None

    # STC GBR, Rn (0000nnnn00010010)
    if (opcode & 0xF0FF) == 0x0012:
        return "stc", f"gbr, r{n}", False, None

    # STC VBR, Rn (0000nnnn00100010)
    if (opcode & 0xF0FF) == 0x0022:
        return "stc", f"vbr, r{n}", False, None

    # LDC Rm, SR (0100mmmm00001110)
    if (opcode & 0xF0FF) == 0x400E:
        return "ldc", f"r{n}, sr", False, None

    # LDC Rm, GBR (0100mmmm00011110)
    if (opcode & 0xF0FF) == 0x401E:
        return "ldc", f"r{n}, gbr", False, None

    # LDC Rm, VBR (0100mmmm00101110)
    if (opcode & 0xF0FF) == 0x402E:
        return "ldc", f"r{n}, vbr", False, None

    # MOV.B R0, @(disp,Rn) (10000000nnnndddd)
    if (opcode & 0xFF00) == 0x8000:
        disp = opcode & 0xF
        rn = (opcode >> 4) & 0xF
        return "mov.b", f"r0, @({disp}, r{rn})", False, None

    # MOV.W R0, @(disp,Rn) (10000001nnnndddd)
    if (opcode & 0xFF00) == 0x8100:
        disp = (opcode & 0xF) * 2
        rn = (opcode >> 4) & 0xF
        return "mov.w", f"r0, @({disp}, r{rn})", False, None

    # MOV.B @(disp,Rm), R0 (10000100mmmmdddd)
    if (opcode & 0xFF00) == 0x8400:
        disp = opcode & 0xF
        rm = (opcode >> 4) & 0xF
        return "mov.b", f"@({disp}, r{rm}), r0", False, None

    # MOV.W @(disp,Rm), R0 (10000101mmmmdddd)
    if (opcode & 0xFF00) == 0x8500:
        disp = (opcode & 0xF) * 2
        rm = (opcode >> 4) & 0xF
        return "mov.w", f"@({disp}, r{rm}), r0", False, None

    # MOV.B R0, @(disp,GBR) (11000000dddddddd)
    if (opcode & 0xFF00) == 0xC000:
        return "mov.b", f"r0, @({d8}, gbr)", False, None

    # MOV.W R0, @(disp,GBR) (11000001dddddddd)
    if (opcode & 0xFF00) == 0xC100:
        return "mov.w", f"r0, @({d8*2}, gbr)", False, None

    # MOV.L R0, @(disp,GBR) (11000010dddddddd)
    if (opcode & 0xFF00) == 0xC200:
        return "mov.l", f"r0, @({d8*4}, gbr)", False, None

    # MOV.B @(disp,GBR), R0 (11000100dddddddd)
    if (opcode & 0xFF00) == 0xC400:
        return "mov.b", f"@({d8}, gbr), r0", False, None

    # MOV.W @(disp,GBR), R0 (11000101dddddddd)
    if (opcode & 0xFF00) == 0xC500:
        return "mov.w", f"@({d8*2}, gbr), r0", False, None

    # MOV.L @(disp,GBR), R0 (11000110dddddddd)
    if (opcode & 0xFF00) == 0xC600:
        return "mov.l", f"@({d8*4}, gbr), r0", False, None

    # MOV.B @(R0,Rm), Rn (0000nnnnmmmm1100)
    if (opcode & 0xF00F) == 0x000C:
        return "mov.b", f"@(r0, r{m}), r{n}", False, None

    # MOV.W @(R0,Rm), Rn (0000nnnnmmmm1101)
    if (opcode & 0xF00F) == 0x000D:
        return "mov.w", f"@(r0, r{m}), r{n}", False, None

    # MOV.L @(R0,Rm), Rn (0000nnnnmmmm1110)
    if (opcode & 0xF00F) == 0x000E:
        return "mov.l", f"@(r0, r{m}), r{n}", False, None

    # MOV.B Rm, @(R0,Rn) (0000nnnnmmmm0100)
    if (opcode & 0xF00F) == 0x0004:
        return "mov.b", f"r{m}, @(r0, r{n})", False, None

    # MOV.W Rm, @(R0,Rn) (0000nnnnmmmm0101)
    if (opcode & 0xF00F) == 0x0005:
        return "mov.w", f"r{m}, @(r0, r{n})", False, None

    # MOV.L Rm, @(R0,Rn) (0000nnnnmmmm0110)
    if (opcode & 0xF00F) == 0x0006:
        return "mov.l", f"r{m}, @(r0, r{n})", False, None

    # MOVA @(disp,PC), R0 (11000111dddddddd)
    if (opcode & 0xFF00) == 0xC700:
        disp = d8 * 4
        addr = (pc & 0xFFFFFFFC) + 4 + disp
        return "mova", f"@(0x{addr:08X}), r0", False, None

    # MOVT Rn (0000nnnn00101001)
    if (opcode & 0xF0FF) == 0x0029:
        return "movt", f"r{n}", False, None

    # DT Rn (0100nnnn00010000)
    if (opcode & 0xF0FF) == 0x4010:
        return "dt", f"r{n}", False, None

    # --- Branch instructions ---

    # BRA disp (1010dddddddddddd)
    if (opcode & 0xF000) == 0xA000:
        disp = sext12(d12) * 2
        target = pc + 4 + disp
        return "bra", f"0x{target:08X}", True, target

    # BSR disp (1011dddddddddddd)
    if (opcode & 0xF000) == 0xB000:
        disp = sext12(d12) * 2
        target = pc + 4 + disp
        return "bsr", f"0x{target:08X}", True, target

    # BT disp (10001001dddddddd)
    if (opcode & 0xFF00) == 0x8900:
        disp = sext8(d8) * 2
        target = pc + 4 + disp
        return "bt", f"0x{target:08X}", True, target

    # BF disp (10001011dddddddd)
    if (opcode & 0xFF00) == 0x8B00:
        disp = sext8(d8) * 2
        target = pc + 4 + disp
        return "bf", f"0x{target:08X}", True, target

    # BT/S disp (10001101dddddddd)
    if (opcode & 0xFF00) == 0x8D00:
        disp = sext8(d8) * 2
        target = pc + 4 + disp
        return "bt/s", f"0x{target:08X}", True, target

    # BF/S disp (10001111dddddddd)
    if (opcode & 0xFF00) == 0x8F00:
        disp = sext8(d8) * 2
        target = pc + 4 + disp
        return "bf/s", f"0x{target:08X}", True, target

    # JMP @Rm (0100mmmm00101011)
    if (opcode & 0xF0FF) == 0x402B:
        return "jmp", f"@r{n}", True, "indirect"

    # JSR @Rm (0100mmmm00001011)
    if (opcode & 0xF0FF) == 0x400B:
        return "jsr", f"@r{n}", True, "indirect"

    # BRAF Rm (0000mmmm00100011)
    if (opcode & 0xF0FF) == 0x0023:
        return "braf", f"r{n}", True, "indirect"

    # BSRF Rm (0000mmmm00000011)
    if (opcode & 0xF0FF) == 0x0003:
        return "bsrf", f"r{n}", True, "indirect"

    # TRAPA #imm (11000011iiiiiiii)
    if (opcode & 0xFF00) == 0xC300:
        return "trapa", f"#0x{i:02X}", True, "trap"

    # --- Floating point ---

    # FMOV FRm, FRn (1111nnnnmmmm1100)
    if (opcode & 0xF00F) == 0xF00C:
        return "fmov", f"fr{m}, fr{n}", False, None

    # FMOV.S @Rm, FRn (1111nnnnmmmm1000)
    if (opcode & 0xF00F) == 0xF008:
        return "fmov.s", f"@r{m}, fr{n}", False, None

    # FMOV.S FRm, @Rn (1111nnnnmmmm1010)
    if (opcode & 0xF00F) == 0xF00A:
        return "fmov.s", f"fr{m}, @r{n}", False, None

    # FMOV.S @Rm+, FRn (1111nnnnmmmm1001)
    if (opcode & 0xF00F) == 0xF009:
        return "fmov.s", f"@r{m}+, fr{n}", False, None

    # FMOV.S FRm, @-Rn (1111nnnnmmmm1011)
    if (opcode & 0xF00F) == 0xF00B:
        return "fmov.s", f"fr{m}, @-r{n}", False, None

    # FMOV.S @(R0,Rm), FRn (1111nnnnmmmm0110)
    if (opcode & 0xF00F) == 0xF006:
        return "fmov.s", f"@(r0, r{m}), fr{n}", False, None

    # FMOV.S FRm, @(R0,Rn) (1111nnnnmmmm0111)
    if (opcode & 0xF00F) == 0xF007:
        return "fmov.s", f"fr{m}, @(r0, r{n})", False, None

    # FADD FRm, FRn (1111nnnnmmmm0000)
    if (opcode & 0xF00F) == 0xF000:
        return "fadd", f"fr{m}, fr{n}", False, None

    # FSUB FRm, FRn (1111nnnnmmmm0001)
    if (opcode & 0xF00F) == 0xF001:
        return "fsub", f"fr{m}, fr{n}", False, None

    # FMUL FRm, FRn (1111nnnnmmmm0010)
    if (opcode & 0xF00F) == 0xF002:
        return "fmul", f"fr{m}, fr{n}", False, None

    # FDIV FRm, FRn (1111nnnnmmmm0011)
    if (opcode & 0xF00F) == 0xF003:
        return "fdiv", f"fr{m}, fr{n}", False, None

    # FCMP/EQ FRm, FRn (1111nnnnmmmm0100)
    if (opcode & 0xF00F) == 0xF004:
        return "fcmp/eq", f"fr{m}, fr{n}", False, None

    # FCMP/GT FRm, FRn (1111nnnnmmmm0101)
    if (opcode & 0xF00F) == 0xF005:
        return "fcmp/gt", f"fr{m}, fr{n}", False, None

    # FLOAT FPUL, FRn (1111nnnn00101101)
    if (opcode & 0xF0FF) == 0xF02D:
        return "float", f"fpul, fr{n}", False, None

    # FTRC FRm, FPUL (1111mmmm00111101)
    if (opcode & 0xF0FF) == 0xF03D:
        return "ftrc", f"fr{n}, fpul", False, None

    # FNEG FRn (1111nnnn01001101)
    if (opcode & 0xF0FF) == 0xF04D:
        return "fneg", f"fr{n}", False, None

    # FABS FRn (1111nnnn01011101)
    if (opcode & 0xF0FF) == 0xF05D:
        return "fabs", f"fr{n}", False, None

    # FSQRT FRn (1111nnnn01101101)
    if (opcode & 0xF0FF) == 0xF06D:
        return "fsqrt", f"fr{n}", False, None

    # FLDS FRm, FPUL (1111mmmm00011101)
    if (opcode & 0xF0FF) == 0xF01D:
        return "flds", f"fr{n}, fpul", False, None

    # FSTS FPUL, FRn (1111nnnn00001101)
    if (opcode & 0xF0FF) == 0xF00D:
        return "fsts", f"fpul, fr{n}", False, None

    # FIPR FVm, FVn (1111nnmm11101101)
    if (opcode & 0xF0FF) == 0xF0ED:
        vn = (n >> 2) & 3
        vm = (n & 3)
        return "fipr", f"fv{vm*4}, fv{vn*4}", False, None

    # FTRV XMTRX, FVn (1111nn0111111101)
    if (opcode & 0xF3FF) == 0xF1FD:
        vn = (n >> 2) & 3
        return "ftrv", f"xmtrx, fv{vn*4}", False, None

    # FRCHG (1111101111111101)
    if opcode == 0xFBFD:
        return "frchg", "", False, None

    # FSCHG (1111001111111101)
    if opcode == 0xF3FD:
        return "fschg", "", False, None

    # ADDC Rm, Rn (0011nnnnmmmm1110)
    if (opcode & 0xF00F) == 0x300E:
        return "addc", f"r{m}, r{n}", False, None

    # ADDV Rm, Rn (0011nnnnmmmm1111)
    if (opcode & 0xF00F) == 0x300F:
        return "addv", f"r{m}, r{n}", False, None

    # SUBC Rm, Rn (0011nnnnmmmm1010)
    if (opcode & 0xF00F) == 0x300A:
        return "subc", f"r{m}, r{n}", False, None

    # SUBV Rm, Rn (0011nnnnmmmm1011)
    if (opcode & 0xF00F) == 0x300B:
        return "subv", f"r{m}, r{n}", False, None

    # NEGC Rm, Rn (0110nnnnmmmm1010)
    if (opcode & 0xF00F) == 0x600A:
        return "negc", f"r{m}, r{n}", False, None

    # PREF @Rn (0000nnnn10000011)
    if (opcode & 0xF0FF) == 0x0083:
        return "pref", f"@r{n}", False, None

    # OCBI @Rn (0000nnnn10010011)
    if (opcode & 0xF0FF) == 0x0093:
        return "ocbi", f"@r{n}", False, None

    # OCBP @Rn (0000nnnn10100011)
    if (opcode & 0xF0FF) == 0x00A3:
        return "ocbp", f"@r{n}", False, None

    # OCBWB @Rn (0000nnnn10110011)
    if (opcode & 0xF0FF) == 0x00B3:
        return "ocbwb", f"@r{n}", False, None

    # XTRCT Rm, Rn (0010nnnnmmmm1101)
    if (opcode & 0xF00F) == 0x200D:
        return "xtrct", f"r{m}, r{n}", False, None

    # ROTCL Rn (0100nnnn00100100)
    if (opcode & 0xF0FF) == 0x4024:
        return "rotcl", f"r{n}", False, None

    # ROTCR Rn (0100nnnn00100101)
    if (opcode & 0xF0FF) == 0x4025:
        return "rotcr", f"r{n}", False, None

    # FMAC FR0, FRm, FRn (1111nnnnmmmm1110)
    if (opcode & 0xF00F) == 0xF00E:
        return "fmac", f"fr0, fr{m}, fr{n}", False, None

    # FCNVDS DRm, FPUL
    if (opcode & 0xF1FF) == 0xF0BD:
        return "fcnvds", f"dr{n}, fpul", False, None

    # FCNVSD FPUL, DRn
    if (opcode & 0xF1FF) == 0xF0AD:
        return "fcnvsd", f"fpul, dr{n}", False, None

    return f".word", f"0x{opcode:04X}", False, None


def find_functions(binary_data, base_addr):
    """
    Find function boundaries using heuristic analysis.
    Functions typically start with sts.l pr, @-r15 and end with rts.
    """
    functions = {}
    size = len(binary_data)

    # First pass: find all BSR/JSR targets (these are function entry points)
    call_targets = set()
    branch_targets = set()

    for offset in range(0, size - 1, 2):
        opcode = struct.unpack_from('<H', binary_data, offset)[0]
        pc = base_addr + offset

        mnemonic, operands, is_branch, target = decode_sh4(opcode, pc)

        if target and isinstance(target, int):
            if mnemonic in ('bsr',):
                call_targets.add(target)
            elif mnemonic in ('bra', 'bt', 'bf', 'bt/s', 'bf/s'):
                branch_targets.add(target)

    # Second pass: find function prologues (sts.l pr, @-r15)
    prologue_addrs = set()
    for offset in range(0, size - 1, 2):
        opcode = struct.unpack_from('<H', binary_data, offset)[0]
        # sts.l pr, @-r15 = 0x4F22
        if opcode == 0x4F22:
            addr = base_addr + offset
            prologue_addrs.add(addr)

    # Combine: BSR targets are definitely functions
    # Addresses with sts.l pr, @-r15 are likely functions
    all_entry_points = sorted(call_targets | prologue_addrs)

    # Also add the entry point
    all_entry_points = sorted(set([base_addr]) | set(all_entry_points))

    # Build function map
    for i, entry in enumerate(all_entry_points):
        if entry < base_addr or entry >= base_addr + size:
            continue

        # Determine function end (next function start or RTS)
        if i + 1 < len(all_entry_points):
            next_func = all_entry_points[i + 1]
            func_size = next_func - entry
        else:
            func_size = (base_addr + size) - entry

        functions[entry] = {
            'addr': entry,
            'size': func_size,
            'has_prologue': entry in prologue_addrs,
            'is_call_target': entry in call_targets,
        }

    return functions


def analyze_binary(filepath, base_addr=LOAD_ADDR):
    """Full analysis of 1ST_READ.BIN."""
    with open(filepath, 'rb') as f:
        data = f.read()

    size = len(data)
    print(f"Binary size: {size:,} bytes ({size // 1024} KB)")
    print(f"Load address: 0x{base_addr:08X}")
    print(f"Address range: 0x{base_addr:08X} - 0x{base_addr + size:08X}")

    # Instruction statistics
    print("\n=== Instruction Analysis ===")
    inst_counts = defaultdict(int)
    total_insts = 0
    branch_count = 0
    float_count = 0
    unknown_count = 0

    for offset in range(0, size - 1, 2):
        opcode = struct.unpack_from('<H', binary_data := data, offset)[0]
        pc = base_addr + offset
        mnemonic, operands, is_branch, target = decode_sh4(opcode, pc)

        inst_counts[mnemonic] += 1
        total_insts += 1

        if is_branch:
            branch_count += 1
        if mnemonic.startswith('f'):
            float_count += 1
        if mnemonic == '.word':
            unknown_count += 1

    print(f"Total instructions: {total_insts:,}")
    print(f"Branch instructions: {branch_count:,}")
    print(f"FP instructions: {float_count:,}")
    print(f"Unknown/data: {unknown_count:,}")
    decode_rate = (1 - unknown_count / total_insts) * 100
    print(f"Decode rate: {decode_rate:.1f}%")

    # Top 30 instructions
    print("\n=== Top 30 Most Common Instructions ===")
    for mnemonic, count in sorted(inst_counts.items(), key=lambda x: -x[1])[:30]:
        pct = count / total_insts * 100
        print(f"  {mnemonic:12s} {count:8,} ({pct:5.1f}%)")

    # Find functions
    print("\n=== Function Analysis ===")
    functions = find_functions(data, base_addr)
    print(f"Functions found: {len(functions)}")

    # Function size distribution
    sizes = [f['size'] for f in functions.values()]
    if sizes:
        print(f"Average function size: {sum(sizes) // len(sizes)} bytes")
        print(f"Largest function: {max(sizes):,} bytes")
        print(f"Smallest function: {min(sizes)} bytes")

    # Disassemble first 100 instructions as sample
    print("\n=== First 100 Instructions (Entry Point) ===")
    for offset in range(0, min(200, size - 1), 2):
        opcode = struct.unpack_from('<H', data, offset)[0]
        pc = base_addr + offset
        mnemonic, operands, is_branch, target = decode_sh4(opcode, pc)

        # Mark function entries
        marker = ""
        if pc in functions:
            marker = " <-- FUNCTION"

        sep = "\t" if operands else ""
        print(f"  0x{pc:08X}:  {opcode:04X}  {mnemonic}{sep}{operands}{marker}")

    # Save function map
    func_map = {}
    for addr, info in sorted(functions.items()):
        func_map[f"0x{addr:08X}"] = {
            'size': info['size'],
            'has_prologue': info['has_prologue'],
            'is_call_target': info['is_call_target'],
        }

    map_file = filepath.replace('.BIN', '_functions.json').replace('.bin', '_functions.json')
    with open(map_file, 'w') as f:
        json.dump({
            'binary': filepath,
            'base_addr': f"0x{base_addr:08X}",
            'binary_size': size,
            'total_instructions': total_insts,
            'decode_rate': decode_rate,
            'function_count': len(functions),
            'functions': func_map,
        }, f, indent=2)
    print(f"\nFunction map saved to {map_file}")

    return functions, inst_counts


if __name__ == '__main__':
    filepath = sys.argv[1] if len(sys.argv) > 1 else "disc_extract/1ST_READ.BIN"
    base = LOAD_ADDR
    for i, arg in enumerate(sys.argv):
        if arg == '--base' and i + 1 < len(sys.argv):
            base = int(sys.argv[i + 1], 0)
    analyze_binary(filepath, base)
