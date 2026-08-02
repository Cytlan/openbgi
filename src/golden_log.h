#ifndef _GOLDEN_LOG_H_
#define _GOLDEN_LOG_H_

#define LOG_TYPE_EXEC 0
#define LOG_TYPE_R8   1
#define LOG_TYPE_RC8  2
#define LOG_TYPE_RC16 3
#define LOG_TYPE_RC32 4
#define LOG_TYPE_PUSH 5
#define LOG_TYPE_POP  6

typedef struct GLogEntry
{
	unsigned int type;
	unsigned int time;
	unsigned int thread;
	unsigned int tick;
	unsigned int value;
} GLogEntry_t;

extern int GoldenLogTotal;
extern int GoldenLogIndex;
extern GLogEntry_t* GoldenLog;

void GoldenLog_Load(const char *filename);

#endif