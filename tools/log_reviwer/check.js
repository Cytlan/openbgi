const fs = require('fs')

let file = fs.readFileSync('../../data/nurseryrhyme_trial/vm_trace.log', 'utf8')

file = file.split('\n')

file.shift()
file.shift()
file.shift()
file.pop()
file.pop()
file.pop()
file.pop()
file.pop()

function getOpcodes(group)
{
	let path = '../../src/opcodes'
	if(group)
		path += '_'+group
	path += '.c'

	let file = fs.readFileSync(path, 'utf8')

	// Find the array initializer (multiline-safe)
	const regex = /Mnemonics\s*\[\s*256\s*\]\s*=\s*\{([\s\S]*?)\};/;
	const match = file.match(regex);
	if (!match) throw new Error('Mnemonics[256] array not found');

	const body = match[1];

	// Extract all "strings"
	const strings = (body.match(/"([^"]*)"/g) || []).map(s => s.slice(1, -1));
	return strings;
}

let opcodeNames = {
	0: getOpcodes(),
	128: getOpcodes('sys0'),
	144: getOpcodes('grp0'),
	145: getOpcodes('grp1'),
	146: getOpcodes('grp2'),
	160: getOpcodes('snd0'),
	176: getOpcodes('ext0'),
	192: getOpcodes('ext1'),
}

function parseLine(line)
{
	line = line.substring(1, line.length - 1)
	let parts = line.split(',')
	let o = {
		threadId: parseInt(parts.shift()),
		tick: parseInt(parts.shift()),
		state: parseInt(parts.shift())
	}
	if(o.state == 0 || o.state == 3)
	{
		o.opcode = parseInt(parts.shift(), 16)
		o.opcodeSize = parseInt(parts.shift(), 16)
		o.programId = parseInt(parts.shift(), 16)
		o.threadId = parseInt(parts.shift(), 16)
		o.prevVMState = parseInt(parts.shift(), 16)
		o.flags = parseInt(parts.shift(), 16)
		o.stackPointer = parseInt(parts.shift(), 16)
		o.instructionPointer = parseInt(parts.shift(), 16)
		o.programCounter = parseInt(parts.shift(), 16)
		o.basePointer = parseInt(parts.shift(), 16)
		o.stackSize = parseInt(parts.shift(), 16)
		o.stackMemConfig = parseInt(parts.shift(), 16)
		o.stack = parseInt(parts.shift(), 16)
		o.codeSpaceSize = parseInt(parts.shift(), 16)
		o.codeSpaceMemConfig = parseInt(parts.shift(), 16)
		o.codeSpace = parseInt(parts.shift(), 16)
		o.programList = parseInt(parts.shift(), 16)
		o.programCount = parseInt(parts.shift(), 16)
		o.codeSpaceTop = parseInt(parts.shift(), 16)
		o.localMemSize = parseInt(parts.shift(), 16)
		o.localMemConfig = parseInt(parts.shift(), 16)
		o.localMem = parseInt(parts.shift(), 16)
	}
	else
	{
		o.value = parseInt(parts.shift(), 16)
	}
	return o
}

let opcodes = {}
for(let i = 0; i < file.length; i++)
{
	let o = parseLine(file[i])
	if(o.state == 0)
	{
		let name = 'Base.'
		let highByte = o.opcode >> 8
		let lowByte = o.opcode & 0xFF
		let names = opcodeNames[highByte]
		if(highByte)
			name = opcodeNames[0][highByte]+'.'
		name += names[lowByte]
		name += ' ('+highByte+'.'+lowByte+')'

		if(!opcodes[name])
			opcodes[name] = 1
		else
			opcodes[name]++
	}
}
//console.log(opcodes)

const sortable = Object.fromEntries(
    Object.entries(opcodes).sort(([,a],[,b]) => a-b)
)
console.log(sortable)
/*
Sys0.IsLauncher (128.253): 1
Sys0.SetGlobalUnknownVal001 (128.104): 1
Sys0.InitGlobalMem (128.112): 1
Sys0.GetSysTime (128.4): 1
Sys0.Srand (128.0): 1
Sys0.SetCursorShape (128.102): 1
Sys0.SetFlagUnknown10 (128.116): 1
Sys0.LoadGlobalDatabase (128.128): 1
Sys0.SetDisplayMode (128.96): 1
Sys0.Unknown_0x3D (128.61): 1
Sys0.IsDirectory (128.42): 1
Sys0.Unknown_98 (128.98): 1
Sys0.Unknown_92 (128.92): 1
Sys0.SwitchToThread (128.94): 1
Sys0.Unknown_31 (128.31): 1
Sys0.SetTimer (128.88): 2
Sys0.Unknown_0x1B (128.27): 2
Sys0.GetThreadID (128.70): 2
Sys0.CreateThread (128.68): 2
Sys0.Unknown_161 (128.161): 2
Sys0.EnableSearchPaths (128.54): 3

Sys0.DeleteProgram (128.65): 5
Sys0.AddSearchPath (128.55): 6
Sys0.LoadProgram (128.64): 6
Sys0.Unknown_89 (128.89): 12
Sys0.Unknown_90 (128.90): 12
Sys0.FindFile (128.52): 62
Sys0.Unknown_160 (128.160): 870
Sys0.Yield (128.95): 1149

Snd0.Unknown_17 (160.17): 1
Snd0.Unknown_20 (160.20): 1
Snd0.Unknown_36 (160.36): 2
Snd0.SetChannelVolume (160.8): 8
Snd0.Unknown_32 (160.32): 8
Snd0.SetEffectVolume (160.9): 40

Grp1.neticMargin (145.152): 1
Grp1.Unknown_141 (145.141): 24

Grp0.SetFramerate (144.2): 1
Grp0.Unknown_0x03 (144.3): 1
Grp0.Unknown_8 (144.8): 1
Grp0.SetAntialiasingLevel (144.13): 1
Grp0.SetFlag1to4 (144.175): 1
Grp0.SetFlagUnknown20 (144.148): 1
Grp0.SetSplits (144.150): 1
Grp0.SetSplits2 (144.149): 1
Grp0.SetUnknownGrp0Val1and2 (144.151): 1
Grp0.SetFlagUnknown21 (144.159): 1
Grp0.SetOpacity (144.12): 1
Grp0.Unknown_76 (144.76): 1
Grp0.Unknown_0x13 (144.19): 1
Grp0.--Unknown-- (144.139): 1
Grp0.Unknown_32 (144.32): 1
Grp0.Unknown_132 (144.132): 1
Grp0.Unknown_185 (144.185): 1
Grp0.Unknown_129 (144.129): 1
Grp0.Unknown_0x0E (144.14): 2
Grp0.CreateWindowObject (144.128): 2
Grp0.Unknown_134 (144.134): 2
Grp0.Unknown_133 (144.133): 2
Grp0.Unknown_135 (144.135): 2
Grp0.Unknown_136 (144.136): 2
Grp0.--Unknown-- (144.138): 2
Grp0.Unknown_184 (144.184): 2
Grp0.Unknown_186 (144.186): 2
Grp0.Unknown_22 (144.22): 2
Grp0.Unknown_182 (144.182): 4
Grp0.Unknown_0 (144.0): 15
Grp0.Unknown_0x10 (144.16): 139
Grp0.Unknown_188 (144.188): 278
Grp0.Unknown_191 (144.191): 279

0x90BC

184 - Create
185 - Destroy
186
188
189
190
191
*/