# The WebAssembly build

A demonstration target: the engine compiled with Emscripten runs in a
browser, drawing into a canvas, with the game's files served by a small
node program over a websocket.  The aim is the title screen of the early
titles; whatever works beyond it is a bonus.  Nothing of the engine above
the OS layer changes for it - `src/os/wasm/` is a third back end next to
`src/os/posix/` and `src/os/win32/`.

    make wasm                                  # emcc -> bin/wasm/openbgi.html (+ .js, .wasm)
    node tools/wasm/server.js /path/to/game    # the page and the files, http://127.0.0.1:8000/
    (open the address in a browser)

The game directory is given once, on the server's command line, and is the
root of everything the browser can see; `games.json` is expected inside it
like the native builds expect it next to the binary.  Query parameters of
the page: `?args=--engine=1.58` hands the engine its command line (the
boot path, `--lang=ja`, ...), `?cache=256` sets the in-memory cache of
game files in MB (512 by default), `?fs=ws://host:port/fs` points at
another server than the page's own.

## How the pieces fit

**Asyncify.**  The engine is a synchronous program: it reads a file and
expects the bytes before the next instruction, sleeps half a millisecond
between passes, and never returns to anybody.  A browser gives a page the
main thread only between events.  Emscripten's Asyncify reconciles the
two: a call marked as asynchronous (`EM_ASYNC_JS`) or `emscripten_sleep`
unwinds the whole WebAssembly stack into a buffer, returns to the browser,
and rewinds it when the promise settles, so that `OS_FileRead` can `await`
a websocket reply and come back with the bytes as if nothing had happened.
Every function that may be on the stack at such a point is instrumented,
which costs code size and some speed; for a visual novel on a desktop
browser that is nothing to notice.  (`-sJSPI`, the browsers' native
version of the same, could replace it when it is on everywhere.)

**The back end's yield.**  The browser paints the canvas, delivers
websocket messages and runs the input handlers only while the engine is
suspended, so the back end suspends at the engine's own idle points:
`OS_IdleSleep` - the half-millisecond sleep of every pass - posts a message
to itself and resumes when it arrives (a `MessageChannel` round trip, well
under a millisecond, unlike `setTimeout`, which browsers clamp to a few
milliseconds), the timed sleeps go through `emscripten_sleep`, and the
message pump yields when a pass that presented a frame skipped the idle
step.  The audio mixer is pumped at the same point (below).

**Files** (`fs.c`, `host.c`, `tools/wasm/client.js`, `tools/wasm/server.js`).
The server speaks three read-only operations - STAT a path, READ a range,
LIST a directory - and never sends a file whole unless the engine asks for
the whole of it.  The engine's own calls (open, seek, read, size, attributes,
directory enumeration) are mapped onto them, with the reads positional: an
open costs one STAT, a read one READ of exactly the engine's range, no
more.  Since the game's files are static, what has been fetched stays in
the page's memory: the metadata without limit, the file contents in 64 KB
blocks under an LRU limit (`?cache=`), so that a read asks only for the
blocks it does not hold - in one request per run of missing blocks - and a
second visit to the same archive entry costs nothing.  The server resolves
paths without regard to ASCII case (the data was written for Windows),
refuses anything that leaves the game directory (`..`, absolute paths,
symbolic links pointing out) and lists nothing over plain HTTP but the
built page.

**Writes stay in the browser.**  There is no write operation on the wire.
Everything the engine writes - saves, settings, the registry emulation of
`OS_Reg*` - goes to an *overlay*: a directory of Emscripten's in-memory
file system, persisted to IndexedDB (IDBFS, synced half a second after
the last write), that shadows the server.  A lookup tries the overlay
first, then the list of deleted paths (deleting a server file leaves a
"tombstone" that hides it), then the server; a directory listing merges
the two sides.  `OS_FileCreate` always creates in the overlay, so a server
file is never written through; moving or copying a server file reads it
and writes the copy there.  Overlay paths are kept in lower case, which
makes the case-insensitive lookups free.  `tests/wasmfs.c` exercises all
of this on a host against a mock of the server (`tools/wasm/hostmock.c`)
and runs with `make test`.

**The canvas** (`window.c`).  The page's `<canvas>` is the main window:
`OS_BlitToWindow` / `OS_StretchToWindow` convert the engine's rows to RGBA
and `putImageData` them, the canvas keeps the engine's client size (the
display mode's size in full screen) and the page's CSS scales it, so no
coordinate mapping is needed on either side.  Input comes from DOM
handlers that queue events for `OS_PumpMessages` to deliver one per call,
as the Windows message loop would; key codes are `keyCode`, which is the
Windows virtual key for all the keys the games use.  The window is created
hidden and shown by the engine, which then also gets its WM_ACTIVATE (the
1.529+ games ignore input while the window is inactive).  Dialogs are the
browser's `confirm` / `alert` / `prompt`; child windows and the text entry
do not exist and their functions report failure.

**Fonts** (`font.c`).  No FreeType and no font files: the browser's own
Japanese fonts draw each character into an off-screen canvas and the
coverage is read back and thresholded into the mono glyph the engine
asked for.  The GDI sizing contract (cell height, half-width advance) is
met through `measureText`'s font bounding box and the advance of "あ".

**Sound** (`audio.c`).  The mixer cannot run from a Web Audio callback -
it decodes music straight from the archive, which here means a websocket
round trip through Asyncify - so the engine thread pumps it: at every
yield the page says how many frames keep the output about 200 ms ahead,
the mixer produces them and they are scheduled as an `AudioBuffer` right
after the previous one.  Until the page has seen a click or a key the
`AudioContext` stays suspended (browsers require a gesture) and the frames
are mixed against the clock and dropped, so that fades and loops keep
their timing.  BW codec 3 (Vorbis) needs `src/snd/stb_vorbis.c` dropped
in, as on the other platforms; without it those sounds are silent.

**What a page has not got** reports failure the way the POSIX back end
does: CD audio, DirectShow movies (`90 F0` answers 0), other processes,
shortcuts, the installer dialogs, a second instance, the clipboard beyond
`navigator.clipboard`.  There is no CPUID: the engine's start-up skips its
processor checks on this platform (`Engine_Main`, `BGI_WASM`).

## Behind nginx with TLS

`tools/wasm/nginx.conf.example` is a server block for a public host:
server.js keeps listening on plain HTTP on the loopback interface, nginx
terminates TLS and proxies `/` (the page and its files) and `/fs` (the
websocket, with the Upgrade handshake passed through and day-long
timeouts, since an idle game sends nothing for a long time).  The page
connects to `wss://` on its own when it was loaded over https, and to
`fs` next to the page (`/bgi/fs` for a page at `/bgi/`), so a site under
a path prefix needs no setting either; the example has the `/bgi/`
variant (the prefix stays on the proxied URL, server.js looks at the
last path segment only, and `/bgi` is redirected to `/bgi/` so that the
page's relative links resolve).  Note that anyone who can reach the host can
read the game's files through the websocket: put the site behind
nginx's own access control (`auth_basic`, `allow` / `deny`) when that
matters.

## Verifying without emcc

The browser build has not been run against a real Emscripten yet, so the
first `make wasm` and the first page load may need a round of small fixes
(`docs/building.md`).  What has been verified without it:

* `make check-wasm` - every file of `src/os/wasm/` compiles with the host
  compiler against `tools/wasmstub/emscripten/emscripten.h`, a stand-in
  that declares the `EM_JS` / `EM_ASYNC_JS` functions and drops their
  JavaScript bodies; the same with clang.  A link of the whole engine
  against the back end on the host leaves only the JavaScript-side
  functions unresolved, so the OS layer's surface is complete.
* `make wasm-test` - `tools/wasm/selftest.js` starts the server on a
  scratch game directory and drives it through the real client
  (`client.js`, under node 22's WebSocket): the three operations, the
  case-insensitive lookup, ranged and short reads, the refusal of `..`,
  absolute paths and links out, overlapping requests.
* `make test` - `tests/wasmfs.c`: the file layer, cache and overlay on the
  host (above).
* `make wasm-sim` - `bin/openbgi-wasmsim`: the engine with the WebAssembly
  back end as a native binary, `tools/wasm/fakebrowser.c` standing in for
  the JavaScript side (a canvas written out as a picture, the host's
  clock, a WAV file for the sound, a scripted mouse) and `hostmock.c` for
  the server.  Run from inside a game directory:

      cd /path/to/game
      BGI_WASMSIM_SECONDS=30 BGI_WASMSIM_SHOT=title.ppm \
      BGI_WASMSIM_SCRIPT="wait 14 click 380 516" bin/openbgi-wasmsim

  Heptagram reaches its title screen this way, starts a new game on the
  click, writes its `bgi.gdb` into the overlay and reads it back on the
  next run.  What the simulation cannot cover is the JavaScript itself:
  the canvas text, the key mapping, the audio scheduling and Asyncify's
  behaviour in the browser.

## Files

    src/os/wasm/fs.c        files and directories: the protocol client side, the block cache, the overlay
    src/os/wasm/host.c      EM_ASYNC_JS bridges to client.js; the overlay mount; the Shift-JIS table
    src/os/wasm/window.c    canvas, input queue and pump, cursor, dialogs, the yield
    src/os/wasm/font.c      glyphs from the canvas
    src/os/wasm/audio.c     the Web Audio pump; MCI / movie stubs
    src/os/wasm/sys.c       process, time, locks, machine, registry, shell stubs
    src/os/wasm/sjis.c      Shift-JIS conversions from the decoded table
    inc/bgi/os_wasm.h       the back end's internal interface (WasmHost_*, the event queue)
    tools/wasm/server.js    the asset server (node, no dependencies)
    tools/wasm/client.js    the websocket client (--pre-js)
    tools/wasm/shell.html   the page
    tools/wasm/selftest.js  the protocol test (make wasm-test)
    tools/wasm/hostmock.c   the server's semantics on a local directory (tests, simulator)
    tools/wasm/fakebrowser.c the JavaScript side on a host (make wasm-sim)
    tools/wasmstub/         the stub Emscripten header (make check-wasm)
    tests/wasmfs.c          the file layer's test (make test)
