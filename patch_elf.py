#!/usr/bin/env python3
import sys
import struct

def patch_verneed(filepath):
    print(f"[Patch] Processing {filepath} ...")
    with open(filepath, 'r+b') as f:
        data = bytearray(f.read())
        
        # Check ELF magic
        if data[:4] != b'\x7fELF':
            print("[Patch] Not an ELF file!")
            return False
        
        is_32bit = (data[4] == 1)
        endian = '<' if data[5] == 1 else '>'
        
        if not is_32bit:
            print("[Patch] Not a 32-bit ELF!")
            return False
            
        # Read section header table
        e_shoff = struct.unpack_from(endian + 'I', data, 32)[0]
        e_shentsize = struct.unpack_from(endian + 'H', data, 46)[0]
        e_shnum = struct.unpack_from(endian + 'H', data, 48)[0]
        
        # Find .dynamic section and .init_array
        dynamic_offset = 0
        dynamic_size = 0
        init_array_offset = 0
        init_array_size = 0
        
        for i in range(e_shnum):
            sh_offset = e_shoff + i * e_shentsize
            sh_type = struct.unpack_from(endian + 'I', data, sh_offset + 4)[0]
            sh_addr = struct.unpack_from(endian + 'I', data, sh_offset + 12)[0]
            sh_file_offset = struct.unpack_from(endian + 'I', data, sh_offset + 16)[0]
            sh_size = struct.unpack_from(endian + 'I', data, sh_offset + 20)[0]
            
            if sh_type == 6: # SHT_DYNAMIC
                dynamic_offset = sh_file_offset
                dynamic_size = sh_size
            elif sh_type == 14: # SHT_INIT_ARRAY
                init_array_offset = sh_file_offset
                init_array_size = sh_size
                
        if dynamic_offset == 0:
            print("[Patch] .dynamic section not found!")
            return False
            
        print(f"[Patch] Found .dynamic at offset 0x{dynamic_offset:x}, size {dynamic_size}")
        
        # Iterate over Elf32_Dyn entries (8 bytes each: 4 bytes tag, 4 bytes val/ptr)
        num_entries = dynamic_size // 8
        patched_count = 0
        for i in range(num_entries):
            entry_offset = dynamic_offset + i * 8
            d_tag = struct.unpack_from(endian + 'i', data, entry_offset)[0]
            
            # DT_VERNEED = 0x6ffffffe, DT_VERNEEDNUM = 0x6fffffff, DT_VERSYM = 0x6ffffff0
            if d_tag in (0x6ffffffe, 0x6fffffff, 0x6ffffff0):
                print(f"[Patch] Neutralizing dynamic tag 0x{d_tag:08x} at offset 0x{entry_offset:x}")
                struct.pack_into(endian + 'i', data, entry_offset, 0)
                struct.pack_into(endian + 'I', data, entry_offset + 4, 0)
                patched_count += 1
            # DT_INIT_ARRAYSZ = 0x1b
            elif d_tag == 0x1b:
                val = struct.unpack_from(endian + 'I', data, entry_offset + 4)[0]
                if val == 508:
                    print(f"[Patch] Adjusting DT_INIT_ARRAYSZ from {val} to 504 bytes (126 constructors)")
                    struct.pack_into(endian + 'I', data, entry_offset + 4, 504)
                    patched_count += 1
                    
        # Also fix the trailing 0 in init_array by pointing to a valid no-op/ret or setting to ctor 125
        if init_array_offset > 0:
            last_slot_offset = init_array_offset + 504
            last_slot_val = struct.unpack_from(endian + 'I', data, last_slot_offset)[0]
            if last_slot_val == 0:
                dummy_ret = 0x00993621 # Address of 'ret' instruction
                print(f"[Patch] Replacing 0x00000000 at init_array[126] with 'ret' address 0x{dummy_ret:08x}")
                struct.pack_into(endian + 'I', data, last_slot_offset, dummy_ret)
                
        f.seek(0)
        f.write(data)
        print(f"[Patch] Successfully applied patches to {filepath}")
        return True

if __name__ == '__main__':
    target = sys.argv[1] if len(sys.argv) > 1 else 'bin/libAngryBirdsClassic.so'
    patch_verneed(target)
