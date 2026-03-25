#!/usr/bin/env python3
"""
Extract Naomi arcade ROM board contents.

Naomi ROM boards use flash ROM chips (ic8, ic9, etc.) instead of GD-ROM discs.
The ROM contains a Naomi header (same format as DC IP.BIN) at a known offset,
followed by an embedded ISO9660 filesystem with the game executable and data.

ROM layout (typical):
  ic8[0x000000-0x7FFFFF]  : Asset/encrypted data (8MB)
  ic8[0x800000-0x8000FF]  : Naomi header (IP.BIN equivalent)
  ic8[0x800100]           : TOC
  ic8[0x808000+]          : ISO9660 filesystem (CD001)
  ic9[0x000000+]          : Additional game data

Usage:
  python extract_naomi_rom.py <ic8_file> [ic9_file] [output_dir]
"""

import os
import sys
import struct
import json

# ISO9660 constants
ISO_PVD_SECTOR = 16
SECTOR_SIZE = 2048

# Naomi header location
NAOMI_HEADER_OFFSET = 0x800000
NAOMI_HEADER_SIZE = 256


def find_naomi_header(data):
    """Search for the Naomi/DC header ('SEGA' signature) in the ROM."""
    # Try known offsets first
    for offset in [0x800000, 0x000000, 0x400000, 0x1000000]:
        if offset + 16 <= len(data):
            if data[offset:offset+4] == b'SEGA':
                return offset

    # Brute force search
    idx = data.find(b'SEGA SEGAKATANA')
    if idx >= 0:
        return idx

    return -1


def parse_naomi_header(data, offset):
    """Parse the Naomi IP.BIN-style header."""
    hdr = data[offset:offset + NAOMI_HEADER_SIZE]
    return {
        'offset': f'0x{offset:08X}',
        'hardware_id':   hdr[0x00:0x10].decode('ascii', errors='replace').strip(),
        'maker_id':      hdr[0x10:0x20].decode('ascii', errors='replace').strip(),
        'device_info':   hdr[0x20:0x30].decode('ascii', errors='replace').strip(),
        'region':        hdr[0x30:0x38].decode('ascii', errors='replace').strip(),
        'product_no':    hdr[0x38:0x40].decode('ascii', errors='replace').strip(),
        'version':       hdr[0x40:0x50].decode('ascii', errors='replace').strip(),
        'date':          hdr[0x50:0x60].decode('ascii', errors='replace').strip(),
        'boot_filename': hdr[0x60:0x70].decode('ascii', errors='replace').strip(),
        'description':   hdr[0x80:0x100].decode('ascii', errors='replace').strip(),
    }


def find_iso9660(data, start_offset):
    """Find the ISO9660 Primary Volume Descriptor in the ROM data."""
    # Search for CD001 signature starting from after the header
    search_start = start_offset
    while search_start < len(data) - 6:
        idx = data.find(b'\x01CD001', search_start)
        if idx < 0:
            break
        # PVD starts 1 byte before 'CD001'
        pvd_offset = idx
        return pvd_offset
        search_start = idx + 1

    return -1


def parse_directory_record(data, offset):
    """Parse a single ISO9660 directory record."""
    if offset >= len(data):
        return None, 0

    record_len = data[offset]
    if record_len == 0:
        return None, 0
    if offset + record_len > len(data):
        return None, 0

    record = data[offset:offset + record_len]
    entry = {
        'record_length': record_len,
        'extent_lba': struct.unpack_from('<I', record, 2)[0],
        'data_length': struct.unpack_from('<I', record, 10)[0],
        'flags': record[25],
        'is_directory': bool(record[25] & 0x02),
    }

    name_len = record[32]
    raw_name = record[33:33 + name_len] if name_len > 0 else b''
    if name_len == 1 and raw_name == b'\x00':
        entry['name'] = '.'
    elif name_len == 1 and raw_name == b'\x01':
        entry['name'] = '..'
    else:
        entry['name'] = raw_name.decode('ascii', errors='replace').split(';')[0]

    return entry, record_len


def list_directory(rom_data, base_offset, abs_lba, size):
    """List all entries in an ISO9660 directory."""
    entries = []
    # Calculate actual byte offset: LBA is relative to the ISO volume
    # On Naomi ROMs, LBAs are relative to the start of the ISO within the ROM
    byte_offset = base_offset + (abs_lba * SECTOR_SIZE) - (base_offset // SECTOR_SIZE * SECTOR_SIZE)

    # Actually, the LBAs in the ISO directory entries are absolute sector numbers
    # relative to the start of the ISO image. The ISO starts at base_offset in the ROM.
    # So the actual byte offset = base_offset + (lba - first_lba) * SECTOR_SIZE
    # But we need to figure out what first_lba maps to base_offset.

    # The PVD is at sector 16 of the ISO, and we found it at base_offset in the ROM.
    # So sector 16 = base_offset, meaning sector 0 = base_offset - 16*2048
    iso_start = base_offset - (ISO_PVD_SECTOR * SECTOR_SIZE)

    byte_offset = iso_start + abs_lba * SECTOR_SIZE
    if byte_offset < 0 or byte_offset >= len(rom_data):
        print(f"  WARNING: directory LBA {abs_lba} maps to offset 0x{byte_offset:X} (out of range)")
        return entries

    sectors_needed = (size + SECTOR_SIZE - 1) // SECTOR_SIZE
    dir_data = rom_data[byte_offset:byte_offset + sectors_needed * SECTOR_SIZE]

    offset = 0
    while offset < size and offset < len(dir_data):
        entry, record_len = parse_directory_record(dir_data, offset)
        if entry is None:
            next_sector = ((offset // SECTOR_SIZE) + 1) * SECTOR_SIZE
            if next_sector >= size:
                break
            offset = next_sector
            continue
        # Store the ISO start so we can extract files
        entry['_iso_start'] = iso_start
        entries.append(entry)
        offset += record_len

    return entries


def extract_file(rom_data, iso_start, abs_lba, size, output_path):
    """Extract a file from the ROM using its ISO9660 extent."""
    os.makedirs(os.path.dirname(output_path) or '.', exist_ok=True)

    byte_offset = iso_start + abs_lba * SECTOR_SIZE
    if byte_offset < 0 or byte_offset + size > len(rom_data):
        print(f"  WARNING: file at LBA {abs_lba} (0x{byte_offset:X}) size {size} out of range")
        return 0

    with open(output_path, 'wb') as out:
        out.write(rom_data[byte_offset:byte_offset + size])

    return size


def extract_directory(rom_data, base_offset, abs_lba, size, output_dir, prefix=""):
    """Recursively extract all files from an ISO9660 directory."""
    entries = list_directory(rom_data, base_offset, abs_lba, size)
    total_bytes = 0

    for entry in entries:
        if entry['name'] in ('.', '..', ''):
            continue

        path = os.path.join(prefix, entry['name']) if prefix else entry['name']
        iso_start = entry.get('_iso_start', base_offset - ISO_PVD_SECTOR * SECTOR_SIZE)

        if entry['is_directory']:
            print(f"  [DIR]  {path}/")
            os.makedirs(os.path.join(output_dir, path), exist_ok=True)
            total_bytes += extract_directory(rom_data, base_offset,
                                            entry['extent_lba'], entry['data_length'],
                                            output_dir, path)
        else:
            output_path = os.path.join(output_dir, path)
            sz = extract_file(rom_data, iso_start, entry['extent_lba'],
                            entry['data_length'], output_path)
            total_bytes += sz
            print(f"  [FILE] {path:30s}  {sz:>12,} bytes")

    return total_bytes


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <ic8_file> [ic9_file] [output_dir]")
        print(f"  Extracts Naomi ROM board contents.")
        print(f"  Also accepts .zip files containing ic8/ic9 ROM chips.")
        sys.exit(1)

    ic8_path = sys.argv[1]
    ic9_path = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('-') else None
    output_dir = sys.argv[-1] if len(sys.argv) > 2 else "extracted"

    # Handle zip files
    if ic8_path.endswith('.zip'):
        import zipfile
        print(f"Opening ZIP: {ic8_path}")
        with zipfile.ZipFile(ic8_path) as z:
            names = z.namelist()
            ic8_name = [n for n in names if 'ic8' in n.lower()]
            ic9_name = [n for n in names if 'ic9' in n.lower()]

            if not ic8_name:
                print("ERROR: No ic8 file found in ZIP!")
                sys.exit(1)

            print(f"  ic8: {ic8_name[0]}")
            ic8_data = z.read(ic8_name[0])

            if ic9_name:
                print(f"  ic9: {ic9_name[0]}")
                ic9_data = z.read(ic9_name[0])
            else:
                ic9_data = b''
    else:
        print(f"Loading ic8: {ic8_path}")
        with open(ic8_path, 'rb') as f:
            ic8_data = f.read()

        if ic9_path:
            print(f"Loading ic9: {ic9_path}")
            with open(ic9_path, 'rb') as f:
                ic9_data = f.read()
        else:
            ic9_data = b''

    print(f"ic8 size: {len(ic8_data):,} bytes ({len(ic8_data) // (1024*1024)} MB)")
    if ic9_data:
        print(f"ic9 size: {len(ic9_data):,} bytes ({len(ic9_data) // (1024*1024)} MB)")

    # Combine into full ROM address space
    rom_data = ic8_data + ic9_data
    print(f"Total ROM: {len(rom_data):,} bytes ({len(rom_data) // (1024*1024)} MB)")

    # Find and parse Naomi header
    header_offset = find_naomi_header(rom_data)
    if header_offset < 0:
        print("ERROR: No Naomi header (SEGA signature) found!")
        sys.exit(1)

    print(f"\n=== Naomi Header at 0x{header_offset:08X} ===")
    header = parse_naomi_header(rom_data, header_offset)
    for key, value in header.items():
        print(f"  {key:16s}: {value}")

    # Find ISO9660 filesystem
    pvd_offset = find_iso9660(rom_data, header_offset)
    if pvd_offset < 0:
        print("\nWARNING: No ISO9660 filesystem found in ROM!")
        print("This ROM may use a different filesystem or direct mapping.")
        # Try to extract raw bootstrap code instead
        boot_offset = header_offset + NAOMI_HEADER_SIZE + 0x100  # After header + TOC
        print(f"\nExtracting raw boot code from 0x{boot_offset:08X}...")
        os.makedirs(output_dir, exist_ok=True)
        # Save a large chunk as potential 1ST_READ.BIN
        boot_data = rom_data[boot_offset:boot_offset + 2*1024*1024]
        boot_path = os.path.join(output_dir, '1ST_READ.BIN')
        with open(boot_path, 'wb') as f:
            f.write(boot_data)
        print(f"  Saved {len(boot_data):,} bytes to {boot_path}")
    else:
        pvd = rom_data[pvd_offset:pvd_offset + SECTOR_SIZE]
        volume_id = pvd[40:72].decode('ascii', errors='replace').strip()
        root_lba = struct.unpack_from('<I', pvd, 156 + 2)[0]
        root_size = struct.unpack_from('<I', pvd, 156 + 10)[0]

        print(f"\n=== ISO9660 Filesystem at 0x{pvd_offset:08X} ===")
        print(f"  Volume:  {volume_id}")
        print(f"  Root:    LBA {root_lba}, size {root_size}")

        # Extract everything
        print(f"\n=== Extracting to {output_dir}/ ===")
        os.makedirs(output_dir, exist_ok=True)
        total = extract_directory(rom_data, pvd_offset, root_lba, root_size, output_dir)
        print(f"\nExtracted {total:,} bytes total")

    # Save header info
    info_path = os.path.join(output_dir, '_naomi_header.json')
    with open(info_path, 'w') as f:
        json.dump(header, f, indent=2)
    print(f"\nHeader info saved to {info_path}")

    # Also dump the raw bootstrap code region (useful for analysis)
    boot_start = header_offset + 0x300  # Approximate start of SH-4 boot code
    boot_end = header_offset + 0x8000   # ~32KB of bootstrap
    boot_path = os.path.join(output_dir, '_bootstrap.bin')
    with open(boot_path, 'wb') as f:
        f.write(rom_data[boot_start:boot_end])
    print(f"Bootstrap code saved to {boot_path} ({boot_end - boot_start:,} bytes)")


if __name__ == '__main__':
    main()
