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

size_to_add = int(sys.argv[2], 0)
exe_path = sys.argv[1]

# Get file alignment - Needed to produce the correct size executable
pe = pefile.PE(exe_path)
file_alignment = pe.OPTIONAL_HEADER.FileAlignment

# Resize the file
original_size = os.path.getsize(exe_path)
fd = open(exe_path, 'a+b')
map = mmap.mmap(fd.fileno(), 0, access=mmap.ACCESS_WRITE)
map.resize(original_size + align(size_to_add, file_alignment))
map.close()
fd.close()

# Get PE info
pe = pefile.PE(exe_path)
number_of_section = pe.FILE_HEADER.NumberOfSections
last_section = number_of_section - 1
file_alignment = pe.OPTIONAL_HEADER.FileAlignment
section_alignment = pe.OPTIONAL_HEADER.SectionAlignment
new_section_offset = (pe.sections[number_of_section - 1].get_file_offset() + 40)

# Look for valid values for the new section header
raw_size = align(size_to_add, file_alignment)
virtual_size = align(size_to_add, section_alignment)
raw_offset = align((pe.sections[last_section].PointerToRawData +
                    pe.sections[last_section].SizeOfRawData),
                   file_alignment)

virtual_offset = align((pe.sections[last_section].VirtualAddress +
                        pe.sections[last_section].Misc_VirtualSize),
                       section_alignment)

# CODE | EXECUTE | READ | WRITE
characteristics = 0xE0000020
# Section name must be equal to 8 bytes
name = ".patch\x00\x00"

# Create the section
# Set the name
pe.set_bytes_at_offset(new_section_offset, bytes(name, 'ascii'))
# Set the virtual size
pe.set_dword_at_offset(new_section_offset + 8, virtual_size)
# Set the virtual offset
pe.set_dword_at_offset(new_section_offset + 12, virtual_offset)
# Set the raw size
pe.set_dword_at_offset(new_section_offset + 16, raw_size)
# Set the raw offset
pe.set_dword_at_offset(new_section_offset + 20, raw_offset)
# Set the following fields to zero
pe.set_bytes_at_offset(new_section_offset + 24, bytes(12 * '\x00', 'ascii'))
# Set the characteristics
pe.set_dword_at_offset(new_section_offset + 36, characteristics)

# STEP 0x03 - Modify the Main Headers
pe.FILE_HEADER.NumberOfSections += 1
pe.OPTIONAL_HEADER.SizeOfImage = virtual_size + virtual_offset

# Print offset to new section
print("%s" % hex(virtual_offset + pe.OPTIONAL_HEADER.ImageBase))

pe.write(exe_path)

pe = pefile.PE(exe_path)
