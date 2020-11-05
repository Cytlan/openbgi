#
# Based on https://axcheron.github.io/code-injection-with-python/
#
import pefile
import mmap
import os
import math

def align(val_to_align, alignment):
    return math.floor((val_to_align + alignment - 1) / alignment) * alignment

size_to_add = 0x2000
exe_path = "downloader/data/tayutama/tayutama.exe"

#in_file = open("patch", "rb")
#shellcode = in_file.read()
#in_file.close()

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
print("\t[+] Section Name = %s" % name)
# Set the virtual size
pe.set_dword_at_offset(new_section_offset + 8, virtual_size)
print("\t[+] Virtual Size = %s" % hex(virtual_size))
# Set the virtual offset
pe.set_dword_at_offset(new_section_offset + 12, virtual_offset)
print("\t[+] Virtual Offset = %s" % hex(virtual_offset))
# Set the raw size
pe.set_dword_at_offset(new_section_offset + 16, raw_size)
print("\t[+] Raw Size = %s" % hex(raw_size))
# Set the raw offset
pe.set_dword_at_offset(new_section_offset + 20, raw_offset)
print("\t[+] Raw Offset = %s" % hex(raw_offset))
# Set the following fields to zero
pe.set_bytes_at_offset(new_section_offset + 24, bytes(12 * '\x00', 'ascii'))
# Set the characteristics
pe.set_dword_at_offset(new_section_offset + 36, characteristics)
print("\t[+] Characteristics = %s\n" % hex(characteristics))

# STEP 0x03 - Modify the Main Headers
print("[*] STEP 0x03 - Modify the Main Headers")
pe.FILE_HEADER.NumberOfSections += 1
print("\t[+] Number of Sections = %s" % pe.FILE_HEADER.NumberOfSections)
pe.OPTIONAL_HEADER.SizeOfImage = virtual_size + virtual_offset
print("\t[+] Size of Image = %d bytes" % pe.OPTIONAL_HEADER.SizeOfImage)

pe.write(exe_path)

pe = pefile.PE(exe_path)

# STEP 0x04 - Inject the Shellcode in the New Section
print("[*] STEP 0x04 - Inject the Shellcode in the New Section")

raw_offset = pe.sections[last_section].PointerToRawData
pe.set_bytes_at_offset(raw_offset, shellcode)
print("\t[+] Shellcode wrote in the new section")

pe.write(exe_path)
