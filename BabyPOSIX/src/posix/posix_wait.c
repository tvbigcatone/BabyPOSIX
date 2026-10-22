/* BabyPOSIX: sys/wait.h — wait/waitpid with guest-stable status macros. */
#include "posix_wait.h"
#include "../platform/guest_thread.h"
#include <errno.h>
#ifdef WIN32
# include <process.h>
#else
# include <sys/types.h>
# include <sys/wait.h>
#endif

static void BabyPosixWaitCall(struct ParseState *p, struct Value *r,
                              struct Value **a, int n)
{
    int *status = (int *)a[0]->Val->Pointer;
    int gil;
    (void)p; (void)n;
#ifdef WIN32
    errno = ENOSYS; r->Val->Integer = -1; return;
#else
    gil = BabycGuestGIL_UnlockIfHeld();
    r->Val->Integer = wait(status);
    BabycGuestGIL_RelockIf(gil);
#endif
}

static void BabyPosixWaitpidCall(struct ParseState *p, struct Value *r,
                                 struct Value **a, int n)
{
    int pid = (int)a[0]->Val->Integer;
    int *status = (int *)a[1]->Val->Pointer;
    int options = (int)a[2]->Val->Integer;
    int gil;
    (void)p; (void)n;
#ifdef WIN32
    errno = ENOSYS; r->Val->Integer = -1; return;
#else
    gil = BabycGuestGIL_UnlockIfHeld();
    r->Val->Integer = waitpid((pid_t)pid, status, options);
    BabycGuestGIL_RelockIf(gil);
#endif
}

const char BabyPosixWaitDefs[] =
    "#ifndef _BABYPOSIX_PID_T\n"
    "#define _BABYPOSIX_PID_T\n"
    "typedef int pid_t;\n"
    "#endif\n"
    "#define WNOHANG 1\n"
    "#define WUNTRACED 2\n"
    "#define WIFEXITED(s) (((s) & 0x7f) == 0)\n"
    "#define WEXITSTATUS(s) (((s) >> 8) & 0xff)\n"
    "#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)\n"
    "#define WTERMSIG(s) ((s) & 0x7f)\n"
    "#define WIFSTOPPED(s) (((s) & 0xff) == 0x7f)\n"
    "#define WSTOPSIG(s) (((s) >> 8) & 0xff)\n"
    "#define WCOREDUMP(s) (((s) & 0x80) != 0)\n";

struct LibraryFunction BabyPosixWaitFunctions[] = {
    { BabyPosixWaitCall, "pid_t wait(int *);" },
    { BabyPosixWaitpidCall, "pid_t waitpid(pid_t, int *, int);" },
    { NULL, NULL }
};

void BabyPosixWaitSetupFunc(Picoc *pc) { (void)pc; }
