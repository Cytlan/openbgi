# Programs: the system scripts

A game's menus, dialogs, save screens and its scenario interpreter are
`._bp` programs in `sysprg.arc`, `system.arc` (`ipl._bp`, the boot
program) and `autoload.arc`. `openbgi bpc` decompiles a program to a
C-like source and compiles it back. `openbgi asm` does the same at the
level of single instructions.

For a whole game use `openbgi dump` and `openbgi pack`
([dump-pack.md](dump-pack.md)).

## openbgi bpc

    openbgi bpc -D [-e ENGINE] [--no-verify] [-v] [-o OUT.bpc] (FILE._bp | --arc ARCHIVE NAME)
    openbgi bpc [-e ENGINE] [-I DIR]... [-u] SOURCE.bpc -o OUT._bp
    openbgi bpc -R [-e ENGINE] [-v] (FILE._bp... | --arc ARCHIVE [NAME...])

| Option | Does |
|---|---|
| `-D` | Decompiles a program |
| (none), `-C` | Compiles a source |
| `-R` | Round trip: decompile, compile, compare |
| `-e` | Engine profile (`openbgi --list-engines`). The source records it in `#engine`. |
| `-I` | A folder to look for included files in |
| `-u` | Stores strings as UTF-8 instead of Shift-JIS |
| `-v`, `-v -v` | Says why a function could not be written as source; traces statements |

Editing one program:

    openbgi asm -l sysprg.arc
    openbgi bpc -D -e 1.553 --arc sysprg.arc title._bp -o title.bpc
    (edit title.bpc)
    openbgi bpc title.bpc -o title._bp
    openbgi scn --pack sysprg.arc -o new.arc title._bp=title._bp

An unedited source compiles to the file it came from, byte for byte.
The decompiler checks this for every function.

## The source

    // kana._bp
    #engine 1.553

    main(p1) { int l20[8];
        *(int*)l20 = {sub_0031, sub_010c, sub_01bf, sub_01e1};
        (*(int*)(l20 + (p1 << 2)))();
    }

    sub_0031(p1, p2) { int l108; int l104; int l100[64];
        l108 = 0x19fe8 + 0x17ae8;
        l104 = 0;
        do {
            if (*(char*)(l108 + 0) == 0) goto L_00b8;
            if (streq(p2, l108 + 0)) goto L_00d5;
            l108 = l108 + 1 * 64;
            l104 = l104 + 1;
        } while (l104 < 4);
        return;
    L_00b8:
        *(int*)(0x19fe8 + 0x17ae4) = *(int*)(0x19fe8 + 0x17ae4) + 1;
    L_00d5:
        gfx1.ruby_set(p2, p1);
    }

Names are the decompiler's and can be changed freely: `main` is the
function at offset 0 (the one the engine starts), `sub_XXXX` a function
by offset, `pN` a parameter, `lXX` a local by frame address, `L_XXXX` a
label.

Nothing is folded: `0x19fe8 + 0x17ae8` and `1 * 64` compile to the
instructions the original has. Folding them by hand changes the bytes
(harmless, but the file is no longer identical).

### Like C

- Functions: `name(params) { body }`. Parameters are untyped words.
- Declarations anywhere a statement may stand: `int x;`,
  `char buf[64];`, `short s;`, `int t[4] = {1, 2, 3, 4};`.
- `if`/`else`, `while`, `do`/`while`, `for`, `break`, `continue`,
  `return [v]`, `goto`, labels, blocks.
- Operators: `+ - * / % & | ^ << >> == != < <= > >= && || ! ~`, unary
  `-`, `c ? a : b`, `=`. `>>` is arithmetic, `>>>` logical.
- Memory: `*(int*)a`, `*(short*)a`, `*(char*)a` read 4, 2, 1 bytes.
  `&x`, `a[i]` as in C.
- Comments: `//` and `/* */`.

### Calls

| Form | Calls |
|---|---|
| `sub_0031(a, b)` | A function of the program |
| `(*(int*)(l20 + 4))(x)`, `l2c(1, 0)` | A code address held in memory or in a local |
| `gfx0.sprite_create()`, `sys.yield()`, `strcpy(d, s)`, `sprintf(buf, "%d", n)` | An engine instruction by name |
| `gfx1.0xf6(...)`, `op.0x77(...)` | An engine instruction by number |
| `user.0x41(...)` | An instruction a program registered ("FF xx") |
| `obgi.text_scroll(...)` | One of OpenBGI's own instructions ([obgi.md](obgi.md)) |

### Unlike C

| Source | Meaning |
|---|---|
| `if ((a || b) && c)` | The parenthesised part is computed as a value. Without the parentheses it is a chain of jumps. Both work; they compile differently. |
| `__sign(v) < 0` (`<= 0`, `> 0`, `>= 0`) | Tests the sign of `v` with the jump itself |
| `0x00000000` (eight hex digits) | Pushed as 32 bits whatever the value |
| `x <- v;` | Stores and drops the value; `x = v` keeps it on the stack |
| `y + f(a)`, `n = y - f(a);`, `g(f(a), b)` | Wrong when `f` has `x = v;` statements: what they kept lies under the value `f` returns, and the addition, the store or `g` takes it for its other operand. See [below](#a-calls-value) |
| `__pop` | The value an earlier statement left on the stack |
| `__inline(addr, "bytes")` | Copies bytes inline |
| `if (c) break;` without braces | Jumps straight from the condition; with braces it is a block. They compile differently. |
| `__asm { ... }` | Listing lines (below) inside a function |
| `__program { ... }` | The whole program as a listing |

There is no `switch`; the original's dispatch is written as
`if (x == k) goto L_k;` chains.

### A call's value

A function returns its value on the stack. Every `x = v;` statement it
ran has left a value there before, and nothing takes those away: the
stack goes round. So a call's value must be the first thing its
statement puts on the stack.

| Right | Why |
|---|---|
| `n = f(a);`, `n <- f(a);` | The value is stored before anything else is pushed (`n = f(a);` is compiled value first, then the address) |
| `if (f(a))`, `if (f(a) == 2)`, `return f(a);` | Nothing lies under it |
| `g(b, f(a))` | A function's arguments are pushed from the last to the first |
| `sys.x(f(a), b)` | An engine instruction's from the first to the last |

| Wrong | What happens |
|---|---|
| `y + f(a)`, `f(a) - f(b)` | The operator takes a left-over value for `y`, or for the first call's value |
| `n = y + f(a);` | The store takes one for the address of `n`, and writes there |
| `g(f(a), b)`, `sys.x(b, f(a))` | The callee takes one for `b` |

Two ways out:

1. Store each call's value first: `u = f(a); v = f(b); n = u - v;`.
2. Write the function so that it leaves nothing: `x <- v;` for every
   assignment that is no call's value, as `trig()` of the launcher's
   program does.

The compiler warns of such a call: `f() leaves values on the stack
under its own, and a value of this statement lies there already`. It
looks into the program's own functions, and into what they call. It
does not look into a call through an address, an engine instruction or
an `__asm` body.

## Naming memory

A program keeps its state at fixed addresses of the engine's global
memory. Give them names:

    global int gAbort @ 1124;
    global char st[0x4f684] @ 0x1bb84;
    global int stIp @ st + 164;
    global int hSprite[16] @ 2732;

    gAbort = 1;            // *(int*)1124 = 1
    hSprite[slot] = 0;     // *(int*)(2732 + slot * 4)

A declaration reserves nothing and cannot give a start value. The
address is compiled as written, every time the name is used.

Other areas (`#engine` must name a real profile):

| Declaration | Area |
|---|---|
| `global int x @ A;` | Global memory |
| `code int x @ A;` | Code area of the running thread |
| `data int x @ A;` | Its data area (the frames of the locals) |
| `heap int x @ A;` | Its local heap |
| `dynamic[n] int x @ A;` | Dynamic global block n (up to 1.529) |

## Records

    struct Point  { int x; int y; };
    struct Sprite {
        int used;
        Point pos;
        char name[32];
        Sprite* next;
        func free;          // the address of a function
        int level @ 64;     // a field at a fixed offset
    };
    global Sprite stSprite[16] @ st + 1620;

    show(Sprite* p, i) { Sprite copy;
        stSprite[i].pos.x = p->pos.x;
        memcpy(copy, &stSprite[i], sizeof(Sprite));
    }

- Fields follow each other without gaps; nothing is aligned.
- Types: `int`, `short`, `char`, a record, `func`, and any of them with
  `*`. Arrays `name[n]`, tables `name[n][m]`.
- A record or array has no value: its name is its address. Copy with
  `memcpy`.
- Pointer arithmetic is plain arithmetic: `p + 1` is one byte further;
  the next element is `&p[1]`.
- `rec.field` always adds the offset, even 0, as the original does;
  `*(int*)(rec)` does not. They are different instructions.

## Strings

The source is UTF-8. Strings are stored as Shift-JIS (CP932); a
character CP932 lacks becomes `?` with a warning.

For a game that runs with UTF-8 text (1.653 and later, or any game on
OpenBGI after `obgi.text_encoding(1)`):

    openbgi bpc -u title.bpc -o title._bp     # every string of the file
    #encoding utf-8                           # or: the strings after this line

`#encoding sjis` switches back.

## Including files and constants

    #include "globals.inc"
    #include "../common/strings.inc"

    #define ST_BASE     0x1bb84
    #define ST_IP       ST_BASE + 164
    #define TXT_TITLE   "Nursery Rhyme"
    #undef ST_IP

- `#include` looks beside the including file, then in the `-I` folders.
  A file is read once per compilation.
- `#define NAME text` replaces NAME by the rest of the line, as text.
  Put parentheses in the text when you mean them. No parameters.
- A string constant used twice is stored twice.

The usual use: one file with all texts of a game, included by its
programs, so that a translation is one file.

    // strings.inc
    #define TXT_FILE_MISSING "The file [ %s ] does not exist"

    // scrdrv.bpc
    #include "../strings.inc"
        sprintf(buf, TXT_FILE_MISSING, name);

## A program of your own

- The engine starts a program at its first function. Put that function
  first, before any `#include` that brings functions.
- A program over 32 KB is compiled with 32-bit references
  automatically.
- `launcher/launcher.bpc` in the source tree is a complete example
  ([launcher.md](launcher.md)).

## openbgi asm: listings

    openbgi asm -d [-e ENGINE] [-b] [-O] [-o OUT.s] (FILE._bp | --arc ARCHIVE NAME)
    openbgi asm -a [-e ENGINE] FILE.s -o OUT._bp
    openbgi asm -r [-e ENGINE] (FILE._bp... | --arc ARCHIVE [NAME...])
    openbgi asm -l ARCHIVE

`-d` disassembles, `-a` assembles, `-r` round-trips, `-l` lists an
archive. `-b` adds the instruction bytes, `-O` the offsets. `-e any`
(the default) accepts the instructions of every engine version.

    .engine 1.64
            push_fp
            push_i16 600
            add
            set_fp
            sys.get_launcher_mode
            push_code_off loc_055f
            jcc 0
            push_code_addr str_0580
            sys.push_search_name
    sub_0a1c:
            ...
            ret
    str_0580:
            .str "text"
    dat_0640:
            .db 0x01, 0x00, 0x00, 0x00

- One statement per line. `label:` defines a label. `;` starts a
  comment.
- Instructions are named `name` (base) or `family.name`; one without a
  name is written by number (`0x73`, `gfx0.0x8b`).
- Operands are numbers, labels, `+`, `-`, and `code(label)` for a
  pointer into the code area.

| Family | Byte | | Family | Byte |
|---|---|---|---|---|
| `sys` | 80 | | `snd` | A0 |
| `sys81` | 81 | | `ext0`, `ext1`, `ext2` | B0, C0, E0 |
| `sys7f` | 7F (1.667 and later) | | `eval` | D0 |
| `gfx0`, `gfx1`, `gfx2` | 90, 91, 92 | | `obgi` | EF (OpenBGI's own) |
| | | | `user` | FF |

| Instruction | Operands |
|---|---|
| `push_i8 v`, `push_i16 v`, `push_i32 v` | An immediate |
| `push_local_addr n` | `fp - n` |
| `push_code_addr L` | A data address |
| `push_code_off L` | A code target |
| `load s`, `store s`, `store_keep s` | Size code: 0 byte, 1 word, 2 dword |
| `store_inline b, b, "text"` | Up to 255 bytes |
| `store_multi s, n` | Size code and count |
| `jcc c` | Condition: 0 `!=0`, 1 `==0`, 2 `>0`, 3 `>=0`, 4 `<=0`, 5 `<0`; the target is popped |

Short forms: `push V` picks the size; `jmp L`, `call L` and `jcc C, L`
push the target first.

| Directive | Meaning |
|---|---|
| `.engine NAME` | The engine profile |
| `.str "text"[, "text"]` | NUL-terminated strings |
| `.db`, `.dw`, `.dd` | Bytes, words, dwords; strings in `.db` get no NUL |
| `.align N[, fill]` | Alignment |

Strings are UTF-8 in the listing and Shift-JIS in the file; `\xNN`
inserts a byte.

The instruction set per engine version and the 1.667 instructions:
[../dev/versions.md](../dev/versions.md).
