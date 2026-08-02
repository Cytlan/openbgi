#include <stdio.h>
#include <stdlib.h>
#include "golden_log.h"

int GoldenLogTotal = 0;
int GoldenLogIndex = 0;
GLogEntry_t* GoldenLog = NULL;

void GoldenLog_Load(const char *filename)
{
	FILE* fp = fopen(filename, "r");
	if(!fp)
		return;

	size_t nlines = 0;
	char buf[256];
	while(fgets(buf, sizeof buf, fp))
		++nlines;

	if(nlines == 0)
	{
		fclose(fp);
		return;
	}

	GoldenLog = (GLogEntry_t*)malloc(nlines * sizeof(GLogEntry_t));
	if(!GoldenLog)
	{
		fclose(fp);
		return;
	}

	GoldenLogTotal = nlines;

	rewind(fp);
	size_t line = 0;
	while(line < nlines && fgets(buf, sizeof(buf), fp))
	{
		unsigned int a, b, c, d, e;
		if(sscanf(buf, "%u,%u,%u,%u,%u", &a, &b, &c, &d, &e) == 5)
		{
			GoldenLog[line].type = a;
			GoldenLog[line].time = b;
			GoldenLog[line].thread = c;
			GoldenLog[line].tick = d;
			GoldenLog[line].value = e;
		}
		else
		{
			GoldenLog[line].type = 0;
			GoldenLog[line].time = 0;
			GoldenLog[line].thread = 0;
			GoldenLog[line].tick = 0;
			GoldenLog[line].value = 0;
		}
		++line;
	}

	fclose(fp);
}