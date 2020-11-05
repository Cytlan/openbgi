//
// Parse execution.log and output a human readable format
//
const fs = require('fs')

const fd = fs.openSync('downloader/data/tayutama/execution.log', 'r')

const TYPE_OPCODE =     1
const TYPE_OPCODE_RES = 2
const TYPE_READ8 =      3
const TYPE_READ16 =     4
const TYPE_READ32 =     5
const TYPE_POP =        6
const TYPE_PUSH =       7
const TYPE_PC =         8

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

let block = Buffer.alloc(1024)
fs.readSync(fd, block)

function makeLog()
{
	return {
		opcode: -1,
		res: -1,
		thread: -1,
		pc: -1,
		stackIn: [],
		stackOut: [],
		bytesIn: []
	}
}

let index = 0
function read8()
{
	return block.readUInt8(index++)
}
function read16()
{
	let n = block.readUInt16LE(index)
	index += 2
	return n
}
function read32()
{
	let n = block.readUInt32LE(index)
	index += 4
	return n
}

/*
	uint8_t opcode;
	uint8_t res;
	uint8_t thread;
	uint16_t numStackIn;
	uint16_t numStackOut;
	uint16_t numBytesIn;
	uint32_t pc;
	uint32_t stackIn[512];
	uint32_t stackOut[512];
	uint8_t bytesIn[1024];
*/

while(index < 1024)
{
	let log = makeLog()

	log.opcode = read8()
	log.res = read8()
	log.thread = read8()

	let stackIn = read16()
	let stackOut = read16()
	let bytesIn = read16()

	log.pc = read32()

	for(let i = 0; i < stackIn; i++)
		log.stackIn.push(read32())

	for(let i = 0; i < stackOut; i++)
		log.stackOut.push(read32())

	let bi = 0
	while(bi < bytesIn)
	{
		let size = read8()
		bi++
		switch(size)
		{
			case 1:
				log.bytesIn.push(read8())
				break
			case 2:
				log.bytesIn.push(read16())
				break
			case 4:
				log.bytesIn.push(read32())
				break
		}
		bi += size
	}
	printLog(log)
}

function printLog(log)
{
	let opcode = opcodes[log.opcode]
	if(!opcode)
		throw new Error('Unknown opcode: 0x'+toHex8(log.opcode)+' ('+log.opcode+')')
	let stackIn = log.stackIn.join(', ')
	let stackOut = log.stackOut.join(', ')
	let bytes = log.bytesIn.join(', ')
	console.log('0x'+toHex8(log.opcode)+' '+opcode.mnemonic+'('+bytes+') ['+stackIn+' → '+stackOut+']')
}

console.log(block)
