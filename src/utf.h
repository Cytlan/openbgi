//
// Unicode helpers
//

#include <stdint.h>

#ifndef _UTF_H_
#define _UTF_H_

int UTF32ToUTF8(uint32_t in, uint8_t* out);
int UTF16ToUTF8Length(uint16_t codepoint);
int UTF16ToUTF8Strlen(uint16_t* str);
int UTF16ToUTF8(uint16_t* in, uint8_t* out);

#endif
