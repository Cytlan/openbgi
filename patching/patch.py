#
# Based on https://axcheron.github.io/code-injection-with-python/
#
import pefile
import mmap
import os
import math
import sys

def align(val_to_align, alignment):
    return math.floor((val_to_align + alignment - 1) / alignment) * alignment

def swap(val):
    return (((val & 0xFF) << 24) |
           ((val & 0xFF00) << 8) |
           ((val & 0xFF0000) >> 8) |
           ((val & 0xFF000000) >> 24))

exe_path = sys.argv[1]
offset = int(sys.argv[2], 0)
symbol = int(sys.argv[3], 0)

pe = pefile.PE(exe_path)
number_of_section = pe.FILE_HEADER.NumberOfSections
last_section = number_of_section - 1
file_alignment = pe.OPTIONAL_HEADER.FileAlignment
section_alignment = pe.OPTIONAL_HEADER.SectionAlignment

shellcode = "ff25%08x" % swap(symbol)
print("Patching %s to jmp [%s] (%s)" % (hex(offset), hex(symbol), shellcode))

bincode = bytes.fromhex(shellcode)

pe.set_bytes_at_rva(offset-pe.OPTIONAL_HEADER.ImageBase, bincode)

pe.write(exe_path)
