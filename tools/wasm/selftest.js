#!/usr/bin/env node
/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * selftest.js - exercises server.js through client.js without a browser
 *
 *   node tools/wasm/selftest.js
 *
 * Builds a throw-away game directory, starts the server on a free port,
 * and checks the three operations, the case-insensitive lookup, the range
 * reads and that nothing outside the directory can be reached (`..`,
 * absolute paths, a symbolic link pointing out).  Needs node 22 (the
 * global WebSocket client); exits 0 when every check passes.
 */
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const net = require('net');
const BgiNet = require('./client.js');

if(typeof WebSocket === 'undefined')
{
	console.error('selftest: this node has no WebSocket client (node 22 or later is needed)');
	process.exit(2);
}

let failures = 0;
function check(cond, what)
{
	console.log((cond ? '  ok   ' : '  FAIL ') + what);
	if(!cond)
		failures++;
}

function freePort()
{
	return new Promise((resolve) => {
		const s = net.createServer();
		s.listen(0, '127.0.0.1', () => {
			const port = s.address().port;
			s.close(() => resolve(port));
		});
	});
}

async function main()
{
	const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'openbgi-selftest-'));
	const outside = fs.mkdtempSync(path.join(os.tmpdir(), 'openbgi-outside-'));
	fs.writeFileSync(path.join(outside, 'secret.txt'), 'outside');
	fs.mkdirSync(path.join(dir, 'Save'));
	const big = Buffer.alloc(300000);
	for(let i = 0; i < big.length; i++)
		big[i] = (i * 7 + (i >> 8)) & 0xff;
	fs.writeFileSync(path.join(dir, 'Data01000.arc'), big);
	fs.writeFileSync(path.join(dir, 'openbgi.json'), '[]');
	fs.writeFileSync(path.join(dir, 'Save', 'sysdat.dat'), 'hello');
	fs.symlinkSync(outside, path.join(dir, 'link'));

	const port = await freePort();
	const server = spawn(process.execPath, [path.join(__dirname, 'server.js'), dir, '--port', String(port)], { stdio: ['ignore', 'pipe', 'inherit'] });
	await new Promise((resolve) => server.stdout.on('data', (d) => { if(String(d).includes('asset server')) resolve(); }));

	try
	{
		await BgiNet.connect('ws://127.0.0.1:' + port + '/fs');

		let r = await BgiNet.stat('data01000.ARC');
		check(r.status === 0 && r.sizeLo === big.length && r.attrs === 0x80, 'STAT finds a file regardless of case');
		r = await BgiNet.stat('save');
		check(r.status === 0 && (r.attrs & 0x10) !== 0, 'STAT reports a directory');
		r = await BgiNet.stat('');
		check(r.status === 0 && (r.attrs & 0x10) !== 0, 'STAT of the root');
		r = await BgiNet.stat('nothing.arc');
		check(r.status === -1, 'STAT of a missing file');
		r = await BgiNet.stat('../' + path.basename(outside) + '/secret.txt');
		check(r.status === -1, 'STAT cannot climb out with ..');
		r = await BgiNet.stat(path.join(outside, 'secret.txt'));
		check(r.status === -1, 'STAT of an absolute path is refused');
		r = await BgiNet.stat('link/secret.txt');
		check(r.status === -1, 'a symbolic link out of the game directory is invisible');
		r = await BgiNet.stat('save/../openbgi.json');
		check(r.status === -1, '.. is refused even when it would stay inside');

		let b = await BgiNet.read('data01000.arc', 1000, 0, 16);
		check(b && b.length === 16 && Buffer.from(b).equals(big.subarray(1000, 1016)), 'READ of a range');
		b = await BgiNet.read('DATA01000.arc', 299990, 0, 100);
		check(b && b.length === 10 && Buffer.from(b).equals(big.subarray(299990)), 'READ is short at the end of the file');
		b = await BgiNet.read('data01000.arc', 400000, 0, 100);
		check(b && b.length === 0, 'READ past the end gives nothing');
		b = await BgiNet.read('data01000.arc', 0, 0, big.length);
		check(b && b.length === big.length && Buffer.from(b).equals(big), 'READ of the whole file when asked');
		b = await BgiNet.read('save', 0, 0, 16);
		check(b === null, 'READ of a directory fails');
		b = await BgiNet.read('link/secret.txt', 0, 0, 16);
		check(b === null, 'READ through the link fails');
		b = await BgiNet.read('data01000.arc', 0, 0, 64 * 1024 * 1024);
		check(b === null, 'a READ longer than the server limit is refused');

		const names = (raw) => {
			const out = [];
			if(!raw)
				return null;
			const v = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
			let p = 0;
			while(p + 14 <= raw.length)
			{
				const attrs = v.getUint32(p, true), n = v.getUint16(p + 12, true);
				out.push({ name: Buffer.from(raw.subarray(p + 14, p + 14 + n)).toString('utf8'), attrs, size: v.getUint32(p + 4, true) });
				p += 14 + n;
			}
			return out;
		};
		let l = names(await BgiNet.list(''));
		check(l && l.map(e => e.name).sort().join() === 'Data01000.arc,Save,openbgi.json', 'LIST of the root keeps the spelling and hides the link out (' + (l ? l.map(e => e.name).sort().join() : 'null') + ')');
		check(l && l.find(e => e.name === 'Save').attrs === 0x10, 'LIST marks the directory');
		check(l && l.find(e => e.name === 'Data01000.arc').size === big.length, 'LIST carries the sizes');
		l = names(await BgiNet.list('SAVE'));
		check(l && l.length === 1 && l[0].name === 'sysdat.dat', 'LIST of a subdirectory by another case');
		l = names(await BgiNet.list('link'));
		check(l === null, 'LIST through the link fails');
		l = names(await BgiNet.list('openbgi.json'));
		check(l === null, 'LIST of a file fails');

		// many requests in flight at once keep their ids straight
		const all = await Promise.all([0, 1, 2, 3, 4, 5, 6, 7].map(i => BgiNet.read('data01000.arc', i * 1000, 0, 8)));
		check(all.every((x, i) => x && Buffer.from(x).equals(big.subarray(i * 1000, i * 1000 + 8))), 'eight overlapping requests');
	}
	finally
	{
		if(BgiNet.ws)
			BgiNet.ws.close();
		server.kill();
		fs.rmSync(dir, { recursive: true, force: true });
		fs.rmSync(outside, { recursive: true, force: true });
	}
	console.log(failures ? failures + ' check(s) failed' : 'all checks passed');
	process.exit(failures ? 1 : 0);
}

main().catch((e) => {
	console.error(e);
	process.exit(1);
});
