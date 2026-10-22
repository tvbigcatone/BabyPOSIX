#ifndef BABYPOSIX_COVER_H
#define BABYPOSIX_COVER_H
#include "../interpreter.h"
extern const char BabyPosixLanginfoDefs[];
extern struct LibraryFunction BabyPosixLanginfoFunctions[];
void BabyPosixLanginfoSetupFunc(Picoc *pc);
extern const char BabyPosixPathsDefs[];
extern struct LibraryFunction BabyPosixPathsFunctions[];
void BabyPosixPathsSetupFunc(Picoc *pc);
extern const char BabyPosixSysexitsDefs[];
extern struct LibraryFunction BabyPosixSysexitsFunctions[];
void BabyPosixSysexitsSetupFunc(Picoc *pc);
extern const char BabyPosixSyslogDefs[];
extern struct LibraryFunction BabyPosixSyslogFunctions[];
void BabyPosixSyslogSetupFunc(Picoc *pc);
extern const char BabyPosixGlobDefs[];
extern struct LibraryFunction BabyPosixGlobFunctions[];
void BabyPosixGlobSetupFunc(Picoc *pc);
extern const char BabyPosixMmanDefs[];
extern struct LibraryFunction BabyPosixMmanFunctions[];
void BabyPosixMmanSetupFunc(Picoc *pc);
extern const char BabyPosixUioDefs[];
extern struct LibraryFunction BabyPosixUioFunctions[];
void BabyPosixUioSetupFunc(Picoc *pc);
extern const char BabyPosixStatvfsDefs[];
extern struct LibraryFunction BabyPosixStatvfsFunctions[];
void BabyPosixStatvfsSetupFunc(Picoc *pc);
extern const char BabyPosixSetjmpDefs[];
extern struct LibraryFunction BabyPosixSetjmpFunctions[];
void BabyPosixSetjmpSetupFunc(Picoc *pc);
extern const char BabyPosixWcharDefs[];
extern struct LibraryFunction BabyPosixWcharFunctions[];
void BabyPosixWcharSetupFunc(Picoc *pc);
extern const char BabyPosixSendfileDefs[];
extern struct LibraryFunction BabyPosixSendfileFunctions[];
void BabyPosixSendfileSetupFunc(Picoc *pc);
#endif
