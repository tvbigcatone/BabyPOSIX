/* BabyPOSIX: signal.h — kill/raise/alarm/pause; signal() is SIG_DFL/IGN only. */
#include "posix_signal.h"
#include "../platform/guest_thread.h"
#include <errno.h>
#ifdef WIN32
# include <signal.h>
# include <process.h>
#else
# include <signal.h>
# include <unistd.h>
#endif

static void BabyPosixKillCall(struct ParseState *p, struct Value *r,
                              struct Value **a, int n)
{
    (void)p; (void)n;
#ifdef WIN32
    errno = ENOSYS; r->Val->Integer = -1;
#else
    r->Val->Integer = kill((pid_t)a[0]->Val->Integer, (int)a[1]->Val->Integer);
#endif
}

static void BabyPosixRaiseCall(struct ParseState *p, struct Value *r,
                               struct Value **a, int n)
{
    (void)p; (void)n;
    r->Val->Integer = raise((int)a[0]->Val->Integer);
}

static void BabyPosixAlarmCall(struct ParseState *p, struct Value *r,
                               struct Value **a, int n)
{
    (void)p; (void)n;
#ifdef WIN32
    errno = ENOSYS; r->Val->Integer = 0;
#else
    r->Val->Integer = (int)alarm((unsigned int)a[0]->Val->Integer);
#endif
}

static void BabyPosixPauseCall(struct ParseState *p, struct Value *r,
                               struct Value **a, int n)
{
    int gil;
    (void)p; (void)n; (void)a;
#ifdef WIN32
    errno = ENOSYS; r->Val->Integer = -1;
#else
    gil = BabycGuestGIL_UnlockIfHeld();
    r->Val->Integer = pause();
    BabycGuestGIL_RelockIf(gil);
#endif
}

static void BabyPosixSignalCall(struct ParseState *p, struct Value *r,
                                struct Value **a, int n)
{
    int sig = (int)a[0]->Val->Integer;
    void *handler = a[1]->Val->Pointer;
    void *prev;
    (void)p; (void)n;
    /* Educational subset: only SIG_DFL (0) and SIG_IGN (1). Guest function
     * pointers are not installed as host signal handlers. */
    if (handler != (void *)0 && handler != (void *)1) {
        errno = ENOSYS;
        r->Val->Pointer = (void *)-1;
        return;
    }
    prev = (void *)signal(sig, handler == (void *)1 ? SIG_IGN : SIG_DFL);
    r->Val->Pointer = (prev == SIG_IGN) ? (void *)1 : (void *)0;
}

const char BabyPosixSignalDefs[] =
    "typedef void (*sighandler_t)(int);\n"
    "#define SIG_DFL ((sighandler_t)0)\n"
    "#define SIG_IGN ((sighandler_t)1)\n"
    "#define SIG_ERR ((sighandler_t)-1)\n"
    "#define SIGHUP 1\n"
    "#define SIGINT 2\n"
    "#define SIGQUIT 3\n"
    "#define SIGILL 4\n"
    "#define SIGTRAP 5\n"
    "#define SIGABRT 6\n"
    "#define SIGBUS 7\n"
    "#define SIGFPE 8\n"
    "#define SIGKILL 9\n"
    "#define SIGUSR1 10\n"
    "#define SIGSEGV 11\n"
    "#define SIGUSR2 12\n"
    "#define SIGPIPE 13\n"
    "#define SIGALRM 14\n"
    "#define SIGTERM 15\n"
    "#define SIGCHLD 17\n"
    "#define SIGCONT 18\n"
    "#define SIGSTOP 19\n"
    "#define SIGTSTP 20\n"
    "#define SIGTTIN 21\n"
    "#define SIGTTOU 22\n"
    "#define SIGWINCH 28\n";

struct LibraryFunction BabyPosixSignalFunctions[] = {
    { BabyPosixKillCall, "int kill(int, int);" },
    { BabyPosixRaiseCall, "int raise(int);" },
    { BabyPosixAlarmCall, "unsigned int alarm(unsigned int);" },
    { BabyPosixPauseCall, "int pause(void);" },
    { BabyPosixSignalCall, "void *signal(int, void *);" },
    { NULL, NULL }
};

void BabyPosixSignalSetupFunc(Picoc *pc) { (void)pc; }
