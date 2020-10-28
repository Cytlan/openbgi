//
// Javascript Buriko-compatible interpreter
//
//

const fs = require('fs')

const BurikoScript = require('./burikoscript.js')

const sjisToUtf8 = JSON.parse(fs.readFileSync('sjisToUtf8.json', 'utf8'))
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

const opcodes = {
	0x00: {
		mnemonic:    'push8',
		operandSize: 1,
		operands:    [{name: 'value', size: 1}],
	},
	0x01: {
		mnemonic:    'push16',
		operandSize: 2,
		operands:    [{name: 'value', size: 2}],
	},
	0x02: {
		mnemonic:    'push32',
		operandSize: 4,
		operands:    [{name: 'value', size: 4}],
	},
	0x04: {
		mnemonic:    'baseptr',
		operandSize: 2,
		operands:    [{name: 'offset', size: 2}],
	},
	0x05: {
		mnemonic:    'codeptr',
		operandSize: 2,
		operands:    [{name: 'offset', size: 2}],
	},
	0x06: {
		mnemonic:    'codeoffset',
		operandSize: 2,
		operands:    [{name: 'offset', size: 2}],
	},
	0x08: {
		mnemonic:    'load',
		operandSize: 1,
		operands:    [{name: 'size', size: 1}],
	},
	0x09: {
		mnemonic:    'storepv',
		operandSize: 1,
		operands:    [{name: 'size', size: 1}],
	},
	0x0A: {
		mnemonic:    'storevp',
		operandSize: 1,
		operands:    [{name: 'size', size: 1}],
	},
	0x0C: {
		mnemonic:    'storemulti',
		operandSize: 2,
		operands:    [{name: 'size', size: 1}, {name: 'count', size: 1}],
	},
	0x10: {
		mnemonic:    'loadbase',
		operandSize: 0,
		operands:    [],
	},
	0x11: {
		mnemonic:    'storebase',
		operandSize: 0,
		operands:    [],
	},
	0x14: {
		mnemonic:    'jmp',
		operandSize: 0,
		operands:    [],
	},
	0x15: {
		mnemonic:    'jc',
		operandSize: 1,
		operands:    [{name: 'condition', size: 1}],
	},
	0x16: {
		mnemonic:    'call',
		operandSize: 0,
		operands:    [],
	},
	0x17: {
		mnemonic:    'ret',
		operandSize: 0,
		operands:    [],
	},
	0x20: {
		mnemonic:    'add',
		operandSize: 0,
		operands:    [],
	},
	0x21: {
		mnemonic:    'sub',
		operandSize: 0,
		operands:    [],
	},
	0x22: {
		mnemonic:    'mul',
		operandSize: 0,
		operands:    [],
	},
	0x23: {
		mnemonic:    'div',
		operandSize: 0,
		operands:    [],
	},
	0x26: {
		mnemonic:    'or',
		operandSize: 0,
		operands:    [],
	},
	0x28: {
		mnemonic:    'not',
		operandSize: 0,
		operands:    [],
	},
	0x29: {
		mnemonic:    'shl',
		operandSize: 0,
		operands:    [],
	},
	0x2A: {
		mnemonic:    'shr',
		operandSize: 0,
		operands:    [],
	},
	0x2B: {
		mnemonic:    'sar',
		operandSize: 0,
		operands:    [],
	},
	0x40: {
		mnemonic:    'ternary',
		operandSize: 0,
		operands:    [],
	},
	0x6f: {
		mnemonic:    'sprintf',
		operandSize: 0,
		operands:    [],
	},
	0x79: {
		mnemonic:    'infobox',
		operandSize: 0,
		operands:    [],
	},
	0x80: {
		mnemonic:    'sys1',
		operandSize: 1,
		operands:    [{name: 'routine', size: 1}],
	},
	0x90: {
		mnemonic:    'grp1',
		operandSize: 1,
		operands:    [{name: 'routine', size: 1}],
	},
	0x91: {
		mnemonic:    'grp2',
		operandSize: 1,
		operands:    [{name: 'routine', size: 1}],
	},
}

class BurikoInterpreter
{
	constructor()
	{
		this.baseLocation = 0x00000000
		this.codeLocation = 0x00100000
		this.codeLoadLocation = this.codeLocation
		this.memorySize = 0x00200000

		this.base = 0
		this.stack = []
		this.sp = 0
		this.pc = 0
		this.returnAddresses = []

		this.programs = []

		this.memory = Buffer.alloc(this.memorySize)

		this.executionCb = null
	}

	executionCallback(cb)
	{
		this.executionCb = cb
	}

	continueExecution()
	{
		if(this.executionCb)
			this.executionCb()
	}

	loadScript(file, callback)
	{
		console.log('Loading script: '+file)
		fs.readFile(file, (err, res) =>
		{
			if(err) return callback(err, null)
			this.parseScript(res, callback)
		})
	}

	printStack()
	{
		for(let i = 0; i < this.sp; i++)
		{
			console.log(i+': '+toHex32(this.stack[i]))
		}
	}

	parseScript(file, callback)
	{
		let script = new BurikoScript()
		script.load(file)
		this.code = script.code
		
		// Get the location and save the location, so that we may unload it later
		let location = this.codeLoadLocation
		this.programs.push({location: location, size: script.code.length})

		console.log('Loading '+script.code.length+' bytes to location '+toHex32(location))

		// Copy the script into ram
		script.code.copy(this.memory, this.codeLoadLocation)
		this.codeLoadLocation += script.code.length

		callback(null, location)
	}

	loadArchiveScript(archive, file, cb)
	{
		this.loadScript(archive+'/'+file, (err, location) =>
		{
			if(err) return cb(err, null)
			cb(null, location)
		})
	}

	setPC(value)
	{
		this.pc = value
		console.log('pc:   '+toHex32(value))
	}

	push(val)
	{
		this.stack[this.sp++] = val
		console.log('push: '+toHex32(val))
	}

	pop()
	{
		if(this.sp == 0)
			throw new Error('Stack underflow')
		let val = this.stack[--this.sp]
		console.log('pop:  '+toHex32(val))
		return val
	}

	getStack(location)
	{
		if(this.sp - location < 0)
			throw new Error('Stack underflow')
		return this.stack[location]
	}

	write8(offset, value)
	{
		if(offset >= this.memorySize)
			throw new Error('Writing out of bounds')
		this.memory.writeUInt8(value, offset)
	}

	write16(offset, value)
	{
		if(offset + 1 >= this.memorySize)
			throw new Error('Writing out of bounds')
		this.memory.writeUInt16LE(value, offset)
	}

	write32(offset, value)
	{
		if(offset + 3 >= this.memorySize)
			throw new Error('Writing out of bounds')
		this.memory.writeUInt16LE(value & 0xFFFF, offset)
		this.memory.writeUInt16LE((value >> 16) & 0xFFFF, offset + 2)
	}

	// Read 1 code byte
	read8(offset)
	{
		if(offset >= this.memorySize)
			throw new Error('Reading out of bounds')
		return this.memory.readUInt8(offset)
	}

	// Read 2 code bytes
	read16(offset)
	{
		if(offset + 1 >= this.memorySize)
			throw new Error('Reading out of bounds')
		return this.memory.readInt16LE(offset)
	}

	// Read 4 code bytes
	read32(offset)
	{
		if(offset + 3 >= this.memorySize)
			throw new Error('Reading out of bounds')
		return this.memory.readInt32LE(offset)
	}

	// Read 1 code byte from PC offset
	readPC8()
	{
		let v = this.read8(this.codeLocation + this.pc)
		this.pc++
		return v
	}

	// Read 2 code bytes from PC offset
	readPC16()
	{
		let v = this.read16(this.codeLocation + this.pc)
		this.pc += 2
		return v
	}

	// Read 4 code bytes from PC offset
	readPC32()
	{
		let v = this.read32(this.codeLocation + this.pc)
		this.pc += 4
		return v
	}

	readSjisString(address)
	{
		let str = ''
		let cl = 0
		let sjis = false
		while(1)
		{
			let c = this.read8(address++)
			if(!c)
				break
			if(sjis)
			{
				if(!sjisToUtf8[cl][c])
					throw new Error('Unknown SJIS codepoint: '+toHex8(cl)+toHex8(c))
				str += sjisToUtf8[cl][c]
				sjis = false
				cl = 0
			}
			else if(sjisToUtf8[c])
			{
				sjis = true
				cl = c
			}
			else
			{
				str += sjisToUtf8[0][c]
			}
		}
		return str
	}

	writeSjisString(string, address)
	{
		let offset = address
		for(let i in string)
		{
			let char = string[i]

			let c = utf8ToSjis[char]
			if(!c)
				throw new Error('Cannot convert to SJIS codepoint: '+c)
			if(c < 0x100)
				this.write8(offset++, c)
			else
			{
				this.write8(offset++, (c >> 8) & 0xFF)
				this.write8(offset++, c & 0xFF)
			}
		}
		this.write8(offset++, 0)
	}

	// Return assembly string for the opcode at the given address
	disassemble(offset)
	{
		let opcode = this.read8(offset)
		if(opcodes[opcode])
		{
			let args = []
			for(let i in opcodes[opcode].operands)
			{
				switch(opcodes[opcode].operands[i].size)
				{
					case 1:
						args.push('0x'+toHex8(this.read8(offset+1)))
						break
					case 2:
						args.push('0x'+toHex16(this.read16(offset+1)))
						break
					case 4:
						args.push('0x'+toHex32(this.read32(offset+1)))
						break
				}
			}
			console.log(toHex32(offset)+' '+opcodes[opcode].mnemonic+' '+args.join(' '))
		}
		else
			console.log('unknown_'+toHex8(opcode))
	}

	executeOpcode(opcode)
	{
		// Get instruction object
		let instruction = opcodes[opcode]
		if(!instruction)
		{
			this.printStack()
			throw new Error('Unknown opcode: '+toHex8(opcode)+' at '+toHex32(this.pc))
		}

		// Get function
		let func = 'op_'+instruction.mnemonic
		if(!this[func])
			throw new Error('Instruction '+instruction.mnemonic+' is not implemented')
		if(typeof this[func] !== 'function')
			throw new Error('Implementation for '+instruction.mnemonic+', '+func+', is not a function')

		// Check how many arguments the instruction expects
		let argNum = instruction.operands.length
		if(this[func].length != argNum)
			throw new Error('Instruction '+instruction.mnemonic+' expects '+argNum+' operands')

		// Read operands
		let args = []
		for(let i = 0; i < argNum; i++)
		{
			switch(instruction.operands[i].size)
			{
				case 1:
					args.push(this.readPC8())
					break
				case 2:
					args.push(this.readPC16())
					break
				case 4:
					args.push(this.readPC32())
					break
			}
		}

		// Call opcode
		// If the instruction returns true, then it has finished executing
		// If it returns false, then the instruction needs to perform some async work and will do a callback instead
		let ret = this[func].apply(this, args)

		return ret
	}

	step()
	{
		this.disassemble(this.codeLocation + this.pc)
		let opcodePc = this.pc
		let opcode = this.readPC8()

		let ret = this.executeOpcode(opcode)

		console.log()

		return ret
	}

	// -------------------------------------------------------------------------
	//
	// Push
	//
	// -------------------------------------------------------------------------
	// 0x00 push8
	op_push8(value)
	{
		this.push(value & 0xFF)
		return true
	}

	// 0x01 push16
	op_push16(value)
	{
		this.push(value & 0xFFFF)
		return true
	}

	// 0x02 push32
	op_push32(value)
	{
		this.push(value & 0xFFFFFFFF)
		return true
	}

	// -------------------------------------------------------------------------
	//
	// Pointers
	//
	// -------------------------------------------------------------------------
	// 0x04 baseptr
	op_baseptr(offset)
	{
		this.push(this.baseLocation + (this.base - offset))
		return true
	}

	// 0x05 codeptr
	op_codeptr(offset)
	{
		this.push(this.codeLocation + ((this.pc - 3) + offset))
		return true
	}

	// 0x06 codeoffset
	op_codeoffset(offset)
	{
		this.push((this.pc - 3) + offset)
		return true
	}

	// 0x08 load
	op_load(size)
	{
		let address = this.pop()
		let value = 0
		switch(size)
		{
			case 0:
				value = this.read8(address)
				break
			case 1:
				value = this.read16(address)
				break
			case 2:
				value = this.read32(address)
				break
		}
		this.push(value)
		return true
	}

	// 0x09 storepv
	op_storepv(size)
	{
		let value = this.pop()
		let pointer = this.pop()
		switch(size)
		{
			case 0:
				this.write8(pointer, value)
				break
			case 1:
				this.write16(pointer, value)
				break
			case 2:
				this.write32(pointer, value)
				break
		}
		return true
	}

	// 0x0A storevp
	op_storevp(size)
	{
		let pointer = this.pop()
		let value = this.pop()
		switch(size)
		{
			case 0:
				this.write8(pointer, value)
				break
			case 1:
				this.write16(pointer, value)
				break
			case 2:
				this.write32(pointer, value)
				break
		}
		return true
	}

	// 0x0C storemulti
	op_storemulti(size, count)
	{
		let address = this.getStack(count)

		for(let i = 0; i < count; i++)
		{
			let value = this.pop()
			switch(size)
			{
				case 0:
					this.write8(address, value)
					break
				case 1:
					this.write16(address, value)
					break
				case 2:
					this.write32(address, value)
					break
			}
		}
		this.pop() // Need to get rid of the address we read earlier
		return true
	}

	// -------------------------------------------------------------------------
	//
	// Base
	//
	// -------------------------------------------------------------------------
	// 0x10 loadbase
	op_loadbase()
	{
		this.push(this.base)
		return true
	}

	// 0x11 loadbase
	op_storebase()
	{
		this.base = this.pop()
		return true
	}

	// -------------------------------------------------------------------------
	//
	// Program flow
	//
	// -------------------------------------------------------------------------
	// 0x14 jmp
	op_jmp()
	{
		this.setPC(this.pop())
		return true
	}

	// 0x15 jc
	op_jc(comparison)
	{
		let location = this.pop()
		let value = this.pop()
		let jump = true
		switch(comparison)
		{
			// jnz
			case 0:
				jump = value != 0
				break
			// jz
			case 1:
				jump = value == 0
				break
			// jg
			case 2:
				jump = value > 0
				break
			// jge
			case 3:
				jump = value >= 0
				break
			// jle
			case 4:
				jump = value <= 0
				break
			// jl
			case 5:
				jump = value < 0
				break
		}

		if(jump)
			this.setPC(location)

		return true
	}

	// 0x16 call
	op_call()
	{
		let address = this.pop()
		this.returnAddresses.push(this.pc)
		this.pc = address

		return true
	}

	// 0x17 ret
	op_ret()
	{
		this.pc = this.returnAddresses.pop()

		return true
	}

	// -------------------------------------------------------------------------
	//
	// Arithmatic
	//
	// -------------------------------------------------------------------------
	// 0x20 add
	op_add()
	{
		this.push(this.pop() + this.pop())
		return true
	}

	// 0x21 sub
	op_sub()
	{
		let right = this.pop()
		let left = this.pop()
		this.push(left - right)
		return true
	}

	// 0x22 mul
	op_mul()
	{
		this.push(this.pop() * this.pop())
		return true
	}

	// 0x23 div
	op_div()
	{
		let right = this.pop()
		let left = this.pop()
		this.push(left / right)
		return true
	}

	// 0x24 mod
	op_mod()
	{
		let right = this.pop()
		let left = this.pop()
		this.push(left % right)
		return true
	}

	// 0x25 and
	op_and()
	{
		this.push(this.pop() & this.pop())
		return true
	}

	// 0x26 or
	op_or()
	{
		this.push(this.pop() | this.pop())
		return true
	}

	// 0x27 xor
	op_or()
	{
		this.push(this.pop() ^ this.pop())
		return true
	}

	// 0x28 not
	op_not()
	{
		this.push(this.pop() ^ 0xFFFFFFFF)
		return true
	}

	// 0x29 shl
	op_shl()
	{
		let shift = this.pop()
		let value = this.pop()
		this.push(value << shift)
		return true
	}

	// 0x2A shr
	op_shr()
	{
		let shift = this.pop()
		let value = this.pop()
		let mask = (1 << (32 - shift)) - 1
		this.push((value >> shift) & mask)
		return true
	}

	// 0x2B sar
	op_sar()
	{
		let shift = this.pop()
		let value = this.pop()
		this.push(value >> shift)
		return true
	}

	// 0x40 ternary
	op_ternary()
	{
		let falseValue = this.pop()
		let trueValue = this.pop()
		let test = this.pop()
		if(test == 0)
			this.push(falseValue)
		else
			this.push(trueValue)
		return true
	}

	// -------------------------------------------------------------------------
	//
	// Other
	//
	// -------------------------------------------------------------------------
	// 0x6f sprintf
	op_sprintf()
	{
		let format = this.readSjisString(this.pop())
		let address = this.pop()

		// TODO: Actually make formatted string

		this.writeSjisString(format, address)
		return true
	}

	// 0x79 infobox
	op_infobox()
	{
		let string = this.readSjisString(this.pop())
		console.log(
			'=====================\n'+
			' Alert\n'+
			'=====================\n'+
			' '+string+'\n'+
			'=====================\n'
		)
		return true
	}

	// 0x80 sys1
	op_sys1(routine)
	{
		switch(routine)
		{
			case 0x00:
				this.pop()
				break

			case 0x04:
				break

			case 0x1B:
				this.pop()
				this.pop()
				break

			case 0x34:
			{
				let address = this.pop()
				let string = this.readSjisString(address)
				let value = this.pop()
				console.log(string)
				this.push(1)
				break
			}

			case 0x36:
				this.pop()
				break

			case 0x37:
			{
				let address = this.pop()
				let string = this.readSjisString(address)
				console.log(string)
				break
			}

			case 0x40:
			{
				let fileAddr = this.pop()
				let fileStr = this.readSjisString(fileAddr)
				let archiveAddr = this.pop()
				let archiveStr = this.readSjisString(archiveAddr)

				this.loadArchiveScript(archiveStr, fileStr, (err, location) =>
				{
					if(err) throw err
					this.push(location - this.codeLocation)
					this.continueExecution()
				})

				return false
			}

			case 0x58:
				this.pop()
				break

			case 0x64:
				this.pop()
				break

			case 0x67:
				this.pop()
				break

			case 0x68:
				this.pop()
				break

			case 0x70:
				this.pop()
				this.push(Date.now())
				break

			case 0x74:
				this.pop()
				break

			case 0x80:
				this.push(0)
				break

			// ???
			case 0xfd:
				this.push(0)
				break

			default:
				this.printStack()
				throw new Error('Unknown sys1 routine: '+toHex8(routine))
		}
		return true
	}

	// 0x90 grp1
	op_grp1(routine)
	{
		switch(routine)
		{
			case 0x02:
				this.pop()
				break

			case 0x03:
				this.pop()
				break

			case 0x08:
				this.pop()
				break

			case 0x0d:
				this.pop()
				break

			case 0x0e:
				this.pop()
				this.pop()
				this.pop()
				this.pop()
				this.pop()
				break

			case 0x94:
				break;

			case 0x95:
				this.pop()
				this.pop()
				break;

			case 0x96:
				this.pop()
				this.pop()
				this.pop()
				this.pop()
				this.pop()
				this.pop()
				this.pop()
				this.pop()
				this.pop()
				this.pop()
				break;

			case 0xaf:
				this.pop()
				break

			default:
				this.printStack()
				throw new Error('Unknown grp1 routine: '+toHex8(routine))
		}
		return true
	}

	// 0x91 grp1
	op_grp2(routine)
	{
		switch(routine)
		{
			case 0x98:
				this.pop()
				this.pop()
				this.pop()
				break

			default:
				this.printStack()
				throw new Error('Unknown grp1 routine: '+toHex8(routine))
		}
		return true
	}
}

module.exports = BurikoInterpreter
