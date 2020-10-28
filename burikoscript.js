
const fs = require('fs')
const utf8ToSjis = JSON.parse(fs.readFileSync('utf8ToSjis.json', 'utf8'))

//
// Hexadecimal printing
//
const HEX_CHAR = '0123456789ABCDEF'
function toHex8(n)
{
	return HEX_CHAR[(n >> 4) & 0xF]+HEX_CHAR[n & 0xF]
}
function toHex16(n)
{
	return toHex8((n >> 8) & 0xFF)+toHex8(n & 0xFF)
}
function toHex32(n)
{
	return toHex16((n >> 16) & 0xFFFF)+toHex16(n & 0xFFFF)
}

//
// Buriko Script assembler/disassembler
//
class BurikoScript
{
	constructor()
	{

	}

	load(file)
	{
		let offset = 0
		this.headerSize = file.readUInt32LE(0)
		this.codeSize = file.readUInt32LE(4)

		this.code = file.slice(this.headerSize, this.headerSize + this.codeSize)
	}

	parseline(line)
	{
		// Hexadecimal decode
		const NUM = {
			'0': 0, '1': 1, '2': 2, '3': 3, '4': 4,
			'5': 5, '6': 6, '7': 7, '8': 8, '9': 9,
			'a': 10, 'A': 10, 'b': 11, 'B': 11, 'c': 12, 'C': 12,
			'd': 13, 'D': 13, 'e': 14, 'E': 14, 'f': 15, 'F': 15
		}

		let data = []
		let token = ''
		let state = 0
		let end = false
		let numstate = 0
		let cmdstate = 'cmd'
		let num = 0
		
		// Loop through all characters
		for(let i = 0; i <= line.length; i++)
		{
			// Are we at the end?
			if(i == line.length)
				end = true

			let c = line[i]
			console.log(c)
			switch(state)
			{
				// Default state: Check if we should switch state, or tread this as a command
				case 0:
					// We don't have a token yet. Check if the first symbol is a ., indicating an assembler directive
					if(token == '')
					{
						if(c == '.')
							cmdstate = 'dir'
						else
							cmdstate = 'cmd'
					}

					// Did we hit a separator?
					if(c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == ':' || c == ';' || end)
					{
						// We hit a quotation mark. Switch to literal string parsing mode
						if(c == '"')
						{
							state = 1
						}

						// If we already have a token, finish parsing that first
						if(token)
						{
							if(c == ':')
								cmdstate = 'label'
							console.log(cmdstate+' '+token+' '+c)
							data.push({type: cmdstate, data: token})
						}

						// Reset token, so we can start adding the string to it
						token = ''

						// Comment found, skip rest of line
						if(c == ';')
						{
							i = line.length
							break
						}
					}

					// Is this the first character? If so, is it a number?
					else if(token == '' && (
							c == '0' || c == '1' || c == '2' || c == '3' || c == '4' ||
							c == '5' || c == '6' || c == '7' || c == '8' || c == '9'
						)
					)
					{
						// Start parsing the number
						state = 2
						numstate = c == '0' ? 8 : 10
						num = NUM[c]
					}

					// The current character is not a number, or a separator. Add to the current token
					else
					{
						token += c
					}
					break

				// String literal
				case 1:
					// Continue until we hit another quotation mark
					// TODO: Escaping
					if(c == '"' || end)
					{
						// move to no state
						state = 0
						data.push({type: 'string', data: token})
						token = ''
					}
					else
					{
						token += c
					}
					break

				// Number (base8, base10 and base16)
				case 2:
					// Did we hit a separator?
					if(
						c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' ||
						c == '+' || c == '-' || c == '*' || c == '/' || c == '>' || c == '<' || c == '%' || end)
					{
						// Finish parsing number
						state = 0
						data.push({type: 'number', data: num})
					}
					// Should we switch to base16?
					else if(numstate == 8 && c == 'x')
					{
						num = 0
						numstate = 16
					}
					else
					{
						// Keep parsing number
						let nn = NUM[c]
						if(nn === undefined || nn > numstate)
							throw new Error('Illegal character "'+c+'" in base'+numstate+' number')
						num *= numstate
						num += nn
					}
					break
			}
		}
		if(state != 0)
		{
			throw new Error('Did not finish in state 0. Finished in state '+state)
		}
		return data
	}

	numOrIdentifier(s, size, pc, bin, labelOffsetFixes)
	{
		if(s.type == 'number')
		{
			if(size >= 1)
			{
				if(size == 1 && s.data > 0xFF)
					throw new Error('Value out of range')
				bin[pc++] = s.data & 0xFF
			}
			if(size >= 2)
			{
				if(size == 2 && s.data > 0xFFFF)
					throw new Error('Value out of range')
				bin[pc++] = (s.data >> 8) & 0xFF
			}
			if(size >= 3)
			{
				if(size == 3 && s.data > 0xFFFFFF)
					throw new Error('Value out of range')
				bin[pc++] = (s.data >> 16) & 0xFF
			}
			if(size >= 4)
			{
				if(size == 4 && s.data > 0xFFFFFFFF)
					throw new Error('Value out of range')
				bin[pc++] = (s.data >> 24) & 0xFF
			}
		}
		else if(s.type == 'cmd')
		{
			let prePc = pc
			if(size >= 1)
				bin[pc++] = 0x00
			if(size >= 2)
				bin[pc++] = 0x00
			if(size >= 3)
				bin[pc++] = 0x00
			if(size >= 4)
				bin[pc++] = 0x00
			labelOffsetFixes.push({label: s.data, offset: prePc, size: size})
		}
		return pc
	}

	assemble(source)
	{
		let lines = source.split('\n')
		let pc = 0
		let labels = {}
		let bin = []
		let labelOffsetFixes = []
		for(let i in lines)
		{
			let line = lines[i]
			let parts = this.parseline(line)

			console.log('Line parts:')
			console.log(parts)

			for(let j = 0; j < parts.length; j++)
			{
				let prePc = pc
				let ins = parts[j]
				console.log('Parsing part:')
				console.log(ins)
				switch(ins.type)
				{
					case 'label':
					{
						let label = ins.data.substring(0, ins.data.length)
						labels[label] = pc
						console.log(label+': '+pc)
						break
					}

					case 'cmd':
					{
						console.log('Assembling cmd type:')
						console.log(ins.data)
						switch(ins.data)
						{
							case 'push8':
							{
								bin[pc++] = 0x00
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 1, pc, bin, labelOffsetFixes)
								break
							}
							case 'push16':
							{
								bin[pc++] = 0x01
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 2, pc, bin, labelOffsetFixes)
								break
							}
							case 'push32':
							{
								bin[pc++] = 0x02
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 4, pc, bin, labelOffsetFixes)
								break
							}
							
							// getvarptr
							case 'baseptr':
							{
								bin[pc++] = 0x04
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 2, pc, bin, labelOffsetFixes)
								break
							}

							// codeptr
							case 'codeptr':
							{
								bin[pc++] = 0x05
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 2, pc, bin, labelOffsetFixes)
								break
							}

							// getcodeoffset
							case 'codeoffset':
							{
								bin[pc++] = 0x06
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 2, pc, bin, labelOffsetFixes)
								break
							}

							case 'load':
							{
								bin[pc++] = 0x08
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 1, pc, bin, labelOffsetFixes)
								break
							}

							// 09 - expects the destination Pointer to be pushed before the Value
							case 'storepv':
							{
								bin[pc++] = 0x09
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 1, pc, bin, labelOffsetFixes)
								break
							}

							// 0A - expects the Value to be pushed before the destination Pointer
							case 'storevp':
							{
								bin[pc++] = 0x0A
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 1, pc, bin, labelOffsetFixes)
								break
							}

							case 'write':
							{
								bin[pc++] = 0x0B
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 1, pc, bin, labelOffsetFixes)
								break
							}

							case 'storemulti':
							{
								bin[pc++] = 0x0C
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 1, pc, bin, labelOffsetFixes)
								s = parts[++j]
								pc = this.numOrIdentifier(s, 1, pc, bin, labelOffsetFixes)
								break
							}

							case 'loadbase':
								bin[pc++] = 0x10
								break

							case 'storebase':
								bin[pc++] = 0x11
								break

							case 'jmp':
								bin[pc++] = 0x14
								break

							// Custom jump
							case 'jc':
							{
								bin[pc++] = 0x15
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 1, pc, bin, labelOffsetFixes)
								break
							}
							// Jump if not zero
							case 'jnz':
								bin[pc++] = 0x15
								bin[pc++] = 0x00
								break
							// Jump if zero
							case 'jz':
								bin[pc++] = 0x15
								bin[pc++] = 0x01
								break
							// Jump if greater than zero
							case 'jg':
								bin[pc++] = 0x15
								bin[pc++] = 0x02
								break
							// Jump if greater than or equal to zero
							case 'jge':
								bin[pc++] = 0x15
								bin[pc++] = 0x03
								break
							// Jump if lesser than or equal to zero
							case 'jle':
								bin[pc++] = 0x15
								bin[pc++] = 0x04
								break
							// Jump if lesser than zero
							case 'jl':
								bin[pc++] = 0x15
								bin[pc++] = 0x05
								break

							// Return
							case 'call':
								bin[pc++] = 0x16
								break

							// Return
							case 'ret':
								bin[pc++] = 0x17
								break

							case 'add':
								bin[pc++] = 0x20
								break

							case 'sub':
								bin[pc++] = 0x21
								break

							case 'mul':
								bin[pc++] = 0x22
								break

							case 'or':
								bin[pc++] = 0x26
								break

							case 'not':
								bin[pc++] = 0x28
								break

							case 'ternary':
								bin[pc++] = 0x40
								break

							case 'sprintf':
								bin[pc++] = 0x6f
								break
								
							case 'infobox':
								bin[pc++] = 0x79
								break

							case 'sys1':
							{
								bin[pc++] = 0x80
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 1, pc, bin, labelOffsetFixes)
								break
							}

							case 'grp1':
							{
								bin[pc++] = 0x90
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 1, pc, bin, labelOffsetFixes)
								break
							}

							case 'grp2':
							{
								bin[pc++] = 0x91
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 1, pc, bin, labelOffsetFixes)
								break
							}

							case 'snd1':
							{
								bin[pc++] = 0xA0
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 1, pc, bin, labelOffsetFixes)
								break
							}

							default:
								throw new Error('Unknown command: '+ins.data)
						}
						break
					}

					case 'dir':
					{
						switch(ins.data)
						{
							case '.ascii':
							case '.asciiz':
							case '.sjis':
							case '.sjisz':
							{
								let n = parts[++j]
								if(!n || n.type != 'string')
									throw new Error('Expected string')
								let escaping = false
								for(let i in n.data)
								{
									let char = n.data[i]
									if(!escaping && char == '\\')
									{
										escaping = true
										continue
									}
									if(escaping)
									{
										switch(char)
										{
											case 'n':
												bin[pc++] = '\n'.charCodeAt(0)
												break

											case '\\':
												bin[pc++] = '\\'.charCodeAt(0)
												break
										}
										escaping = false
									}
									else
									{
										if(ins.data == '.ascii' || ins.data == '.asciiz')
											bin[pc++] = char.charCodeAt(0)
										if(ins.data == '.sjis' || ins.data == '.sjisz')
										{
											let c = utf8ToSjis[char]
											if(c < 0x100)
												bin[pc++] = c
											else
											{
												bin[pc++] = (c >> 8) & 0xFF
												bin[pc++] = c & 0xFF
											}
										}
									}
								}
								if(ins.data == '.asciiz' || ins.data == '.sjisz')
									bin[pc++] = 0
								break
							}
							case '.db':
							{
								let s = parts[++j]
								pc = this.numOrIdentifier(s, 1, pc, bin, labelOffsetFixes)
							}
						}
						break
					}

					default:
						throw new Error('Unknown type: '+ins.type)
				}

				let res = 'Result: '
				for(let k = prePc; k < pc; k++)
					res += toHex8(bin[k])+' '
				console.log(res)
				console.log()
			}
		}

		for(let i in labelOffsetFixes)
		{
			let lof = labelOffsetFixes[i]
			let labelStr = lof.label
			
			// Should we calculate a relative offset?
			let relative = false
			if(labelStr[0] == '+')
			{
				labelStr = labelStr.substring(1)
				relative = true
			}

			if(labels[labelStr] === undefined)
				throw new Error('Undefined symbol: '+labelStr)

			let label
			if(relative)
				label = labels[labelStr] - lof.offset + 1
			else
				label = labels[labelStr]

			console.log(lof, label)
			for(let j = 0; j < lof.size; j++)
			{
				bin[lof.offset+j] = label & 0xFF
				label >>= 8
			}
		}

		let binBuf = Buffer.from(bin)
		let padLen = 0x10 - (binBuf.length & 0xF)
		if(padLen)
			binBuf = Buffer.concat([binBuf, Buffer.alloc(padLen)])
		let header = Buffer.from([
			0x10, 0x00, 0x00, 0x00,
			(binBuf.length >> 0) & 0xFF,
			(binBuf.length >> 8) & 0xFF,
			(binBuf.length >> 16) & 0xFF,
			(binBuf.length >> 24) & 0xFF,
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00,
		])

		let code = Buffer.concat([header, binBuf])

		return code
	}

	disassemble()
	{
		const multibyteOps = {
			0x00: 1, // push8
			0x01: 2, // push16
			0x02: 4, // push32
			
			0x04: 2, // baseptr
			0x05: 2, // string
			0x06: 2, // offset

			0x08: 1, // load
			0x09: 1, // storepv
			0x0A: 1, // storevp
			0x0B: 1, // write
			0x0C: 1, // storemulti

			0x15: 1, // jc

			0x80: 1, // sys1

			0x90: 1, // grp1
			0x91: 1, // grp2
			0x92: 1, // grp3

			0xA0: 1, // snd1

			0xB0: 1, // usr1

			0xC0: 1, // usr2
		}
		let offset = 0
		let offset2
		let dis = []
		let offs = []
		let n
		let n2
		let sn
		let stringLabels = []
		let codeLabels = []
		let run = true
		let sizeLabels = ['i8', 'i16', 'i32']
		while(offset < this.code.length && run)
		{
			// Stop when we reach strings
			for(let i in stringLabels)
			{
				if(offset >= stringLabels[i])
				{
					run = false
					break
				}
			}
			if(!run) break

			// Get opcode
			offs.push(offset)
			let op = this.code[offset++]
			if(op == 0x00 && offset == this.code.length)
				break

			switch(op)
			{
				case 0x00:
					n = this.code.readUInt8(offset); offset += 1
					dis.push('push8 0x'+toHex8(n))
					break
				case 0x01:
					n = this.code.readUInt16LE(offset); offset += 2
					dis.push('push16 0x'+toHex16(n))
					break
				case 0x02:
					n = this.code.readUInt32LE(offset); offset += 4
					dis.push('push32 0x'+toHex32(n))
					break

				case 0x04:
					n = this.code.readUInt16LE(offset); offset += 2
					dis.push('memptr 0x'+toHex16(n))
					break
				case 0x05:
					// Get string
					offset2 = offset
					n = this.code.readInt16LE(offset); offset += 2
					offset2 += n
					stringLabels.push(offset2)
					dis.push('codeptr +L_'+toHex16(offset2))
					break
				case 0x06:
					// Get offset
					offset2 = offset - 1
					n = this.code.readInt16LE(offset); offset += 2
					offset2 += n
					codeLabels.push(offset2)
					dis.push('codeoffset L_'+toHex16(offset2))
					break

				case 0x08:
					n = this.code.readUInt8(offset); offset += 1
					sn = '0x'+toHex8(n)
					if(sizeLabels[n]) sn = sizeLabels[n]
					dis.push('load '+sn)
					break
				case 0x09:
					n = this.code.readUInt8(offset); offset += 1
					sn = '0x'+toHex8(n)
					if(sizeLabels[n]) sn = sizeLabels[n]
					dis.push('storepv '+sn)
					break
				case 0x0A:
					n = this.code.readUInt8(offset); offset += 1
					sn = '0x'+toHex8(n)
					if(sizeLabels[n]) sn = sizeLabels[n]
					dis.push('storevp '+sn)
					break
				case 0x0B:
					n = this.code.readUInt8(offset); offset += 1
					sn = '0x'+toHex8(n)
					if(sizeLabels[n]) sn = sizeLabels[n]
					dis.push('write '+sn)
					break
				case 0x0C:
					n = this.code.readUInt8(offset); offset += 1
					n2 = this.code.readUInt8(offset); offset += 1
					sn = '0x'+toHex8(n)
					if(sizeLabels[n]) sn = sizeLabels[n]
					dis.push('storemulti '+sn+', 0x'+toHex8(n2))
					break

				case 0x10: dis.push('loadbase');  break
				case 0x11: dis.push('storebase'); break
				case 0x14: dis.push('jump');      break
				case 0x15:
					n = this.code.readUInt8(offset); offset += 1
					switch(n)
					{
						case 0x00: dis.push('jnz'); break
						case 0x01: dis.push('jz');  break
						case 0x02: dis.push('jg');  break
						case 0x03: dis.push('jge'); break
						case 0x04: dis.push('jle'); break
						case 0x05: dis.push('jl');  break
						default:
							dis.push('jc 0x'+toHex(n))
							break
					}
					break
				case 0x16: dis.push('call'); break
				case 0x17: dis.push('ret'); break

				case 0x20: dis.push('add'); break
				case 0x21: dis.push('sub'); break
				case 0x22: dis.push('mul'); break
				case 0x23: dis.push('div'); break
				case 0x24: dis.push('mod'); break
				case 0x25: dis.push('and'); break
				case 0x26: dis.push('or');  break
				case 0x27: dis.push('xor'); break
				case 0x28: dis.push('not'); break
				case 0x29: dis.push('shl'); break
				case 0x2A: dis.push('shr'); break
				case 0x2B: dis.push('sar'); break

				case 0x30: dis.push('eq');  break
				case 0x31: dis.push('neq'); break
				case 0x32: dis.push('leq'); break
				case 0x33: dis.push('geq'); break
				case 0x34: dis.push('lt');  break
				case 0x35: dis.push('gt');  break
				case 0x38: dis.push('bool_and'); break
				case 0x39: dis.push('bool_or');  break
				case 0x3A: dis.push('iszero');   break

				case 0x40: dis.push('ternary'); break
				case 0x48: dis.push('sin');     break
				case 0x49: dis.push('cos');     break

				case 0x60: dis.push('memcpy');  break
				case 0x61: dis.push('memclr');  break
				case 0x62: dis.push('memset');  break
				case 0x63: dis.push('memcmp');  break
				case 0x68: dis.push('strlen');  break
				case 0x69: dis.push('streq');   break
				case 0x6A: dis.push('strcpy');  break
				case 0x6B: dis.push('concat');  break
				case 0x6C: dis.push('getchar'); break
				case 0x6D: dis.push('tolower'); break
				case 0x6E: dis.push('quote');   break
				case 0x6F: dis.push('sprintf'); break

				case 0x70: dis.push('malloc');   break
				case 0x71: dis.push('free');     break
				case 0x78: dis.push('confirm');  break
				case 0x79: dis.push('message');  break
				case 0x7A: dis.push('assert');   break
				case 0x7B: dis.push('dumpmem');  break
				case 0x7D: dis.push('loadfont'); break
				case 0x7F: dis.push('debug');    break

				case 0x80:
					n = this.code.readUInt8(offset); offset += 1
					sn = '0x'+toHex8(n)
					switch(n)
					{
						case 0x00: sn = 'srand'; break
						case 0x01: sn = 'rand'; break
						case 0x02: sn = 'randrange'; break
						case 0x04: sn = 'time'; break
						case 0x08: sn = 'cursorpos'; break
						case 0x0C: sn = 'locatime'; break
						case 0x0F: sn = 'windowactive'; break

						case 0x28: sn = 'mkdir'; break
						case 0x2A: sn = 'isdir'; break

						case 0x37: sn = 'adddir'; break
						case 0x3D: sn = 'path'; break

						case 0x40: sn = 'loadcode'; break
						case 0x41: sn = 'unloadcode'; break
						case 0x44: sn = 'newthread'; break
						case 0x46: sn = 'threadid'; break

						case 0x64: sn = 'showwindow'; break
						case 0x66: sn = 'windowtitle'; break
						case 0x67: sn = 'cursorshape'; break
					}
					dis.push('grp1.'+sn)
					break;

				case 0x90:
					n = this.code.readUInt8(offset); offset += 1
					dis.push('grp1.0x'+toHex8(n))
					break;

				case 0x91:
					n = this.code.readUInt8(offset); offset += 1
					dis.push('grp2.0x'+toHex8(n))
					break;

				case 0x92:
					n = this.code.readUInt8(offset); offset += 1
					dis.push('grp3.0x'+toHex8(n))
					break;

				case 0xA0:
					n = this.code.readUInt8(offset); offset += 1
					dis.push('snd1.0x'+toHex8(n))
					break;

				case 0xB0:
					n = this.code.readUInt8(offset); offset += 1
					dis.push('usr1.0x'+toHex8(n))
					break;

				case 0xC0:
					n = this.code.readUInt8(offset); offset += 1
					dis.push('usr2.0x'+toHex8(n))
					break;

				default:
					console.log('Unknown instruction 0x'+toHex8(op))
					offset = this.code.length
					break
			}
		}

		//console.log(codeLabels)
		//return ''

		let asm = ''
		for(let i in dis)
		{
			let off = offs[i]
			let line = dis[i]
			for(let i in codeLabels)
			{
				if(off == codeLabels[i])
				{
					asm += '\nL_'+toHex16(off)+':\n'
					break
				}
			}
			asm += '\t'+line+'\n'
		}
		return asm
	}

}

module.exports = BurikoScript
