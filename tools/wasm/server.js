#!/usr/bin/env node
/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * server.js - the asset server of the WebAssembly build (docs/dev/wasm.md)
 *
 *   node tools/wasm/server.js <game directory> [--port 8000] [--bind 127.0.0.1]
 *                             [--web bin/wasm] [--verbose]
 *
 * Serves two things to the browser:
 *   * the built page (openbgi.html / .js / .wasm from the --web directory)
 *     over plain HTTP;
 *   * the game's files over a websocket at /fs, through the three read-only
 *     operations the engine's file layer needs (src/host/wasm/fs.c):
 *       STAT path                 -> attributes, size, modification time
 *       READ path offset length   -> that range of the file
 *       LIST path                 -> the entries of a directory
 *     A file is never sent whole unless the engine asks for all of it (an
 *     archive entry, a script); the multi-gigabyte archives are read in
 *     the ranges the engine reads.  There is no write operation: what the
 *     engine writes (saves, settings) stays in the browser (fs.c's overlay).
 *
 * One game per server: the directory given on the command line is the root
 * of everything the browser can see.  Paths from the client are relative
 * to it, resolved component by component without regard to ASCII case
 * (the game data was written for a case-insensitive file system), and a
 * path that leaves the root - through "..", an absolute path or a symbolic
 * link - is answered with "not found".  openbgi.json is expected inside the
 * game directory like any other file.
 *
 * No dependencies: the websocket framing (RFC 6455) is implemented below so
 * that the server runs with the bare `node` binary (18 or later).
 *
 * Wire format (little-endian; shared with tools/wasm/client.js and
 * tests/wasmfs.c):
 *   request   u32 id | u8 op | payload
 *   response  u32 id | i32 status | payload
 *   op 1 STAT   payload: path            status 0 ok / -1 missing;
 *               reply: u32 attrs, u32 sizeLo, u32 sizeHi, u32 mtimeLo, u32 mtimeHi (FILETIME)
 *   op 2 READ   payload: u32 offLo, u32 offHi, u32 len, path
 *               status = bytes read (short at the end of the file) / -1; reply: the bytes
 *   op 3 LIST   payload: path            status = entry count / -1;
 *               reply per entry: u32 attrs, u32 sizeLo, u32 sizeHi, u16 nameLen, name (UTF-8)
 * Attributes are the Windows ones the engine knows: 0x01 read-only, 0x02
 * hidden (a dot file), 0x10 directory, 0x80 normal.
 */
'use strict';

const fs = require('fs');
const http = require('http');
const path = require('path');
const crypto = require('crypto');

const OP_STAT = 1, OP_READ = 2, OP_LIST = 3;
const ATTR_READONLY = 0x01, ATTR_HIDDEN = 0x02, ATTR_DIRECTORY = 0x10, ATTR_NORMAL = 0x80;
const MAX_READ = 16 * 1024 * 1024; // the longest range one READ answers
const MAX_FRAME = 1 << 20;         // the longest request frame accepted
const FILETIME_EPOCH_MS = 11644473600000n;

// ---- command line -------------------------------------------------------------------

function usage(msg)
{
	if(msg)
		console.error('server.js: ' + msg);
	console.error('usage: node tools/wasm/server.js <game directory> [--port N] [--bind ADDR] [--web DIR] [--verbose]');
	process.exit(2);
}

const opts = { port: 8000, bind: '127.0.0.1', web: path.join(__dirname, '..', '..', 'bin', 'wasm'), verbose: false, root: null };
{
	const argv = process.argv.slice(2);
	for(let i = 0; i < argv.length; i++)
	{
		const a = argv[i];
		if(a === '--port')
			opts.port = parseInt(argv[++i], 10);
		else if(a === '--bind')
			opts.bind = argv[++i];
		else if(a === '--web')
			opts.web = argv[++i];
		else if(a === '--verbose' || a === '-v')
			opts.verbose = true;
		else if(a === '--help' || a === '-h')
			usage();
		else if(a.startsWith('--'))
			usage('unknown option ' + a);
		else if(opts.root === null)
			opts.root = a;
		else
			usage('one game directory only');
	}
	if(opts.root === null || !(opts.port > 0))
		usage(opts.root === null ? 'the game directory is missing' : 'bad port');
}

let ROOT;
try
{
	ROOT = fs.realpathSync(opts.root);
	if(!fs.statSync(ROOT).isDirectory())
		usage(opts.root + ' is not a directory');
}
catch(e)
{
	usage('cannot open ' + opts.root + ': ' + e.message);
}
const WEB = path.resolve(opts.web);

function log(...args)
{
	if(opts.verbose)
		console.log(...args);
}

// ---- paths ----------------------------------------------------------------------------

/* The real path of a client path, or null when it does not exist or leaves
 * the root.  Each component is matched against the directory's entries:
 * the exact spelling first, then a single case-insensitive match. */
const dirCache = new Map(); // real directory -> {names, lower, time}

function dirEntries(realDir)
{
	const c = dirCache.get(realDir);
	const now = Date.now();
	if(c && now - c.time < 2000)
		return c;
	let names;
	try
	{
		names = fs.readdirSync(realDir);
	}
	catch(e)
	{
		return null;
	}
	const lower = new Map();
	for(const n of names)
	{
		const l = n.toLowerCase();
		if(!lower.has(l))
			lower.set(l, n);
	}
	const rec = { names, lower, time: now };
	dirCache.set(realDir, rec);
	return rec;
}

function resolveClientPath(rel)
{
	if(typeof rel !== 'string' || rel.indexOf('\0') >= 0)
		return null;
	const parts = rel.split(/[\\/]+/).filter(p => p !== '' && p !== '.');
	let cur = ROOT;
	for(const p of parts)
	{
		if(p === '..')
			return null;
		const ents = dirEntries(cur);
		if(!ents)
			return null;
		let name = null;
		if(ents.names.includes(p))
			name = p;
		else
			name = ents.lower.get(p.toLowerCase()) || null;
		if(name === null)
			return null;
		cur = path.join(cur, name);
	}
	// a symbolic link may still point outside: the real path has to stay under the root
	let real;
	try
	{
		real = fs.realpathSync(cur);
	}
	catch(e)
	{
		return null;
	}
	if(real !== ROOT && !real.startsWith(ROOT + path.sep))
		return null;
	return real;
}

function attrsOf(st, name)
{
	let a = 0;
	if(st.isDirectory())
		a |= ATTR_DIRECTORY;
	if((st.mode & 0o200) === 0)
		a |= ATTR_READONLY;
	if(name.startsWith('.'))
		a |= ATTR_HIDDEN;
	return a || ATTR_NORMAL;
}

function fileTime(ms)
{
	const ft = (BigInt(Math.floor(ms)) + FILETIME_EPOCH_MS) * 10000n;
	return [Number(ft & 0xffffffffn), Number((ft >> 32n) & 0xffffffffn)];
}

// ---- the file operations ----------------------------------------------------------------

// open file descriptors are kept for a while: the engine reads one archive in many ranges
const fdCache = new Map(); // real path -> {fd, time}
const FD_CACHE_MAX = 32;

function openCached(real)
{
	let rec = fdCache.get(real);
	if(rec)
	{
		rec.time = Date.now();
		return rec.fd;
	}
	if(fdCache.size >= FD_CACHE_MAX)
	{
		let oldest = null;
		for(const [k, v] of fdCache)
			if(!oldest || v.time < fdCache.get(oldest).time)
				oldest = k;
		fs.closeSync(fdCache.get(oldest).fd);
		fdCache.delete(oldest);
	}
	const fd = fs.openSync(real, 'r');
	fdCache.set(real, { fd, time: Date.now() });
	return fd;
}

function u32(buf, off, v)
{
	buf.writeUInt32LE(v >>> 0, off);
}

function doStat(rel)
{
	const real = resolveClientPath(rel);
	let st = null;
	if(real)
	{
		try
		{
			st = fs.statSync(real);
		}
		catch(e)
		{
			st = null;
		}
	}
	if(!st)
		return { status: -1, body: Buffer.alloc(0) };
	const body = Buffer.alloc(20);
	const [mlo, mhi] = fileTime(st.mtimeMs);
	u32(body, 0, attrsOf(st, path.basename(real)));
	u32(body, 4, Number(BigInt(st.size) & 0xffffffffn));
	u32(body, 8, Number(BigInt(st.size) >> 32n));
	u32(body, 12, mlo);
	u32(body, 16, mhi);
	return { status: 0, body };
}

function doRead(rel, offLo, offHi, len)
{
	const real = resolveClientPath(rel);
	if(!real || len > MAX_READ)
		return { status: -1, body: Buffer.alloc(0) };
	try
	{
		const st = fs.statSync(real);
		if(!st.isFile())
			return { status: -1, body: Buffer.alloc(0) };
		const off = offHi * 4294967296 + offLo;
		if(off >= st.size || len === 0)
			return { status: 0, body: Buffer.alloc(0) };
		if(off + len > st.size)
			len = st.size - off;
		const fd = openCached(real);
		const body = Buffer.allocUnsafe(len);
		let got = 0;
		while(got < len)
		{
			const n = fs.readSync(fd, body, got, len - got, off + got);
			if(n <= 0)
				break;
			got += n;
		}
		return { status: got, body: got === len ? body : body.subarray(0, got) };
	}
	catch(e)
	{
		fdCache.delete(real);
		return { status: -1, body: Buffer.alloc(0) };
	}
}

function doList(rel)
{
	const real = resolveClientPath(rel);
	let names;
	try
	{
		if(!real || !fs.statSync(real).isDirectory())
			return { status: -1, body: Buffer.alloc(0) };
		names = fs.readdirSync(real);
	}
	catch(e)
	{
		return { status: -1, body: Buffer.alloc(0) };
	}
	const parts = [];
	let count = 0;
	for(const n of names)
	{
		let st;
		try
		{
			const full = path.join(real, n);
			if(fs.lstatSync(full).isSymbolicLink())
			{ // a link is listed only when it stays inside the game directory
				const target = fs.realpathSync(full);
				if(target !== ROOT && !target.startsWith(ROOT + path.sep))
					continue;
			}
			st = fs.statSync(full);
		}
		catch(e)
		{
			continue; // a dangling link
		}
		const nameBuf = Buffer.from(n, 'utf8');
		if(nameBuf.length > 0xffff)
			continue;
		const e = Buffer.alloc(14);
		u32(e, 0, attrsOf(st, n));
		u32(e, 4, Number(BigInt(st.size) & 0xffffffffn));
		u32(e, 8, Number(BigInt(st.size) >> 32n));
		e.writeUInt16LE(nameBuf.length, 12);
		parts.push(e, nameBuf);
		count++;
	}
	return { status: count, body: Buffer.concat(parts) };
}

// one request frame -> one response frame
function handleRequest(msg)
{
	if(msg.length < 5)
		return null;
	const id = msg.readUInt32LE(0);
	const op = msg[4];
	let r;
	if(op === OP_STAT)
	{
		const rel = msg.toString('utf8', 5);
		r = doStat(rel);
		log('STAT', JSON.stringify(rel), r.status);
	}
	else if(op === OP_READ && msg.length >= 17)
	{
		const offLo = msg.readUInt32LE(5), offHi = msg.readUInt32LE(9), len = msg.readUInt32LE(13);
		const rel = msg.toString('utf8', 17);
		r = doRead(rel, offLo, offHi, len);
		log('READ', JSON.stringify(rel), offHi * 4294967296 + offLo, len, '->', r.status);
	}
	else if(op === OP_LIST)
	{
		const rel = msg.toString('utf8', 5);
		r = doList(rel);
		log('LIST', JSON.stringify(rel), r.status);
	}
	else
		r = { status: -1, body: Buffer.alloc(0) };
	const head = Buffer.alloc(8);
	u32(head, 0, id);
	head.writeInt32LE(r.status, 4);
	return Buffer.concat([head, r.body]);
}

// ---- websocket framing (RFC 6455, server side) ----------------------------------------

function wsFrame(opcode, payload)
{
	let head;
	if(payload.length < 126)
	{
		head = Buffer.alloc(2);
		head[1] = payload.length;
	}
	else if(payload.length < 0x10000)
	{
		head = Buffer.alloc(4);
		head[1] = 126;
		head.writeUInt16BE(payload.length, 2);
	}
	else
	{
		head = Buffer.alloc(10);
		head[1] = 127;
		head.writeBigUInt64BE(BigInt(payload.length), 2);
	}
	head[0] = 0x80 | opcode; // FIN + opcode
	return Buffer.concat([head, payload]);
}

function wsConnection(socket)
{
	let buf = Buffer.alloc(0);
	let fragments = [];
	let fragOpcode = 0;
	let open = true;

	const send = (opcode, payload) => {
		if(open)
			socket.write(wsFrame(opcode, payload));
	};
	const close = (code) => {
		if(!open)
			return;
		open = false;
		const p = Buffer.alloc(2);
		p.writeUInt16BE(code, 0);
		try
		{
			socket.end(wsFrame(8, p));
		}
		catch(e)
		{
		}
	};

	socket.on('data', (chunk) => {
		buf = buf.length ? Buffer.concat([buf, chunk]) : chunk;
		for(;;)
		{
			if(buf.length < 2)
				return;
			const fin = (buf[0] & 0x80) !== 0;
			const opcode = buf[0] & 0x0f;
			const masked = (buf[1] & 0x80) !== 0;
			let len = buf[1] & 0x7f;
			let pos = 2;
			if(len === 126)
			{
				if(buf.length < 4)
					return;
				len = buf.readUInt16BE(2);
				pos = 4;
			}
			else if(len === 127)
			{
				if(buf.length < 10)
					return;
				const big = buf.readBigUInt64BE(2);
				if(big > BigInt(MAX_FRAME))
					return close(1009);
				len = Number(big);
				pos = 10;
			}
			if(len > MAX_FRAME)
				return close(1009);
			if(!masked)
				return close(1002); // a client must mask
			if(buf.length < pos + 4 + len)
				return;
			const mask = buf.subarray(pos, pos + 4);
			const payload = Buffer.allocUnsafe(len);
			for(let i = 0; i < len; i++)
				payload[i] = buf[pos + 4 + i] ^ mask[i & 3];
			buf = buf.subarray(pos + 4 + len);

			if(opcode === 8)
				return close(1000);
			if(opcode === 9)
			{
				send(10, payload); // ping -> pong
				continue;
			}
			if(opcode === 10)
				continue;
			if(opcode === 1 || opcode === 2 || opcode === 0)
			{
				if(opcode !== 0)
					fragOpcode = opcode;
				fragments.push(payload);
				if(!fin)
					continue;
				const msg = fragments.length === 1 ? fragments[0] : Buffer.concat(fragments);
				fragments = [];
				if(fragOpcode === 2)
				{
					const reply = handleRequest(msg);
					if(reply)
						send(2, reply);
				}
				continue;
			}
			return close(1002);
		}
	});
	socket.on('error', () => { open = false; });
	socket.on('close', () => { open = false; });
}

function wsHandshake(req, socket)
{
	const key = req.headers['sec-websocket-key'];
	if(!key || (req.headers.upgrade || '').toLowerCase() !== 'websocket')
	{
		socket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
		return;
	}
	const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
	socket.write('HTTP/1.1 101 Switching Protocols\r\n' +
		'Upgrade: websocket\r\n' +
		'Connection: Upgrade\r\n' +
		'Sec-WebSocket-Accept: ' + accept + '\r\n\r\n');
	socket.setNoDelay(true);
	wsConnection(socket);
}

// ---- the static page ------------------------------------------------------------------

const MIME = {
	'.html': 'text/html; charset=utf-8',
	'.js': 'text/javascript; charset=utf-8',
	'.mjs': 'text/javascript; charset=utf-8',
	'.wasm': 'application/wasm',
	'.data': 'application/octet-stream',
	'.map': 'application/json',
	'.json': 'application/json',
	'.css': 'text/css; charset=utf-8',
	'.png': 'image/png',
	'.ico': 'image/x-icon',
};

function serveStatic(req, res)
{
	if(req.method !== 'GET' && req.method !== 'HEAD')
	{
		res.writeHead(405);
		return res.end();
	}
	// only the last segment matters: a proxy may or may not strip its prefix (/bgi/openbgi.js)
	let url = decodeURIComponent(req.url.split('?')[0]);
	url = url.substring(url.lastIndexOf('/'));
	if(url === '/' || url === '')
		url = '/openbgi.html';
	const file = path.resolve(WEB, '.' + url);
	if(file !== WEB && !file.startsWith(WEB + path.sep))
	{
		res.writeHead(404);
		return res.end();
	}
	fs.stat(file, (err, st) => {
		if(err || !st.isFile())
		{
			res.writeHead(404, { 'Content-Type': 'text/plain' });
			return res.end(url === '/openbgi.html' ? 'openbgi.html is not built yet: run `make wasm` first (docs/dev/wasm.md)\n' : 'not found\n');
		}
		res.writeHead(200, {
			'Content-Type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream',
			'Content-Length': st.size,
			'Cache-Control': 'no-cache',
		});
		if(req.method === 'HEAD')
			return res.end();
		fs.createReadStream(file).pipe(res);
	});
}

const server = http.createServer(serveStatic);
server.on('upgrade', (req, socket) => {
	const u = req.url.split('?')[0];
	if(u === '/fs' || u.endsWith('/fs')) // with or without a proxy's prefix
		wsHandshake(req, socket);
	else
		socket.end('HTTP/1.1 404 Not Found\r\n\r\n');
});
server.listen(opts.port, opts.bind, () => {
	console.log('openbgi asset server: game ' + ROOT);
	console.log('  page http://' + opts.bind + ':' + opts.port + '/   files ws://' + opts.bind + ':' + opts.port + '/fs');
	if(!fs.existsSync(path.join(WEB, 'openbgi.html')))
		console.log('  (no ' + path.join(WEB, 'openbgi.html') + ' yet: run `make wasm`)');
});
