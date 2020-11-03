0x00 - push8
============

Pushes an 8-bit value onto the stack. The value is signed, and will be sign extended when pushing.

Mnemonic  | Opcode | Other bytes | Stack
----------|--------|-------------|--------------
push8     | 00     | 1: value    | → value

Operands
--------

#### Other bytes

* 1 - `char` - Value to push

#### Stack in

None

#### Stack out

* 1 - `int` - Value

Example
-------

```
	push0 0
```

Compatibility
-------------

Version | Note
--------|-------------
1.58    | Confirmed
1.633   | Confirmed

Equivalent C code
-----------------

```c
int op_push8(VMThread* thread)
{
	VM_PushStack(thread, VM_ReadPC8(thread));
	return 0;
}
```
