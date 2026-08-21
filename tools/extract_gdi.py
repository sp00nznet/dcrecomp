#!/usr/bin/env python3
"""
Extract Dreamcast GD-ROM filesystem from CUE/BIN disc image.
Handles LBA offset correction for high-density area.
"""

import os
import sys
import struct

SECTOR_SIZE_RAW = 2352
SECTOR_DATA_OFFSET = 16  # MODE1: 16 bytes sync+header
SECTOR_DATA_SIZE = 2048
GD_ROM_HD_START = 45000  # Standard GD-ROM high-density area start LBA
ISO_PVD_SECTOR = 16

def read_sector(f, sector_num):
    """Read user data from a raw MODE1/2352 sector."""
    f.seek(sector_num * SECTOR_SIZE_RAW + SECTOR_DATA_OFFSET)
    return f.read(SECTOR_DATA_SIZE)

def parse_ip_bin(f):
    """Parse the Dreamcast IP.BIN header."""
    data = read_sector(f, 0)
    return {
        'hardware_id':   data[0x00:0x10].decode('ascii', errors='replace').strip(),
        'maker_id':      data[0x10:0x20].decode('ascii', errors='replace').strip(),
        'device_info':   data[0x20:0x30].decode('ascii', errors='replace').strip(),
        'area_symbols':  data[0x30:0x38].decode('ascii', errors='replace').strip(),
        'peripherals':   data[0x38:0x40].decode('ascii', errors='replace').strip(),
        'product_no':    data[0x40:0x50].decode('ascii', errors='replace').strip(),
        'release_date':  data[0x50:0x60].decode('ascii', errors='replace').strip(),
        'boot_filename': data[0x60:0x70].decode('ascii', errors='replace').strip(),
        'sw_maker_name': data[0x70:0x80].decode('ascii', errors='replace').strip(),
        'game_title':    data[0x80:0x100].decode('ascii', errors='replace').strip(),
    }

def abs_to_rel(abs_lba):
    """Convert absolute disc LBA to track-relative sector."""
    return abs_lba - GD_ROM_HD_START

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

def list_directory(f, abs_lba, size):
    """List all entries in an ISO9660 directory."""
    entries = []
    rel_lba = abs_to_rel(abs_lba)
    sectors_needed = (size + SECTOR_DATA_SIZE - 1) // SECTOR_DATA_SIZE

    dir_data = b''
    for i in range(sectors_needed):
        dir_data += read_sector(f, rel_lba + i)

    offset = 0
    while offset < size:
        entry, record_len = parse_directory_record(dir_data, offset)
        if entry is None:
            next_sector = ((offset // SECTOR_DATA_SIZE) + 1) * SECTOR_DATA_SIZE
            if next_sector >= size:
                break
            offset = next_sector
            continue
        entries.append(entry)
        offset += record_len

    return entries

def extract_file(f, abs_lba, size, output_path):
    """Extract a file from the disc image."""
    os.makedirs(os.path.dirname(output_path) if os.path.dirname(output_path) else '.', exist_ok=True)

    rel_lba = abs_to_rel(abs_lba)
    with open(output_path, 'wb') as out:
        remaining = size
        sector = rel_lba
        while remaining > 0:
            data = read_sector(f, sector)
            write_size = min(remaining, SECTOR_DATA_SIZE)
            out.write(data[:write_size])
            remaining -= write_size
            sector += 1

    return size

def extract_directory(f, abs_lba, size, output_dir, prefix=""):
    """Recursively extract all files from a directory."""
    entries = list_directory(f, abs_lba, size)
    total_bytes = 0

    for entry in entries:
        if entry['name'] in ('.', '..', ''):
            continue

        path = os.path.join(prefix, entry['name']) if prefix else entry['name']

        if entry['is_directory']:
            print(f"  [DIR]  {path}/")
            os.makedirs(os.path.join(output_dir, path), exist_ok=True)
            total_bytes += extract_directory(f, entry['extent_lba'],
                                           entry['data_length'], output_dir, path)
        else:
            output_path = os.path.join(output_dir, path)
            sz = extract_file(f, entry['extent_lba'], entry['data_length'], output_path)
            total_bytes += sz
            print(f"  [FILE] {path:30s}  {sz:>12,} bytes")

    return total_bytes

def main():
    track3_path = sys.argv[1] if len(sys.argv) > 1 else "Crazy Taxi (USA) (Track 3).bin"
    output_dir = sys.argv[2] if len(sys.argv) > 2 else "disc_extract"
    os.makedirs(output_dir, exist_ok=True)

    file_size = os.path.getsize(track3_path)
    total_sectors = file_size // SECTOR_SIZE_RAW
    print(f"Track 3: {file_size:,} bytes ({total_sectors} sectors)")

    with open(track3_path, 'rb') as f:
        # Parse IP.BIN
        print("\n=== Dreamcast IP.BIN Header ===")
        ip_info = parse_ip_bin(f)
        for key, value in ip_info.items():
            print(f"  {key:16s}: {value}")

        # Save IP.BIN info
        with open(os.path.join(output_dir, '_ip_bin_info.txt'), 'w') as info_f:
            for key, value in ip_info.items():
                info_f.write(f"{key}: {value}\n")

        # Read PVD
        pvd = read_sector(f, ISO_PVD_SECTOR)
        if pvd[1:6] != b'CD001':
            print("ERROR: No valid ISO9660 PVD found!")
            sys.exit(1)

        volume_id = pvd[40:72].decode('ascii', errors='replace').strip()
        root_lba = struct.unpack_from('<I', pvd, 156 + 2)[0]
        root_size = struct.unpack_from('<I', pvd, 156 + 10)[0]

        print(f"\n=== ISO9660 Filesystem ===")
        print(f"  Volume: {volume_id}")
        print(f"  Root directory: LBA {root_lba} (track sector {abs_to_rel(root_lba)})")

        # Extract everything
        print(f"\n=== Extracting files to {output_dir}/ ===")
        os.makedirs(output_dir, exist_ok=True)
        total = extract_directory(f, root_lba, root_size, output_dir)
        print(f"\nDone! Extracted {total:,} bytes total")

if __name__ == '__main__':
    main()
