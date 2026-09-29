#ifndef RBOT_V0_1_0_COMMANDS_H
#define RBOT_V0_1_0_COMMANDS_H

#include <stdbool.h>

void showHelp(void);
int cmdInit(void);

/* jobs <= 0 berarti serial (1); >= 2 memicu kompilasi paralel -jN. */
int cmdBuild(int jobs);
int cmdClean(void);

#endif /* RBOT_V0_1_0_COMMANDS_H */
