#!/bin/bash
#
# This script is for patching Tayumata to allow us to dump
# an execution log from the Buriko engine.
#
# It's not meant to be pretty.
#

EXEDIR=../downloader/data/tayutama
EXEFILE=$EXEDIR/tayutama.exe
BACKUPFILE=$EXEDIR/tayutama_org.exe
INITJMP=0x00451f0c
EXECJMP=0x004519cd
CAVESIZE=0x2000

# Do we have a backup file?
if [ ! -f "$BACKUPFILE" ]; then
	# No. Let's make one.
	cp $EXEFILE $BACKUPFILE
fi

# Copy the backup file to the original file
echo "Copying backup file"
cp $BACKUPFILE $EXEFILE

# Build DLL
echo "Compile DLL"
i686-w64-mingw32-c++-win32 -shared -o patch.dll patch.cpp -static-libstdc++ -static-libgcc || exit 1
cp patch.dll $EXEDIR/patch.dll 

# Add a new code cave section to the executable
SECTIONADDR=$(python add_section.py $EXEFILE $CAVESIZE)
echo "Added section at $SECTIONADDR"

# Make linking map
{
	echo "SECTIONS"
	echo "{"
	echo "    .patch $SECTIONADDR :"
	echo "    {"
	echo "        patch.o (.patch)"
	echo "    }"
	echo "    .jinit $INITJMP :"
	echo "    {"
	echo "        init_jmp.o (.jinit)"
	echo "    }"
	echo "    .jexec $EXECJMP :"
	echo "    {"
	echo "        execution_jmp.o (.jexec)"
	echo "    }"
	echo "}"
} > link.map

# Assemble our files
echo "Assembling"
nasm -o patch.o -f elf32 patch.asm || exit 1
nasm -o init_jmp.o -f elf32 init_jmp.asm || exit 1
nasm -o execution_jmp.o -f elf32 execution_jmp.asm || exit 1

# Linking
echo "Linking"
ld -m elf_i386 --oformat=elf32-i386 -T link.map -o patch.tmp || exit 1

# Get flat binaries
echo "Copying"
objcopy -O binary -j .patch patch.tmp patch.bin
objcopy -O binary -j .jinit patch.tmp init_jmp.bin
objcopy -O binary -j .jexec patch.tmp execution_jmp.bin

# Call into Python to do the patching
echo "Injecting"
python inject.py $EXEFILE patch.bin $SECTIONADDR
python inject.py $EXEFILE init_jmp.bin $INITJMP
python inject.py $EXEFILE execution_jmp.bin $EXECJMP
