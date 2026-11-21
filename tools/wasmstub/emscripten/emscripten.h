/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * emscripten.h - a stand-in for the Emscripten header so that the host
 * compiler can syntax-check src/host/wasm/ (`make check-wasm`, see
 * docs/dev/building.md).  EM_JS / EM_ASYNC_JS declare the function and drop the
 * JavaScript body (the declaration ends itself, as the real macros' text
 * does: no ';' follows a use); the few runtime calls the back end makes are
 * declared.
 * Never used by the real build: emcc finds its own header first.
 */
#ifndef WASMSTUB_EMSCRIPTEN_H_
#define WASMSTUB_EMSCRIPTEN_H_

#define EM_JS(ret, name, params, ...)       ret name params;
#define EM_ASYNC_JS(ret, name, params, ...) ret name params;
#define EMSCRIPTEN_KEEPALIVE

void emscripten_sleep(unsigned ms);
double emscripten_get_now(void);

#endif // WASMSTUB_EMSCRIPTEN_H_
