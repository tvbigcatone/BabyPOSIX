#ifndef BABYPOSIX_WAIT_H
#define BABYPOSIX_WAIT_H
#include "../interpreter.h"
extern const char BabyPosixWaitDefs[];
extern struct LibraryFunction BabyPosixWaitFunctions[];
void BabyPosixWaitSetupFunc(Picoc *pc);
#endif
