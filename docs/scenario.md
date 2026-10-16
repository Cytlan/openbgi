# The scenario files and bgiscn

The games from 1.494 on keep their story in `data01000.arc` as compiled
scenario files (`BurikoCompiledScriptVer1.00`): one file per chapter or
scene plus a few library files the scenes call.  Unlike the `._bp`
programs (`docs/asm.md`), which the engine executes, these files are
interpreted by the game's own system programs: `scrdrv._bp` fetches
32-bit tokens, `scrctrl._bp` implements the small stack machine behind
them, and `scrsys`, `scrmsg`, `scrslct`, `scrsnd`, `scrgrp1` to `scrgrp6`
implement the game commands (show a message, play a voice, draw a
sprite).  The original sources were C-like text; `src/scn/` recovers
that text from the files and compiles it back, so that the story - the
text to translate - can be edited and put back into the game.  The
older games have two earlier forms of the same idea - the 1.66 / 1.69
games the same token stream without the container, the 1.64 game a flat
list of 16-bit opcodes - and `bgiscn` handles those too (the section
"The older games" below).

`bin/bgiscn` (`make tools`) is the command:

    bgiscn -d [-s SYSPRG.arc] [-o OUT] [--raw] [--no-verify] [-v] (FILE | --arc ARCHIVE NAME)
    bgiscn -t [-s SYSPRG.arc] (FILE | --arc ARCHIVE NAME)
    bgiscn -c [-s SYSPRG.arc] [-I DIR]... SOURCE -o OUT
    bgiscn -r [-s SYSPRG.arc] [-v] (FILE... | --arc ARCHIVE [NAME...])
    bgiscn -l ARCHIVE
    bgiscn --pack ARCHIVE -o OUT.arc NAME=FILE...
    bgiscn --scan SYSPRG.arc

`-d` decompiles one file (or one entry of an archive) to source, `-t`
lists its tokens, `-c` compiles a source file, `-r` round-trips files
(decompile, compile, compare; the module's acceptance test), `-l` lists
an archive, `--pack` copies an archive replacing or adding entries, and
`--scan` prints the command table of a game.  The command names come from
the game's `sysprg.arc`: `-s` names it, and without `-s` the one beside
the archive given with `--arc` is used.  Without a table the commands are
numbered (`cmd_0x140`), which still round-trips.  The format of a file
is recognized from the file (the container, the headerless layout or a
16-bit scene) and that of a source from its `#style` line.

Every scenario file of the twenty games examined decompiles to source
and compiles back to the identical bytes - the 2,646 files of the
sixteen games with the container, the 497 headerless files of the three
1.66 / 1.69 games and the 353 scenes of the 1.64 game; `bgiscn -r --arc
data01000.arc` (or `nrarc02.arc`) prints `N files, 0 failed`, and no
function of them needed a token block.  `tests/scn.c` checks the
containers, the code generation and the round trip on small sources.

## Translating a game

`bgidump` and `bgipack` (`docs/dump.md`) do the steps below for every
scenario file of a game at once, along with its programs, pictures and
sounds, and are the easier way to a whole translation; `bgiscn` is the
tool for a single file.

    bgiscn -l data01000.arc                         # the entries
    bgiscn -d --arc data01000.arc _01 -o _01.bsc    # decompile a scene
    (edit _01.bsc)
    bgiscn -c -s sysprg.arc _01.bsc -o _01.bin      # compile it
    bgiscn --pack data01000.arc -o new.arc _01=_01.bin
    (replace data01000.arc with new.arc in the game folder)

The engine opens the data archives in directory order and takes the
first archive that has an entry of the name asked for, so a changed
scene has to go into `data01000.arc` itself; `--pack` keeps every other
entry as it is (the entries are stored as they are, compressed or not,
and the archive format - `PackFile` or `BURIKO ARC20` - is kept).  A
message is a line like

    voice("d_000001"); msg("「興味深い──」", "真乎", 0, 1, 1);

whose first argument is the text and second the speaker's name; the
other message commands of a game (`set_name_alias`, the selection
commands, the chapter titles) carry text the same way.  The source is
UTF-8 and the strings become Shift-JIS (CP932) in the file: a character
CP932 lacks turns into `?`, and the compiler warns about it.  (For a
game that runs with UTF-8 text - `docs/obgi.md` says how any generation
does on OpenBGI - `bgiscn -c -u`, or a line `#encoding utf-8` at the top
of the scene, keeps the strings as they are written, and a language
outside the code page can be used.)  The text
engine of the game then decides how the message is shown (the message
window's width, the font, the ruby markup `<Rまお>真乎</R>` in the text
and the rest of the markup are the game's, not the compiler's).

The line markers in the file (the source line of every statement, kept
for the game's debugger) are regenerated from the lines of the edited
text, so an edit that adds or removes lines produces a file whose
markers differ from the original - which changes nothing for the game.
`#line N` resets the count when the numbers matter.

## The container

| offset | content |
|---|---|
| 0 | `BurikoCompiledScriptVer1.00` and a NUL (28 bytes) |
| 0x1c | the size of the rest of the header |
| 0x20 | the import count, then the imported file names (NUL-terminated) |
| | the export count, then (name, dword offset) pairs |
| | 1 to 16 zero bytes: the token stream starts at a multiple of 16 |
| | the token stream: 32-bit words |
| | the string table: NUL-terminated Shift-JIS strings |

A string or code operand is a byte offset from the start of the token
stream, so the string table begins at `codeLen * 4` and the stream ends
where the lowest string any token refers to begins.  The imports are the
files loaded with this one (a scene imports the libraries whose
functions it calls); the exports are the functions a library offers,
with their offsets.  Some entries of a `data01000.arc` are not scenario
files (`StringsDB`, `ScenarioDB`: tables); `bgiscn` reports and skips
them.

## The interpreter

`scrdrv._bp` reads a token, advances by four and calls the handler the
dispatch table (`scrdrv2` fills it) holds for that number.  Tokens below
0x100 are the base machine of `scrctrl._bp`, the commands from 0x100 are
the modules'; `--scan` finds the table in a game's `sysprg.arc`,
disassembles the handlers (`src/scn/scncmds.c`) and prints for every
command its module, its name, how many values it pops, whether it pushes
a result, and the parameter names its range checks mention
(Shift-JIS):

    ; token  module    name                 args result handler  parameters
      0x140  scrmsg    msg                     5      0  0x1ab1
      0x145  scrmsg    set_text_speed          2      0  0x20c8   テキスト表示速度
      0x300  scrgrp4   sprite_draw            16      0  0x0b47   スプライト番号|ビットマップ名|...

The names of the base tokens and of the commands the games share
(`msg`, `voice`, `bgm_play`, `set_name_alias`, `sprite_draw` ...) are
built in; a command without a known name is `<module>_<token>`
(`sys_120`, `grp1_236`).  The base tokens:

| token | what it does |
|---|---|
| 0x00 `push v` | an immediate |
| 0x01 `addr off` | a code address |
| 0x02 `local n` | the address of the local at frame - n |
| 0x03 `str off` | a string |
| 0x08 `load s` | pop an address, push the byte (s 0) or dword (else) there |
| 0x09 `store s` | pop a value and an address, store (`x = v`) |
| 0x0a `store2 s` | pop an address and a value, store (`x <- v`; the switch slot) |
| 0x10 / 0x11 | push / set the frame pointer |
| 0x18 `jmp` | pop a target |
| 0x19 `jcc c` | pop a target and a value; jump when the value is zero (c 0) or nonzero (1) |
| 0x1a `call`, 0x1b `ret` | pop a target / return |
| 0x1c `callfn` | call a function of an import by name (the string on top) |
| 0x1d `setret`, 0x1e `try`, 0x1f `endtry` | the return value and the function's exit frame |
| 0x20 - 0x2b | add sub mul div mod and or xor shl shr sar |
| 0x30 - 0x35, 0x38 - 0x3a | eq ne le ge lt gt, land lor lnot |
| 0x3f `args n` | reverse the top n values |
| 0x40 `memcpy_r`, 0x48 `strcpy_r` | copies with the source on top (what the compilers' own code uses) |
| 0x80 - 0x83 | `memcpy`, `memclr`, `memset`, `memcmp` |
| 0x90 - 0x99 | `strcpy`, `strcat`, `strcpy_lower`, `strreplace`, `strlen`, `streq`, `itoa`, `substr` |
| 0xa0 - 0xc1 | `random`, `atoi`, `bool`, `mulshift`, `atan2`, `vec_len` |
| 0x7b `srcinfo`, 0x7f `line`, 0xfe `lineinfo` | source position markers |
| 0xe0 - 0xea | the work and flag variables (`set_lwork`, `lwork`, `set_gwork`, `gwork`, `set_lflag`, `lflag`, `set_gflag`, `gflag`, `lstr`, `set_gflag2`, `gflag2`) |
| 0xec `regstr`, 0xf0 `call_scenario`, 0xf1 `jump_alias`, 0xf3 `jump_scenario`, 0xf4 `quit`, 0xf5 `quit_if_idle`, 0xf6 `set_label`, 0xf7 `test_label`, 0xf8 `msgbox_num`, 0xf9 `msgbox`, 0xfa `debug_dump`, 0xfc `file_dialog`, 0xff `debug_pc` | the scene control and the debugging aids |

A command pops its arguments first argument first, so the caller leaves
the first argument on top.  Two conventions do that: the arguments
pushed right to left (the library compiler, and the message command of
the scene compiler), or pushed left to right and reversed with `args n`
(everything else in the scenes).

## Two compilers, two styles

The files were made by two compilers, and the source carries which in
its first directive, because the decompiler reproduces each one's
habits exactly.

`#style library` is the compiler of the library files (`Framework`,
`Function`, `Yuzuriha`, `Setup*`, `s_*` ...): C functions with
parameters and local declarations.  It writes a line marker (`line`)
before every statement and before every closing brace, pushes command
arguments right to left without `args`, calls a function of the same
file with `addr f; call` and one of an import with `str "f"; callfn`
(arguments left to right in both), lays the functions out `main` first
and the rest sorted by their upper-cased names (the export list follows
that order), starts the string table with the source name and the
included headers, and writes the unary minus as `x * 0xffffffff`.  Its
`if` has no `else if` form (`else { if .. }` is what it accepts), a
`switch` keeps its value in the slot just below the parameters, and
nested switches share it.

`#style scenario` is the compiler of the scenes (`_01`, `a01`, `main`,
`MakerLogo`, `macro_*`, `_GM`, `replay*` ...): a flat list of
statements and labels.  It writes its marker (`str F; push L; args 2;
lineinfo`) before simple statements only, pushes command arguments left
to right followed by `args n`, writes the message command as pushes
with no `args`, the unary minus as `0 - x`, and ends the file with a
label dispatcher: the file starts with a jump to it, it tests the label
the caller set (`test_label`) against every label of the file and jumps
there, and complains ("the specified label was not found") when none
matches.  The labels are listed in order of first mention.  `#srcinfo`
marks the newer variant that prefixes every message with a `srcinfo`
token carrying a running index and the text.

`#style scenario-line` is the older scene compiler (the 1.494 game): the
markers are `line` tokens (also before an `if`), `goto` goes through the
dispatcher (`str NAME; set_label; addr 0; jmp`), the message command is
an ordinary command in the `args` form, and the labels are listed in
order of definition.

A file whose compiler wrote no markers at all (the `replay*` files) has
no `#source` line, and the compiler then writes none either.

## The source

A file is directives, then either functions (library style) or
statements (scenario style).

    // _01 - decompiled by bgiscn (docs/scenario.md)
    #style scenario
    #import "framework", "yuzuriha", "emotion"
    #source "_01.txt"
    #line 1

    set_scene_name("帰ってきた幼馴染");
    set_name_alias("女子学院生Ａ", "女子学院生");
    set_window_design(1);
    _FadeScene(1000, 0, 0);
    voice("d_000001"); msg("「興味深い──」", "真乎", 0, 1, 1);
    slct01("昔よりもしっかり者になった", "昔よりも立派な身体になった"); if (lwork(0) == 0) {
        goto _Sel_01_01_01;
    }
    _Sel_01_01_01:
    lstr(0) = "bg03D_mx"; set_lwork(1, 400); call "_GM", "__cutin2_5A";
    jump "_02";

`#import` lists the imported files, `#source` the name the markers
carry (a full Windows path in the older variant), `#include` a header
name the library compiler put into the string table, `#srcinfo` the
message prefix described above, and `#line N` makes the next line
number N (the decompiler writes one after a token block, which has no
lines of its own).  Comments are `//` to the end of the line.

Statements end with `;`.  A command or function call is `name(a, b)`;
the arguments are in the order the handler takes them (what `--scan`
lists).  `name!(a, b)` is the direct form in a scenario file: the
arguments pushed right to left and no `args` token - what a few
machine-generated files (`replay*`, `_GM`) use for the base commands.
`cmd_0x140(..)` names a command by number, which is also how the
decompiler writes the message command when a file calls it in the
`args` form, since `msg(..)` is the direct form in the newer scene
style.  Expressions have the C operators (`+ - * / % & | ^ << >> >>>
== != < <= > >= && || !` and the unary minus; `>>` is arithmetic, `>>>`
logical), numbers are decimal or `0x` hex, strings are `"..."` with the
escapes `\n \r \t \\ \" \0 \xNN`.  `x = v;` stores through `store`, `x
<- v;` through `store2`; a condition may be an assignment (`if (l_d8 =
17)`), which the original sources have.

The scene statements: `msg(text, name, ..)`; `jump "file"[, "label"];`
and `call "file"[, "label"];` (another scenario, at a label); `call
label;` (a label of this file); `goto label;`, `label:`; `lstr(n) =
"text";` and `lstr(n) = lstr(m) + "text" + 3;` (the string variables: the
items are copied into a buffer on the frame and the buffer into the
variable; a number is pushed without a copy, as the original compiler
does); `end;` (an explicit `ret`); `if`, `else`, `else if`, `while`,
`switch` as in C.  `lstr(n)` in an expression reads a string variable.

The library statements, inside functions:

    _FileNameMaker(p1, p2, char* p3) { char l100[256]; int l204;
        memclr(l100, 256);
        switch (p1) {
            case 1:
                strcat(l100, l100, "a");
                break;
            default:
                itoa(l100, lwork(205 + p1), 0);
        }
        if (p2 > 3) { return l204; }
        while (l204 < p2) {
            l100[l204] = p3[l204];
            l204 = l204 + 1;
        }
        return l100;
    }

Parameters are `p1 ..` (`char* p` or `int* p` when the body indexes
through the parameter), locals are declared after the brace as `int x;`,
`char buf[256];`, `int arr[4];` (the names the decompiler gives are the
frame offsets: `l4`, `l100`); `_tmp` is the switch slot.  `name[i]`
indexes arrays and pointer parameters, `*(int*)(e)` / `*(char*)(e)`
reads other addresses.  `return [v];`, `break;`, `continue;` are as in
C; `switch @0xNN (v)` names the slot a nested switch uses when it is
not the usual one.  A function that is a single `__asm { .. }` block is
emitted verbatim: the decompiler falls back to that for a function it
cannot reproduce exactly (none in the games examined), and the block
takes the mnemonics of the table above (`push 1`, `addr L_12`, `str
"text"`, `line "f", 3`, `srcinfo`, a command's name or `cmd_0xNNN`,
labels `L_xx:`).

The decompiler verifies itself: after writing a file's source it
compiles it and compares; a function (or a scene body) that does not
come back identical is written as a token block instead, and `-d`
returns the number of those.  `--no-verify` skips the check, `--raw`
(or `-t`) lists the tokens.

## The older games

### 1.66 and 1.69: the headerless files

The three games of these builds (Itsusora, Tayutama and its HD release)
keep their scenario in `data010.arc` / `data01000.arc` in the same token
stream as the newer games, but without the container: the file is the
tokens from offset 0 and the strings after them, so every string and
code operand is a file offset, and there are no imports or exports.
`bgiscn` recognizes such a file by the frame prologue of its first
function (`push_fp; push N; add; set_fp`), and `bgiscn -r --arc` and `-d
--arc` take them like the others.  The source says `#style library-raw`:
the library compiler's style (every file is functions), with these
differences.

The game's `main` (Itsusora: `mainscript`) is one big library of
everything the scenes call, and the scenes call its functions by
address: `push ADDR; 0xf2`, written `@0x326f4(9721, -1, -1)`.  The
decompiler reads the arity of such a call from the game's `main` in the
same archive (which is why a scene is best decompiled with `--arc`, the
addresses being meaningless without it), and names the functions of a
library `fn_0001`, `fn_0002`, .. in their layout order - the name sort of
the layout (`main` first, then the names) then reproduces the original
order, whatever that order was.  The functions of `main` come from
several source files (its `#include` lines name them: `inst_bustshot.h`,
`chapter00.bss`, ..), and the line markers name the file each function
is in; `#file "name"` in the source switches the file the markers carry
(and restarts the line count), at the top level or inside a body.  The
compiler interns the string table in source order, the functions taken
by file (in include order) and line; `#string "text"` puts a literal
into the table before any code uses it, which the decompiler writes
where it finds a string taken in early (a constant of a header, by the
look of it).  Commands take their arguments left to right with `args n`
for two or more (none for one), the unary minus is `x * 0xffffffff`, and
`char p` declares a parameter stored as a byte (`store2 0`).  A scene is
one function `main` with no markers at all.  The message command of
these games is `msg(text, name, ..)` as in the newer ones (TayutamaHD
has its own number for it, `msg_145`, since its table differs).

Translating one of these games is the workflow above with the scene
files (`Scenario0003` ..) in place of `_01`; `bgiscn --scan` finds the
command table in their `sysprg.arc` as for the newer games.

### 1.64: the 16-bit scenes

The 1.64 game (NurseryRhyme) keeps its scenes in `nrarc02.arc` (`Start`,
`00_001_0`, `font`, ..) in a format of its own: no stack, no
expressions, but a flat stream of 16-bit opcodes, each followed by its
operands inline - dwords and NUL-terminated strings - as the handler of
that opcode reads them (`scrmain._bp` fetches the opcode and calls the
handler the dispatch table holds; the handler reads its operands through
two helpers of `scrdrv._bp`).  A jump carries its target as a file
offset, a message the file offset of its text, and the texts lie after
the code in order of reference, from the next multiple of 16 (zero
padding between; a file without a message ends with its code).  A
variable operand is a dword whose high word selects the array (1 the
scene's work variables, 2 the global ones) and whose low word is the
index; a value whose high word is zero is a literal.

The opcodes and their operands are built in (`src/scn/scn16.c`, read
from the decompiled handlers of the game's `scrmsg`, `scrgrp`, `scrgrp2`,
`scrsnd`, `scrsys`, `scrctrl`, `scrslct` and `anmscr`; `bgiscn --scan
sysprg.arc` prints the table for a 1.64 game).  The names are this
implementation's, after what the handlers do; the few opcodes nothing
is known about are `sys_da` and the like.  The source says `#style
inline` and is one statement per line:

    // 00_001_0 - decompiled by bgiscn (docs/scenario.md)
    #style inline

    lwork(1) = 0;
    bg("bg_310d1", 1000);
    set_scene_name("運命の再会？");
    kana("支倉静真", "はせくらしずま");
    msg(0, 1, "　すっかり春めいた３月末のこの日。");
    name("静真");
    msg(0, 1, "「……待ち合わせまで、まだちょっとあるな」");
    voice("mak_0001");
    msg(0, 1, "「えへへへ」");
    L_005d:
    if (lwork(1) < 5) goto L_00cd;
    select("自分から話す", "何も言わない");
    select_jump(L_1a27, L_1b55);
    call "eye_073m";
    jump "0703m_0";
    end;

A command is `name(args)` with the arguments in the order of the file;
`msg(a, b, text)` carries two flags before its text (the second is 1
when the message waits for a click, 0 when it is timed) and `name` sets
the speaker of the next message.  The control opcodes have a C-like
spelling: `goto L;`, `if (lwork(1) == 5) goto L;` (`!=`, `<`, `<=`, `>`,
`>=`), `if (flag(3)) goto L;` and `if (!flag(3)) goto L;`, `lwork(1) =
0;` (`+=`, `-=`, `*=`, `/=`, `%=`; `gwork(n)` is a global variable, and
the right side a variable or a number), `call L;` and `return;` (a
subroutine of the file), `jump "file";`, `call "file";` and `end;` (the
scene files).  `select_jump(L1, L2, ..)` jumps to the label the last
`select` chose.  Labels are `L_XXXX:` by the decompiler, any identifier
in a source; a jump target that is not an instruction boundary is
written as a number.  Numbers are decimal (negative allowed) or `0x`
hex; a dword outside the 16-bit range is written in hex
(`sprite_move(1, 0x80000010, ..)`: the high word flags a relative
move).  Strings are UTF-8 for the file's Shift-JIS as everywhere else.
The decompiler verifies its output by compiling it back; a file it
cannot reproduce gets a note on its first line (none of the game's).

A scene may include files and use constants, exactly as the program
sources do (`docs/bpc.md`, "Including files" and "Constants"; the two
share the preprocessor):

    // strings.inc
    #define NAME_SHIZUMA    "静真"
    #define T_00_001_0_0005 "「……待ち合わせまで、まだちょっとあるな」"  // NAME_SHIZUMA

    // 00_001_0.bsc
    #style inline
    #include "strings.inc"
    name(NAME_SHIZUMA);
    msg(0, 1, T_00_001_0_0005);

`#include "name"` puts the file in the place of the line - looked for
beside the scene, then in the directories of `bgiscn -I` - and `#define
NAME text` makes the name stand for the rest of its line (`#undef NAME`
ends that).  A constant may stand for anything a statement holds, a
number as well as a string, and compiles to what its text would: a
message's text still gets its own entry in the text table at every
use.  `#style inline` stays the first line.  The decompiler writes
neither; a translation kept this way is one file of constants and
scenes that only name them.  This is for the 16-bit scenes only: in the
other styles `#include` records the name of a header of the original
source (above) and no preprocessing takes place.

Translating works as above with `nrarc02.arc` in place of
`data01000.arc`:

    bgiscn -d --arc nrarc02.arc 00_001_0 -o 00_001_0.bsc
    (edit 00_001_0.bsc)
    bgiscn -c 00_001_0.bsc -o 00_001_0.bin
    bgiscn --pack nrarc02.arc -o new.arc 00_001_0=00_001_0.bin

## The API (inc/bgi/scn.h)

`Scn_Load` / `Scn_Save` / `Scn_Free` handle the container (and the
headerless layout, `raw`, and a 16-bit scene, kept as its `image`),
`Scn_String` resolves a string operand.  `Scn_ScanCommands` builds a
command table from a `sysprg.arc` (`Scn_FindCommand`,
`Scn_FindCommandByName`, `Scn_WriteCommands`; `scenes16` marks a 1.64
game).  `Scn_ListFunctions` reads a library's exported functions and
their parameter counts from their prologues (the decompiler needs the
arity of every function a file calls; a raw file's functions are listed
by address).  `Scn_Decompile` writes the source of a file to a stream
(the options carry the command table, the import functions, the name
and the verify switch), `Scn_Compile` compiles source text into a
container; both hand a 16-bit scene to `scn16.c`.  `src/scn/scnfile.c`
is the container, `scncmds.c` the scan, `scndec.c` the decompiler,
`scncomp.c` the compiler, `scntok.c` the token and operator tables,
`scn16.c` the 16-bit scenes (their table, decoder, decompiler and
compiler); `src/core/arcwrite.c` writes archives for `--pack`.

## Limits

The command table scan reads the register pattern of `scrdrv2` and the
pops of each handler; a handler that pops in a deeper subroutine is
counted short, which the decompiler compensates for at the end of a
statement.  Nested switches sharing a slot and `default:` in last
position are told apart by heuristics (a bare marker that a break of
the switch jumps to is its closing brace), which the round trip guards.
The order of a raw library's string table follows from where its own
functions stand among its includes, which the decompiler settles by
trying every place and keeping the one that leaves the fewest strings
out of order (`-v -v` shows the count).  The 16-bit opcode table covers
what the one 1.64 game registers; another game of that build would need
its handlers read the same way (`src/scn/scn16.c` says how).
