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

exe_path = sys.argv[1]
bin_path = sys.argv[2]
offset = int(sys.argv[3], 0)

in_file = open(bin_path, "rb")
shellcode = in_file.read()
in_file.close()

pe = pefile.PE(exe_path)
number_of_section = pe.FILE_HEADER.NumberOfSections
last_section = number_of_section - 1
file_alignment = pe.OPTIONAL_HEADER.FileAlignment
section_alignment = pe.OPTIONAL_HEADER.SectionAlignment

print("Patching %s at address %s" % (bin_path, hex(offset-pe.OPTIONAL_HEADER.ImageBase)))
binstr = ""
for byte in shellcode:
    binstr += "%02x " % byte
print(binstr)

pe.set_bytes_at_rva(offset-pe.OPTIONAL_HEADER.ImageBase, shellcode)

pe.write(exe_path)
