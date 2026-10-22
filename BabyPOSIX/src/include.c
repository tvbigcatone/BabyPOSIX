/* picoc include system - can emulate system includes from built-in libraries
 * or it can include and parse files if the system has files */
 
#include "picoc.h"
#include "interpreter.h"

#ifndef NO_HASH_INCLUDE

/* BabyPOSIX + libWINAPI are desktop thin layers (Linux↔Windows educational
 * compatibility). Excluded from BABYC_MICRO / minimal footprint builds. */
#ifndef BABYC_MICRO
#include "posix/posix_time.h"
#include "posix/posix_unistd.h"
#include "posix/posix_fd.h"
#include "posix/posix_dirent.h"
#include "posix/posix_pthread.h"
#include "posix/posix_stat.h"
#include "posix/posix_misc.h"
#include "posix/posix_more.h"
#include "posix/posix_cover.h"
#include "posix/posix_types.h"
#include "posix/posix_wait.h"
#include "posix/posix_pwd.h"
#include "posix/posix_signal.h"
void WinSetupFunc(Picoc *pc);
#endif

#ifdef PERSOPORT
extern int enhanced_mode ;
void IncludeExternalTools(Picoc *pc) ;
#endif


#ifdef WITH_GETOPT
extern struct LibraryFunction GetoptFunctions[];
extern const char GetoptDefs[];
void GetoptSetupFunc(Picoc *pc);
#endif

#ifdef WITH_INIH
extern struct LibraryFunction IniFunctions[];
extern const char IniDefs[];
void IniSetupFunc(Picoc *pc);
#endif

#ifdef WITH_EZXML
extern struct LibraryFunction EzxmlFunctions[];
extern const char EzxmlDefs[];
void EzxmlSetupFunc(Picoc *pc);
#endif

#ifdef WITH_SQLITE
/* SQLite3 support */
extern struct LibraryFunction SqliteFunctions[];
extern const char SqliteDefs[];
void SqliteSetupFunc(Picoc* pc);
#endif

#ifdef WITH_CSTDLIB_EXT
/* Optional desktop cstdlib extensions (stdarg.h, ctype extras) */
extern struct LibraryFunction StdargFunctions[];
void StdargSetupFunc(Picoc *pc);
extern struct LibraryFunction CtypeExtFunctions[];
extern const char CtypeExtDefs[];
void CtypeExtSetupFunc(Picoc *pc);
extern struct LibraryFunction AssertFunctions[];
extern const char AssertDefs[];
void AssertSetupFunc(Picoc *pc);
void LimitsSetupFunc(Picoc *pc);
#endif

/* initialise the built-in include libraries */

void IncludeInit(Picoc *pc)
{
#ifndef BUILTIN_MINI_STDLIB
    IncludeRegister(pc, "ctype.h", NULL, &StdCtypeFunctions[0], NULL);
#ifdef WITH_CSTDLIB_EXT
    IncludeRegister(pc, "ctype_ext.h", &CtypeExtSetupFunc,
        &CtypeExtFunctions[0], CtypeExtDefs);
#endif
    IncludeRegister(pc, "errno.h", &StdErrnoSetupFunc, NULL, NULL);
# ifndef NO_FP
    IncludeRegister(pc, "math.h", &MathSetupFunc, &MathFunctions[0], NULL);
# endif
    IncludeRegister(pc, "stdbool.h", &StdboolSetupFunc, NULL, StdboolDefs);
    IncludeRegister(pc, "stdint.h", &StdintSetupFunc, NULL, StdintDefs);
#ifdef WITH_CSTDLIB_EXT
    IncludeRegister(pc, "limits.h", &LimitsSetupFunc, NULL, NULL);
    IncludeRegister(pc, "assert.h", &AssertSetupFunc, &AssertFunctions[0], AssertDefs);
#endif
	
#ifdef PERSOPORT
	IncludeRegister(pc, "popen.h", &PopenSetupFunc, &PopenFunctions[0], PopenDefs) ;
	if( enhanced_mode ) { IncludeExternalTools(pc);	}
	IncludeRegister(pc, "stat.h", &StatSetupFunc, &StatFunctions[0], StatDefs);
	IncludeRegister(pc, "regex.h", &RegexSetupFunc, &RegexFunctions[0], RegexDefs);
	IncludeRegister(pc, "socket.h", &SocketSetupFunc, &SocketFunctions[0], SocketDefs);
	IncludeRegister(pc, "various.h", &VariousSetupFunc, &VariousFunctions[0], VariousDefs);
#endif

#ifdef WITH_SQLITE
    IncludeRegister(pc, "sqlite3.h", &SqliteSetupFunc, &SqliteFunctions[0], SqliteDefs);
#endif

#ifdef WITH_GETOPT
    IncludeRegister(pc, "getopt.h", &GetoptSetupFunc, &GetoptFunctions[0], GetoptDefs);
#endif

#ifdef WITH_INIH
    IncludeRegister(pc, "ini.h", &IniSetupFunc, &IniFunctions[0], IniDefs);
#endif

#ifdef WITH_EZXML
    IncludeRegister(pc, "ezxml.h", &EzxmlSetupFunc, &EzxmlFunctions[0], EzxmlDefs);
#endif


    IncludeRegister(pc, "stdio.h", &StdioSetupFunc, &StdioFunctions[0], StdioDefs);
#ifdef WITH_CSTDLIB_EXT
    /* Prepend after stdio so PicocIncludeAllSystemHeaders hits stdarg first
     * (list is LIFO) and va_list exists before vprintf prototypes. */
    IncludeRegister(pc, "stdarg.h", &StdargSetupFunc,
        &StdargFunctions[0], NULL);
#endif
    IncludeRegister(pc, "stdlib.h", &StdlibSetupFunc, &StdlibFunctions[0], NULL);
    IncludeRegister(pc, "string.h", &StringSetupFunc, &StringFunctions[0], NULL);
    IncludeRegister(pc, "time.h", &StdTimeSetupFunc, &StdTimeFunctions[0], StdTimeDefs);

#ifndef BABYC_MICRO
    /* ---- Desktop thin layers (educational bidirectional compatibility) ----
     * BabyPOSIX:  #include <unistd.h> / <fcntl.h> / <dirent.h> / <pthread.h>
     *             / <sys/time.h>
     *             → wrapper over native POSIX on Linux; portable subset on Windows.
     * libWINAPI:  #include <windows.h>
     *             → wrapper over native Win32 on Windows; libWINAPI on Linux.
     * There is no win.h: not an official Windows header; do not register it.
     * All Win32 guest APIs live under windows.h only (cstdlib_ext/win.c bridge).
     * Both layers are omitted from BABYC_MICRO / Makefile.minimal builds. */
    IncludeRegister(pc, "sys/types.h", &BabyPosixTypesSetupFunc,
        &BabyPosixTypesFunctions[0], BabyPosixTypesDefs);
    IncludeRegister(pc, "sys/wait.h", &BabyPosixWaitSetupFunc,
        &BabyPosixWaitFunctions[0], BabyPosixWaitDefs);
    IncludeRegister(pc, "pwd.h", &BabyPosixPwdSetupFunc,
        &BabyPosixPwdFunctions[0], BabyPosixPwdDefs);
    IncludeRegister(pc, "grp.h", &BabyPosixGrpSetupFunc,
        &BabyPosixGrpFunctions[0], BabyPosixGrpDefs);
    IncludeRegister(pc, "signal.h", &BabyPosixSignalSetupFunc,
        &BabyPosixSignalFunctions[0], BabyPosixSignalDefs);
    IncludeRegister(pc, "sys/time.h", &BabyPosixTimeSetupFunc,
        &BabyPosixTimeFunctions[0], BabyPosixTimeDefs);
    IncludeRegister(pc, "sys/times.h", &BabyPosixTimesSetupFunc,
        &BabyPosixTimesFunctions[0], BabyPosixTimesDefs);
    IncludeRegister(pc, "unistd.h", &BabyPosixUnistdSetupFunc,
        &BabyPosixUnistdFunctions[0], BabyPosixUnistdDefs);
    IncludeRegister(pc, "fcntl.h", &BabyPosixFcntlSetupFunc,
        &BabyPosixFcntlFunctions[0], BabyPosixFcntlDefs);
    IncludeRegister(pc, "dirent.h", &BabyPosixDirentSetupFunc,
        &BabyPosixDirentFunctions[0], BabyPosixDirentDefs);
    IncludeRegister(pc, "pthread.h", &BabyPosixPthreadSetupFunc,
        &BabyPosixPthreadFunctions[0], BabyPosixPthreadDefs);
    IncludeRegister(pc, "sys/stat.h", &BabyPosixStatSetupFunc,
        &BabyPosixStatFunctions[0], BabyPosixStatDefs);
#ifndef WITH_GETOPT
    IncludeRegister(pc, "getopt.h", &BabyPosixGetoptSetupFunc,
        &BabyPosixGetoptFunctions[0], BabyPosixGetoptDefs);
#endif
    IncludeRegister(pc, "libgen.h", &BabyPosixLibgenSetupFunc,
        &BabyPosixLibgenFunctions[0], BabyPosixLibgenDefs);
    IncludeRegister(pc, "utime.h", &BabyPosixUtimeSetupFunc,
        &BabyPosixUtimeFunctions[0], BabyPosixUtimeDefs);
    IncludeRegister(pc, "sys/utsname.h", &BabyPosixUtsnameSetupFunc,
        &BabyPosixUtsnameFunctions[0], BabyPosixUtsnameDefs);
    IncludeRegister(pc, "sys/ioctl.h", &BabyPosixIoctlSetupFunc,
        &BabyPosixIoctlFunctions[0], BabyPosixIoctlDefs);
    IncludeRegister(pc, "termios.h", &BabyPosixTermiosSetupFunc,
        &BabyPosixTermiosFunctions[0], BabyPosixTermiosDefs);
    IncludeRegister(pc, "sys/select.h", &BabyPosixSelectSetupFunc,
        &BabyPosixSelectFunctions[0], BabyPosixSelectDefs);
    IncludeRegister(pc, "poll.h", &BabyPosixPollSetupFunc,
        &BabyPosixPollFunctions[0], BabyPosixPollDefs);
    IncludeRegister(pc, "sys/resource.h", &BabyPosixResourceSetupFunc,
        &BabyPosixResourceFunctions[0], BabyPosixResourceDefs);
    IncludeRegister(pc, "strings.h", &BabyPosixStringsSetupFunc,
        &BabyPosixStringsFunctions[0], BabyPosixStringsDefs);
    IncludeRegister(pc, "locale.h", &BabyPosixLocaleSetupFunc,
        &BabyPosixLocaleFunctions[0], BabyPosixLocaleDefs);
    IncludeRegister(pc, "inttypes.h", &BabyPosixInttypesSetupFunc,
        &BabyPosixInttypesFunctions[0], BabyPosixInttypesDefs);
    IncludeRegister(pc, "stddef.h", &BabyPosixStddefSetupFunc,
        &BabyPosixStddefFunctions[0], BabyPosixStddefDefs);
    IncludeRegister(pc, "sys/param.h", &BabyPosixParamSetupFunc,
        &BabyPosixParamFunctions[0], BabyPosixParamDefs);
    IncludeRegister(pc, "sys/sysmacros.h", &BabyPosixSysmacrosSetupFunc,
        &BabyPosixSysmacrosFunctions[0], BabyPosixSysmacrosDefs);
    IncludeRegister(pc, "sys/file.h", &BabyPosixFileSetupFunc,
        &BabyPosixFileFunctions[0], BabyPosixFileDefs);
    IncludeRegister(pc, "fnmatch.h", &BabyPosixFnmatchSetupFunc,
        &BabyPosixFnmatchFunctions[0], BabyPosixFnmatchDefs);
    IncludeRegister(pc, "sched.h", &BabyPosixSchedSetupFunc,
        &BabyPosixSchedFunctions[0], BabyPosixSchedDefs);
    IncludeRegister(pc, "paths.h", &BabyPosixPathsSetupFunc,
        &BabyPosixPathsFunctions[0], BabyPosixPathsDefs);
    IncludeRegister(pc, "sysexits.h", &BabyPosixSysexitsSetupFunc,
        &BabyPosixSysexitsFunctions[0], BabyPosixSysexitsDefs);
    IncludeRegister(pc, "langinfo.h", &BabyPosixLanginfoSetupFunc,
        &BabyPosixLanginfoFunctions[0], BabyPosixLanginfoDefs);
    IncludeRegister(pc, "syslog.h", &BabyPosixSyslogSetupFunc,
        &BabyPosixSyslogFunctions[0], BabyPosixSyslogDefs);
    IncludeRegister(pc, "glob.h", &BabyPosixGlobSetupFunc,
        &BabyPosixGlobFunctions[0], BabyPosixGlobDefs);
    IncludeRegister(pc, "sys/mman.h", &BabyPosixMmanSetupFunc,
        &BabyPosixMmanFunctions[0], BabyPosixMmanDefs);
    IncludeRegister(pc, "sys/uio.h", &BabyPosixUioSetupFunc,
        &BabyPosixUioFunctions[0], BabyPosixUioDefs);
    IncludeRegister(pc, "sys/statvfs.h", &BabyPosixStatvfsSetupFunc,
        &BabyPosixStatvfsFunctions[0], BabyPosixStatvfsDefs);
    IncludeRegister(pc, "sys/vfs.h", NULL, NULL, "#include <sys/statvfs.h>\n");
    IncludeRegister(pc, "setjmp.h", &BabyPosixSetjmpSetupFunc,
        &BabyPosixSetjmpFunctions[0], BabyPosixSetjmpDefs);
    IncludeRegister(pc, "wchar.h", &BabyPosixWcharSetupFunc,
        &BabyPosixWcharFunctions[0], BabyPosixWcharDefs);
    IncludeRegister(pc, "wctype.h", NULL, NULL, "#include <wchar.h>\n");
    IncludeRegister(pc, "sys/sendfile.h", &BabyPosixSendfileSetupFunc,
        &BabyPosixSendfileFunctions[0], BabyPosixSendfileDefs);

    IncludeRegister(pc, "windows.h", &WinSetupFunc, NULL, NULL);
#endif

#ifdef PERSOPORT
    IncludeRegister(pc, "types.h", &TypesSetupFunc, &TypesFunctions[0], TypesDefs);
#endif

#endif
}

/* clean up space used by the include system */
void IncludeCleanup(Picoc *pc)
{
    struct IncludeLibrary *ThisInclude = pc->IncludeLibList;
    struct IncludeLibrary *NextInclude;
    
    while (ThisInclude != NULL)
    {
        NextInclude = ThisInclude->NextLib;
        HeapFreeMem(pc, ThisInclude);
        ThisInclude = NextInclude;
    }

    pc->IncludeLibList = NULL;
}

/* register a new build-in include file */
void IncludeRegister(Picoc *pc, const char *IncludeName, void (*SetupFunction)(Picoc *pc), struct LibraryFunction *FuncList, const char *SetupCSource)
{
    struct IncludeLibrary *NewLib = HeapAllocMem(pc, sizeof(struct IncludeLibrary));
    NewLib->IncludeName = TableStrRegister(pc, IncludeName);
    NewLib->SetupFunction = SetupFunction;
    NewLib->FuncList = FuncList;
    NewLib->SetupCSource = SetupCSource;
    NewLib->NextLib = pc->IncludeLibList;
    pc->IncludeLibList = NewLib;
}

/* include all of the system headers */
void PicocIncludeAllSystemHeaders(Picoc *pc)
{
    struct IncludeLibrary *ThisInclude = pc->IncludeLibList;
    
    for (; ThisInclude != NULL; ThisInclude = ThisInclude->NextLib)
        IncludeFile(pc, ThisInclude->IncludeName, NULL);
}

/* include one of a number of predefined libraries, or perhaps an actual file */
void IncludeFile(Picoc *pc, char *FileName, struct ParseState *Parser)
{
    struct IncludeLibrary *LInclude;
    
    /* scan for the include file name to see if it's in our list of predefined includes */
    for (LInclude = pc->IncludeLibList; LInclude != NULL; LInclude = LInclude->NextLib)
    {
        if (strcmp(LInclude->IncludeName, FileName) == 0)
        {
            /* found it - protect against multiple inclusion */
            if (!VariableDefined(pc, FileName))
            {
                VariableDefine(pc, NULL, FileName, NULL, &pc->VoidType, FALSE);
                
                /* run an extra startup function if there is one */
                if (LInclude->SetupFunction != NULL)
                    (*LInclude->SetupFunction)(pc);
                
                /* parse the setup C source code - may define types etc. */
                if (LInclude->SetupCSource != NULL)
                    PicocParse(pc, FileName, LInclude->SetupCSource, strlen(LInclude->SetupCSource), TRUE, TRUE, FALSE, FALSE);
                
                /* set up the library functions */
                if (LInclude->FuncList != NULL)
                    LibraryAdd(pc, &pc->GlobalTable, FileName, LInclude->FuncList);
            }
            
            return;
        }
    }
    
    /* not a predefined library — try a real file; on failure report like other
     * parse errors (source line + caret), not a bare ProgramFailNoParser. */
    {
        FILE *Probe = fopen(FileName, "r");
        if (Probe != NULL)
        {
            fclose(Probe);
            PicocPlatformScanFile(pc, FileName);
            return;
        }
    }
    if (Parser != NULL)
    {
        if (FileName != NULL && FileName[0] != '\0')
            ParserSetCaretOnIdent(Parser, FileName);
        ProgramFail(Parser, "can't read file %s", FileName ? FileName : "?");
    }
    ProgramFailNoParser(pc, "can't read file %s\n", FileName ? FileName : "?");
}

#endif /* NO_HASH_INCLUDE */
