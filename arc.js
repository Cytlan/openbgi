//
// arc.js - Reading ARC files
//
//
const fs = require('fs')
const PNG = require('pngjs').PNG

const HEX = "0123456789ABCDEF";
const BINARY_4BITS = ["0000", "0001", "0010", "0011", "0100", "0101", "0110", "0111", "1000", "1001", "1010", "1011", "1100", "1101", "1110", "1111"];
function toHex4( num )
{
	return HEX[num & 0x0F];
}

function toHex8( num )
{
	return toHex4(num >> 4) + toHex4(num);
}

function toHex16( num )
{
	return toHex8(num >> 8) + toHex8(num & 0xFF);
}

function toHex32( num )
{
	return toHex16(num >> 16) + toHex16(num & 0xFFFF);
}

class ArcFile
{
	constructor(arc, filename, size, offset)
	{
		this.arc = arc
		this.filename = filename
		this.size = size
		this.offset = offset
	}
}

class Arc
{
	constructor(options)
	{
		this.files = {}
		this.filenames = []
	}

	openArchive(filename, callback)
	{
		this.filename = filename
		fs.open(filename, 'r', (err, fd) =>
		{
			if(err)
			{
				if(callback) return callback(err, null)
				throw err
			}
			this.fd = fd
			this.parseArchive(callback)
		})
	}

	parseArchive(callback)
	{
		// Read header
		let header = new Buffer(16)
		fs.read(this.fd, header, 0, header.length, 0, (err, bytesRead, buffer) =>
		{
			// If read error
			if(err)
			{
				if(callback) return callback(err, null)
				throw err
			}

			// Validate header
			if(bytesRead != header.length || header.toString('ascii', 0, 12) != 'PackFile    ')
			{
				let err = new Error('Header mismatch')
				if(callback) return callback(err, null)
				throw err
			}

			// Read number of files
			this.fileCount = header.readUInt32LE(12)

			// Calculate data offset
			this.fileOffset = 16 + (this.fileCount * 32)

			// Read file list
			if(this.fileCount)
				this.readFileList(callback)
			else if(callback)
				callback(null, [])
		})
	}

	readFileList(callback)
	{
		// Read entire file list in one go
		let bufferSize = this.fileCount * 32
		let buffer = new Buffer(bufferSize)
		fs.read(this.fd, buffer, 0, buffer.length, 16, (err, bytesRead, buffer) =>
		{
			// Read error
			if(err)
			{
				if(callback) return callback(err, null)
				throw err
			}

			// Make sure we read as many bytes as we requested
			if(bytesRead != buffer.length)
			{
				let err = new Error('Failed to read file list')
				if(callback) return callback(err, null)
				throw err
			}

			// Parse all file entries
			for(let i = 0; i < this.fileCount; i++)
			{
				// Offset
				let off = i * 32

				// Filename
				let name = buffer.toString('ascii', off, off + 16);

				// Cut filename at null, if any is found
				let n = name.indexOf('\0');
				if(n > 0)
					name = name.substr(0, n);

				// Offset and size
				let offset = buffer.readUInt32LE(off + 16);
				let size = buffer.readUInt32LE(off + 20);
				// 8 bytes: unknown, always 0

				this.files[name] = new ArcFile(this, name, size, offset);
				this.filenames.push(name)
			}

			if(callback)
				callback(null, this.files)
		})
	}

	queueFile(filename, path)
	{
		this.files[filename] = {path: path}
		if(!this.fileCount)
			this.fileCount = 0
		this.fileCount++
	}

	openFile(filename, callback)
	{
		if(this.fd)
			return callback(null, this.fd)
		fs.open(filename, 'w', (err, fd) =>
		{
			if(err) return callback(err)
			this.fd = fd
			callback(null, this.fd)
		})
	}

	writeFileList(outfile, callback)
	{
		let headerSize = 16 + (this.fileCount * 32)
		let out = Buffer.alloc(headerSize)
		let offset = 0
		out.write('PackFile    ', offset); offset += 12
		out.writeUInt32LE(this.fileCount, offset); offset += 4

		let filenameBuffer = Buffer.alloc(16)

		for(let i in this.files)
		{
			let file = this.files[i]
			let filename = i

			filenameBuffer.fill(0)
			filenameBuffer.write(filename, 0)

			filenameBuffer.copy(out, offset, 0, 16); offset += 16
			out.writeUInt32LE(file.offset ? file.offset : 0, offset); offset += 4
			out.writeUInt32LE(file.size ? file.size : 0, offset); offset += 4
			out.writeUInt32LE(0, offset); offset += 4
		}

		this.dataOffset = headerSize
		this.fileOffset = headerSize

		this.openFile(outfile, (err, fd) =>
		{
			if(err) return callback(err, null)
			fs.write(fd, out, 0, headerSize, 0, (err, bytesWritten, buffer) =>
			{
				if(err) return callback(err, null)
				if(bytesWritten != headerSize) return callback(new Error('Not enough bytes written to header'), null)
				callback(null, null)
			})
		})
	}

	appendFile(outfile, filename, callback)
	{
		console.log('== Appending '+filename+' ==')
		console.log('Reading')
		let file = this.files[filename]
		fs.readFile(file.path, (err, rawFile) =>
		{
			if(err) return callback(err)
			console.log('Encoding')
			let encoded = this.encodeDCSFormat(rawFile)
			this.files[filename] =
			{
				size: encoded.length,
				offset: this.fileOffset - this.dataOffset
			}

			this.openFile(outfile, (err, fd) =>
			{
				if(err) return callback(err, null)
				console.log('Writing')
				fs.write(fd, encoded, 0, encoded.length, this.fileOffset, (err, bytesWritten, buffer) =>
				{
					if(err) return callback(err, null)
					if(bytesWritten != encoded.length) return callback(new Error('Not enough bytes written to header'), null)
					this.fileOffset += encoded.length
					console.log('== Finished ==')
					//console.log('====')
					callback(null, null)
				})
			})
		})
		
	}

	appendFiles(outfile, callback)
	{
		let files = Object.keys(this.files)
		let index = 0
		let loop = () =>
		{
			this.appendFile(outfile, files[index], (err, res) =>
			{
				if(err) return callback(err)
				if(++index == files.length)
					return callback(null, null)
				loop()
			})
		}
		loop()
	}

	finish(outfile, callback)
	{
		console.log('Open output file')
		this.openFile(outfile, (err, fd) =>
		{
			if(err) return callback(err)
			console.log('Write file list')
			this.writeFileList(outfile, (err, res) =>
			{
				if(err) return callback(err)
				console.log('Appending files')
				this.appendFiles(outfile, (err, res) =>
				{
					if(err) return callback(err)
					console.log('Updating file list')
					this.writeFileList(outfile, (err, res) =>
					{
						if(err) return callback(err)
						fs.close(fd, () =>
						{
							callback(null, null)
						})
					})
				})
			})
		})
	}

	getFile(filename, callback)
	{
		// Do we have the file in our list?
		if(!this.files[filename])
		{
			let err = new Error('No such file')
			if(callback) return callback(err, null)
			throw err
		}

		// Read the file
		let fileInfo = this.files[filename]
		let buffer = new Buffer(fileInfo.size)
		fs.read(this.fd, buffer, 0, buffer.length, this.fileOffset + fileInfo.offset, (err, bytesRead, buffer) =>
		{
			// Read error
			if(err)
			{
				if(callback) return callback(err, null)
				throw err
			}

			// Make sure we read enough bytes
			if(bytesRead != buffer.length)
			{
				let err = new Error('Failed to read file: Too few bytes')
				if(callback) return callback(err, null)
				throw err
			}

			// Attempt to parse file
			this.parseFile(buffer, callback)
		})
	}

	parseFile(buffer, callback)
	{
		//console.log('parseFile: '+buffer.toString('hex'))
		let data = null
		let header = buffer.toString('ascii', 0, 16)
		if(header == 'DSC FORMAT 1.00\0') // LZ + Huffman
		{
			//console.log('DSC FORMAT')
			data = this.decodeDSCFormat(buffer.slice(16))
		}
		else if(header == 'CompressedBG___\0')
		{
			//console.log('CompressedBG___')
			data = this.decodeCompressedBG(buffer)
		}
		else
			data = buffer

		if(callback) return callback(null, data)
	}

	/*
	getKey(key)
	{
		let work1 = 20021 * (key.key & 0xFFFF)
		let work2 = 20021 * (key.key >> 16)
		let work  = 346 * key.key + work2 + (work1 >> 16)

		key.key = (work << 16) + (work1 & 0xFFFF) + 1

		return work & 0x7FFF
	}
	*/

	getKey(key)
	{
		let magic = 0
		if(key.magic)
			magic = (key.magic[1] << 24) | (key.magic[0] << 16)
		let v0 = (key.key & 0xffff) * 20021
		let v1 = magic | (key.key >> 16)
		v1 = v1 * 20021 + key.key * 346
		v1 = (v1 + (v0 >> 16)) & 0xffff

		key.key = (v1 << 16) + (v0 & 0xffff) + 1

		return v1 & 0x7FFF
	}
	

	_LZFindVariableMatch(data, sourceFrom, matchFrom, matchSize, minDist = 0)
	{
		let dataLen = data.length
		let matchLen = 0
		let matchPos = 0
		let windowSize = matchFrom - sourceFrom
		if(windowSize > matchSize)
			windowSize = matchSize

		if(matchSize <= 0 || windowSize <= 0)
			return {pos: matchPos, len: matchLen}

		let maxSize = matchSize - minDist
		for(let i = 0; i < maxSize; i++)
		{
			for(let j = 0; j < windowSize - i; j++)
			{
				if(data[matchFrom + j] != data[sourceFrom + i + j])
				{
					if(j >= matchLen && (maxSize - i) > matchPos)
					{
						matchLen = j
						matchPos = maxSize - i
					}
					break
				}
			}
		}
		//matchPos = maxSize - matchPos

		return {pos: matchPos, len: matchLen}
	}

	_makeHuffmanTree(freqs, totalFreqs = 512, totalLevels = 512)
	{
		let cnodes = []
		for(let i = 0; i < totalFreqs; i++)
		{
			if(freqs[i] > 0)
				cnodes.push({value: i, freq: freqs[i]})
		}

		function findWithoutParent(nodes, from)
		{
			let len = nodes.length
			for(let i = from; i < len; i++)
				if(nodes[i].parent === undefined)
					return i
			return -1
		}

		// Make huffman tree
		while(1)
		{
			cnodes.sort((a, b) => { return a.freq - b.freq; })
			let left = findWithoutParent(cnodes, 0)
			if(left === -1) break
			let right = findWithoutParent(cnodes, left + 1)
			if(right === -1) break

			let pid = cnodes.length
			cnodes[left].parent = pid
			cnodes[right].parent = pid
			cnodes.push({freq: cnodes[left].freq + cnodes[right].freq, children: [cnodes[left], cnodes[right]], cid: [left, right]})
		}
		cnodes.sort((a, b) => { return a.freq - b.freq; })

		// Make codes
		function propagateLevels(node, level, encode)
		{
			node.level = level
			node.encode = encode
			if(node.children)
			{
				propagateLevels(node.children[0], level + 1, (encode << 1) | 0)
				propagateLevels(node.children[1], level + 1, (encode << 1) | 1)
			}
		}
		let cnodesLen = cnodes.length
		propagateLevels(cnodes[cnodesLen - 1], 0, 0)

		let levels = new Array(totalLevels)
		for(let i = 0; i < totalLevels; i++)
			levels[i] = 0

		for(let i = 0; i < cnodesLen; i++)
		{
			if(cnodes[i].children === undefined)
				levels[cnodes[i].value] = cnodes[i].level
		}

		return {levels: levels, cnodes: cnodes}
	}

	findLZMatch(searchFrom, searchLength, matchFrom, data, posOffset, maxLen)
	{
		let bestSize = 0
		let bestPos = 0
		for(let i = searchFrom; i < searchLength - posOffset; i++)
		{
			for(let j = 0; j < searchLength - i; j++)
			{
				if(data[matchFrom + j] != data[i + j] || j >= maxLen - 1)
				{
					if(j > bestSize)
					{
						bestSize = j
						bestPos = i
					}
					break
				}
			}
		}
		return {
			len: bestSize,
			pos: matchFrom - bestPos,
			data: data.slice(bestPos, bestPos + bestSize)
		}
	}

	encodeDCSFormat(data, compressionLevel = 9)
	{
		if(compressionLevel > 9)
			compressionLevel = 9
		if(!compressionLevel)
			compressionLevel = 0
		const maxLZLen = 0x100
		const minLZLen = 2
		const maxLZPos = 0x1000
		const minLZPos = 2
		let dataLen = data.length
		let maxLZLen2 = (maxLZLen * compressionLevel) / 9
		let maxLZPos2 = (maxLZPos * compressionLevel) / 9

		let freq = new Array(512)   // Huffman frequencies
		for(let i = 0; i < 512; i++)
		{
			freq[i] = 0
		}

		let lzBlocks = 0
		let blocks = []

		// LZ
		for(let i = 0; i < dataLen;)
		{
			let match = {pos: 0, len: 0}
			let searchResult = ''

			// Find matching data
			if(compressionLevel > 0)
			{
				let searchFrom = i - maxLZPos2
				if(searchFrom < 0) searchFrom = 0
				let searchLength = i - searchFrom
				let matchFrom = i
				match = this.findLZMatch(searchFrom, searchLength, matchFrom, data, minLZPos, maxLZLen)
	
				searchResult = 'Encoded LZ: src: '+searchFrom+', srcSize: '+match.len+', dest: '+i+', data: '+match.data.toString('hex')
			}

			// Add block
			let id = 0
			if(match.len >= minLZLen && match.len < maxLZLen)
			{
				lzBlocks++
				let encodedLen = match.len - minLZLen
				id = 0x100 | (encodedLen & 0xFF)
				match.pos -= minLZPos
				blocks.push({
					value: id,
					src: match.pos,
					dest: i
				})
				i += match.len
				//console.log(searchResult+' (Added)')
			}
			else
			{
				id = data[i] & 0xFF;
				blocks.push({value: id})
				i++
			}
			freq[id]++
		}

		let huffmanTree = this._makeHuffmanTree(freq)
		let levels = huffmanTree.levels

		//let encryptionKey = 0x022B1011
		let encryptionKey = (Math.random() * 0x100000000) | 0
		let key = {key: encryptionKey}

		// Encode and output table
		let huffmanTableOutput = Buffer.alloc(512)
		for(let i = 0; i < 512; i++)
		{
			let work = this.getKey(key) // Obfuscation
			let b = (levels[i] + (work & 0xFF)) & 0xFF
			huffmanTableOutput[i] = b
		}

		// Decode Huffman table
		let huffmanTable = [];
		for(var i = 0; i < 512; i++)
		{
			let b = levels[i]
			if(b != 0)
				huffmanTable.push({level: b, value: i});
		}
		huffmanTable.sort((a, b) =>
		{
			if(a.level != b.level)
				return a.level - b.level
			else
				return a.value - b.value
		})

		// Reconstruct huffman tree
		let vector = new Array(1024)
		let nodes = new Array(1024)
		for(let i = 0; i < 1024; i++)
			nodes[i] = {}
		vector[0] = 0;
		let v13 = 0
		let tableIndex = 0
		let toggle = 0
		let dec0 = 1
		let valueSet = 1
		for(var i = 0; tableIndex < huffmanTable.length - 1; i++)
		{
			toggle ^= 512
			let vectorIndex = toggle
			let groupCount = 0
			while(huffmanTable[tableIndex] && huffmanTable[tableIndex].level == i)
			{
				nodes[vector[v13++]].value = huffmanTable[tableIndex++].value
				groupCount++
			}

			let v18 = 2 * (dec0 - groupCount)
			if(groupCount < dec0)
			{
				dec0 = dec0 - groupCount
				for(let j = 0; j < dec0; j++)
				{
					nodes[vector[v13]].children = []
					for(let k = 0; k < 2; k++)
					{
						nodes[vector[v13]].children.push(valueSet)
						vector[vectorIndex++] = valueSet++
					}
					v13++
				}
			}
			dec0 = v18
			v13 = toggle
		}

		// Make our huffman code
		function iterate(rnodes, nodes, nodeIndex = 0, level = 0, val = 0)
		{
			if(nodes[nodeIndex].children)
			{
				iterate(rnodes, nodes, nodes[nodeIndex].children[0], level + 1, (val << 1) | 0)
				iterate(rnodes, nodes, nodes[nodeIndex].children[1], level + 1, (val << 1) | 1)
			}
			else
			{
				rnodes[nodes[nodeIndex].value] =
				{
					v: val,
					bits: level
				}
			}
		}
		let rnodes = new Array(512)
		iterate(rnodes, nodes)

		// Lazy
		function writeBits(buf, value, bits)
		{
			//let i = 0
			while(bits)
			{
				buf.push((value >> (bits - 1)) & 1)
				bits--
			}
		}

		let bitBuffer = []
		for(let i = 0; i < blocks.length; i++)
		{
			let rnode = rnodes[blocks[i].value]
			writeBits(bitBuffer, rnode.v, rnode.bits)
			if(blocks[i].value & 0x100)
			{
				writeBits(bitBuffer, blocks[i].src, 12)
			}
		}
		if(bitBuffer.length & 0x3)
			writeBits(bitBuffer, 0, 8 - (bitBuffer.length & 0x3))

		// Construct file
		let headerSize = 16 + 4 + 4 + 4 + 4 + 512
		let outputSize = Math.ceil(bitBuffer.length / 8) + headerSize
		let file = Buffer.alloc(outputSize)

		let outOffset = 0
		file.write('DSC FORMAT 1.00\0', outOffset); outOffset += 16
		file.writeInt32LE(encryptionKey, outOffset); outOffset += 4
		file.writeInt32LE(dataLen,       outOffset); outOffset += 4
		file.writeInt32LE(blocks.length, outOffset); outOffset += 4
		file.writeInt32LE(0,             outOffset); outOffset += 4
		huffmanTableOutput.copy(file, outOffset, 0, 512); outOffset += 512

		while(bitBuffer.length >= 8)
		{
			let combinedByte = 0
			for(let i = 0; i < 8; i++)
			{
				combinedByte = (combinedByte << 1) | bitBuffer.shift()
			}
			file[outOffset++] = combinedByte
		}

		//console.log(lzBlocks)

		return file
	}
/*
typedef struct {
	s8 magic[16];			
	u32 key;
	u32 comprlen;
	u32 uncomprlen;
	u16 sum_check;			
	u16 xor_check;			
} gdb_header_t;
*/

	decodeSDCFormat(data)
	{
		let header = data.toString('ascii', 0, 16)
		if(header != 'SDC FORMAT 1.00\0')
			throw new Error('Not a SDC FORMAT 1.00 file')

		let key = {key: data.readUInt32LE(16), magic: [data.readUInt8(0), data.readUInt8(1)]}
		let compressedLength = data.readUInt32LE(20)
		let uncompressedLength = data.readUInt32LE(24)
		let sumCheck = data.readUInt16LE(28)
		let xorCheck = data.readUInt16LE(30)

		let sum = 0
		let xor = 0
		let decoded = Buffer.alloc(data.length)

		for(let i = 32; i < compressedLength + 32; i++)
		{
			decoded[i] = data[i] - this.getKey(key)
			sum = (sum + data[i]) & 0xFFFF
			xor = (xor ^ data[i]) & 0xFFFF
		}

		if((sum & 0xFFFF) != sumCheck || (xor & 0xFFFF) != xorCheck)
			throw new Error('Checksum mismatch')

		console.log(decoded.toString('hex'))

		
		let uncompressed = Buffer.alloc(uncompressedLength)

		let bytesWritten = 0
		for(let i = 0; i < compressedLength;)
		{
			let copyBytes
			let windowPosition
			
			if(decoded[i] & 0x80)
			{
				let code = decoded[i] & 0x7f
				
				copyBytes = (code >> 3) + 2
				windowPosition = ((code & 7) << 8) | decoded[i + 1] + 2

				let offset = bytesWritten - windowPosition
				uncompressed.copy(uncompressed, bytesWritten, offset, offset + copyBytes)
				
				bytesWritten += copyBytes
				i += 2
			}
			else
			{
				copyBytes = decoded[i++] + 1

				let offset = i
				decoded.copy(uncompressed, bytesWritten, offset, offset + copyBytes)
				
				bytesWritten += copyBytes
				i += copyBytes
			}
		}
		if(bytesWritten != uncompressedLength)
		{
			console.log(bytesWritten)
			console.log(uncompressedLength)
			//throw new Error('Uncompressed length mismatch')
		}

		return uncompressed
	}

	decodeDSCFormat(data)
	{
		let key = {key: data.readUInt32LE(0)}
		let outSize = data.readUInt32LE(8)
		//let v2 = data.readUInt32LE(8)
		data = data.slice(16)

		let huffmanTable = []

		let decodedHuffmanTable = Buffer.alloc(512)

		// Decode Huffman table
		for(var i = 0; i < 512; i++)
		{
			let work = this.getKey(key) // Obfuscation
			let b = (data[i] - (work & 0xFF)) & 0xFF
			decodedHuffmanTable[i] = b
			if(b != 0)
			{
				huffmanTable.push({
					level: b,
					value: i
				});
			}
		}
		huffmanTable.sort((a, b) =>
		{
			if(a.level != b.level)
				return a.level - b.level
			else
				return a.value - b.value
		})

		let vector = new Array(1024)
		let nodes = new Array(1024)
		for(let i = 0; i < 1024; i++)
			nodes[i] = {}

		// Reconstruct huffman tree
		vector[0] = 0;
		let v13 = 0
		let tableIndex = 0
		let toggle = 0
		let dec0 = 1
		let valueSet = 1
		for(var i = 0; tableIndex < huffmanTable.length - 1; i++)
		{
			toggle ^= 512
			let vectorIndex = toggle
			let groupCount = 0
			while(huffmanTable[tableIndex] && huffmanTable[tableIndex].level == i)
			{
				nodes[vector[v13++]].value = huffmanTable[tableIndex++].value
				groupCount++
			}

			let v18 = 2 * (dec0 - groupCount)
			if(groupCount < dec0)
			{
				dec0 = dec0 - groupCount
				for(let j = 0; j < dec0; j++)
				{
					nodes[vector[v13]].children = []
					for(let k = 0; k < 2; k++)
					{
						nodes[vector[v13]].children.push(valueSet)
						vector[vectorIndex++] = valueSet++
					}
					v13++
				}
			}
			dec0 = v18
			v13 = toggle
		}

		let blockCount = 0

		// Huffman + LZ decompress
		let inIndex = 512
		let outIndex = 0
		let out = new Array(outSize)
		let huffmanBitBuffer = 0 // 8-bit buffer
		let bitsInBuffer = 0     // Bits remaining in buffer
		while(outIndex < outSize)
		{
			let nodeIndex = 0
			let bitsRead = 0
			let bitsReadValue = 0

			// Traverse the tree until we find a node without children (a leaf node)
			while(nodes[nodeIndex].children)
			{
				// No bits left in the buffer
				// Let's get the next 8 bits
				if(!bitsInBuffer)
				{
					bitsInBuffer = 8
					huffmanBitBuffer = data[inIndex++]
				}

				// Get the next node based on the next bit
				bitsReadValue <<= 1
				bitsReadValue |= (huffmanBitBuffer >> 7) & 1
				nodeIndex = nodes[nodeIndex].children[(huffmanBitBuffer >> 7) & 1]

				// Remove upper bit
				bitsInBuffer--
				huffmanBitBuffer = (huffmanBitBuffer << 1) & 0xFF
				bitsRead++
			}

			// Get the decoded huffman value from this leaf node
			let info = nodes[nodeIndex].value

			// If the decoded value is 0x01xx, it means there is LZ compressed data
			if((info >> 8) == 1)
			{
				// Shift the bits we have in the buffer down
				// so that the LSB remaining in the buffer is moved to position 0
				let lzValue = huffmanBitBuffer >> (8 - bitsInBuffer)
				let bitsInLZBuffer = bitsInBuffer

				// Fetch more bits from the stream until we have at least 12 bits
				// If the buffer contains 3 bits or less, we must read 2 bytes
				// If the buffer contains 4 bits or more, we only need to read 1 byte
				let bytesNeeded = ((11 - bitsInBuffer) >> 3) + 1
				bitsInLZBuffer = bitsInBuffer
				while(bytesNeeded--)
				{
					lzValue = (lzValue << 8) | data[inIndex++]
					bitsInLZBuffer += 8
				}

				// Move the bits we don't need into the huffman bit buffer
				bitsInBuffer = bitsInLZBuffer - 12;
				huffmanBitBuffer = (lzValue << (8 - (bitsInLZBuffer - 12))) & 0xFF

				// Remove overflow bits - We're only interested in the upper 12 bits
				let offset = (lzValue >> (bitsInLZBuffer - 12)) + 2

				// Offset to data (back-reference)
				let lzIndex = outIndex - offset

				// Number of bytes to read
				let count = (info & 0xFF) + 2

				let llzIndex = lzIndex
				let ccount = count
				let lzData = Buffer.alloc(count)
				let dIndex = 0
				while(ccount--)
					lzData[dIndex++] = out[llzIndex++];

				// Copy bytes from earlier in the buffer
				while(count--)
					out[outIndex++] = out[lzIndex++];

				blockCount++
			}
			// Uncompressed byte
			else
				out[outIndex++] = info & 0xFF;
		}

		//console.log(blockCount)

		return new Buffer(new Uint8Array(out))
	}

	decodeCompressedBG(data)
	{
		let offset = 0

		let signature = data.slice(0, 16).toString('ascii'); offset += 16
		if(signature != 'CompressedBG___\0')
		{
			let err = new Error('File is not a CompressedBG___ file')
			//if(callback) return callback(err, null)
			throw err
		}

		// Image data
		let width = data.readUInt16LE(offset); offset += 2
		let height = data.readUInt16LE(offset); offset += 2
		let bpp = data.readUInt16LE(offset); offset += 2
		let decompressedSize = width * height * (bpp / 8)
		console.log('width: '+width)
		console.log('height: '+height)
		console.log('bpp: '+bpp)

		// Unknown fields (always zero?)
		let unknown1 = data.readUInt16LE(offset); offset += 2
		let unknown2 = data.readUInt32LE(offset); offset += 4
		let unknown3 = data.readUInt32LE(offset); offset += 4

		// Huffman vars
		let huffmanSize = data.readUInt32LE(offset); offset += 4 // Size of the huffman decoded data
		let huffmanFreq = []
		let huffmanTree = []
		let huffmanTotalFreq = 0

		// Decryption
		let key = {key: data.readUInt32LE(offset)}; offset += 4
		let decryptSize = data.readUInt32LE(offset); offset += 4

		// More unkown fields
		let unknown4 = data.readUInt32LE(offset); offset += 4

		// Decrypt the file
		for(let i = 0; i < decryptSize; i++)
		{
			let work = this.getKey(key)
			let b = (data[offset + i] - (work & 0xFF)) & 0xFF
			data[offset + i] = b
		}

		// Get huffman table
		function getVariableLengthData(varReader)
		{
			let variable = 0 // The finished unsigned int
			let shift = 1    // Positions the next byte must be shiftet
			let currentByte  // Current byte

			// Do while the current byte has the control bit set
			do
			{
				currentByte = varReader.data[varReader.offset++]
				variable += (currentByte & 0x7F) * shift // Mask off the last bit and shift it into place
				shift *= 128
			}
			while(currentByte & 0x80)

			return variable
		}
		let varReader = {offset: offset, data: data}
		let largestFreq = 0
		for(let i = 0; i < 256; i++)
		{
			huffmanFreq[i] = getVariableLengthData(varReader)
			if(huffmanFreq[i] > largestFreq)
				largestFreq = huffmanFreq[i]
		}
		largestFreq = 1 << 30

		// Initialize the Huffman tree
		for(let i = 0; i < 256; i++)
		{
			huffmanTree[i] = {
				valid:     huffmanFreq[i] > 0,
				frequency: huffmanFreq[i],
				leftNode:  i,
				rightNode: i
			}
			huffmanTotalFreq += huffmanFreq[i]
		}
		for(let i = 256; i < 511; i++)
		{
			huffmanTree[i] = {
				valid:      false,
				frequency:  0,
				leftNode:  -1,
				rightNode: -1
			}
		}

		// Create the huffman tree
		let node
		for(node = 256; node < 511; node++)
		{
			let minimumFrequency
			let frequency = 0
			let children = [-1, -1]

			// Find 2 nodes to use
			for(let i = 0; i < 2; i++)
			{
				// Find a valid nodes with the lowest frequency
				minimumFrequency = largestFreq
				for(let j = 0; j < node; j++)
				{
					if(huffmanTree[j].valid && huffmanTree[j].frequency < minimumFrequency)
					{
						minimumFrequency = huffmanTree[j].frequency
						children[i] = j
					}
				}

				// Mark the node we found as invalid, so that it won't be used again
				if(children[i] != -1)
				{
					huffmanTree[children[i]].valid = false
					frequency += huffmanTree[children[i]].frequency
				}
			}

			huffmanTree[node].valid = true
			huffmanTree[node].frequency = frequency
			huffmanTree[node].leftNode =  children[0] // Lowest frequency node is always to the left
			huffmanTree[node].rightNode = children[1] // The 2nd lowest frequency node is to the right

			// Once we've found the 2 last nodes, we are done building the tree
			// (If the frequencies of the 2 childred used in the node adds up to the total frequency of the tree, we know there are no mode nodes)
			if(frequency == huffmanTotalFreq)
				break
		}

		// Huffman decode
		let rootNode = node     // The node to begin walking down the Huffman tree
		let mask = 0x80         // Bit mask for walking down the tree
		let huffmanDecoded = [] // Decoded huffman data
		offset = varReader.offset
		for(let i = 0; i < huffmanSize; i++)
		{
			// Reset the node
			node = rootNode

			// Do until we reach the end of the Huffman tree
			while(node >= 256)
			{
				// Determine what path to take in the Huffman tree
				if(varReader.data[offset] & mask)
					node = huffmanTree[node].rightNode
				else
					node = huffmanTree[node].leftNode
				
				// Process the next bit
				mask >>= 1

				// If we're done with this byte, fetch the next one
				if(mask == 0)
				{
					offset++
					mask = 0x80
				}
			}

			huffmanDecoded[i] = node & 0xFF
		}

		// Decode variable length data
		let clear = false
		varReader = {offset: 0, data: huffmanDecoded}
		let out = []
		let outOffset = 0
		while(varReader.offset < huffmanSize)
		{
			// Get length of data
			let length = getVariableLengthData(varReader)

			// Every other iteration can either add 0 to the data stream, or a block of raw data
			// Zero's are RLE compressed
			if(clear)
			{
				for(let i = 0; i < length; i++)
					out[outOffset++] = 0
			}
			// Raw data is uncompressed
			else
			{
				for(let i = 0; i < length; i++)
					out[outOffset++] = varReader.data[varReader.offset++]
			}
			clear = !clear
		}

		// Every pixel (except the top row and left-most column) uses the pixel above it and left of it
		// as a difference filter in an attempt to make the data more huffman compressable
		let channelCount = bpp / 8
		let rowSize = width * channelCount
		let pointer = 0
		let temp = 0
		for(let y = 0; y < height; y++)
		{
			for(let x = 0; x < width; x++)
			{
				for(let i = 0; i < channelCount; i++)
				{
					if(y == 0 && x == 0)
						temp = 0
					else if(y == 0)
						temp = out[pointer - channelCount]
					else if(x == 0)
						temp = out[pointer - rowSize]
					else
						temp = (out[pointer - channelCount] + out[pointer - rowSize]) >> 1
					out[pointer] = (out[pointer] + temp) & 0xFF
					pointer++
				}
			}
		}

		// Normalise the bitmap to RGBA format
		let pixelSize = bpp / 8

		// Grayscale
		if(bpp == 8)
		{
			let newOut = Buffer.alloc(out.length * 4)
			let j = 0
			for(let i = 0; i < decompressedSize; i += pixelSize)
			{
				// To RGB
				newOut[j+0] = out[i+0]
				newOut[j+1] = out[i+0]
				newOut[j+2] = out[i+0]
				newOut[j+3] = 0xff
				j += 4
			}
			out = newOut
		}
		// RGB
		if(bpp == 24)
		{
			let newOut = Buffer.alloc((out.length / 3) * 4)
			let j = 0
			for(let i = 0; i < decompressedSize; i += pixelSize)
			{
				// From BGR
				let blue  = out[i+0]
				let green = out[i+1]
				let red   = out[i+2]

				// To RGB
				newOut[j+0] = red
				newOut[j+1] = green
				newOut[j+2] = blue
				newOut[j+3] = 0xff
				j += 4
			}
			out = newOut
		}
		// RGBA
		if(bpp == 32)
		{
			for(let i = 0; i < decompressedSize; i += pixelSize)
			{
				// From BGRA
				let blue  = out[i+0]
				let green = out[i+1]
				let red   = out[i+2]
				let alpha = out[i+3]

				// To RGBA
				out[i+0] = red
				out[i+1] = green
				out[i+2] = blue
				out[i+3] = alpha
			}
		}
		
		// pngjs
		let colorType = 6
		if(bpp == 24) colorType = 4
		if(bpp == 8) colorType = 0

		let png = new PNG({
			width:          width,
			height:         height,
			bitDepth:       8,
			colorType:      colorType,
			inputColorType: colorType,
			inputHasAlpha:  bpp == 32 ? true : false
		})
		png.data = Buffer.from(out)
		let buff = PNG.sync.write(png)
		return buff
	}
}

module.exports = Arc
