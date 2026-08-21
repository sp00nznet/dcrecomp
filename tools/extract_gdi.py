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
    """Read user data from a sector.

    `f` is a Disc, addressed by absolute LBA. Kept as a free function so the
    rest of the file reads unchanged.
    """
    if isinstance(f, Disc):
        return f.read_sector(sector_num)
    f.seek(sector_num * SECTOR_SIZE_RAW + SECTOR_DATA_OFFSET)
    return f.read(SECTOR_DATA_SIZE)


class Disc:
    """Absolute-LBA view over the per-track .bin files chdman extractcd emits.

    Tracks are laid out in two areas: the single-density tracks start at LBA 0,
    the high-density area starts at LBA 45000. Every track consumes LBA space,
    audio included, so a file's absolute LBA has to be resolved against the
    whole layout rather than against one track.
    """

    def __init__(self, path):
        self.tracks = []            # (start_lba, sectors, file handle or None)
        if path.lower().endswith('.cue'):
            self._load_cue(path)
        else:
            # A bare track file: assume it is the high-density data track.
            f = open(path, 'rb')
            n = os.path.getsize(path) // SECTOR_SIZE_RAW
            self.tracks.append([GD_ROM_HD_START, n, f])

    def _load_cue(self, cue_path):
        base = os.path.dirname(os.path.abspath(cue_path))
        entries, high = [], False
        pending_file, pregap = None, 0
        for line in open(cue_path, 'r', errors='replace'):
            t = line.strip()
            up = t.upper()
            if up.startswith('REM') and 'HIGH-DENSITY' in up:
                high = True
            elif up.startswith('FILE'):
                pending_file = t.split('"')[1]
            elif up.startswith('TRACK'):
                entries.append({'file': pending_file, 'high': high,
                                'audio': 'AUDIO' in up, 'pregap': 0})
            elif up.startswith('PREGAP') and entries:
                mm, ss, ff = (int(x) for x in t.split()[1].split(':'))
                entries[-1]['pregap'] = (mm * 60 + ss) * 75 + ff

        lba = 0
        for e in entries:
            if e['high'] and lba < GD_ROM_HD_START:
                lba = GD_ROM_HD_START
            lba += e['pregap']
            full = os.path.join(base, e['file'])
            n = os.path.getsize(full) // SECTOR_SIZE_RAW
            f = None if e['audio'] else open(full, 'rb')
            self.tracks.append([lba, n, f])
            lba += n

    def read_sector(self, abs_lba):
        for start, n, f in self.tracks:
            if f is not None and start <= abs_lba < start + n:
                f.seek((abs_lba - start) * SECTOR_SIZE_RAW + SECTOR_DATA_OFFSET)
                return f.read(SECTOR_DATA_SIZE)
        return b''

    def describe(self):
        out = []
        for i, (start, n, f) in enumerate(self.tracks, 1):
            kind = 'data' if f else 'audio'
            out.append(f"  track {i:2d}: LBA {start:>7,}..{start + n - 1:>7,}  {kind}")
        return chr(10).join(out)


def parse_ip_bin(f):
    """Parse the Dreamcast IP.BIN header."""
    data = read_sector(f, GD_ROM_HD_START)
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
    """Identity: Disc.read_sector takes absolute LBAs and finds the track."""
    return abs_lba

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

    written = 0
    with open(output_path, 'wb') as out:
        remaining = size
        sector = abs_to_rel(abs_lba)
        while remaining > 0:
            data = read_sector(f, sector)
            if not data:
                break          # ran off the end of the mapped tracks
            write_size = min(remaining, SECTOR_DATA_SIZE, len(data))
            out.write(data[:write_size])
            written += write_size
            remaining -= write_size
            sector += 1

    return written

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
    disc_path = sys.argv[1] if len(sys.argv) > 1 else "disc.cue"
    output_dir = sys.argv[2] if len(sys.argv) > 2 else "disc_extract"
    os.makedirs(output_dir, exist_ok=True)

    # Prefer the cue: a disc with CD audio spreads its high-density area over
    # several tracks and the payload is usually not in track 3.
    disc = Disc(disc_path)
    print("=== Disc layout ===")
    print(disc.describe())

    if True:
        f = disc
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
        pvd = read_sector(f, GD_ROM_HD_START + ISO_PVD_SECTOR)
        if pvd[1:6] != b'CD001':
            print("ERROR: No valid ISO9660 PVD found!")
            sys.exit(1)

        volume_id = pvd[40:72].decode('ascii', errors='replace').strip()
        root_lba = struct.unpack_from('<I', pvd, 156 + 2)[0]
        root_size = struct.unpack_from('<I', pvd, 156 + 10)[0]

        print(f"\n=== ISO9660 Filesystem ===")
        print(f"  Volume: {volume_id}")
        print(f"  Root directory: LBA {root_lba}")

        # Extract everything
        print(f"\n=== Extracting files to {output_dir}/ ===")
        os.makedirs(output_dir, exist_ok=True)
        total = extract_directory(f, root_lba, root_size, output_dir)
        print(f"\nDone! Extracted {total:,} bytes total")

if __name__ == '__main__':
    main()
