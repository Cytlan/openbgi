# Scenario files: the story

The story of a game is in compiled scenario files. `openbgi scn`
decompiles one to text and compiles it back. For a whole game use
`openbgi dump` and `openbgi pack` ([dump-pack.md](dump-pack.md)), which
do the same for every file at once.

| Engine version | Archive | Form |
|---|---|---|
| 1.494 and later | `data01000.arc` | Container (`BurikoCompiledScriptVer1.00`) |
| 1.66, 1.69 | `data010.arc`, `data01000.arc` | The same code without the container |
| 1.64 | `nrarc02.arc` | 16-bit scenes |

The binary formats: [../dev/formats/scenario.md](../dev/formats/scenario.md).

## The command

    openbgi scn -d [-s SYSPRG.arc] [-o OUT] [--raw] [--no-verify] [-v] (FILE | --arc ARCHIVE NAME)
    openbgi scn -c [-s SYSPRG.arc] [-I DIR]... [-u] SOURCE -o OUT
    openbgi scn -t [-s SYSPRG.arc] (FILE | --arc ARCHIVE NAME)
    openbgi scn -r [-s SYSPRG.arc] [-v] (FILE... | --arc ARCHIVE [NAME...])
    openbgi scn -l ARCHIVE
    openbgi scn --pack ARCHIVE -o OUT.arc NAME=FILE...
    openbgi scn --scan SYSPRG.arc

| Option | Does |
|---|---|
| `-d` | Decompiles a file, or an entry of an archive, to source |
| `-c` | Compiles a source file |
| `-t` | Lists a file's tokens |
| `-r` | Round trip: decompile, compile, compare |
| `-l` | Lists an archive |
| `--pack` | Copies an archive, replacing or adding entries |
| `--scan` | Prints a game's command table |
| `-s` | The game's `sysprg.arc`, where the command names come from. Default: the one beside the archive given with `--arc`. |
| `-u` | Keeps strings as UTF-8 (for a game switched to UTF-8, [obgi.md](obgi.md)) |

Without a command table, commands are numbered (`cmd_0x140`); that
still compiles back.

## Translating one file

    openbgi scn -l data01000.arc
    openbgi scn -d --arc data01000.arc _01 -o _01.bsc
    (edit _01.bsc)
    openbgi scn -c -s sysprg.arc _01.bsc -o _01.bin
    openbgi scn --pack data01000.arc -o new.arc _01=_01.bin

Put the result in a patch ([patches.md](patches.md)) or in place of the
game's archive. The engine takes an entry from the first data archive
that has it, so a changed scene must go into the archive it came from.

A message looks like this:

    voice("d_000001"); msg("text", "speaker", 0, 1, 1);

- The source is UTF-8. Strings become Shift-JIS (CP932) in the file; a
  character CP932 lacks becomes `?` with a warning. With `-u`, or a
  line `#encoding utf-8` at the top, they stay UTF-8.
- Line width, font and markup (ruby: `<Rreading>text</R>`) are the
  game's, not the compiler's.
- Adding or removing lines is safe: line markers are regenerated.
  `#line N` sets the next line's number.

## The source

A file is directives, then statements (scenes) or functions
(libraries). The first directive is `#style`; keep it as the decompiler
wrote it.

| Style | Files |
|---|---|
| `scenario` | Scenes (`_01`, `main`, `macro_*`, `replay*`) |
| `scenario-line` | Scenes of the 1.494 game |
| `library` | Library files (`Framework`, `Function`, `Setup*`, `s_*`) |
| `library-raw` | Every file of 1.66 and 1.69 |
| `inline` | The 16-bit scenes of 1.64 |

| Directive | Meaning |
|---|---|
| `#import "a", "b"` | The files loaded with this one |
| `#source "name"` | The name the line markers carry |
| `#srcinfo` | Every message is prefixed with an index and its text |
| `#line N` | The next line is number N |
| `#file "name"` | (library-raw) The source file the following functions belong to |
| `#string "text"` | (library-raw) Puts a string into the table at this point |
| `#encoding utf-8` | Strings stay UTF-8 |

Common syntax:

- Statements end with `;`. Comments are `//`.
- `name(a, b)` calls a command or function. `cmd_0x140(...)` names a
  command by number. `name!(a, b)` is the direct call form some
  generated files use.
- Operators: `+ - * / % & | ^ << >> >>> == != < <= > >= && || !`
  (`>>` arithmetic, `>>>` logical). Numbers are decimal or `0x` hex.
- Strings: `"..."` with `\n \r \t \\ \" \0 \xNN`.
- `x = v;` stores; `x <- v;` stores with the operands in the other
  order (the switch slot).

### Scenes

    #style scenario
    #import "framework", "yuzuriha", "emotion"
    #source "_01.txt"

    set_scene_name("title");
    voice("d_000001"); msg("text", "speaker", 0, 1, 1);
    slct01("first choice", "second choice"); if (lwork(0) == 0) {
        goto _Sel_01_01_01;
    }
    _Sel_01_01_01:
    lstr(0) = "bg03D_mx"; set_lwork(1, 400); call "_GM", "__cutin2_5A";
    jump "_02";

| Statement | Meaning |
|---|---|
| `msg(text, name, ...)` | A message |
| `jump "file"[, "label"];` | Go to another scenario |
| `call "file"[, "label"];` | Call another scenario |
| `call label;`, `goto label;`, `label:` | Within this file |
| `lstr(n) = "text";`, `lstr(n) = lstr(m) + "text" + 3;` | String variables |
| `lwork(n)`, `gwork(n)`, `lflag(n)`, `gflag(n)` and their `set_` forms | Local and global work and flag variables |
| `end;` | Return |
| `if`, `else`, `else if`, `while`, `switch` | As in C |

### Libraries

    _FileNameMaker(p1, p2, char* p3) { char l100[256]; int l204;
        memclr(l100, 256);
        switch (p1) {
            case 1:
                strcat(l100, l100, "a");
                break;
            default:
                itoa(l100, lwork(205 + p1), 0);
        }
        while (l204 < p2) {
            l100[l204] = p3[l204];
            l204 = l204 + 1;
        }
        return l100;
    }

- Parameters are `p1, p2, ...`; `char* p` or `int* p` when the body
  indexes through one.
- Locals are declared after the brace: `int x;`, `char buf[256];`,
  `int arr[4];`. The decompiler names them by frame offset.
- `name[i]` indexes; `*(int*)(e)` and `*(char*)(e)` read an address.
- `return [v];`, `break;`, `continue;` as in C. No `else if`: write
  `else { if ... }`.
- A function the decompiler cannot reproduce exactly is written as an
  `__asm { ... }` block of tokens. None of the examined games needs one.

In 1.66 and 1.69, scenes call functions of the game's `main` by
address: `@0x326f4(9721, -1, -1)`. Decompile such a scene with `--arc`,
so that the arity is read from `main` in the same archive.

### 16-bit scenes (1.64)

One statement per line:

    #style inline

    lwork(1) = 0;
    bg("bg_310d1", 1000);
    name("speaker");
    msg(0, 1, "text");
    L_005d:
    if (lwork(1) < 5) goto L_00cd;
    select("first", "second");
    select_jump(L_1a27, L_1b55);
    jump "0703m_0";
    end;

| Statement | Meaning |
|---|---|
| `msg(a, b, text)` | A message; `b` is 1 when it waits for a click |
| `name("x")` | The speaker of the next message |
| `goto L;`, `if (cond) goto L;`, `call L;`, `return;` | Within this file |
| `lwork(n) = v;` and `+= -= *= /= %=` | Variables; `gwork(n)` is global |
| `if (flag(n)) goto L;`, `if (!flag(n)) goto L;` | Flags |
| `select(...)`, `select_jump(L1, L2, ...)` | A choice and its targets |
| `jump "file";`, `call "file";`, `end;` | Between scenes |

These scenes may use `#include "file"` and `#define NAME text`, as the
program sources do ([programs.md](programs.md)). A translation can then
be one file of constants:

    // strings.inc
    #define T_00_001_0_0005 "text"

    // 00_001_0.bsc
    #style inline
    #include "strings.inc"
    msg(0, 1, T_00_001_0_0005);

In the other styles `#include` only records a header name of the
original source.

## Checking

`openbgi scn -r --arc data01000.arc` round-trips every file and prints
`N files, 0 failed`. The decompiler also verifies each file as it writes
it; `--no-verify` skips that.
