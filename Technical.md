Buriko General Interpreter
==========================

This document attempts to document and explain the technical aspects of the Buriko General Interpreter (BGI). It is a combined documentation and technical reverse-engineering document.

The goal of this project is to make an open source, BGI compatible engine.

This document is still a work in progress and some parts of it may not be accurate, and some parts of it only applies to the BGI engine found in the Nursery Rhyme visual novel.

We have borrowed some of the instruction names from the bgidis project and the EthornellTools project, and some assumptions regarding the engine's inner workings are also derived from these projects until we have more information.

The virtual machine
-------------------

The Buriko General Interpreter is a 32-bit stack machine.

#### Security

The engine is not properly sandboxed, and script files are able to access memory used by the engine itself. It should be fairly trivial to escape from the engine and run native code. Therefore, script files should not be considered safe.

#### Memory

The VM uses a flat memory space which is shared by both the code and the stack.

The VM operates in the memory spaces of:
* 0x00000000 to 0x00FFFFFF (gGlobalMemory - Global memory?)
* 0x10000000 to 0x12FFFFFF Local thread code and memory
* 0x40000000 to 0x40FFFFFF ????

Any attempt at accessing memory outside this region will crash the VM.

Note: It is entierly possible to access unallocated memory. Especially the 0x00000000 to 0x00FFFFFF range is dangerous, as there is no checks performed to make sure only allocated memory is accessed.

#### Base

The "base" is basically the VM's own memory, and it acts similarily to the stack, except you can't push or pull from it.

This memory space is used to store variables and other things the running code needs, and can be thought of as the running code's local scope.

The code is expected to increase the base pointer with how many bytes it wants to allocate for its own local use, and access it using the baseptr opcode.

The base is assigned the address space from 0x10000000 to 0x10FFFFFF.

#### Code

The code resides in the address space of 0x11000000 to 0x11FFFFFF.

Code space is managed like a stack, and it's possible to push and pop programs to/from the code space.

loadcode pushes a code block to the code area.
unloadcode pops the most recently added code block from the code area.

#### Memory

Local memory resides in 0x12000000 to 0x12FFFFFF. Not much is known about local memory right now.

#### Stack

The VM is a stack machine. That means for every operation, it has to either push or pop the stack. For example, if you push 0x01 and 0x01 to the stack and execute an add instruction, the add instruction will pop the 2 top-most values off the stack, add them together and finally push the result back onto the stack.

It's a bit strage to describe the arguments each operation takes due to the FILO nature of a stack, but for convenience and consistency, the data on the top of the stack is always referred to as the first argument, the element under the top element is the 2nd argument and so on.

The "top of the stack" is always the last element that was pushed onto the stack.

The stack pointer itself is inaccessible to the code running in the VM.

In addition to values that have been push'd and pop'd, the stack contain a few elements pushed by the VM itself to keep track of where we are in execution. It seems like it uses the bottom-most 8 values of the stack for this purpose.

The value in stack[1] seems to be pointing to the end of the code space. If an attempt is made to jump to an address over the address in state[1], the VM vill crash with the error message "IPにコード領域のサイズを超える値 $%X が設定されました" ("The value $%X that exceeds the size of the code area is set in the IP")

The value in stack[7] might be a pointer to the next stack frame. If the current basepointer is less than 4 bytes away from the value in stack[7], and error will be thrown: "スタック領域が不足していま" ("Insufficient stack space")

Notes:

* The bottom 8 values might not be part of the stack at all, and we may have misinterpreted the stack pointer location.
* Does the stack pointer decrement or incrememnt when something it pushed onti it? For now, the assumption is that its decremented.

#### Call stack

When calling subroutines, the return address is pushed onto a separate callstack. This stack is inaccessible to the program.

#### Registers

Like most stack machines, BGI does not utilise a lot of registers. The only useful registers to keep in mind are:

##### PC

PC stands for Program Counter, and points to the next instruction to be executed by the virtual machine. It's incremented by the number of bytes the instruction executed consumes, as expected.

You can write to this register using the jmp opcodes, but you cannot read from it.

##### SP

SP means Stack Pointer, and points to the next available slot in the stack. The stack grows upwards. That means for every item that's pushed onto the stack, the stack pointer is decremented. When pop'ing, the stack pointer is incremented.

##### BP

BP is Base Pointer, and is used for locally scoped variables variables.

#### Signed values

The VM only operates with two's complement signed 32 bit ints. That means any value >= 0x80000000 are considered negative numbers.

Values that are pushed by push8 and push16 will be sign extended once they're pushed onto the stack.

Example:
```
	push8 0x7F ; Will push 0x0000007F
	push8 0x80 ; Will push 0xFFFFFF80
	push16 0x7FFF ; Will push 0x00007FFF
	push16 0x8000 ; Will push 0xFFFF8000
```

Opcodes
-------

How to read the inputs/outputs:

The input args / output results are displayed in the order the should appear on the stack. "Top of stack" will always be the most recent element pushed onto the stack.

```
Inputs:
   1 - Arg 1 (top of stack)
   2 - Arg 2 (2nd most top of stack)
   3 - Arg 3 (3rd most top of stack)

Outputs:
   1 - Res 1 (top of stack)
   2 - Res 2 (2nd most top of stack)
   3 - Res 3 (3rd most top of stack)
```

#### 0x00 - push8

Takes 1 8-bit immediate argument and pushes it onto the stack.

Inputs:

Outputs:

#### 0x01 - push16

Takes 1 16-bit immediate argument and pushes it onto the stack.

Inputs:

Outputs:

#### 0x02 - push32

Takes 1 32-bit immediate argument and pushes it onto the stack.

Inputs:

Outputs:

#### 0x04 - baseptr

Takes a 16-bit immediate arguments, subtracts it from the base pointer and pushes the address of the resulting base pointer onto the stack.

(The base pointer itself remains unchanged)

Inputs:

Outputs:


#### 0x05 - codeptr

Takes a 2-byte immediate relative address, adds the current PC location and pushes the result the stack.

The result is thus the absolute address of the relative address.

Due to the way the VM loads code, it's nessecary to use this opcode for jumps.

#### 0x06 - codeoffset

Takes a 2-byte immediate relative address and pushes the memory address of that location onto the stack.

#### 0x08 - load

Takes an address as the first argument and the value size as the immediate argument.

This will read the value from the given memory location and push the resulting value onto the stack.

The value size argument can be as follows:
* 0x00 - 1 byte
* 0x01 - 2 bytes
* 0x02 - 4 bytes

Example:
```
	; Read a 32-bit value from base offset 0x10
	baseptr 0x10
	load 0x02
```

#### 0x09 - storepv

Takes an address as the first argument, a value as the 2nd argument and the value size as the immediate argument.

The value given will be written to the memory location by the address given.

The value size argument can be as follows:
* 0x00 - 1 byte
* 0x01 - 2 bytes
* 0x02 - 4 bytes

Anything above 0x02 will result in 0 being used for the vaule.

Example:
```
	; Store value 0xDEADBEEF at base offset 0x10
	push32 0xDEADBEEF
	baseptr 0x10
	storevp 0x02
```

#### 0x0A - storevp

Exactly the same as `0x09 storepv`, but the value/address arguments are swapped.

Example:
```
	; Store value 0xDEADBEEF at sp offset 0x10
	baseptr 0x10
	push32 0xDEADBEEF
	storepv 0x02
```

#### 0x0B - write

This opcode is currently unexplored.

#### 0x0C - storemulti

Pops multiple items off the stack and writes them to a memory location in reverse order.

It takes 2 immediate arguments: 1st argument is the size of the values on the stack (i8, i16 or i32). The 2nd argument is the number of items to pop off the stack.

Example:
```
push8 0x10
push8 0x01
push8 0x02
push8 0x03
storemulti i8, 3
```

This is the same as:
stack[0x10] = 0x01
stack[0x0A] = 0x02
stack[0x0B] = 0x03

Note: I'm not 100% sure this explanation is correct, but it is close.

Inputs:
    n - Item to write
    n-1 - Destination pointer

Outputs:

#### 0x10 - loadbase

Pushes the current base pointer value onto the stack

#### 0x11 - storebase

Takes 1 argument from the stack and stores it as the base pointer.

#### 0x14 - jmp

Takes 1 argument from the stack and stores it in the PC register. The result is that the program execution will continue from the updated PC location.

#### 0x15 - Conditional jump

This opcode is a bit special. It takes 1 immediate argument, as well as 2 arguments from the stack. The first argument is the jump location, and the second argument is the value to compare against. The immediate argument is which comparison routine to use.

The comparison routines for the second argument are as follows:
* 0x00 - Jump if not zero (jnz) (x != 0)
* 0x01 - Jump if zero (jz) (x == 0)
* 0x02 - Jump if greater than zero (signed) (x > 0)
* 0x03 - Jump if greater than or equal to zero (signed) (x >= 0)
* 0x04 - Jump if lesser than or equal to zero (signed) (x <= 0)
* 0x05 - Jump if lesser than or equal to zero (signed) (x < 0)

Any routine above 0x05 always jumps.

Example:
```
    push8 0x00
    offset +JumpToMe
    jc 0x01 ; Just if zero

    ; Not executed

JumpToMe:
	; Jump is taken and execution continues here

```

#### 0x16 - call

Pushes the current PC address on the call stack, then pops 1 argument off the stack and jumps to it.

Can give the following error: "スタック領域が不足していま" ("Insufficient stack space")

#### 0x17 - ret

Pops an address from the call stack and jumps to it.

#### 0x20 - add

Pops 2 values from the stack, adds them together and pushes the result onto the stack.

#### 0x21 - sub

Pops 2 values from the stack, subtracts the first value from the 2nd and pushes the result onto the stack.

#### 0x22 - mul

Pops 2 values from the stack, multiplies them and pushes the result onto the stack.

#### 0x23 - div

Pops 2 values from the stack, divides the first value by the 2nd value and pushes the result onto the stack.
If the first value is 0 (as in, dividing by zero), -1 will be pushed instead of attempting a division.

#### 0x24 - mod

Pops 2 values from the stack and performs a modulo operation with first value as the right-hand value, and the 2nd value as the left-hand value. The result is pushed on the stack.
If the first value is 0 (as in, dividing by zero), -1 will be pushed instead of attempting a modulo.

#### 0x25 - and

Pops 2 value from the stack, bitwise and's it and pushes the result onto the stack.

#### 0x26 - or

Pops 2 values from the stack, bitwise or's them and pushes the result onto the stack.

#### 0x27 - xor

Pops 2 value2 from the stack, bitwise xor's it and pushes the result onto the stack.

#### 0x28 - not

Pops 1 value from the stack, bitwise not's it and pushes the result onto the stack.

#### 0x29 - shl

Pops 2 values from the stack, left-shifts the first value by the 2nd value.

#### 0x2A - shr

Pops 2 values from the stack, right-shifts the first value by the 2nd value.

#### 0x2B - sar

Pops 2 values from the stack, right-shifts the first value by the 2nd value and preserves the upper-most bit.

#### 0x30 - eq

Pops 2 values from the stack. Pushes 1 onto the stack if they are equal, otherwise it pushes 0.

#### 0x30 - eq

Pops 2 values from the stack. Pushes 1 onto the stack if they are equal, otherwise it pushes 0.

#### 0x40 - ternary

Pops 3 values from the stack.

If the 3rd argument is not equal to 0, then the 2nd argument will be pushed. If the 3rd argument is equal to 0, then the 1st argument will be pushed.

Example:
```
	; Ternary test
	push8 0x01 ; test value
	push8 0x7F ; if test value != 0
	push8 0x11 ; if test value == 0
	ternary    ; 0x11 will be pushed
```

#### 0x48 - sin

Pops 1 value and pushes the sin() result.

The sin operation is done as follows:
```c
pushStack((unsigned int)(fsin((float)popStack() * (float)0.00000027));
```

Note: Shouldn't this always cause 0 to be pushed?

#### 0x49 - cos

Pops 1 value and pushes the cos() result.

The sin operation is done as follows:
```c
pushStack((unsigned int)(fcos((float)popStack() * (float)0.00000027));
```

Note: Shouldn't this always cause 0 to be pushed?

#### 0x60 - memcpy

Copies n bytes from the source into the destination.

Takes 3 arguments:
1 - Length
2 - Source pointer
3 - Destination pointer

#### 0x61 - memclr

Sets n bytes in the destination to 0.

Takes 2 arguments:
1 - Length
2 - Destination pointer

#### 0x62 - memset

Sets n bytes in the destination to x.

Takes 3 arguments:
1 - Value to set
2 - Length
3 - Destination pointer

#### 0x63 - memcmp

Compares n bytes. Pushes 1 if equal, otherwise 0.

Takes 3 arguments:
1 - Length
2 - Source pointer
3 - Destination pointer

#### 0x68 - strlen

Counts the length of a null-terminated string and pushes the result.

Takes 1 argument:
1 - Pointer to string

#### 0x69 - streq

Compares two null-terminated strings and pushes the result.

Takes 2 arguments:
2 - String 1 pointer
3 - String 2 pointer

#### 0x6f - sprintf

sprintf is a user-defined instruction to my knowledge. That means it may vary across games and version of the engine.

It takes n arguments.
The first argument is the format string.
The second argument is the location for the resulting string.
All other arguments will be passed to printf as is.

Example:
```
	codeptr strWorld
	codeptr strHello
	baseptr 0x00
	codeptr strFormat
	sprintf
	baseptr 0x00
	infobox

strHello:
	.asciiz "Hello"
strWorld:
	.asciiz "World"
strFormat:
	.asciiz "%s %s!"
```

This will print "Hello World!"

#### 0x70 - malloc

Takes 1 argument (size), and attempts to allocate local memory. Pushes the address of the allocated memory block onto the stack.

Maximum size allowed is 0x1000000. Any attempt to allocate more (in total or in a single go) will crash the VM.

Takes 1 argument:
1 - Size

#### 0x71 - free

Takes 1 argument (address), and attempts to free local memory. Pushes ??? onto the stack.

Any attempt to free memory ourside the 0x12______ range will cause the VM to crash.

Takes 1 argument:
1 - Address of block to free

#### 0x78 - cmd_78 / confirm

#### 0x79 - cmd_79 / infobox / message

#### 0x7A - cmd_7a / assert

#### 0x7B - cmd_7b / dumpmem

???

#### 0x7D - cmd_7d

Load font?

#### 0x7F - ???

This opcode makes a call to SearchPathA, but the result is discarded. Nothing is pushed or popped to the stack.

This might have been used as a debug opcode at some point.

The body of the opcode is:

```c
int op_7f(VMState* vmState)
{
	char* str;
	char buf[260];
	SearchPathA("H:\\", ".", NULL, 260, &buf[0], &str);
	return 0;
}
```

#### 0x80 - sys1

This opcode takes (at least) 1 single byte argument. This argument will be used as an index in a jump table, and different actions will be executed depending on the index. If an index is undefined (that is, there is no address associated with it and is left 0x00000000), the VM will crash.

Tests
-----

Jump tests performed:
```
	push8 0x00  ; Comparison value
	offset +Yes ; Jump address (argument 1)
	jc 0x07     ; Conditional jump

	; jc 0x00 - Jump if not zero (x != 0) (jnz)
	; push8 0xFF; jc 0x00 - Jump taken
	; push8 0x7F; jc 0x00 - Jump taken
	; push8 0x00; jc 0x00 - Jump not taken
	; push8 0x01; jc 0x00 - Jump taken

	; jc 0x01 - Jump if zero (x == 0) (jz)
	; push8 0xFF; jc 0x01 - Jump not taken
	; push8 0x00; jc 0x01 - Jump taken
	; push8 0x01; jc 0x01 - Jump not taken
	; push8 0x02; jc 0x01 - Jump not taken
	; push8 0x7F; jc 0x01 - Jump not taken
	
	; jc 0x02 - Jump if greater than zero (signed) (x > 0) (jg)
	; push8 0xFF; jc 0x02 - Jump not taken
	; push8 0x80; jc 0x02 - Jump not taken
	; push8 0x7F; jc 0x02 - Jump taken
	; push8 0x01; jc 0x02 - Jump taken
	; push8 0x00; jc 0x02 - Jump not taken

	; jc 0x03 - Jump if greater than or equal to zero (signed) (x >= 0) (jge)
	; push8 0xFF; jc 0x03 - Jump not taken
	; push8 0x80; jc 0x03 - Jump not taken
	; push8 0x7F; jc 0x03 - Jump taken
	; push8 0x01; jc 0x03 - Jump taken
	; push8 0x00; jc 0x03 - Jump taken

	; jc 0x04 - Jump if lesser than or equal to zero (signed) (x <= 0) (jle)
	; push8 0xFF; jc 0x04 - Jump taken
	; push8 0x80; jc 0x04 - Jump taken
	; push8 0x7F; jc 0x04 - Jump not taken
	; push8 0x01; jc 0x04 - Jump not taken
	; push8 0x00; jc 0x04 - Jump taken

	; jc 0x05 - Jump if lesser than zero (signed) (x < 0) (jl)
	; push8 0xFF; jc 0x05 - Jump taken
	; push8 0x80; jc 0x05 - Jump taken
	; push8 0x7F; jc 0x05 - Jump not taken
	; push8 0x01; jc 0x05 - Jump not taken
	; push8 0x00; jc 0x05 - Jump not taken

	; jc 0x06 - Jump always
	; push8 0xFF; jc 0x06 - Jump taken
	; push8 0x80; jc 0x06 - Jump taken
	; push8 0x7F; jc 0x06 - Jump taken
	; push8 0x01; jc 0x06 - Jump taken
	; push8 0x00; jc 0x06 - Jump taken
	; push16 0x100; jc 0x06 - Jump taken
	; push16 0xFFFF; jc 0x06 - Jump taken
	; push32 0xFFFFFFFF; jc 0x06 - Jump taken

	; jc 0x07 - Jump 
	; push8 0xFF;        jc 0x07 - Jump taken
	; push8 0x80;        jc 0x07 - Jump taken
	; push8 0x7F;        jc 0x07 - Jump taken
	; push8 0x01;        jc 0x07 - Jump taken
	; push8 0x00;        jc 0x07 - Jump taken
	; push16 0x100;      jc 0x07 - Jump taken
	; push16 0xFFFF;     jc 0x07 - Jump taken
	; push32 0xFFFFFFFF; jc 0x07 - Jump taken
```

Technical
---------

#### Routine: CrashVM

Creates an error message, writes the error message to BGIError.txt, and throws an exception. The VM will stop executing if an exception is thrown, so this effectively crashes the VM.

#### Opcode: load

The load opcode calls nurseryrhyme.43E9E0, which pops 1 argument off the stack and fetches something (An address?). It seems to be expecting.

It then reads 1 byte from PC. I'm not sure what the value is used for, but values 0, 1 and 2 or above are significant.

It seems that the following:
```
	baseptr 0x200
	load 0x02
```
will load the value stored at the given offset, and push it onto the stack.


#### System: Stack

The stack actually grows downwards inside the engine, but to the VM, it appears to be growing upwards.

#### Routine: StackPop

Pops a value from stack into EAX

#### Routine: StackPush

The StackPush routine pushes 32-bit values onto the VM stack.

`ecx+10` - StackSize
`ecx+28` - Stack

Address: `00422400`
Arguments:
    DWORD on ESI+4
    VM Struct offset in ECX

#### Opcode: sys1 0x58

4A78C4 - Read in 4440C0
4A78E0 - Read in 444220
4A78A8 - Read in 444330 loop

#### Opcode: grp1 0x02

Pops 1 argument off the stack. If the value is 0x01 or 0x03E8, CrashVM will be called.

Writes to:
`00471AFC` - EAX/ECX
`004A471C` - 0

#### Routine: VM execution loop

At address `0043DDA8`, the engine calls the subroutine of whatever opcode is being executed. You can find the pointer for the VMStateStruct in the EDI register when execution reaches this address.

#### Routine: GetTime

If value at `004A78F0` is 1, timeGetTime() will be called.
Otherwise, GetTickCount() will be called.

#### Struct VMStateStruct

This is the VMState struct. It's actually a VM thread, and there can be more than 1 VMState during execution.

```c
struct
{
	uint32         programId;           // 0x00 ID of the currently running program, as given by the VM
	uint32         threadId;            // 0x04 ID of the currently executing thread, as given by the VM
	VMState*       prevVMState;         // 0x08 - Pointer to the previous thread state
	uint32         unknown4;            // 0x0C
	uint32         stackSize_;          // 0x10 ??? Wrong? Maybe stack pointer?
	uint32         instructionPointer;  // 0x14 - Instruction pointer
	uint32         instructionPointer2; // 0x18 - Next instruction pointer?
	uint32         basePointer;         // 0x1C - Base pointer
	uint32         stackSize;           // 0x20 - Size of stack (in units of 4)
	VMMemConfig*   stackMemConfig;      // 0x24 - Stack memory config
	uint32         stack;               // 0x28 - Pointer to the stack
	uint32         codeSpaceSize;       // 0x2C - Size of code space (in units of 4 bytes)
	VMMemConfig*   codeSpaceMemConfig;  // 0x30 - Code space memory config
	byte*          codeSpace;           // 0x34 - Pointer to code space
	VMProgramList* programList;         // 0x38 - List of loaded programs
	int            programCount;        // 0x3C - Count of how many programs are in the list
	byte*          emptyCodeSpacePtr;   // 0x40 - Pointer to the first free byte of code space 
	uint32         unknown17;           // 0x44 - Local memory size?
	VMMemConfig*   unknown18;           // 0x48 - Local memory?
	byte*          unknown19;           // 0x4C - Local memory?
	uint32         unknown20;           // 0x50
	uint32         unknown21;           // 0x54 - VMCallback
	uint32         timer1;              // 0x58
}
```

#### Struct VMMemConfig

Keeps track of allocated memory blocks and their sizes

```c
struct
{
	int isAllocated;
	byte* data;
	int size;
}
```

#### Struct VMProgramList

Used to keep track of which programs are loaded into the code table

```c
struct
{
	char* filename;
	int size;
	int location;
	VMProgramList* prevProgram;
}
```

#### Struct VMCallback

Guess: Calls the callback function once the timeout value is reached.

If a new VMCallback is placed in unknown21, then the already loaded callback will immediately be called.

#### Global variables

gIsWindowActive - Is the main window in focus? Is set to 0 if WA_INACTIVE was sent, and set to 1 if any other activation message was sent.
gSearchPaths - VMLinkedList of directory paths (Assumed to be search paths)
DAT_00471758 - Probably a list of directories used for searching when reading files
DAT_00484130 - Might be a default directory string
gMillisecondsPerFrame - Framerate in milliseconds between frames

#### Opcode: move_arg

Only acceps values in the region of: 0x10000000 to 0x12FFFFFF and 0x40000000 to 0x40FFFFFF. Otherwise it calls CrashVM.

EBX: 0x0000000F
ECX: 0x0000001F
EDX: 0x00000003
EBP: 0x0019FEF0
ESI: 0x0000000A
EDI: 0x022F61A8

-----

ESI: 0x022F61A8
PUSH EDI 0x022F61A8
PUSH ESI 0x022F61A8

PopStack
EAX: 0x10000008
ECX: 0x022F6258

s[0]: 0x10000008
s[1]: 0x022F61A8
sub_43E8A0

ESI: 0x10000008 (Value that we popped from the stack)
if(esi == 0) return;
mov edi esi
edi = edi >> 24
if(edi > 0x12) something...
else
jump addresses[lookup[0x43E9CC + edi]]

        0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2
lookup={0,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1,2,3}
addresses={0043E8D0, 0043E8E8, 0043E905, 0043E922, 0043E93E}

0043E8D0:
0043E8E8:
EAX = `*((&VMStateStruct)+0x4C)`
0043E905:
EAX = `*((&VMStateStruct)+0x34)`
0043E922
EAX = `*((&VMStateStruct)+0x50)`

#### FUN_0042ca10

This is found in the sys1.0x64 (showwindow) opcode as well as sys1.0x10

Unknown what it does or why.

```c
void FUN_0042ca10(void)
{
  undefined4 *puVar1;
  
  puVar1 = &DAT_004a47b4; // 0x1400 bytes, 5 entries each
  do {
    puVar1[-1] = 0;
    *puVar1 = 0;
    puVar1[1] = 0;
    puVar1[3] = 0;
    puVar1 = puVar1 + 5;
  } while ((int)puVar1 < 0x4a5bb4);
  return;
}
```

#### FUN_0042cf60

Takes 1 constant and 1 pointer.
Copies up to 15 values from pointer to the destination as determined by the constant.

If the array given by the pointer contains more than 15 values, the VM will crash. The array is NULL terminated.

Can return these error codes:
-0x7fffffff - Invalid key size
-0x7ffffffe - Key array too big

Returns 0 on success

Destination depends on param_1.
0x8000 = DAT_00471ef8
0x4000 = DAT_00471eb8
0x2000 = DAT_00471e78
0x1000 = DAT_00471e38
0x0200 = DAT_00471df8
0x0100 = DAT_00471db8
0x0080 = DAT_00471d78
0x0040 = DAT_00471d38

Tables
------

The engine uses a few different jump tables. This section lists all of them, with the index on the left followed by the address, and an optional comment.

If there are 2 or more consequtive undefined entries, they'll be collapsed into an ellipsis. If there are 2 or more blocks of 16 undefined entries, they too will be collapsed.

#### Opcodes

These are the base opcodes used by the VM.

```
0x00 0x00430650 - 1 push8
0x01 0x00430670 - 2 push16
0x02 0x00430690 - 4 push32
0x03 0x00000000
0x04 0x004306B0 - 2 baseptr / getvarptr / memptr
0x05 0x004306F0 - 2 string / getcodeptr / codeptr
0x06 0x00430720 - 2 offset / getcodeoffset / codeoffset
0x07 0x00000000
0x08 0x00430750 - 1 load
0x09 0x00430800 - 1 move / storepv
0x0A 0x00430840 - 1 arg / storevp
0x0B 0x00430880 - 1 cmd_0b / write
0x0C 0x004308B0 - 1 cmd_0c / storemulti
0x0D 0x00000000
0x0E 0x00000000
0x0F 0x00000000

0x10 0x00430960 - loadbase
0x11 0x00430980 - storebase
0x12 0x00000000
0x13 0x00000000
0x14 0x004309E0 - jump
0x15 0x00430A50 - 1 jc
0x16 0x00430B20 - call
0x17 0x00430B70 - ret
0x18 0x00000000
...
0x1F 0x00000000

0x20 0x00430BA0 - add
0x21 0x00430BD0 - sub
0x22 0x00430C00 - mul
0x23 0x00430C30 - div
0x24 0x00430C70 - mod
0x25 0x00430CB0 - and
0x26 0x00430CE0 - or
0x27 0x00430D10 - xor
0x28 0x00430D40 - not
0x29 0x00430D60 - shl
0x2A 0x00430D90 - shr
0x2B 0x00430DC0 - sar
0x2C 0x00000000
...
0x2F 0x00000000

0x30 0x00430DF0 - eq
0x31 0x00430E20 - neq
0x32 0x00430E50 - leq
0x33 0x00430E80 - geq
0x34 0x00430EB0 - lt
0x35 0x00430EE0 - gt
0x36 0x00000000
0x37 0x00000000
0x38 0x00430F10 - bool_and
0x39 0x00430F50 - bool_or
0x3A 0x00430F90 - iszero
0x3B 0x00000000
...
0x3F 0x00000000

0x40 0x00430FB0 - ternary
0x41 0x00000000
...
0x47 0x00000000
0x48 0x00430FF0 - sin
0x49 0x00431030 - cos
0x4A 0x00000000
...
0x4F 0x00000000

0x50 0x00000000
...
0x5F 0x00000000

0x60 0x00431070 - memcpy
0x61 0x004310B0 - memclr
0x62 0x004310F0 - memset
0x63 0x00431140 - memcmp
0x64 0x00000000
...
0x67 0x00000000
0x68 0x00431190 - cmd_68 / strlen
0x69 0x004311C0 - cmd_69 / streq
0x6A 0x00431220 - cmd_6a / strcpy
0x6B 0x00431250 - cmd_6b / concat
0x6C 0x00431290 - cmd_6c / getchar
0x6D 0x004312E0 - cmd_6d / tolower
0x6E 0x00431300 - cmd_6e / quote
0x6F 0x00431340 - cmd_6f / sprintf

0x70 0x00431900 - cmd_70 / malloc
0x71 0x00431950 - cmd_71 / free
0x72 0x00000000
...
0x77 0x00000000
0x78 0x004319D0 - cmd_78 / confirm
0x79 0x00431A30 - cmd_79 / infobox / message - infobox (Message: %s)
0x7A 0x00431AA0 - cmd_7a / assert
0x7B 0x00431B10 - cmd_7b / dumpmem
0x7C 0x00000000
0x7D 0x00431CF0 - cmd_7d - Load font?
0x7E 0x00000000
0x7F 0x00431E10 - cmd_7f - Command does nothing but: SearchPathA("H:\\", ".", NULL, 260, &buf[0], &str); Nothing is pushed or pulled from stack. Old debug function?

0x80 0x0043DAB0 - 1 sys1
0x81 0x00000000
...
0x8F 0x00000000

0x90 0x004389B0 - 1 grp1
0x91 0x0043A020 - 1 grp2
0x92 0x0043A7C0 - 1 grp3
0x93 0x00000000
...
0x9F 0x00000000

0xA0 0x0043AFA0 - 1 snd1
0xA1 0x00000000
...
0xAF 0x00000000

0xB0 0x00433750 - 1 usr1
0xB1 0x00000000
...
0xBF 0x00000000

0xC0 0x00432C00 - 1 usr2
0xC1 0x00000000
...
0xCF 0x00000000

0xD0 0x00000000
...
0xFF 0x00000000
```

sys1 - System Control
---------------------

#### 0x80 0x00 - sys1.srand

Seeds the random number generator (using \_srand()).

Inputs:
    1 - Seed

Outputs:

#### 0x80 0x01 - sys1.rand

Calls \_rand() to get a random number between 0 and 32767 (0x7FFF), and pushes it onto the stack.

Inputs:

Outputs:
    1 - Random number (between 0x0000 to 0x7FFF inclusive)

#### 0x80 0x02 - sys1.randrange

Generate a random number between 0 and a given max value (non-inclusive) and push it onto the stack.

No generated number will ever exceed the value 0x7FFFFFFF.

Inputs:
    1 - Range (between 0x00000000 to 0x80000000 - result is non-inclusive)

Outputs:
    1 - Random number

#### 0x80 0x04 - sys1.time

Pushes the current system time in milliseconds onto the stack (precision depends on gUseGetTime).

Inputs:

Outputs:
    1 - Time in milliseconds

#### 0x80 0x08 - sys1.cursorpos

Gets the cursor position relative to the main window, and pushes the X and Y coordinates onto the stack (X first, then Y, meaning Y will be on the top of the stack).

Inputs:

Outputs:
    1 - Y position
    2 - X position

#### 0x80 0x0C - sys1.localtime

Get's the system's local time (using GetLocalTime) and writes it to the pointer given as the first argument.

The data written will be in the format of the Windows SYSTEMTIME structure.

Inputs:
    1 - Pointer where result will be written

Outputs:

#### 0x80 0x0E - sys1.0x0E ???

Reads the global variable gIsMinimised_maybe and pushes it onto the stack. Might be to check if the game is minimised or not.

Inputs:

Outputs:
    1 - gIsMinimised_maybe

#### 0x80 0x0F - sys1.windowactive

Pushes 1 onto the stack if the main window is active, otherwise it pushes 0.

Inputs:

Outputs:
    1 - gIsWindowActive

#### 0x80 0x28 - sys1.mkdir

Create a directory at the given path.
Returns 1 if the directory was created, or 0 is the directory could not be created or already exists.

Inputs:
    1 - Pointer to path string

Outputs:
    1 - 1 if successful, 0 otherwise.

#### 0x80 0x2A - sys1.isdir

Return 1 if the path given is a directory, otherwise it returns 0.

Inputs:
    1 - Pointer to path string

Outputs:
    1 - 1 if path is a directory, otherwise 0.

#### 0x80 0x34 - sys1.fileexists

Takes 2 arguments, and return 1 if the file exists, otherwise 0.

There's something funky going on with someVisualStudioLibFunc(&DAT_00484130). 

Inputs:
    1 - Filename
    2 - Directory (optional)

Outputs:
    1 - 1 is file exists, otherwise 0

#### 0x80 0x37 - sys1.adddir

This opcode seems to be appending a string to a linked list.

The tail of the linked list is at gSearchPaths.

The linked list follows this structure:
```c
struct LinkedStrList
{
	char* string;
	struct LinkedStrList prevEntry;
}
```

#### 0x80 0x3D - sys1.path

Takes 2 arguments; One a pointer to the destination in memory, and the 2nd the path type.

If the type is 0, then we get gCurrentDirectory.
If the type is 1, then we get DAT_00484130.

Inputs:
    1 - Type
    2 - Pointer to destination

Outputs:
    1 - 1 if successful, or 0 if no suitable path was found for the given type.

#### 0x80 0x40 - sys1.loadcode

Takes 2 arguments from the stack. The first argument is the code filename. The 2nd argument is the archive file containing the code file.

The 2nd argument, the archive file, is optional and can be NULL.

The code will be loaded into memory at the end of the current code space, and the address to the new code will be pushed onto the stack.

For example, if ipl.bp is 0x100 bytes and is loaded at code address 0x00, then the next code file will be loaded into code address 0x100.

Example:
```
	codeptr +strSysprgArc
	codeptr +strEnvironBp
	sys1 0x40

strSysprgArc:
	.asciiz "sysprg.arc"
strEnvironBp:
	.asciiz "environ._bp"
```

Note 1:
When loading the file, the VM allocates 128KiB of heap memory. The size of the file is not checked, thus is the file is greater than 128KiB, a buffer overflow will occur and might crash the VM.

Note 2:
The error string buffer is only 256 bytes, thus if the file name is long enough, the sprintf will cause a buffer overflow on the stack.

Possible errors:
    `指定されたプログラム [ %s : %s ] は存在しません`
    `The specified program [ %s : %s ] does not exist`
    
    `"コード領域のサイズが不足しているので、プログラム [ %s : %s ] を読み込むことができません"`
    `The program [ %s : %s ] cannot be loaded because the code area size is insufficient`

    (From readEncodedFile())
    `指定されたファイル [ %s ] は存在しません`
    `The specified file [ %s ] does not exist`

Inputs:
    1 - Pointer to filename
    2 - Opinter to archive filename (optional)

Outputs:

#### 0x80 0x41 - sys1.unloadcode

Unload the program at the top of the program code stack.

Unloading the main program will result in a crash.

`メインプログラムまで削除されてしまいました`
`The main program has been deleted`

Inputs:

Outputs:
1 - Number of programs loaded (after freeing the top most), or -0x7FFFFFFF on error.

#### 0x80 0x44 - sys1.newthread

Load the code file and create a new VM thread.

Max filesize: 0x20000 bytes (128 KiB)

Inputs:
    1 - Local memory size???
    2 - Codespace size
    3 - Stack size
    4 - Filename
    5 - Archive filename

Outputs:
    1 - threadId of the new VM thread

#### 0x80 0x46 - sys1.threadid

Pushes the current VMState's threadId onto the stack.

Inputs:

Outputs:
    1 - vmState->threadId

#### 0x80 0x5A - 0x5A

Creates a new callback struct with the time remaining in vmState->timer1, and attaches it to the VM.

The callback func is set to FUN_0041d770.

It's unknown what the consequences of this is.

Inputs:

Outputs:
	1 - 1 if there is remaining time, 0 is the current tim is greater than that in timer1

#### 0x80 0x64 - sys1.displaywindow

Takes 1 argument from stack. If the value if 0, close the window. If the value is 1 or above, show the window.

#### 0x80 0x66 - windowtitle

Sets the title of the main window. It does this by passing a pointer to a string from the stack to SetWindowTextA().

Takes 1 argument:
1 - Pointer to string to use as window title

#### 0x80 0x67 - cursorshape

Sets the cursor shape (gCursorShape) as given by the 1st argument on stack.

Possible values:
0 - ???
1 - ???
2 - ???
3 - ???
4 - ???

Any other value will crash the VM.

Takes 1 argument:
1 - Cursor shape ID

#### 0x80 0x70 - sys1.0x70

Allocates global memory (and frees any previously allocated global memory).

The memory size is defined as follows:

0 - 0x1000   -   1 KiB
1 - 0x2000   -   2 KiB
2 - 0x4000   -   4 KiB
3 - 0x8000   -   8 KiB
4 - 0x10000  -  64 KiB
5 - 0x20000  - 128 KiB
6 - 0x40000  - 256 KiB
7 - 0x80000  - 512 KiB
8 - 0x100000 -   1 MiB

Possible errors:
	`無効なサイズ番号 [ %d ] が指定されました`
	`Invalid size number [ %d ] specified`

Inputs:
    1 - Memory size

Outputs:
    1 - 1 if successful, 0 if unsuccessful

#### Sys1 Table

These addresses are associated with the sys1 ("System control") instruction. The sys1 instruction takes a 1 byte argument which it uses to select an address from the jumptable.

```
0x00 0x0043B000 - sys1.srand
0x01 0x0043B020 - sys1.rand
0x02 0x0043B040 - sys1.randrange
0x03 0x00000000
0x04 0x0043B090 - sys1.time
0x05 0x00000000
0x06 0x00000000
0x07 0x00000000
0x08 0x0043B0B0 - sys1.cursorpos
0x09 0x00000000
0x0A 0x00000000
0x0B 0x00000000
0x0C 0x0043B0F0 - sys1.localtime
0x0D 0x00000000
0x0E 0x0043B110 - Pushes gIsMinimised_maybe
0x0F 0x0043B130 - sys1.windowactive

0x10 0x0043B150 - Pops 1 arg into gSetBySys1_0x10, then calls FUN_0042ca10
0x11 0x0043B170 - Maybe get keyboard input?
0x12 0x0043B1A0 - Pops a pointer, pushes another pointer
0x13 0x0043B1E0 - Pushes DAT_004a5bfc
0x14 0x0043B200 - Pops 1 arg into DAT_00471fdc
0x15 0x0043B220 - Pops 1 arg into DAT_004a5be8
0x16 0x0043B240 - Calls FUN_0042c990. Uses DAT_004a5bec and DAT_00471fb8
0x17 0x0043B250 - Pushes the result of calling FUN_0042ce10. Uses DAT_004a5be8, gSetBySys1_0x1, DAT_00471fb8, DAT_004a5bec and DAT_00471fdc
0x18 0x0043B270 - 
0x19 0x0043B2A0 - 
0x1A 0x0043B2D0 - 
0x1B 0x0043B300 - Pops 1 pointer and 1 arg. Seems to be keyboard layout related - Specifies key array length?
0x1C 0x0043B3A0 - 
0x1D 0x0043B3D0 - 
0x1E 0x0043B430 - 
0x1F 0x0043B460 - 

0x20 0x0043B4C0 - 
0x21 0x0043B500 - 
0x22 0x00000000
...
0x27 0x00000000
0x28 0x0043B560 - sys1.mkdir - Create a directory
0x29 0x0043B590 - 
0x2A 0x0043B5C0 - sys1.isdir - Is the path given a directory?
0x2B 0x00000000
0x2C 0x0043B600 - 
0x2D 0x0043B630 - 
0x2E 0x00000000
0x2F 0x0043B660 - 

0x30 0x0043B6B0 - 
0x31 0x0043B6F0 - 
0x32 0x0043B750 - 
0x33 0x0043B790 - 
0x34 0x0043B800 - fileexists? getfilepath? Pops 2 args: 1 str pointer, 1 constant. Pushes 1 var.
0x35 0x0043B830 - 
0x36 0x0043B860 - Pop 1 arg into DAT_00471758
0x37 0x0043B880 - sys1.adddir
0x38 0x0043B8A0 - 
0x39 0x00000000
0x3A 0x00000000
0x3B 0x0043B910 - 
0x3C 0x0043B980 - 
0x3D 0x0043B9C0 - sys1.path - Get gCurrentDirectory or DAT_00484130
0x3E 0x0043B9F0 - 
0x3F 0x0043BA50 - 

0x40 0x0043BAA0 - sys1.loadcode
0x41 0x0043BB60 - sys1.unloadcode
0x42 0x00000000
0x43 0x00000000
0x44 0x0043BB90 - sys1.newthread
0x45 0x0043BBF0 - 
0x46 0x0043BC00 - sys1.threadid
0x47 0x0043BC20 - 
0x48 0x0043BC50 - 
0x49 0x0043BCA0 - 
0x4A 0x0043BCD0 - 
0x4B 0x0043BD70 - 
0x4C 0x0043BE10 - 
0x4D 0x00000000
0x4E 0x00000000
0x4F 0x00000000

0x50 0x0043BE80 - 
0x51 0x00000000
0x52 0x00000000
0x53 0x00000000
0x54 0x0043BEA0 - 
0x55 0x00000000
0x56 0x00000000
0x57 0x00000000
0x58 0x0043BF10 - ??? Set vmState->timer1 to arg 1 + getMilliseconds()
0x59 0x0043BF30 - 
0x5A 0x0043BF60 - ??? Adds a callback struct to the VM
0x5B 0x00000000
0x5C 0x0043BFF0 - 
0x5D 0x00000000
0x5E 0x0043C080 - 
0x5F 0x00416990 - 

0x60 0x0043C0A0 - 
0x61 0x0043C100 - 
0x62 0x0043C120 - 
0x63 0x00000000
0x64 0x0043C160 - sys1.showwindow
0x65 0x0043C180 - 
0x66 0x0043C1C0 - sys1.windowtitle
0x67 0x0043C1E0 - sys1.cursorshape
0x68 0x0043C230 - ??? Writes arg1 from stack to DAT_00471aac. Used when WndProc recieved a WM_CLOSE (0x10) message
0x69 0x0043C250 - 
0x6A 0x0043C270 - 
0x6B 0x0043C280 - 
0x6C 0x0043C2B0 - 
0x6D 0x0043C2D0 - 
0x6E 0x00000000
0x6F 0x00000000

0x70 0x0043C2F0 - ??? Allocates memory into gGlobalMemory. Size is stored in gGlobalMemorySize
0x71 0x0043C350 - 
0x72 0x00000000
0x73 0x00000000
0x74 0x0043C380 - ??? Pop 1 arg and store it in DAT_00471b0c
0x75 0x00000000
0x76 0x00000000
0x77 0x00000000
0x78 0x0043C3A0 - 
0x79 0x0043C410 - 
0x7A 0x0043C470 - 
0x7B 0x0043C4A0 - 
0x7C 0x00000000
...
0x7F 0x00000000

0x80 0x0043C4D0 - ??? Read the Bgi.gdb file, the "global database"
0x81 0x0043C530 - 
0x82 0x0043C550 - 
0x83 0x0043C600 - 
0x84 0x0043C6B0 - 
0x85 0x0043C6D0 - 
0x86 0x00000000
0x87 0x00000000
0x88 0x0043C6F0 - 
0x89 0x0043C720 - 
0x8A 0x0043C7E0 - 
0x8B 0x0043C8E0 - 
0x8C 0x00000000
...
0x8F 0x00000000

0x90 0x0043C990 - 
0x91 0x0043C9B0 - 
0x92 0x00000000
0x93 0x00000000
0x94 0x0043C9D0 - 
0x95 0x0043CB60 - 
0x96 0x0043CBD0 - 
0x97 0x0043CCD0 - 
0x98 0x00000000
...
0x9F 0x00000000

0xA0 0x0043CD40 - 
0xA1 0x0043CD60 - 
0xA2 0x00000000
...
0xA7 0x00000000
0xA8 0x0043CD90 - 
0xA9 0x0043CDE0 - 
0xAA 0x00000000
0xAB 0x00000000
0xAC 0x0043CE30 - 
0xAD 0x00000000
0xAE 0x00000000
0xAF 0x00000000

0xB0 0x0043CE90 - 
0xB1 0x0043CED0 - 
0xB2 0x00000000
0xB3 0x00000000
0xB4 0x0043CF00 - 
0xB5 0x0043CF90 - 
0xB6 0x0043CFD0 - 
0xB7 0x00000000
...
0xBF 0x00000000

0xC0 0x0043D010 - 
0xC1 0x0043D0A0 - 
0xC2 0x00000000
...
0xCF 0x00000000

0xD0 0x0043D0D0 - 
0xD1 0x0043D100 - 
0xD2 0x0043D130 - 
0xD3 0x0043D170 - 
0xD4 0x0043D1A0 - 
0xD5 0x00000000
0xD6 0x00000000
0xD7 0x00000000
0xD8 0x0043D1F0 - 
0xD9 0x0043D200 - 
0xDA 0x00000000
0xDB 0x00000000
0xDC 0x0043D230 - 
0xDD 0x0043D260 - 
0xDE 0x00000000
0xDF 0x00000000

0xE0 0x0043D2A0 - 
0xE1 0x0043D2F0 - 
0xE2 0x0043D3E0 - 
0xE3 0x0043D420 - 
0xE4 0x00000000
...
0xE7 0x00000000
0xE8 0x0043D440 - 
0xE9 0x00000000
0xEA 0x00000000
0xEB 0x00000000
0xEC 0x0043D470 - 
0xED 0x00000000
0xEE 0x00000000
0xEF 0x00000000

0xF0 0x0043D490 - 
0xF1 0x0043D520 - 
0xF2 0x0043D590 - 
0xF3 0x0043D770 - 
0xF4 0x0043D800 - 
0xF5 0x0043D870 - 
0xF6 0x0043D8E0 - 
0xF7 0x0043D920 - 
0xF8 0x0043D980 - 
0xF9 0x0043D9C0 - 
0xFA 0x0043D9F0 - 
0xFB 0x0043DA20 - 
0xFC 0x0043DA40 - 
0xFD 0x0043DA90 - ??? Pushes 1 unknown value onto the stack. Usually 0. Maybe it returns something different on error.
0xFE 0x0043D960 - 
0xFF 0x00000000
```

grp1 - Graphics Control
-----------------------

#### 0x90 0x02 - grp1.setframerate

Sets the framerate. Framerates from 1 to 1000 are allowed.

Technical:
    It will set the global variable gMillisecondsPerFrame to (int)(1000/framerate)

Possible errors:
	If framerate is 0, or above 1000
	`無効なフレームレート [ %d ] が指定されました`
	`Invalid frame rate [ %d ] specified`

Inputs:
    1 - Framerate

Outputs:

#### 0x90 0x03 - grp1.setcachesize

Sets the cache size (What kind of cache?).

Technical:
    Allocates a cache struct into gGraphicsCache.

Possible errors:
    If cache size is set to more than 0x4000000
    `無効なキャッシュサイズ [ %d ] が指定されました`
    `Invalid cache size [ %d ] specified`

Inputs:
    1 - Cache size

Outputs:

#### 0x90 0x0C - grp1.0x0C

Possible errors:
    If arg2 is over 0x100
    `無効なエフェクトレベル／トランスペアレンシィ／アディションレベル [ %d ] が指定されました`
    `Invalid effect level / transparency / addition level [ %d ] specified`

Inputs:
    1 - 
    2 - 

#### 0x90 0x0D - grp1.setantialiasing

Sets anti-aliasing level.

Allowed values: 0, 1 and 2.

Technical:
    Writes to DAT_004710c4 and DAT_004710c8.
    The values written are [4, 2], [5, 4] or [6, 6], depending on the level.

Possible errors:
    `無効なアンチエイリアスレベル [ %d ] が指定されました`
    `Invalid antialiasing level [ %d ] specified`

Inputs:
    1 - Anti-aliasing level

Outputs:

#### 0x90 0x0E - grp1.addfont

Sets, prepares or loads fonts?

Possible errors:
	???
	`無効なキャッシュ量 [ %d ] が指定されました`
	`Invalid cache amount [ %d ] specified`

	???
	`無効なフォントサイズ [ %d ] が指定されました`
	`Invalid font size [ %d ] specified`

	???
	`無効なフォント幅 [ %d ] が指定されました`
	`Invalid font width [ %d ] specified`

	If the font ID does not correlate to a valid font
	`無効なフォント番号 [ %d ] が指定されました`
	`Invalid font number [ %d ] specified`

Inputs:
    1 - Cache amount
    2 - ???
    3 - Font width
    4 - Font size
    5 - Font ID

Outputs:

#### 0x90 0x60 - grp1.0x60

Possible errors:
    `これ以上、フィルターオブジェクトを生成する事は出来ません`
    `No more filter objects can be created`

Inputs:

Outputs:
    1 - ????

#### 0x90 0x94 - grp1.0x94

Writes arg1 to:
DAT_00471188

Inputs:
    1 - ???

Outputs:

#### 0x90 0x95 - grp1.0x95

Possible errors:
    If arg1 is under 0x1 or above 0x100
    `無効な分割回数 [ %d ] が指定されました`
    `Invalid split count [ %d ] specified`

Inputs:
    1 - ??? into DAT_0047118c
    2 - ??? into DAT_00471190

Outputs:

#### 0x90 0x96 - grp1.0x96

Possible errors:
    If arg1 is under 0x1 or above 0x100
    `無効な分割回数 [ %d ] が指定されました`
    `Invalid split count [ %d ] specified`

Inputs:
    1 - ??? into DAT_00471194
    2 - ??? into DAT_00471198

Outputs:

#### 0x90 0x97 - grp1.0x97

Inputs:
    1 - ??? into DAT_004823c8
    2 - ??? into DAT_004823cc

Outputs:


#### 0x90 0x98 - grp1.0x98

Possible errors:
    `指定されたビットマップ [ %d ] は存在しないか、スクリーンと互換性がありません`
    `The specified bitmap [ %d ] does not exist or is incompatible with the screen`

Inputs:
    1 - ???
    2 - ??? Pointer

Outputs:

#### 0x90 0xAF - grp1.0xAF

Writes arg1 to:
DAT_00482404
DAT_00482408
DAT_00482440

Inputs:
    1 - ???

Outputs:

#### 0x90 0x9F - grp1.0x9F

Writes arg1 to:
DAT_004823d0

Inputs:
    1 - ???

Outputs:

#### Table

These addresses are associated with the grp1 instruction. The grp1 instruction is a 2-byte instruction. The 2nd byte is used to select an address from the jumptable.

Possible errors:
`未定義のグラフィック制御命令 $90%02X を検出しました`
`Undefined graphic control instruction $90%02X detected`

```
0x00 0x00B03743 - 
0x01 0x00D03743 - 
0x02 0x00F03743 - grp1.setframerate
0x03 0x00503843 - grp1.setcachesize
0x04 0x00B03843 - 
0x05 0x00E03843 - 
0x06 0x00000000 - 
0x07 0x00000000 - 
0x08 0x00203943 - ???? Writes to DAT_004a4720 and DAT_004710bc
0x09 0x00403943 - 
0x0A 0x00703943 - 
0x0B 0x00000000 - 
0x0C 0x00A03943 - grp1.0x0C ???
0x0D 0x00D03943 - grp1.setantialiasing
0x0E 0x00203A43 - grp1.addfont
0x0F 0x00303B43 - 

0x10 0x00503B43 - 
0x11 0x00103D43 - 
0x12 0x00903D43 - 
0x13 0x00C03D43 - 
0x14 0x00003E43 - 
0x15 0x00603E43 - 
0x16 0x00103F43 - 
0x17 0x00000000 - 
0x18 0x00503F43 - 
0x19 0x00604043 - 
0x1A 0x00F04143 - 
0x1B 0x00A04343 - 
0x1C 0x00C04443 - 
0x1D 0x00504643 - 
0x1E 0x00404743 - 
0x1F 0x00304843 - 

0x20 0x00204943 - 
0x21 0x00D04A43 - 
0x22 0x00F04943 - 
0x23 0x00D04B43 - 
0x24 0x00D04C43 - 
0x25 0x00000000 - 
0x26 0x00000000 - 
0x27 0x00000000 - 
0x28 0x00204E43 - 
0x29 0x00000000 - 
... 
0x2F 0x00000000 - 

0x30 0x00504F43 - 
0x31 0x00904F43 - 
0x32 0x00D04F43 - 
0x33 0x00205043 - 
0x34 0x00705043 - 
0x35 0x00C05043 - 
0x36 0x00000000 - 
0x37 0x00000000 - 
0x38 0x00105143 - 
0x39 0x00000000 - 
0x3A 0x00000000 - 
0x3B 0x00000000 - 
0x3C 0x00D05143 - 
0x3D 0x00605243 - 
0x3E 0x00000000 - 
0x3F 0x00A05243 - 

0x40 0x00005343 - 
0x41 0x00505343 - 
0x42 0x00C05343 - 
0x43 0x00905443 - 
0x44 0x00005643 - 
0x45 0x00205743 - 
0x46 0x00705843 - 
0x47 0x00305943 - 
0x48 0x00A05A43 - 
0x49 0x00605B43 - 
0x4A 0x00005C43 - 
0x4B 0x00000000 - 
0x4C 0x00105D43 - 
0x4D 0x00000000 - 
0x4E 0x00000000 - 
0x4F 0x00000000 - 

0x50 0x00405D43 - 
0x51 0x00705D43 - 
0x52 0x00000000 - 
0x53 0x00000000 - 
0x54 0x00E05D43 - 
0x55 0x00205E43 - 
0x56 0x00B05E43 - 
0x57 0x00605F43 - 
0x58 0x00B05F43 - 
0x59 0x00906043 - 
0x5A 0x00906343 - 
0x5B 0x00F06443 - 
0x5C 0x00000000 - 
...
0x5F 0x00000000 - 

0x60 0x00506643 - grp1.0x60
0x61 0x00806643 - 
0x62 0x00000000 - 
0x63 0x00000000 - 
0x64 0x00B06643 - 
0x65 0x00F06643 - 
0x66 0x00606743 - 
0x67 0x00000000 - 
...
0x6F 0x00000000 - 

0x70 0x00906943 - 
0x71 0x00C06943 - 
0x72 0x00000000 - 
0x73 0x00000000 - 
0x74 0x00F06943 - 
0x75 0x00306A43 - 
0x76 0x00206B43 - 
0x77 0x00000000 - 
0x78 0x00F06B43 - 
0x79 0x00906C43 - 
0x7A 0x00000000 - 
...
0x7F 0x00000000 - 

0x80 0x00506D43 - 
0x81 0x00D06D43 - 
0x82 0x00000000 - 
0x83 0x00000000 - 
0x84 0x00406E43 - 
0x85 0x00806E43 - 
0x86 0x00306F43 - 
0x87 0x00007043 - 
0x88 0x00207143 - 
0x89 0x00000000 - 
0x8A 0x00D07143 - 
0x8B 0x00507243 - 
0x8C 0x00907243 - 
0x8D 0x00D07243 - 
0x8E 0x00000000 - 
0x8F 0x00000000 - 

0x90 0x00C07343 - 
0x91 0x00000000 - 
0x92 0x00000000 - 
0x93 0x00000000 - 
0x94 0x00607443 - grp1.0x94 ???
0x95 0x00807443 - grp1.0x95 ???
0x96 0x00B07443 - grp1.0x96 ???
0x97 0x00E07443 - grp1.0x97 ???
0x98 0x00107543 - grp1.0x98 ???
0x99 0x00807543 - 
0x9A 0x00A07543 - 
0x9B 0x00E07543 - 
0x9C 0x00107643 - 
0x9D 0x00307643 - 
0x9E 0x00000000 - 
0x9F 0x00D07643 - grp1.0x9F ???

0xA0 0x00F07643 - 
0xA1 0x00307843 - 
0xA2 0x00407B43 - 
0xA3 0x00507B43 - 
0xA4 0x00607B43 - 
0xA5 0x00907B43 - 
0xA6 0x00B07B43 - 
0xA7 0x00F07B43 - 
0xA8 0x00000000 - 
...
0xAE 0x00000000 - 
0xAF 0x00307C43 - grp1.0xAF ???

0xB0 0x00507C43 - 
0xB1 0x00607D43 - 
0xB2 0x00000000 - 
0xB3 0x00000000 - 
0xB4 0x00707E43 - 
0xB5 0x00F07E43 - 
0xB6 0x00F07F43 - 
0xB7 0x00000000 - 
0xB8 0x00208043 - 
0xB9 0x00508043 - 
0xBA 0x00808043 - 
0xBB 0x00000000 - 
0xBC 0x00B08043 - 
0xBD 0x00E08043 - 
0xBE 0x00108143 - 
0xBF 0x00408143 - 

0xC0 0x00000000 - 
...
0xCF 0x00000000 - 

0xD0 0x00708143 - 
0xD1 0x00008243 - 
0xD2 0x00000000 - 
0xD3 0x00000000 - 
0xD4 0x00408243 - 
0xD5 0x00808243 - 
0xD6 0x00D08243 - 
0xD7 0x00208343 - 
0xD8 0x00708343 - 
0xD9 0x00F08343 - 
0xDA 0x00708443 - 
0xDB 0x00B08443 - 
0xDC 0x00D08443 - 
0xDD 0x00108543 - 
0xDE 0x00408543 - 
0xDF 0x00708543 - 

0xE0 0x00A08543 - 
0xE1 0x00D08543 - 
0xE2 0x00000000 - 
0xE3 0x00000000 - 
0xE4 0x00008643 - 
0xE5 0x00408643 - 
0xE6 0x00000000 - 
0xE7 0x00000000 - 
0xE8 0x00A08643 - 
0xE9 0x00308743 - 
0xEA 0x00000000 - 
...
0xEF 0x00000000 - 

0xF0 0x00908743 - 
0xF1 0x00908843 - 
0xF2 0x00A08843 - 
0xF3 0x00C08843 - 
0xF4 0x00000000 - 
...
0xF7 0x00000000 - 
0xF8 0x00E08843 - 
0xF9 0x00000000 - 
0xFA 0x00F08843 - 
0xFB 0x00208943 - 
0xFC 0x00508943 - 
0xFD 0x00708943 - 
0xFE 0x00000000 - 
0xFF 0x00000000 - 
```

#### grp2

These addresses are associated with the grp2 instruction. The grp2 instruction is a 2-byte instruction. The 2nd byte is used to select an address from the jumptable.

```
```

#### grp3

These addresses are associated with the grp3 instruction. The grp3 instruction is a 2-byte instruction. The 2nd byte is used to select an address from the jumptable.

```
```

