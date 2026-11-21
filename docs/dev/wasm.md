# The browser build

The engine compiled with Emscripten runs in a browser page, draws into
a canvas, and reads the game's files from a small node server over a
websocket. `src/host/wasm/` is a back end like `posix/` and `win32/`;
nothing above the OS layer changes.

It is a demonstration: it boots a game's system archives; a whole game
has not been played in a browser yet.

    make wasm                                  # bin/wasm/openbgi.html, .js, .wasm
    node tools/wasm/server.js /path/to/game    # http://127.0.0.1:8000/

The game folder is given on the server's command line and is all the
browser can see. `openbgi.json` is looked for in it; the page writes
nothing there, so the file is used only with `"staticConfig": true`.

Page parameters: `?args=--engine=1.58` passes a command line;
`?cache=256` sets the page's file cache in MB.

## How it works

| Part | Files | Design |
|---|---|---|
| Blocking calls | | The engine is synchronous. Emscripten's Asyncify unwinds the stack at calls marked asynchronous (`EM_ASYNC_JS`, `emscripten_sleep`) and rewinds it when the promise settles. |
| Yielding | `host.c` | The browser paints and delivers events only while the engine is suspended. `OS_IdleSleep` posts a message to itself (a `MessageChannel` round trip, under a millisecond). |
| Files | `fs.c`, `tools/wasm/client.js`, `server.js` | Three read-only operations: STAT, READ a range, LIST. Reads are positional and exactly the engine's range. What was fetched stays cached in the page. |
| Writes | `fs.c` | No write goes to the server. Saves, settings and the registry go to an overlay in IndexedDB (IDBFS) that shadows the server; deleted server files leave a tombstone. |
| Picture | `window.c` | The canvas is the main window: rows are converted to RGBA and put with `putImageData`; CSS scales the canvas. |
| Text input | `window.c` | While the engine takes text, keys go to an invisible text field placed over the engine's, so that the browser's input method can compose. |
| Touch and pads | `window.c`, `pad.c` | Touch events go to `OpenBGI_Touch`; pads are `navigator.getGamepads()` ([input.md](input.md)). |
| Fonts | `font.c` | No FreeType: the page is the engine's renderer. The browser's fonts draw each character into an off-screen canvas; the coverage is read back. |
| Sound | `audio.c` | The engine's thread reads its own mix at every yield and schedules it as `AudioBuffer`s about 200 ms ahead. |

Not available on a page: CD audio, movies, shortcuts, the installer's
dialogs, a second instance, the debugger, CPUID.

## Behind nginx with TLS

`tools/wasm/nginx.conf.example` is a server block for a public host:
`server.js` listens on plain HTTP on the loopback interface; nginx
terminates TLS and proxies `/` and the websocket `/fs` (with the
Upgrade handshake and day-long timeouts). The page connects to `wss://`
by itself when loaded over https, and to `fs` beside the page, so a
path prefix needs no setting.

## Checking without a browser

| Command | Checks |
|---|---|
| `make check-wasm` | `src/host/wasm/*.c` compiles with the host compiler against `tools/wasmstub/`, which drops the JavaScript bodies |
| `make wasm-test` | `tools/wasm/selftest.js` drives the server through the real client under node |
| `make wasm-sim` | The back end as a native Linux binary, `bin/linux/openbgi-wasmsim-<arch>`, with the JavaScript side replaced by C |

    cd /path/to/game
    BGI_WASMSIM_SECONDS=30 BGI_WASMSIM_SHOT=title.ppm \
    BGI_WASMSIM_SCRIPT="wait 14 click 380 516" /path/to/bin/linux/openbgi-wasmsim-x86_64

Script words: `wait S`, `click X Y`, `rclick X Y`, `move X Y`,
`key VK`, `char CP`, `touch X Y`, `pad N`, `shot FILE`.

The JavaScript bodies must tokenise as C for `check-wasm`: no
regular-expression literals and no empty `''` strings in them.

What none of this covers is the JavaScript in a real browser over a
whole game: canvas text, key mapping, audio scheduling, Asyncify.
