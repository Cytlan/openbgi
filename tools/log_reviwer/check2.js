const fs = require('fs')

let file = fs.readFileSync('../../data/nurseryrhyme_trial/vm_trace.log', 'utf8').split('\n')

let trace = []

let last = file.length - 5
for(let i in file)
{
	if(i < 3 || i > last)
		continue
	let line = file[i].trim()
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
			if(log[3] == '0')
			{
				trace.push(['0', parseInt(log[0]), parseInt(log[1]), parseInt(log[2]), parseInt(log[4])]);
				inExec = true
				execTick = log[2]
			}
			else
				inExec = false
			break;
		case 'r8':
			trace.push(['1', parseInt(log[0]), parseInt(log[1]), parseInt(log[2]), parseInt(log[4])]);
			break;
		case 'rc8':
			trace.push(['2', parseInt(log[0]), parseInt(log[1]), parseInt(log[2]), parseInt(log[4])]);
			break;
		case 'rc16':
			trace.push(['3', parseInt(log[0]), parseInt(log[1]), parseInt(log[2]), parseInt(log[4])]);
			break;
		case 'rc32':
			//if(!inExec) log[1] = execTick
			trace.push(['4', parseInt(log[0]), parseInt(log[1]), parseInt(log[2]), parseInt(log[4])]);
			break;
		case 'push':
			//if(!inExec) log[2] = execTick
			trace.push(['5', parseInt(log[0]), parseInt(log[1]), parseInt(log[2]), parseInt(log[4])]);
			break;
		case 'pop':
			//if(!inExec) log[2] = execTick
			trace.push(['6', parseInt(log[0]), parseInt(log[1]), parseInt(log[2]), parseInt(log[4])]);
			break;
		case 'mem':
			if(!inExec)
			{
				console.log('gmem outside of exec on line '+i+' time '+log[0])
			}
	}
}

/*
s = '#include "golden_log.h"\n'
s += 'int GoldenLogIndex = 0;\n'
s += 'GLogEntry_t GoldenLog[] = {\n'
for(let i in trace) s += '\t{'+(trace[i]).join(', ')+'},\n'
s += '};\n'
*/
s = '';
for(let i in trace) s += (trace[i]).join(',')+'\n'
fs.writeFileSync('golden_log.log', s, 'utf8')
