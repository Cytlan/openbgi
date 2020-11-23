//
// A helper script to convert the Shift-JIS JSON files into a C lookup table
//
const fs = require('fs')

const table = JSON.parse(fs.readFileSync('sjisToUtf8.json', 'utf8'))

const firstByte = Object.keys(table)

function makeName(i)
{
	const HEX = '0123456789ABCDEF'
	return 'sjis'+HEX[(i >> 4) & 0xF]+HEX[i & 0xF]
}

function makeTables(table)
{
	let arrs = []
	let arrayNames = []
	for(let i = 0; i < 256; i++)
	{
		let utfTable = makeArray(i, table)
		if(!utfTable)
		{
			arrayNames.push('NULL  ')
			continue
		}
		arrayNames.push(makeName(i))
		arrs.push(utfTable)
	}

	let indexArr = 'uint16_t* sjis[256] = {\n'
	for(let j = 0; j < 16; j++)
		indexArr += '\t'+arrayNames.slice(j*16, (j*16)+16).join(', ')+(j != 15 ? ',' : '')+'\n'
	indexArr += '};'

	return arrs.join('\n')+'\n'+indexArr
}

function toHex16(n)
{
	const HEX = '0123456789ABCDEF'
	if(n > 0xFFFF) throw new Error('Number '+n+' doesn\'t fit in 16 bits')
	return (
		HEX[(n >> 12) & 0xF]+
		HEX[(n >>  8) & 0xF]+
		HEX[(n >>  4) & 0xF]+
		HEX[(n >>  0) & 0xF]
	)
}

function makeArray(i, table)
{
	if(!table[i])
		return null
	let arr = []
	for(let j = 0; j < 256; j++)
	{
		if(!table[i][j])
			arr.push('0x0000')
		else
			arr.push('0x'+toHex16(table[i][j].codePointAt(0)))
	}
	let str = 'uint16_t '+makeName(i)+'[256] = {\n'
	for(let j = 0; j < 16; j++)
		str += '\t'+arr.slice(j*16, (j*16)+16).join(', ')+(j != 15 ? ',' : '')+'\n'
	str += '};'
	return str
}

console.log(makeTables(table))
