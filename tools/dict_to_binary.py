#!/usr/bin/env python3
"""
Dictionary to Binary Converter
Converts C dictionary arrays to compact binary format

Binary Format:
--------------
Header (32 bytes):
    - Magic:     4 bytes "EDCT" (espeak Dictionary)
    - Version:   4 bytes (0x00010000 = v1.0)
    - Entries:   4 bytes (number of entries)
    - Flags:     4 bytes (compression, etc.)
    - StrPool:   4 bytes (string pool size)
    - Reserved:  12 bytes (future use)

Entry Table (16 bytes per entry):
    - word_offset:     4 bytes (offset into string pool)
    - phoneme_offset:  4 bytes (offset into string pool)
    - flags:           4 bytes (entry flags)
    - reserved:        4 bytes (alignment/future)

String Pool:
    - Null-terminated UTF-8 strings
    - Deduplicated (same string = same offset)

Optional LZ4 Compression:
    - Compresses String Pool only
    - Header + Entry Table remain uncompressed for fast access
"""

import struct
import sys
import re
from pathlib import Path
from typing import List, Tuple, Dict
from collections import defaultdict

# Binary format constants
MAGIC = b"EDCT"
VERSION = 0x00010000
FLAG_COMPRESSED = 0x00000001

class DictEntry:
    """Single dictionary entry"""
    def __init__(self, word: str, phonemes: str, flags: int):
        self.word = word
        self.phonemes = phonemes
        self.flags = flags

class StringPool:
    """Deduplicated string pool"""
    def __init__(self):
        self.strings: Dict[str, int] = {}  # string -> offset
        self.data = bytearray()
        
    def add(self, s: str) -> int:
        """Add string and return offset"""
        if s in self.strings:
            return self.strings[s]
        
        offset = len(self.data)
        self.strings[s] = offset
        self.data.extend(s.encode('utf-8'))
        self.data.append(0)  # null terminator
        return offset
    
    def get_data(self) -> bytes:
        return bytes(self.data)

def parse_c_dict(c_file: Path) -> Tuple[str, List[DictEntry]]:
    """Parse C dictionary array file"""
    content = c_file.read_text(encoding='utf-8')
    
    # Extract language code from filename (e.g., "en_dict.c" -> "en")
    lang_code = c_file.stem.replace('_dict', '')
    
    # Find array declaration: const espyak_dict_entry_embedded_t DICT_XX[N] = {
    array_pattern = r'const\s+espyak_dict_entry_embedded_t\s+DICT_(\w+)\[\d+\]\s*='
    match = re.search(array_pattern, content)
    if not match:
        raise ValueError(f"Could not find dictionary array in {c_file}")
    
    dict_name = match.group(1).lower()
    
    # Extract entries: {"word", "phonemes", flags},
    entry_pattern = r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*(\d+)\s*\}'
    entries = []
    
    for match in re.finditer(entry_pattern, content):
        word = match.group(1)
        phonemes = match.group(2)
        flags = int(match.group(3))
        entries.append(DictEntry(word, phonemes, flags))
    
    if not entries:
        raise ValueError(f"No entries found in {c_file}")
    
    return dict_name, entries

def create_binary_dict(lang_code: str, entries: List[DictEntry], compress: bool = False) -> bytes:
    """Create binary dictionary"""
    
    # CRITICAL: Sort entries by word (case-insensitive) for binary search!
    # This enables O(log n) lookup in espyak_dictionary.c
    entries.sort(key=lambda e: e.word.lower())
    
    # Build string pool
    pool = StringPool()
    entry_offsets = []
    
    for entry in entries:
        word_off = pool.add(entry.word)
        phon_off = pool.add(entry.phonemes)
        entry_offsets.append((word_off, phon_off, entry.flags))
    
    pool_data = pool.get_data()
    
    # Optional compression (placeholder - would use python-lz4)
    flags = 0
    if compress:
        try:
            import lz4.frame
            pool_data = lz4.frame.compress(pool_data)
            flags |= FLAG_COMPRESSED
        except ImportError:
            print("Warning: lz4 not available, skipping compression", file=sys.stderr)
            compress = False
    
    # Build binary
    output = bytearray()
    
    # Header (32 bytes)
    output.extend(MAGIC)
    output.extend(struct.pack('<I', VERSION))
    output.extend(struct.pack('<I', len(entries)))
    output.extend(struct.pack('<I', flags))
    output.extend(struct.pack('<I', len(pool_data)))
    output.extend(b'\x00' * 12)  # reserved
    
    # Entry table (16 bytes per entry)
    for word_off, phon_off, entry_flags in entry_offsets:
        output.extend(struct.pack('<I', word_off))
        output.extend(struct.pack('<I', phon_off))
        output.extend(struct.pack('<I', entry_flags))
        output.extend(struct.pack('<I', 0))  # reserved
    
    # String pool
    output.extend(pool_data)
    
    return bytes(output)

def convert_dict(input_file: Path, output_file: Path, compress: bool = False):
    """Convert C dict to binary"""
    print(f"Converting {input_file.name}...", end=" ", flush=True)
    
    try:
        lang_code, entries = parse_c_dict(input_file)
        binary = create_binary_dict(lang_code, entries, compress)
        
        output_file.write_bytes(binary)
        
        # Stats
        c_size = input_file.stat().st_size
        bin_size = len(binary)
        ratio = (1 - bin_size / c_size) * 100
        
        print(f"OK {len(entries):,} entries | {c_size:,}B -> {bin_size:,}B ({ratio:.1f}% smaller)")
        
    except Exception as e:
        print(f"ERROR: {e}")
        return False
    
    return True

def main():
    import argparse
    
    parser = argparse.ArgumentParser(description="Convert dictionary C arrays to binary format")
    parser.add_argument('input', type=Path, help="Input C file or directory")
    parser.add_argument('-o', '--output', type=Path, help="Output binary file or directory")
    parser.add_argument('-c', '--compress', action='store_true', help="Enable LZ4 compression")
    parser.add_argument('-a', '--all', action='store_true', help="Convert all dictionaries in directory")
    
    args = parser.parse_args()
    
    if args.all or args.input.is_dir():
        # Batch convert
        input_dir = args.input
        output_dir = args.output or (input_dir.parent / "bin")
        output_dir.mkdir(exist_ok=True)
        
        dict_files = sorted(input_dir.glob("*_dict.c"))
        print(f"Converting {len(dict_files)} dictionaries to {output_dir}/")
        print("=" * 70)
        
        success = 0
        for c_file in dict_files:
            bin_file = output_dir / f"{c_file.stem.replace('_dict', '')}.dictbin"
            if convert_dict(c_file, bin_file, args.compress):
                success += 1
        
        print("=" * 70)
        print(f"Converted {success}/{len(dict_files)} dictionaries successfully")
        
    else:
        # Single file
        input_file = args.input
        output_file = args.output or input_file.with_suffix('.dictbin')
        
        if convert_dict(input_file, output_file, args.compress):
            sys.exit(0)
        else:
            sys.exit(1)

if __name__ == '__main__':
    main()
