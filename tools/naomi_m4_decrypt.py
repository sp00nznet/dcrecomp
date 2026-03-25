#!/usr/bin/env python3
"""
Naomi M4 ROM Board Decryption

Decrypts Naomi M4 board encrypted ROM data using the security chip (317-xxxx-com).
The M4 encryption is a stream cipher built on a 16-bit SP-network block cipher,
processing data in 16-bit words with a 32-byte IV reset period.

Usage:
  python naomi_m4_decrypt.py <encrypted_rom> <security_chip.ic3> <output_file>
  python naomi_m4_decrypt.py --zip <rom.zip> <output_file>
"""

import os
import sys
import struct

# Fixed 4-to-4 S-boxes used by the M4 cipher
SBOX = [
    [9, 8, 2, 11, 1, 14, 5, 15, 12, 6, 0, 3, 7, 13, 10, 4],
    [2, 10, 0, 15, 14, 1, 11, 3, 7, 12, 13, 8, 4, 9, 5, 6],
    [4, 11, 3, 8, 7, 2, 15, 13, 1, 5, 14, 9, 6, 12, 0, 10],
    [1, 13, 8, 2, 0, 5, 6, 14, 4, 11, 15, 10, 12, 3, 7, 9],
]


def build_one_round_table():
    """Precompute the one-round S-box + P-box transformation for all 16-bit inputs."""
    table = [0] * 65536

    for val in range(65536):
        input_nibbles = [(val >> (i * 4)) & 0xF for i in range(4)]
        output_nibbles = [0, 0, 0, 0]
        aux = input_nibbles[3]

        for nibble_idx in range(4):
            aux ^= SBOX[nibble_idx][input_nibbles[nibble_idx]]
            for bit in range(4):
                output_nibbles[(nibble_idx - bit) & 3] |= aux & (1 << bit)

        table[val] = (output_nibbles[0] |
                      (output_nibbles[1] << 4) |
                      (output_nibbles[2] << 8) |
                      (output_nibbles[3] << 12))

    return table


def extract_subkeys(ic3_data):
    """Extract the two 16-bit subkeys from the security chip data.

    The 2048-byte PIC16C621A readout stores keys at specific offsets:
      subkey1 = (data[0x5E2] << 8) | data[0x5E0]
      subkey2 = (data[0x5E6] << 8) | data[0x5E4]
    """
    if len(ic3_data) < 0x5E7:
        raise ValueError(f"Security chip data too small: {len(ic3_data)} bytes (need >= 0x5E7)")

    subkey1 = (ic3_data[0x5E2] << 8) | ic3_data[0x5E0]
    subkey2 = (ic3_data[0x5E6] << 8) | ic3_data[0x5E4]

    return subkey1, subkey2


def decrypt_rom(encrypted_data, subkey1, subkey2, one_round):
    """Decrypt the ROM data using the M4 stream cipher.

    The cipher processes 16-bit words with:
    - A feedback IV that resets every 16 words (32 bytes)
    - Two rounds of the SP-network per word
    """
    # Work with a mutable copy
    data = bytearray(encrypted_data)

    # Ensure even length
    if len(data) % 2 != 0:
        data.append(0)

    output = bytearray(len(data))
    counter = 0
    iv = 0

    for offset in range(0, len(data), 2):
        # Read encrypted word (little-endian)
        enc = data[offset] | (data[offset + 1] << 8)

        # Decrypt
        dec = iv
        iv = one_round[(enc ^ iv) ^ subkey1] ^ subkey1
        dec ^= one_round[iv ^ subkey2] ^ subkey2

        # Write decrypted word (little-endian)
        output[offset] = dec & 0xFF
        output[offset + 1] = (dec >> 8) & 0xFF

        # Reset IV every 32 bytes (16 words)
        counter += 1
        if counter == 16:
            counter = 0
            iv = 0

    return bytes(output)


def verify_decryption(decrypted_data):
    """Quick sanity check - look for SH-4 code patterns in decrypted data."""
    # Count SH-4 prologues (STS.L PR,@-R15 = 0x4F22)
    prologue_count = 0
    for offset in range(0, min(len(decrypted_data), 0x800000), 2):
        word = decrypted_data[offset] | (decrypted_data[offset + 1] << 8)
        if word == 0x4F22:
            prologue_count += 1

    # Look for ASCII strings
    ascii_count = 0
    i = 0
    while i < min(len(decrypted_data), 0x100000):
        if 32 <= decrypted_data[i] < 127:
            end = i
            while end < len(decrypted_data) and 32 <= decrypted_data[end] < 127:
                end += 1
            if end - i >= 8:
                ascii_count += 1
            i = end
        else:
            i += 1

    return prologue_count, ascii_count


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <encrypted_rom> <security_chip.ic3> [output_file]")
        print(f"       {sys.argv[0]} --zip <rom.zip> [output_file]")
        sys.exit(1)

    if sys.argv[1] == '--zip':
        import zipfile
        zip_path = sys.argv[2]
        output_path = sys.argv[3] if len(sys.argv) > 3 else None

        with zipfile.ZipFile(zip_path) as z:
            names = z.namelist()
            ic3_name = [n for n in names if n.endswith('.ic3')][0]
            ic8_name = [n for n in names if 'ic8' in n.lower()][0]

            print(f"Security chip: {ic3_name}")
            print(f"ROM ic8: {ic8_name}")

            ic3_data = z.read(ic3_name)
            rom_data = z.read(ic8_name)
    else:
        rom_path = sys.argv[1]
        ic3_path = sys.argv[2]
        output_path = sys.argv[3] if len(sys.argv) > 3 else None

        with open(ic3_path, 'rb') as f:
            ic3_data = f.read()
        with open(rom_path, 'rb') as f:
            rom_data = f.read()

    print(f"ROM size: {len(rom_data):,} bytes")
    print(f"Chip size: {len(ic3_data):,} bytes")

    # Extract subkeys
    subkey1, subkey2 = extract_subkeys(ic3_data)
    print(f"\nSubkeys extracted:")
    print(f"  subkey1 = 0x{subkey1:04X}")
    print(f"  subkey2 = 0x{subkey2:04X}")

    # Build lookup table
    print("\nBuilding one-round lookup table (65536 entries)...")
    one_round = build_one_round_table()

    # Decrypt the ROM
    # Only the first 8MB is typically encrypted on M4 boards
    # The header at 0x800000 and filesystem after it are unencrypted
    encrypt_end = min(0x800000, len(rom_data))

    print(f"Decrypting first {encrypt_end // (1024*1024)}MB of ROM...")
    decrypted_region = decrypt_rom(rom_data[:encrypt_end], subkey1, subkey2, one_round)

    # Verify decryption
    prologues, strings = verify_decryption(decrypted_region)
    print(f"\nDecryption verification:")
    print(f"  SH-4 prologues found: {prologues}")
    print(f"  ASCII strings found: {strings}")

    if prologues > 50:
        print("  -> Looks like valid SH-4 code! Decryption likely successful.")
    elif prologues > 10:
        print("  -> Some code detected. Might be partially correct.")
    else:
        print("  -> Very few prologues. Decryption may have failed.")
        print("     (Keys might be wrong, or encrypted region differs)")

    # Show first few strings found
    print("\nFirst strings found in decrypted data:")
    found = 0
    i = 0
    while i < len(decrypted_region) and found < 10:
        if 32 <= decrypted_region[i] < 127:
            end = i
            while end < len(decrypted_region) and 32 <= decrypted_region[end] < 127:
                end += 1
            if end - i >= 8:
                s = decrypted_region[i:end].decode('ascii', errors='replace')
                print(f"  0x{i:08X}: \"{s[:60]}\"")
                found += 1
            i = end
        else:
            i += 1

    # Combine decrypted region with rest of ROM
    full_output = decrypted_region + rom_data[encrypt_end:]

    if output_path:
        with open(output_path, 'wb') as f:
            f.write(full_output)
        print(f"\nDecrypted ROM saved to {output_path} ({len(full_output):,} bytes)")
    else:
        # Default output name
        output_path = "decrypted_ic8.bin"
        with open(output_path, 'wb') as f:
            f.write(full_output)
        print(f"\nDecrypted ROM saved to {output_path}")


if __name__ == '__main__':
    main()
