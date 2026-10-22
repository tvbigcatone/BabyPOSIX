#ifndef BABYPOSIX_SIGNAL_H
#define BABYPOSIX_SIGNAL_H
#include "../interpreter.h"
extern const char BabyPosixSignalDefs[];
extern struct LibraryFunction BabyPosixSignalFunctions[];
void BabyPosixSignalSetupFunc(Picoc *pc);
#endif
