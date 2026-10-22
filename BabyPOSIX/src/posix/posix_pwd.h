#ifndef BABYPOSIX_PWD_H
#define BABYPOSIX_PWD_H
#include "../interpreter.h"
extern const char BabyPosixPwdDefs[];
extern struct LibraryFunction BabyPosixPwdFunctions[];
void BabyPosixPwdSetupFunc(Picoc *pc);
extern const char BabyPosixGrpDefs[];
extern struct LibraryFunction BabyPosixGrpFunctions[];
void BabyPosixGrpSetupFunc(Picoc *pc);
#endif
