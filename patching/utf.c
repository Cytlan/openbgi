//
// Unicode Helpers
//

#include "utf.h"

int UTF32ToUTF8(uint32_t in, uint8_t* out)
{
	// 0x0000 to 0x007F
	if(in < 0x00000080)
	{
		*out = in & 0xFF;
		return 1;
	}
	// 0x0080 to 0x07FF
	else if(in < 0x00000800)
	{
		*out++ = 0xC0 | ((in >> 6) & 0x1F);
		*out   = 0x80 | (in & 0x3F);
		return 2;
	}
	// 0x0800 to 0xFFFF
	else if(in < 0x00010000)
	{
		*out++ = 0xE0 | ((in >> 12) & 0x0F);
		*out++ = 0x80 | ((in >> 6) & 0x3F);
		*out   = 0x80 | (in & 0x3F);
		return 3;
	}
	// 0x10000 to 0x10FFFF
	else if(in < 0x00110000)
	{
		*out++ = 0xF0 | ((in >> 18) & 0x07);
		*out++ = 0x80 | ((in >> 16) & 0x3F);
		*out++ = 0x80 | ((in >> 6) & 0x3F);
		*out   = 0x80 | (in & 0x3F);
		return 4;
	}
	// Invalid code point
	else
		return -1;
}

int UTF16ToUTF8Length(uint16_t codepoint)
{
	// Surrogate pairs
	if(codepoint >= 0xD800 && codepoint <= 0xDFFF)
		return 4;
	// 0x0000 to 0x007F
	if(codepoint < 0x80)
		return 1;
	// 0x0080 to 0x07FF
	if(codepoint < 0x800)
		return 2;
	// 0x0800 to 0xFFFF
	return 3;
}

// Count how many bytes are needed to convert a UFT-16 string into UTF-8
int UTF16ToUTF8Strlen(uint16_t* str)
{
	int len = 0;
	uint16_t* ptr = str;
	uint16_t c = *ptr++;
	while(c)
	{
		int uLen = UTF16ToUTF8Length(c);

		// Skip trailing surrogate
		if(uLen == 4)
			ptr++;
		
		c = *ptr++;
	}
	return len;
}

// Convert UTF-16 string into UTF-8
int UTF16ToUTF8(uint16_t* in, uint8_t* out)
{
	int len = 0;
	uint16_t* inPtr = in;
	uint8_t* outPtr = out;
	uint32_t c = *inPtr++; // UTF-32 intermediate
	while(c)
	{
		int uLen = UTF16ToUTF8Length(c);

		// Decode surrogate pair
		if(uLen == 4)
		{
			c -= 0xD800;
			c <<= 10;
			c |= (*inPtr++) - 0xDC00;
		}

		// Invalid code point
		if(c >= 0x00110000)
			return -1;

		int utf8Len = UTF32ToUTF8(c, outPtr);
		outPtr += utf8Len;
		len += utf8Len;
		
		c = *inPtr++;
	}
	return len;
}
