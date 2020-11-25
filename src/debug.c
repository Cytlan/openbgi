//
// Debug helper functions 
//

#include "debug.h"
#include "sjis.h"

#define UNICODE_BUF_SIZE 1024
uint16_t unicodeStrBuf[UNICODE_BUF_SIZE];

void Debug_PrintfSJIS(uint16_t* message, uint8_t* str)
{
	char* ptr = str;
	char c = *ptr;
	int i = 0;
	while(c)
	{
		c = *ptr;
		unicodeStrBuf[i] = SjisToUTF16(ptr);
		i++;
		if(SjisIsTwobyte(ptr))
			ptr++;
		ptr++;
		if(i == UNICODE_BUF_SIZE - 1)
			break;
	}
	unicodeStrBuf[i] = 0;

	wprintf(L"%s: %s\n", message, &unicodeStrBuf[0]);
}