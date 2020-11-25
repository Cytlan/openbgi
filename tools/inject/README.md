# Inject

Inject is a tool which injects an ordered list of shared libraries into the address space of a binary executable. The created process is initially suspended, and resumes execution once the ordered list of shared libraries have been loaded into its address space, and their respective DllMain functions have finished executing.

## Usage

```
$ inject EXE [DLL...]
```

## Examples

```
$ inject a.exe b.dll c.dll
```

Order of execution:

1. Create a suspended process of `a.exe`.
2. Load `b.dll` into the address space of `a.exe`.
3. Execute the `DllMain` function of `b.dll`.
4. Load `c.dll` into the address space of `a.exe`.
5. Execute the `DllMain` function of `d.dll`.
6. Resume execution of `a.exe`.

## Credits

Inject was greatly inspired by Risc's use of DLL-injection in [diablo-improvements](https://github.com/r1sc/diablo-improvements).

## Public domain

The source code and any original content of this repository is hereby released into the [public domain].

[public domain]: https://creativecommons.org/publicdomain/zero/1.0/
