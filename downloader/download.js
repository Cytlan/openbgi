//
// Game Downloader
// Assists with downloading trial versions of various games
//
// Part of the BGI-RE Project
//

const fs = require('fs')
const unpacker = require('all-unpacker')
const http = require('http')
const https = require('https')
const URL = require('url')
const crypto = require('crypto')

const timeout = 10000

const games = {
	'nurseryrhyme': {
		'title': 'Nursery Rhyme',
		'url': 'https://www.lumpofsugar.co.jp/product/nursery_rhyme/index.html',
		'mirrors': [
			'http://www.sumica.net/mirror/lumpofsugar/nurseryrhyme_trial.lzh',
			'http://suezou.dyndns.org/dr/nurseryrhyme/nurseryrhyme_trial.lzh',
			'http://dl.studio-ramble.com:8081/304/nurseryrhyme_trial.lzh',
			'http://oic.storage-service.jp:8080/lumpofsugar/nurseryrhyme_trial.lzh'
		],
		'hash': '0307b275280d41f59a1c28b5b54851a2365959f5292e5877ab7e5ebfcb6d1227',
		'archiveName': 'nurseryrhyme_trial.lzh',
		'postscript': [
			(cb) => { fs.renameSync('NurseryRhyme【体験版】', 'nurseryrhyme'); cb() }
		]
	},
	'tayutama': {
		'title': 'タユタマ (Tayutama)',
		'url': 'https://www.lumpofsugar.co.jp/product/tayutama/index.html',
		'mirrors': [
			'http://oic.storage-service.jp:8080/lumpofsugar/tayutama_trial_Web.zip',
			'http://mirror.studio-ramble.com/download.php?id=3750',
			'http://mirror.tsundere.ne.jp/download/lumpofsugar/9d23058b91254689d5abad64ada8afce/tayutama_trial_Web.zip'
		],
		'hash': '9d2edb1e6efa35fef17b70c1dc7b2424074d97c2e9d1165139b171230ec3e783',
		'archiveName': 'tayutama_trial_Web.zip',
		'postscript': [
			(cb) => { fs.renameSync('tayutama_trial_Web', 'tayutama'); cb() }
		]
	},
	'hanairo': {
		'title': '花色ヘプタグラム (Color of the flower Heptagram)',
		'url': 'https://www.lumpofsugar.co.jp/product/hanairo/index.php',
		'mirrors': [
			'https://oic.storage-service.jp:8080/lumpofsugar/HanairoHeptagram_WebTrial.zip',
			'http://suezou.dyndns.org/dl2012/lumpofsugar/hanairo/HanairoHeptagram_WebTrial.zip'
		],
		'hash': '56140f676eacdb539db5d7f071fc01462afb81608940e043075f2ff83866083b',
		'archiveName': 'HanairoHeptagram_WebTrial.zip',
		'postscript': [
			(cb) => { fs.renameSync('HanairoHeptagram_WebTrial', 'hanairo'); cb() }
		]
	},
	'tayutama2': {
		'title': 'タユタマ２ (Tayutama 2)',
		'url': 'https://www.lumpofsugar.co.jp/product/tayutama2/index.html',
		'mirrors': [
			'http://mirror6.maidservant.org:8000/lumpofsugar/ad869570719b591998d0fb2e19dfe561/tayutama2_trial_EX.zip',
			'http://mirror11.maidservant.org:8000/lumpofsugar/ad869570719b591998d0fb2e19dfe561/tayutama2_trial_EX.zip'
			//'http://mirror0.maidservant.org/file/lumpofsugar/ad869570719b591998d0fb2e19dfe561/tayutama2_trial_EX.zip'
		],
		'hash': '074539c57fd78c5e54aa8aa3197ef58bd27feaf16a01e35f767e37dc8bf6e2f7',
		'archiveName': 'tayutama2_trial_EX.zip',
		'postscript': [
			(cb) => { fs.renameSync('tayutama2_trial_EX', 'tayutama2'); cb() }
		]
	}
}

function downloadFile(mirrorUrl, outfile, cb)
{
	const mirror = URL.parse(mirrorUrl)

	const options = {
		hostname: mirror.hostname,
		port: mirror.port ? mirror.port : (mirror.protocol == 'https:' ? 443 : 80),
		path: mirror.path,
		method: 'GET'
	}

	let httpObj = options.port == 443 ? https : http
	const req = httpObj.request(options, (res) =>
	{
		// Follow redirects
		if(res.statusCode == 301 || res.statusCode == 302)
		{
			console.log('Redirected: '+res.headers['location'])
			let redirected = true
			res.destroy()
			downloadFile(res.headers['location'], outfile, cb)
			return
		}
		if(res.statusCode != 200)
			return cb(new Error('Mirror did not respond with 200 OK, got '+res.statusCode))

		const totalFile = parseInt(res.headers['content-length']) || 0
		let downloaded = 0

		res.on('data', (d) =>
		{
			fs.writeSync(outfile, d)
			downloaded += d.length
			if(totalFile)
				process.stdout.write('\r'+((downloaded/totalFile)*100).toFixed(3)+'% ('+(downloaded/1048510).toFixed(2)+' MiB / '+(totalFile/1048510).toFixed(2)+' MiB)')
			else
				process.stdout.write('\r'+(downloaded/1048510)+' MiB')
		})

		res.on('end', () =>
		{
			process.stdout.write('\n')
			fs.closeSync(outfile)
			cb()
		})

		res.on('error', (error) =>
		{
			process.stdout.write('\n')
			fs.closeSync(outfile)
			cb(error)
		})
	})

	req.on('timeout', () =>
	{
		req.destroy(new Error('Timeout'))
	})

	req.on('error', (error) =>
	{
		fs.closeSync(outfile)
		cb(error)
	})

	req.on('socket', (s) =>
	{
		s.setTimeout(timeout, () =>
		{
			s.destroy()
		})
	})

	req.setTimeout(timeout, () =>
	{
		req.destroy(new Error('Timeout'))
	})

	req.end()
}

function checkFile(filename, hash, cb)
{
	console.log('Checking '+filename)
	let shasum = crypto.createHash('sha256')
	try
	{
		let s = fs.ReadStream(filename)
		s.on('data', (data) =>
		{
			shasum.update(data)
		})
		// making digest
		s.on('end', () =>
		{
			const calcHash = shasum.digest('hex')
			console.log('Calculated hash: '+calcHash)
			if(calcHash === hash)
				cb(null, true)
			else
				cb(new Error('Hash mismatch'), false)
		})

		s.on('error', (error) =>
		{
			cb(error, false)
		})
	}
	catch(error)
	{
		cb(error, false)
	}
}

function unpackFile(filename, cb)
{
	console.log('Extracting '+filename)
	unpacker.unpack(filename,
	{
		quiet: true,
		copyTime: true,
		forceOverwrite: true,
		targetDir: './data/'
	}, (err, files, text) =>
	{
		if(err) return cb(err)
		return cb()
	})
}

function downloadGame(game, cb)
{
	console.log('Fetching '+game.title)

	let finalise = (err) =>
	{
		if(err) return cb(err)

		// Run post-scripts
		if(game.postscript && game.postscript.length > 0)
		{
			let cwd = process.cwd()
			process.chdir('./data')
			let index = 0
			let postscriptLoop = () =>
			{
				game.postscript[index]((err) =>
				{
					if(err)
					{
						process.chdir(cwd)
						return cb(err)
					}
					index++
					if(index >= game.postscript.length)
					{
						process.chdir(cwd)
						return cb()
					}
					else
						postscriptLoop()
				})
			}
			postscriptLoop()
		}
		else
			return cb()
	}

	// Is the file already downloaded?
	let aname = './data/'+game.archiveName
	checkFile(aname, game.hash, (err, res) =>
	{
		// Hash mismatch or file doesn't exist
		if(err)
		{
			console.log('Must re-download archive')
			let mirrorIndex = 0
			let mirrors = game.mirrors.sort((a, b) => { return Math.random() - 0.5 })
			const downloadLoop = () =>
			{
				if(mirrorIndex == mirrors.length)
					return cb(new Error('All mirrors failed to download'))

				console.log('Downloading from: '+mirrors[mirrorIndex])
				const fd = fs.openSync(aname, 'w')
				downloadFile(mirrors[mirrorIndex], fd, (err) =>
				{
					if(err)
					{
						console.log('Error: '+err)
						mirrorIndex++
						return downloadLoop()
					}
					checkFile(aname, game.hash, (err, res) =>
					{
						if(err) return cb(err)
						unpackFile(aname, finalise)
					})
				})
			}
			downloadLoop()
		}
		else
		{
			console.log('File matches')
			unpackFile(aname, finalise)
		}
	})
}

// Make sure the data directory exists
try
{
	fs.mkdirSync('data')
}
catch(e)
{
	if(e.code != 'EEXIST') throw e
}

// Delete all directories in the data dir (but don't delete already downloaded archives)
let dirs = fs.readdirSync('./data')
for(let i in dirs)
{
	let p = './data/'+dirs[i]
	let stat = fs.statSync(p)
	if(stat.isDirectory())
		fs.rmdirSync(p, {recursive: true})
}

// Download/extract games
let gameIndex = 0
let gameKeys = Object.keys(games)
let gameLoop = () =>
{
	if(gameIndex == gameKeys.length)
		process.exit(0)

	downloadGame(games[gameKeys[gameIndex]], (err) =>
	{
		if(err) throw err
		console.log('Done')
		console.log()
	
		gameIndex++
		gameLoop()
	})
}
gameLoop()
