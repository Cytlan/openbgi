#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "thread.h"
#include "engine.h"
#include "platform_detection.h"
#include "version.h"
#include "golden_log.h"
#include "os.h"

void PrintVersion()
{
	printf("%s\n", VERSION_STRING);
	printf("Built with %s version %d.%d.%d on %s (%s), %s, %s\n\n",
		COMPILER_NAME,
		COMPILER_VERSION_MAJOR,
		COMPILER_VERSION_MINOR,
		COMPILER_VERSION_PATCH,
		PLATFORM_NAME,
		ARCH_NAME,
		__DATE__,
		__TIME__
	);
}

Engine_t* gEngine;

int main()
{
	PrintVersion();


	GoldenLog_Load("golden_log.log");

	Engine_t engine;
	gEngine = &engine;
	Engine_Init(&engine);
	OS_Init(&engine);

	uint32_t mainThreadId = Engine_LoadProgram(gEngine, "system", "ipl._bp", 0x1000, 0x20000, 0x20000);

	//Engine_ExecuteThread(gEngine, mainThreadId);
	Engine_Execute(gEngine);

/*
	int runSteps = 2000;
	int steps = runSteps;
	printf("[Engine]: Running %d instructions...\n", steps);
	while(steps)
	{
		uint32_t res = Thread_Execute(thread);
		if(res == 0xFFFFFFFF)
			break;
		steps--;
	}
	printf("[Engine]: Ran %d instructions; Exiting...\n", runSteps - steps);
*/

	OS_Quit();
	Engine_Free(&engine);


	printf("\nThanks for playing!\n\n");
}
