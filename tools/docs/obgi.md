# OpenBGI's own instructions: the "EF xx" family

The original engine has no instruction that starts with the byte `EF`, in
any generation.  OpenBGI puts its own there: instructions for things the
original cannot do, which a game's system programs call when they are
rebuilt for OpenBGI.  They do what a translation project runs into first -
text that does not fit three lines of a message window, text in UTF-8,
languages that are written from right to left.

* The family exists in every engine profile (1.58 to 1.71), with the same
  numbers and meanings in all of them.
* The assembler and the program compiler call it `obgi`: `obgi.version`
  is `EF 00`, and a `.bpc` source calls `obgi.text_scroll(1, 150);` like
  any other instruction (`docs/asm.md`, `docs/bpc.md`).
* A program that uses one **does not run on the original engine**, which
  stops at the first `EF` byte with "undefined instruction".  A program
  that uses none is not affected in any way: every switch below is off
  until a program turns it on.
* The code is `src/vm/ops_obgi.c`; the tests are `TestExtensions` of
  `tests/vm.c`, `TestLayoutExtensions` of `tests/text.c` and
  `tests/bidi.c`.

| Instruction | Name | Stack: inputs → outputs | |
|---|---|---|---|
| `EF 00` | `obgi.version` | → level | the level of the extensions: 1 |
| `EF 01` | `obgi.text_direction` | mode → | the layout of rich text: 0 the original, 1 right to left, 2 left to right for everything else |
| `EF 02` | `obgi.text_encoding` | mode → | the encoding of every string: 0 Shift-JIS, 1 UTF-8 |
| `EF 03` | `obgi.text_word_wrap` | on → | break lines between words (the generations before 1.494 do not) |
| `EF 08` | `obgi.text_scroll` | on, msPerLine → | a message longer than its window scrolls |
| `EF 09` | `obgi.text_scroll_arrows` | bmpUp, upX, upY, bmpDown, downX, downY → | the pictures and places of the two scroll arrows |
| `EF 0A` | `obgi.text_fit` | h, dst, src, lines, maxBytes, rubyOn, hang → used | how much of a string fills so many lines of a window |
| `EF 0B` | `obgi.text_width` | str, fontNo, size, widthPct, bold, prop → width | the width of a string in pixels |
| `EF 0C` | `obgi.text_scroll_pos` | h → y | how far the text of a window is scrolled |

The inputs are in the order a program pushes them (the last one is on
top of the stack), which is the order of the arguments in a source.
`obgi.version()` lets a program ask before it uses a later addition; the
level is raised whenever instructions are added.

## The encoding of the strings: `obgi.text_encoding`

    obgi.text_encoding(1);      // every string from here on is UTF-8

The engine reads the strings of the scripts - messages, names, file names,
everything - in one encoding: Shift-JIS (code page 932) by default, or
UTF-8.  The generations from 1.653 on have an instruction of their own
for this (`81 00`); `EF 02` gives it to all of them.  The values are 0
(Shift-JIS) and 1 (UTF-8); anything else changes nothing.

The switch belongs at the very start of the boot program (`ipl._bp`),
before any string is used: nothing converts the strings, so **all**
programs and **all** scenes of the game have to be written in the
encoding that is switched on.  The tools write UTF-8 strings with `-u`
(`bpasm -C -u`, `bgiscn -c -u`) or when the source says `#encoding utf-8`
(`docs/bpc.md`, `docs/scenario.md`).  A string in the other encoding
shows as garbage.

In UTF-8 text the code of a character is its Unicode code point.  The
codes U+FF01 .. U+FFFF are also the private codes of the glyph sheet
(`gfx0.text_set_glyph_sheet`) and of the character pictures; in UTF-8
mode a code that has neither a picture nor a cell of the sheet is drawn
from the font, so that the full-width forms (`！`, `，`, `（`) are
characters as they should be.

## The layout: `obgi.text_direction`

    obgi.text_direction(0);     // the original layout: Japanese, Chinese
    obgi.text_direction(1);     // right to left: Arabic, Hebrew
    obgi.text_direction(2);     // left to right, for every other language

The original layout is made for Japanese: a character of one byte takes
half a cell, everything else a full one.  In UTF-8 that gives `å`, `é`
or a Cyrillic letter the cell of a kanji, and it has no idea of text
that runs from right to left.  Modes 1 and 2 use a layout of OpenBGI's
own (`src/gfx/text/layout_bidi.c`) for all "rich" text - what the
message windows show (`gfx1.text_out_styled`, `gfx1.draw_text`,
`gfx1.draw_text_styled`, `gfx0.text_out`) and the text drawn into
bitmaps (`gfx2.text`, `gfx1.bmp_draw_text`, `gfx1.bmp_draw_text_styled`:
name plates, the backlog):

* A character that is not East Asian (kana, ideographs, Hangul, their
  punctuation, the full-width forms) is as wide as its font makes it.
  With a Japanese fixed-pitch font an ASCII letter is still half a cell,
  so English looks as before; `å` is as wide as `a`; with a font that has
  proportional Latin or Cyrillic letters the text is set proportionally.
* Lines break between words.  East Asian characters still break
  anywhere.
* The direction of every character is resolved by the Unicode
  bidirectional algorithm (UAX #9, without the explicit embedding and
  isolate controls and without the pairing of brackets): Hebrew and
  Arabic run from right to left, Latin words and numbers inside them
  from left to right, and brackets are mirrored where they have to be.
* Arabic letters are joined: each becomes its isolated, initial, medial
  or final form, and lam + alef become their ligature.  The forms are
  taken from the Arabic Presentation Forms of Unicode, so the font has
  to have those (the usual Arabic fonts do); there is no OpenType
  shaping, which Arabic text in a simple typeface does not need.
  Vowel marks are centred over their letter.

Mode 1 makes the paragraphs themselves right to left: a line starts at
the right edge of the text area and the click cursor stands at its left
end.  Mode 2 keeps the paragraphs left to right; a Hebrew or Arabic
phrase inside is still set correctly.  The text is typed out in reading
order in both, so an Arabic message appears from the right.

The strings are written in logical order - the order they are read and
typed in - without any direction marks.  Right-to-left scripts need the
UTF-8 encoding.

What the layout of modes 1 and 2 does not do: the readings (ruby) of the
dictionary, the Japanese line-breaking rules (kinsoku), the inline tags
of 1.494 on, vertical windows.  Japanese and Chinese text belongs in
mode 0, which is untouched.

### Fonts

The engine draws with the font the game asks for, and that is a
Japanese one.  For a character the font does not have, the Linux build
takes the glyph from another font that has it (through fontconfig, or
from the usual font directories); the Windows build leaves that to GDI,
the browser build to the browser.  So Arabic or Chinese text shows on a
machine that has fonts for it, in whatever those fonts look like next to
the game's.  For a release in another script, give the game a font made
for it: `BGI_FONT_GOTHIC`, `BGI_FONT_MINCHO` or `BGI_FONT` name the font
file on Linux (`docs/os_layer.md`).

Japanese fonts draw Cyrillic and Greek letters and the typographic
quotes (`“ ” ‘ ’`) a full cell wide.  That is the font's choice and
modes 1 and 2 respect it; a translation into English is better off with
the ASCII `"` and `'`, one into Russian with a font that has
proportional Cyrillic.

Right-to-left text and the font fallback have been tried on Linux only.
The Windows and browser builds compile with them and measure the glyphs
through the same interface (`OS_FontCharAdvance`), but have not been
run.

## Lines broken between words: `obgi.text_word_wrap`

    obgi.text_word_wrap(1);

Up to 1.494 the original breaks a line where it is full, in the middle
of a word too.  With the switch on, the original layout (mode 0) keeps a
Latin word together and drops the space a line would end with, as the
generations from 1.494 on do by themselves.  Modes 1 and 2 always break
between words.

## Long messages: `obgi.text_scroll`

    obgi.text_scroll(1, 150);   // on; a line scrolls in 150 ms

The original lays a message out, reveals it glyph by glyph and does not
show what lies below the text area: a message that is too long is cut
off.  With scrolling on, a window keeps the glyphs of its text as a
document and shows a view of it:

* **While it is printed** at the pace of the text speed, the view moves
  up one line whenever the next glyph lies below it, and printing goes
  on.
* **When the output is hurried** - a click, the "instant" setting, a text
  speed of zero - only what the view has room for is shown at once.  If
  more follows, the output pauses: the down arrow blinks, and a click
  (or the auto mode's timer) turns the page.
* **While the output waits for the click** - in such a pause and at the
  end of the message - the player can move the view: the mouse wheel,
  the up and down cursor keys and the two arrows scroll by a line (an
  arrow that is held down keeps scrolling).  Scrolling down past the end
  is the usual "go on".
* When nobody is reading - the message is followed at once by something
  else, the output was told to finish, the skip key is down - the view
  jumps to the end.

A message that fits its window behaves exactly as before.  The click
cursor is the original one; it is hidden while its place is scrolled
out of view.  A page break (`\x0c`), `gfx1.window_clear` and a new text
area empty the document.  Vertical windows are not scrolled.

`msPerLine` is the time a scroll of one line takes; 0 or less keeps the
current one (120 at start).

The two arrows are a triangle in the text colour, sized by the font, at
the end of the first line (up) and of the last line (down) - the right
end, or the left end in right-to-left text.  A game can give them its
own pictures and places:

    obgi.text_scroll_arrows(BMP_UP, 600, 4, BMP_DOWN, 600, 70);
    obgi.text_scroll_arrows(-1, -1, -1, -1, -1, -1);    // the built-in ones again

Each arrow takes a bitmap number (-1: the triangle) and its position in
the window in pixels (a negative x or y: the default place).  The
pictures are copied, so the bitmaps may be freed afterwards.

A program that gives the mouse wheel a meaning of its own asks
`obgi.text_scroll_pos(window)` first: it is 0 while the view is at the
top, and otherwise how far the text is scrolled, in pixels.  Nursery
Rhyme opens the backlog on "wheel up"; its OpenBGI version does so only
at the top, and lets the wheel scroll the message back otherwise.

## Measuring text: `obgi.text_fit`, `obgi.text_width`

    used = obgi.text_fit(hMsgWnd, rec.text, msg, 3, 255, 1, 1);

lays the string `msg` out as a message in window `hMsgWnd` (with the
same `rubyOn` and `hang` arguments as `gfx1.draw_text`), from the start
of a line, and copies what fills 3 lines to `rec.text` - at most 255
bytes and the terminator, cut at the end of a line, without the space or
line break the line ended with.  The result is the number of bytes of
`msg` that were used up (0 only for an empty string or a window without
a font).  Called in a loop it cuts a long text into pieces of a few
lines: that is how a long message becomes several entries of a backlog
whose entries hold three lines.

    x = (204 - obgi.text_width(name, 0, 24, 100, 0, 1)) / 2;

is the width in pixels of a string on one line in the numbered font
(size in pixels, width in percent, bold, proportional), as the layout
places it - right for proportional text and for joined Arabic, where
counting bytes is not.

## An example: Nursery Rhyme

The Nursery Rhyme script set (`nr-scripts`) has the programs of that
game as sources, and in its `openbgi/` folder the three that use the
extensions: `ipl` sets the encoding, the layout and the word wrap from
the settings of `openbgi.inc`; `scrmsg` turns scrolling on, cuts a long
message into backlog entries with `obgi.text_fit`, centres the name
plate with `obgi.text_width`; `sysmsg` asks `obgi.text_scroll_pos`
before it opens the backlog.  `translations/` there has the first scene
of the story in English, Chinese, Russian, Hebrew, Arabic and Norwegian
to try them with.

## Adding an instruction

A handler in `src/vm/ops_obgi.c` and its line in `Vm_OptableEFInit`; the
number in the "family EF" row of `src/vm/opset.c` (and in `OBGI_OPS` of
its generator, `internal/re_tools/mkopset.py`, which is not part of the
release); its name in `src/asm/opnames.c` and its stack effect in
`src/asm/opstack.c`; `OBGI_VERSION` one higher; a section here.
