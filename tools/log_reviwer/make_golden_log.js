const fs = require('fs')

let tid = 2
let max = 7
let s2 = ''
let s = '#include "golden_log.h"\n';
s2 += 'ThreadGLog_t gThreadLogs[8] = {\n'
for(let i = tid; i <= max; i++)
{
	let file = fs.readFileSync('../../data/nurseryrhyme_trial/vm_trace_thread_'+i+'.log', 'utf8')

	s2 += '\t['+i+'] = {\n'
	s2 += '\t\t.tid  = '+i+',\n'
	s2 += '\t\t.execIdx = 0,\n'
	s2 += '\t\t.r8Idx = 0,\n'
	s2 += '\t\t.rc8Idx = 0,\n'
	s2 += '\t\t.rc16Idx = 0,\n'
	s2 += '\t\t.rc32Idx = 0,\n'
	s2 += '\t\t.pushIdx = 0,\n'
	s2 += '\t\t.popIdx = 0,\n'
	s2 += '\t\t.exec = &G_T'+i+'_LOG_EXEC[0],\n'
	s2 += '\t\t.r8   = &G_T'+i+'_LOG_R8[0],\n'
	s2 += '\t\t.rc8  = &G_T'+i+'_LOG_RC8[0],\n'
	s2 += '\t\t.rc16 = &G_T'+i+'_LOG_RC16[0],\n'
	s2 += '\t\t.rc32 = &G_T'+i+'_LOG_RC32[0],\n'
	s2 += '\t\t.push = &G_T'+i+'_LOG_PUSH[0],\n'
	s2 += '\t\t.pop  = &G_T'+i+'_LOG_POP[0]\n'
	s2 += '\t},\n'

	file = file.split('\n')

	let traces = {
		'exec': [],
		'r8': [],
		'rc8': [],
		'rc16': [],
		'rc32': [],
		'push': [],
		'pop': [],
	}

	let inExec = false
	let execTick = 0
	for(let line in file)
	{
		line = file[line].trim()
		if(!line)
			continue
		let s = line.split(':')
		let type = s[0]
		let log = s[1].trim()
		log = log.substr(1, log.length-2).split(',')
		log = log.map(str => str.trim())
		switch(type)
		{
			case 'exec':
				traces.exec.push([log[1], log[3]]);
				if(log[2] == '0')
				{
					inExec = true
					execTick = log[1]
				}
				else
					inExec = false
				break;
			case 'r8':
				//if(!inExec) log[1] = execTick
				traces.r8.push([log[1], log[3], log[4]]);
				break;
			case 'rc8':
				//if(!inExec) log[1] = execTick
				traces.rc8.push([log[1], log[3], log[4]]);
				break;
			case 'rc16':
				//if(!inExec) log[1] = execTick
				traces.rc16.push([log[1], log[3], log[4]]);
				break;
			case 'rc32':
				//if(!inExec) log[1] = execTick
				traces.rc32.push([log[1], log[3], log[4]]);
				break;
			case 'push':
				if(!inExec) log[1] = execTick
				traces.push.push([log[1], log[3]]);
				break;
			case 'pop':
				if(!inExec) log[1] = execTick
				traces.pop.push([log[1], log[3]]);
				break;
		}
	}

	s += 'int G_T'+i+'_LOG_EXEC[][2] = {\n'
	for(let i in traces.exec) s += '\t{'+(traces.exec[i]).join(', ')+'},\n'
	s += '\t{0xDEADBEEF, 0},\n'
	s += '};\n'
	s += 'int G_T'+i+'_LOG_R8[][3] = {\n'
	for(let i in traces.r8) s += '\t{'+(traces.r8[i]).join(', ')+'},\n'
	s += '\t{0xDEADBEEF, 0, 0},\n'
	s += '};\n'
	s += 'int G_T'+i+'_LOG_RC8[][3] = {\n'
	for(let i in traces.rc8) s += '\t{'+(traces.rc8[i]).join(', ')+'},\n'
	s += '\t{0xDEADBEEF, 0, 0},\n'
	s += '};\n'
	s += 'int G_T'+i+'_LOG_RC16[][3] = {\n'
	for(let i in traces.rc16) s += '\t{'+(traces.rc16[i]).join(', ')+'},\n'
	s += '\t{0xDEADBEEF, 0, 0},\n'
	s += '};\n'
	s += 'int G_T'+i+'_LOG_RC32[][3] = {\n'
	for(let i in traces.rc32) s += '\t{'+(traces.rc32[i]).join(', ')+'},\n'
	s += '\t{0xDEADBEEF, 0, 0},\n'
	s += '};\n'
	s += 'int G_T'+i+'_LOG_PUSH[][2] = {\n'
	for(let i in traces.push) s += '\t{'+(traces.push[i]).join(', ')+'},\n'
	s += '\t{0xDEADBEEF, 0},\n'
	s += '};\n'
	s += 'int G_T'+i+'_LOG_POP[][2] = {\n'
	for(let i in traces.pop) s += '\t{'+(traces.pop[i]).join(', ')+'},\n'
	s += '\t{0xDEADBEEF, 0},\n'
	s += '};\n'
}
s2 += '};'
s += s2;
fs.writeFileSync('golden_log.c', s, 'utf8')