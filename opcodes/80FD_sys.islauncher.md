0x80 0xFD - sys.islauncher
==========================

Pushes the `gCmdFlagExecuteAsLauncher` flag onto the stack.

Mnemonic       | Opcode | Other bytes | Stack
---------------|--------|-------------|--------------
sys.islauncher | 80 FD  |             | → value

Operands
--------

#### Other bytes

None

#### Stack in

None

#### Stack out

* 1 - 0: Is not in launcher mode. 1: Is in launcher mode.

Example
-------

```
; Jump if in launcher mode
	sys.islauncher
	codeoffset isLauncherBranch
	jnz
```

Compatibility
-------------

Version | Note
--------|-------------
1.633   | Confirmed

Equivalent C code
-----------------

```c
int op_sys0_islauncher(VMThread* thread)
{
	BGI_PushStack(thread, BGI_FlagIsLauncher());
	return 0;
}
```
