#ifndef _OS_H_
#define _OS_H_

typedef struct Engine Engine_t;

int OS_Init(Engine_t* engine);
int OS_Poll();
int OS_Quit();

#endif