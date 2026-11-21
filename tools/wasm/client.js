/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * client.js - the browser side of the asset protocol (tools/wasm/server.js)
 *
 * Linked into the WebAssembly build as a --pre-js file; src/host/wasm/host.c
 * calls BgiNet from its EM_ASYNC_JS functions.  The same file runs under
 * node for the protocol test (tools/wasm/selftest.js), which is why nothing
 * here depends on the Emscripten runtime.
 *
 * Every request is a binary websocket message answered by exactly one
 * binary message with the same id (the layout is in server.js).  The C
 * side awaits the reply through Asyncify, so a request is in flight at most
 * once at a time from the engine; the map of pending requests still allows
 * more, for a future audio or prefetch path.
 */
'use strict';

var BgiNet = {
	OP_STAT: 1,
	OP_READ: 2,
	OP_LIST: 3,

	url: null,
	ws: null,
	ready: null,          // the promise of the open connection
	pending: new Map(),   // id -> {resolve}
	nextId: 1,
	encoder: new TextEncoder(),
	stats: { requests: 0, bytes: 0 },

	// open (or reopen) the connection; resolves when the socket is open
	connect: function(url)
	{
		if(url)
			this.url = url;
		if(this.ready)
			return this.ready;
		var self = this;
		this.ready = new Promise(function(resolve, reject) {
			var ws = new WebSocket(self.url);
			ws.binaryType = 'arraybuffer';
			ws.onopen = function() { resolve(ws); };
			ws.onerror = function(e) { reject(new Error('websocket error')); };
			ws.onmessage = function(ev) { self.onMessage(ev.data); };
			ws.onclose = function() {
				self.ws = null;
				self.ready = null;
				// everything in flight fails; the next request reconnects
				for(var p of self.pending.values())
					p.resolve(null);
				self.pending.clear();
			};
			self.ws = ws;
		});
		this.ready.catch(function() { self.ready = null; self.ws = null; });
		return this.ready;
	},

	onMessage: function(data)
	{
		var bytes = data instanceof ArrayBuffer ? new Uint8Array(data) : new Uint8Array(data.buffer || data);
		if(bytes.length < 8)
			return;
		var view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
		var id = view.getUint32(0, true);
		var p = this.pending.get(id);
		if(!p)
			return;
		this.pending.delete(id);
		this.stats.bytes += bytes.length;
		p.resolve({ status: view.getInt32(4, true), body: bytes.subarray(8) });
	},

	// one request; resolves to {status, body} or null when the connection failed
	request: function(op, payload)
	{
		var self = this;
		return this.connect().then(function(ws) {
			var id = self.nextId++;
			if(self.nextId > 0xfffffff0)
				self.nextId = 1;
			var msg = new Uint8Array(5 + payload.length);
			new DataView(msg.buffer).setUint32(0, id, true);
			msg[4] = op;
			msg.set(payload, 5);
			self.stats.requests++;
			return new Promise(function(resolve) {
				self.pending.set(id, { resolve: resolve });
				ws.send(msg);
			});
		}, function() {
			return null;
		});
	},

	// STAT: {status, attrs, sizeLo, sizeHi, mtimeLo, mtimeHi} (status -1: missing or unreachable)
	stat: function(path)
	{
		return this.request(this.OP_STAT, this.encoder.encode(path)).then(function(r) {
			if(!r || r.status !== 0 || r.body.length < 20)
				return { status: -1, attrs: 0, sizeLo: 0, sizeHi: 0, mtimeLo: 0, mtimeHi: 0 };
			var v = new DataView(r.body.buffer, r.body.byteOffset, r.body.byteLength);
			return { status: 0, attrs: v.getUint32(0, true), sizeLo: v.getUint32(4, true), sizeHi: v.getUint32(8, true),
				mtimeLo: v.getUint32(12, true), mtimeHi: v.getUint32(16, true) };
		});
	},

	// READ: the bytes (possibly fewer than asked at the end of the file), null on failure
	read: function(path, offLo, offHi, len)
	{
		var p = this.encoder.encode(path);
		var payload = new Uint8Array(12 + p.length);
		var v = new DataView(payload.buffer);
		v.setUint32(0, offLo, true);
		v.setUint32(4, offHi, true);
		v.setUint32(8, len, true);
		payload.set(p, 12);
		return this.request(this.OP_READ, payload).then(function(r) {
			if(!r || r.status < 0)
				return null;
			return r.body.subarray(0, r.status);
		});
	},

	// LIST: the raw entry records (see server.js), null when the directory is missing
	list: function(path)
	{
		return this.request(this.OP_LIST, this.encoder.encode(path)).then(function(r) {
			if(!r || r.status < 0)
				return null;
			return r.body;
		});
	},
};

// under node (the self-test) this is a module; inside the Emscripten output `Module` exists and nothing is exported
if(typeof Module === 'undefined' && typeof module !== 'undefined' && module.exports)
	module.exports = BgiNet;
