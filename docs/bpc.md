# The program decompiler and compiler

The `._bp` programs - the system programs of `sysprg.arc`, the boot
program `ipl._bp` of `system.arc`, the `autoload.arc` entries - were
written in a C-like language and compiled by Buriko's own compiler to the
stack bytecode the engine runs (`docs/vm.md`).  `src/bpc/` recovers that
source from a program and compiles such source back, so that a system
program can be edited - its texts (the window titles, the messages of the
configuration and save dialogs) translated, a behaviour changed - and put
back into the game.  The assembler and disassembler (`docs/asm.md`) are
the layer below: the decompiler starts from the disassembler's analysis
of a file, the compiler produces a listing and assembles it.

`bin/bpasm` (`make tools`) is the command:

    bpasm -D [-e ENGINE] [--no-verify] [-v] [-o OUT.bpc] (FILE._bp | --arc ARCHIVE NAME)
    bpasm -C [-e ENGINE] [-I DIR]... SOURCE.bpc -o OUT._bp
    bpasm -R [-e ENGINE] [-v] (FILE._bp... | --arc ARCHIVE [NAME...])

`-D` decompiles one program (or one entry of an archive), `-C` compiles a
source file, `-R` round-trips programs (decompile, compile, compare; the
module's acceptance test).  `-e` is the engine profile (`bgi
--list-engines`); the source records it in its `#engine` line, so `-C`
needs it only when that line is missing.  `-I` names a directory in
which `-C` looks for included files (below).  `-v` makes `-D` and `-R`
say why a function could not be written as source, `-v -v` traces the
statements.

Every program of the 21 games (1856 programs of their `system.arc`,
`sysprg.arc` and `autoload.arc`) decompiles to source and compiles back
to the identical bytes, and none of their functions needed an `__asm`
block; `bpasm -R --arc sysprg.arc` prints `N programs, 0 failed`.
`tests/bpc.c` checks the shapes and the round trip on small sources.

## Editing a system program

`bgidump` (`docs/dump.md`) decompiles every program of a game in one go
and `bgipack` compiles the edited ones back into their archives; `bpasm`
does it for one program:

    bpasm -l sysprg.arc                                # the entries
    bpasm -D -e 1.553 --arc sysprg.arc title._bp -o title.bpc
    (edit title.bpc)
    bpasm -C title.bpc -o title._bp
    bgiscn --pack sysprg.arc -o new.arc title._bp=title._bp
    (replace sysprg.arc with new.arc in the game folder)

`bgiscn --pack` (`docs/scenario.md`) repacks any archive.  The decompiler
verifies its own output - every function is compiled and compared with
the original bytes before it is written - so an unedited source always
compiles to the file it came from, and what the compiler accepts is
exactly what the original compiler's output can express.  Text is in
string literals as Shift-JIS (the source file is Shift-JIS as the
original was, with `\` escapes for what is not printable); a literal that
does not fit the window it is drawn in is the editor's problem, as it was
for the original programmers.

A sample, the ruby (reading) table of Heptagram, `kana._bp`:

    // kana._bp - decompiled by bpasm (docs/bpc.md)
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
        sprintf(l100, "読み仮名は「%d」個までしか登録できません", 4);
        debug_msg(l100);
        return;
    L_00b8:
        *(int*)(0x19fe8 + 0x17ae4) = *(int*)(0x19fe8 + 0x17ae4) + 1;
    L_00d5:
        gfx1.ruby_set(p2, p1);
        memclr(l108, 64);
        strcpy(l108 + 0, p2);
        strcpy(l108 + 32, p1);
    }

The names are the decompiler's: `main` is the function at offset 0 (the
one the engine calls), `sub_XXXX` a function by its offset, `pN` the N-th
parameter, `lXX` a local by its frame address (`push_local_addr 0xXX`),
`L_XXXX` a label by its offset.  Any of them can be renamed in the
source; nothing depends on the spelling.  The original compiler did no
constant folding, so `0x19fe8 + 0x17ae8` is what the source said (a
global's address through two macros, by the look of it) and `1 * 64`
too; folding them would change the bytes.

## The source

A source file is a list of functions, each `name(params) { body }`; the
parameters are untyped words.  Declarations - `int x;`, `char buf[64];`,
`short s;`, `int t[4] = {1, 2, 3, 4};`, `int n = f();` - may stand
anywhere a statement may, and lay out the frame in the order they are
written; the decompiler writes them on the function's first line.  A
program addresses its globals and the engine's memory numerically,
`*(int*)1208`, `*(short*)(128 + 160)`, `*(char*)(base + i)` with `int`,
`short` and `char` the 4-, 2- and 1-byte accesses (`load 2` / `load 1` /
`load 0`); that is all the compiled programs hold and all the decompiler
writes, but a source may give such addresses names ("Variables at fixed
addresses" below) and the records at them fields ("Records").  `&x` is a
variable's address, an array's name is its address, as in C, and `a[i]`
is an element of an array.

The statements are those of C: `if` / `else`, `while`, `do` / `while`,
`for`, `break`, `continue`, `return [v]`, `goto` and labels, blocks, `;`,
expression statements.  The expressions are C's, with `+ - * / % & | ^
<< >> == != < <= > >= && || ! ~` and unary `-`, `c ? a : b`, assignment
`=`, calls, and `>>>` for the logical shift (`>>` is the arithmetic
one).  Numbers are decimal or hex; `'c'` is a character; a string
literal is a `push_code_addr` of a copy in the file's string table (every
occurrence its own copy, as the original compiler did it).

Calls: a function of the program by name, `sub_0031(a, b)`; a value as
a function, `(*(int*)(l20 + 4))(x)` or `l2c(1, 0)` for a local holding a
code address (what `sys.load_program` returns); an engine instruction by
its listing name, `gfx0.sprite_create()`, `sys.yield()`, `strcpy(dst,
src)`, `sprintf(buf, "%d", n)` (`docs/asm.md` for the names; the
instruction's result is its value, an instruction that pushes nothing is
a statement).  An instruction without a name is `gfx1.0xf6(...)` by its
number, a base opcode without one `op.0x77(...)`, a script-defined one
("FF xx", a program a scenario registered) `user.0x41(...)`.

The differences from C are what the original compiler's output can and
cannot say.  A parenthesized condition is a value: `if ((a || b) && c)`
computes `a || b` with `lor` and tests the result, while `if (a || b && c)`
is a chain of jumps; the decompiler writes the parentheses it finds
and no others, and `a && b` as an operand of `&&` is never
parenthesized.  `__sign(v) < 0` (`<= 0`, `> 0`, `>= 0`) in a condition
tests the sign of `v` with the jump itself (`jcc 2` .. `jcc 5`), which
the old programs' counted loops did: `do { .. } while (__sign(i - 4) < 0);`
is not `i - 4 < 0`, which would push a `lt`.  A hex number of eight
digits, `0x00000000`, is pushed as 32 bits whatever its value.  `x <- v`
is the store that drops the value (`store`), where `x = v` keeps it on
the stack (`store_keep`) - every expression statement leaves its value
there, the machine's stack being a ring that is never cleared - and
`__pop` is a value an earlier statement left: `l4 <- __pop;` takes one
into a variable, `f(a, __pop)` passes one on, `if (__pop)` tests one.
`__inline(addr, "bytes")` is `store_inline`.  A brace-less `if (c) break;`
(`continue;`, `goto L;`, `return;`) jumps straight from the condition
(the direct form), while `if (c) { break; }` is a block with a jump in
it: the two compile differently, and the decompiler writes the form it
finds.  A `switch` there is not; the original dispatch is written as it
was compiled, a chain of `if (x == k) goto L_k;`.

`#engine NAME` names the engine profile (the opcode set).  `__asm { ..
}` holds listing lines where source cannot say what the bytes do (the
decompiler writes a function so when it could not express it or when its
source did not compile back to the same bytes; a string literal may stand
in such a block as `push_code_addr "text"`), and `__program { .. }` the
whole program as a listing.  Comments are `//` and `/* */`.

## Variables at fixed addresses

What a program keeps beyond a function call lies at fixed addresses of
the engine's memory, which every program of a game has to agree on; the
original sources evidently named them through macros, and the compiled
programs have only the numbers.  A source can declare a name for such an
address:

    global int gAbort @ 1124;
    global char st[0x4f684] @ 0x1bb84;      // a block ..
    global int stIp @ st + 164;             // .. and a variable in it
    global char stScript[32] @ st + 96;
    global int hSprite[16] @ 2732;
    global int drvReadDword @ 5584;         // holds the address of a function

A declaration stands outside the functions - usually in a file the
programs of a game include, see below - and consists of the memory area
(`global`: the engine's global memory, which nearly everything uses), the
type (`int`, `short` or `char`), the name, the number of elements for an
array, and after `@` the address.  It reserves nothing and compiles to
nothing: the engine has no notion of a program's variables, so the name
only stands for the address, and a declaration that is not used leaves no
trace in the program.  For the same reason a declaration cannot give a
value (`global int x @ 1124 = 1;` is an error): the engine does not fill
script memory from a program, and whatever needs a start value is
assigned by the code that runs first, as the original programs do it.

The name is then used like a local of that type:

    gAbort = 1;                     // *(int*)1124 = 1
    if (stIp > 0) return;           // *(int*)(0x1bb84 + 164)
    strcpy(buf, stScript);          // 0x1bb84 + 96: an array's name is its address
    sys.gflag_get(&gAbort, ..);     // 1124: the address of a variable
    hSprite[slot] = 0;              // *(int*)(2732 + slot * 4)
    n = drvReadDword();             // (*(int*)5584)(): a call through the variable's value

and compiles to exactly what the form in the comment compiles to.  The
address is compiled as the declaration wrote it, every time the name is
used: nothing is folded here either, so `@ st + 164` is `push_i32
0x1bb84; push_i16 164; add`, which is what the original programs do for
a variable inside a block, and `@ 113696` would be one push.  An address
is made of numbers and the operators `+ - * / % & | ^ << >> >>>`, and
may name arrays declared before it in the same area (and `&name` of a
variable), which stand for their own addresses.  A local variable, a
parameter or a label of the same name hides a declared one inside its
function; a function cannot share a name with one.

`a[i]` is an element of an array, a declared or a local one: the address
of the array, the index, the element size and `mul` for `int` and `short`
elements (none for `char`), then `add` - `*(int*)(a + i * 4)`,
`*(short*)(a + i * 2)`, `*(char*)(a + i)` written out, the shape most
table accesses of the original programs have.  `&a[i]` is the address
alone.  Where the original computed an address another way (`a + (i <<
2)`, a record's field `a + i * 16 + 4`) the source writes that address
with the array's name in it, `*(int*)(a + (i << 2))`.  The length of an
array is not checked, except that a constant index beyond it draws a
warning.

The other areas the engine's pointers can name are declared the same
way.  A script pointer is a tag in its top bits and an offset
(`docs/vm.md`, `docs/versions.md`); in the global memory the tag is 0
and the address is the plain number, elsewhere the address is an offset
into the area and the compiler puts the tag of the program's engine
(`#engine`) on it:

| Declaration | Area | The pointer in 1.58 .. 1.66 | 1.69 builds 444, 451 | 1.69 build 472 on |
|---|---|---|---|---|
| `global int x @ A;` | the global memory | `A` | `A` | `A` |
| `code int x @ A;` | the code area of the running thread: its program from offset 0, then the programs it loaded (`sys.load_program`) | `0x11000000 + A` | `0x10000000 + A` | `0x04000000 + A` |
| `data int x @ A;` | its data area, where the frames of the locals lie (the frame of the thread's first function starts at 0) | `0x10000000 + A` | `0x12000000 + A` | `0x08000000 + A` |
| `heap int x @ A;` | its local heap (`lalloc`) | `0x12000000 + A` | `0x14000000 + A` | `0x0c000000 + A` |
| `dynamic[n] int x @ A;` | the dynamic global block number n (`sys.galloc`) | `((0x40 + n) << 24) + A`, n below 192 | `((0x10 + n) << 25) + A`, n below 48 | `((0x10 + n) << 26) + A`, n below 48, up to 1.529 |

The tag goes onto the first number of the address, which is therefore
pushed as 32 bits: `heap int h @ 0x10 + 4;` in a 1.64 program is `push_i32
0x12000010; push_i8 4; add`.  An address there starts with a number or
with the name of a variable of the same area, and names nothing else;
its offset has to fit the bits the engine's pointers leave for it (24,
25 or 26).  These areas need the engine named - a source for `any`
engine cannot use them - and the numbered dynamic blocks exist up to
1.529 only: from 1.535 the blocks come from pools by size, and a
pointer into one has to come from the program at run time.  The words
`global`, `code`, `data`, `heap` and `dynamic` mean this only where a
declaration starts; anywhere else they are ordinary names.  None of the
system programs of the twenty games addresses anything but the global
memory by a fixed address, so the other areas are there for new code.

## Records

Most of what a program addresses is a record: a sprite slot with nine
numbers, a font with packed bytes and shorts, a button with its
pictures.  The compiled programs have only the arithmetic - `*(int*)(rec
+ 16)`, `*(int*)(*(int*)(panel + 4) + group * 52 + 8)` - but a source can
declare the layout and write the fields by name:

    struct Point  { int x; int y; };
    struct Sprite {
        int used;           // 0
        Point pos;          // 4: a record inside a record
        char name[32];      // 12
        Sprite* next;       // 44: the address of another one
        func free;          // 48: the address of a function
        int level @ 64;     // a field at a fixed offset: bytes 52..63 are not named
    };

    global Sprite stSprite[16] @ st + 1620;
    global Font   stFont       @ st + 44104;

    show(Sprite* p, i) { Sprite copy; Point* q; char* s; func tbl[4];
        stSprite[i].pos.x = p->pos.x;
        ...
    }

A record is a name for a piece of memory, as a declared variable is a
name for an address: it reserves nothing and compiles to nothing, and
an access through it compiles to exactly the arithmetic the original
programs do.  The fields follow each other without gaps - the original
compiler did not align them (a font record is a byte, two shorts, five
bytes ...) - and `@ offset` puts a field where the layout has a part
nobody names.  A record is as long as its last field ends.  The types
are `int`, `short`, `char`, a record declared before, `func` (the
address of a function) and any of them followed by `*` for the address
of one (`char**`: the address of a table of string addresses; `func*`:
of a table of functions); an array is `name[n]`, a table of rows
`name[n][m]`.

The same types can be given to a local, to a parameter and to a
variable at a fixed address.  An untyped parameter is an `int`, as
before.

| Source | Compiles as | |
|---|---|---|
| `rec.field` | `rec + offset` | the address of the record, the offset, `add` - also for offset 0, as the original has it |
| `p->field` | `*(int*)p + offset` | the value of the pointer, the offset, `add` |
| `a[i]` | `a + i * size` | the element size and `mul` for records, `int` and `short`; nothing for `char` |
| `t[i]` (addresses) | `t + (i << 2)` | a table of pointers or of `func` is indexed with a shift, which is how the original compiler did it |
| `p[i]` | `*(int*)p + i * size` | an element of what a pointer points to |
| `names[i]` (rows) | `names + i * rowsize` | a row of a two-dimensional array: an array itself |
| `&a[i]`, `&rec.field` | the address alone | |
| `sizeof(Sprite)`, `sizeof(copy)` | the number of bytes | of a type or of a variable; an array's whole length |
| `*p` | `*(char*)p` for a `char* p` | a bare `*` reads what the name was declared to point to, and the first element of an array; without a declaration it is a dword, as it always was |
| `tbl = {f, g};` `t[3] = {f, g};` | `store_multi` | a list fills an array, or the elements from one on |

A field or element is read and written with its own size.  A record and
an array have no value: their names stand for their addresses, so
`memcpy(copy, &stSprite[i], sizeof(Sprite))`, `strcpy(buf, p->name)` and
`memclr(copy, sizeof(copy))` read as they do in C, and a record is
copied with `memcpy`, not assigned.  A pointer's arithmetic is plain
arithmetic on its value (`p + 1` is one byte further; the next element
is `&p[1]`).

Because the original always adds a field's offset - also when it is 0 -
`rec.used` and a cast `*(int*)(rec)` are different instructions.  The
labelled sources of a game are written with the form the bytes have
(`tools/label.py` of the Nursery Rhyme set does that from a table of the
types), which is why now and then a `&rec.name[0]` or a `*p` appears
where C would not care.

## Strings in UTF-8

A source file is UTF-8, and a string literal is stored in Shift-JIS
(code page 932), the encoding the games use; a character the code page
does not have becomes `?`, with a warning.  For a game that runs with
UTF-8 text (1.653 on, or any generation on OpenBGI with
`obgi.text_encoding(1)`, `docs/obgi.md`) the strings are stored as they
are written instead:

    bpasm -C -u title.bpc -o title._bp      # every string of the file
    #encoding utf-8                         # or a line in the source: the strings after it

`#encoding sjis` switches back.  The directive stands on a line of its
own, anywhere outside a function; in an included file it holds on after
the include.  The decompiler writes the line at the top of a source when
it was told `-u`.

## Including files

    #include "globals.inc"
    #include "../common/strings.inc"

reads the named file in the place of the line - declarations and
constants as a rule, but anything a source may hold, functions too.  The
name is relative to the directory of the file that includes it (`/`
between directories on every system); when it is not found there, the
directories given with `-I` are tried in order.  A file is read once in
a compilation however often it is named, so a file may include what it
needs without regard to who else does.  A message about an included
file names that file and its line; a byte order mark at the start of a
file is skipped; sixteen files may include one another in a row.
`bgipack` (`docs/dump.md`) looks for an included file beside the `.bpc`
that includes it, in the dump; a source that includes one differs from
the dumped one and so is always compiled, also when only the included
file has changed.

## Constants

    #define ST_BASE     0x1bb84
    #define ST_IP       ST_BASE + 164           // may use other names
    #define SAVE_SLOTS  60
    #define TXT_TITLE   "Nursery Rhyme"         // the window title
    #undef SAVE_SLOTS

`#define NAME text` makes NAME stand for the rest of its line, without
the comment, from that line on; `#undef NAME` ends it.  Wherever the
name then appears as a word of the code - not inside a string or
character literal, not in a comment, not after the dot of an
instruction's name - the text takes its place, and is read again there,
so it may hold other names (a name inside its own text stays as it is).
Defining a name a second time with the same text is allowed, with
another text an error.  Names with parameters (`#define F(x)`) are not
supported.

The replacement is one of text, as in C, and the compiler then sees
what it would have seen had the text been written out.  That is what
makes a constant exact: nothing is folded here either, so `*(int*)(ST_IP)`
is `push_i32 0x1bb84; push_i16 164; add; load 2` - the unfolded sums in
the original programs are very likely this, its compiler's macros - and
a string constant used twice is two copies in the string table, as two
equal literals are.  It also means that a text with an operator in it
is not a unit: with `#define TWO 1 + 1`, `TWO * 3` is `1 + 1 * 3`;
write the parentheses into the text when they are meant.  A
constant may stand anywhere a number or string may, also for the length
of a local array and in the address of a declared variable (`global int
stIp @ ST_IP;`).

The usual use is a file of the texts of a game, included by its
programs, so that a translation is one file:

    // strings.inc
    #define TXT_ASK_FILE_MISSING "指定されたファイル [ %s ] は存在しません"

    // scrdrv.bpc
    #include "../strings.inc"
        sprintf(buf, TXT_ASK_FILE_MISSING, name);

`#include`, `#define` and `#undef` are carried out before the compiler
proper reads the text (`inc/bgi/prep.h`; the 16-bit scenes of
`docs/scenario.md` go through the same step, so one file of constants
can serve both).  A message names the file and line of the source
whatever was replaced on the line.  The decompiler writes none of the
three.

## The shapes of the original compiler

The decompiler relies on the original compiler's fixed way of compiling
each construct, and the compiler reproduces it; `src/bpc/bpc_internal.h`
lists the shapes.  A function is `push_fp; push F; add; set_fp`, then
the parameters popped into the frame (the first parameter popped first,
lying highest), the body, and one exit `push_fp; push F; sub; set_fp;
ret`; `return v` pushes v and jumps to the exit.  A function's arguments
are pushed right to left, an instruction's left to right, `sprintf`'s
conversions right to left before the destination and the format.  An
assignment is address, value, `store_keep`, except that a call's value
and a declaration's initializer are value, address, `store`.
Immediates are `push_i8` up to 127 and `push_i16` up to 32767; a
negative constant is the positive one negated (`not; push_i8 1; add`).
Conditions are chains of `jcc`: `a && b` jumps to the false target when
a is false, `a || b` to the true target when a is true, `!x` swaps the
targets, and a leaf jumps to whichever target is not the fall-through.
`if` jumps past the then part, `else` is a `jmp` over it, `while` tests
at the head and jumps back from the end, `do` tests at the end, `for` is
a `while` with its step before the back jump.  The compiler emitted dead
code as written (statements after a `return`, a loop's back jump after a
`break`), so does this one.

What the decompiler cannot settle it leaves to the source: locals are
typed from how they are accessed (a 4-byte load makes an `int`), sized
from the gap to the next local, and arrays when their address is taken
or they are not accessed at all (`char __pad[8]` covers a frame area
nothing refers to); a jump into a loop from outside, or into the
condition of a `do` from a nested loop, makes that loop labels and
gotos; a jump to the very next instruction (an empty `if (c) {}`, a
`goto` to the following statement) is written as the decompiler reads
it.  Everything is checked by compiling and comparing: the output is the
original's bytes or an `__asm` block.

## The API (inc/bgi/bpc.h)

`Bpc_Decompile(file, size, options, out)` writes the source of a program
file (header included) and returns the number of functions it had to
write as `__asm` blocks; `options.gen` is the engine generation,
`options.noVerify` skips the compile-and-compare, `options.verbose`
reports on stderr.  `Bpc_Compile(text, len, name, gen, includes, log,
result)` compiles source text into a program file (`result.file`,
`result.size`; `result.errors` counts the messages written to `log` as
`name:line: error: ...`) and lists the labels of the listing it assembled
(`result.labels`: every function by its name, `__codeend` where the code
ends); `includes` (a `PrepIncludes_t` of `prep.h`, may be NULL) says
where `#include` looks beyond the directory of `name` and can replace
the file reader.  `Prep_Expand` (`src/asm/prep.c`) is the preprocessor
by itself: text in, the expanded text and the origin of its lines out.  `Dis_Analyze` (`asm.h`) is the analysis the decompiler starts
from: the code / data map of a file with its labels.
