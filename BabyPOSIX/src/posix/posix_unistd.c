/* BabyPOSIX: deliberately small cross-platform unistd.h subset.
 * Host APIs are selected here; interpreted declarations live in one place.
 */
#include "posix_unistd.h"
#include "posix_fd.h"
#include "../platform/guest_thread.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#ifndef WIN32
# include <time.h>
#endif

#ifdef WIN32
# include <windows.h>
# include <direct.h>
# include <io.h>
# include <process.h>
# include <stdlib.h>
# define babyposix_native_chdir _chdir
# define babyposix_native_getcwd _getcwd
# define babyposix_native_access _access
# define babyposix_native_unlink _unlink
# define babyposix_native_rmdir _rmdir
# define babyposix_native_getpid _getpid
#else
# include <unistd.h>
# include <sys/types.h>
# include <fcntl.h>
# include <stdlib.h>
# include <limits.h>
# define babyposix_native_chdir chdir
# define babyposix_native_getcwd getcwd
# define babyposix_native_access access
# define babyposix_native_unlink unlink
# define babyposix_native_rmdir rmdir
# define babyposix_native_getpid getpid
#endif

static void BabyPosixAccessCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        ReturnValue->Val->Integer = -1;
        errno = EFAULT;
        return;
    }
#ifdef WIN32
    {
        int mode = (int)Param[1]->Val->Integer;
        /* Windows has no POSIX executable permission bit. X_OK is therefore
         * approximated as an existence check; this limitation is documented. */
        if (mode == 1) mode = 0;
        ReturnValue->Val->Integer = babyposix_native_access(
            (const char *)Param[0]->Val->Pointer, mode);
    }
#else
    ReturnValue->Val->Integer = babyposix_native_access(
        (const char *)Param[0]->Val->Pointer, (int)Param[1]->Val->Integer);
#endif
}

static void BabyPosixEaccessCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        ReturnValue->Val->Integer = -1;
        errno = EFAULT;
        return;
    }
#ifdef WIN32
    ReturnValue->Val->Integer = babyposix_native_access(
        (const char *)Param[0]->Val->Pointer, (int)Param[1]->Val->Integer);
#else
# ifdef _GNU_SOURCE
    ReturnValue->Val->Integer = eaccess(
        (const char *)Param[0]->Val->Pointer, (int)Param[1]->Val->Integer);
# else
    ReturnValue->Val->Integer = access(
        (const char *)Param[0]->Val->Pointer, (int)Param[1]->Val->Integer);
# endif
#endif
}

static void BabyPosixConfstrCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int name = (int)Param[0]->Val->Integer;
    char *buf = (char *)Param[1]->Val->Pointer;
    unsigned int len = (unsigned int)Param[2]->Val->Integer;
    const char *val;
    size_t n;
    (void)Parser; (void)NumArgs;
    if (name == 0)
        val = "/bin:/usr/bin";
    else {
        ReturnValue->Val->Integer = 0;
        return;
    }
    n = strlen(val) + 1;
    if (buf != NULL && len > 0) {
        if (len >= n)
            memcpy(buf, val, n);
        else {
            memcpy(buf, val, len - 1);
            buf[len - 1] = '\0';
        }
    }
    ReturnValue->Val->Integer = (int)n;
}

static void BabyPosixChdirCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        ReturnValue->Val->Integer = -1;
        errno = EFAULT;
        return;
    }
    ReturnValue->Val->Integer = babyposix_native_chdir(
        (const char *)Param[0]->Val->Pointer);
}

static void BabyPosixGetcwdCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        ReturnValue->Val->Pointer = NULL;
        errno = EFAULT;
        return;
    }
    if (Param[1]->Val->Integer <= 0) {
        ReturnValue->Val->Pointer = NULL;
        errno = EINVAL;
        return;
    }
    ReturnValue->Val->Pointer = babyposix_native_getcwd(
        (char *)Param[0]->Val->Pointer, (size_t)Param[1]->Val->Integer);
}

static void BabyPosixGetpidCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)Param; (void)NumArgs;
    ReturnValue->Val->Integer = (long)babyposix_native_getpid();
}

static void BabyPosixSleepCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    unsigned long seconds = (unsigned long)Param[0]->Val->Integer;
    int gil;
    (void)Parser; (void)NumArgs;
    gil = BabycGuestGIL_UnlockIfHeld();
#ifdef WIN32
    /* Sleep accepts milliseconds and is limited to DWORD. Split long waits. */
    while (seconds > 0) {
        DWORD chunk = seconds > (unsigned long)(INFINITE - 1) / 1000UL
            ? INFINITE - 1 : (DWORD)(seconds * 1000UL);
        Sleep(chunk);
        seconds -= (unsigned long)(chunk / 1000UL);
        if (chunk == 0) break;
    }
    ReturnValue->Val->Integer = 0;
#else
    ReturnValue->Val->Integer = (long)sleep((unsigned int)seconds);
#endif
    BabycGuestGIL_RelockIf(gil);
}

static void BabyPosixUsleepCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    long long usec = (long long)Param[0]->Val->Integer;
    (void)Parser; (void)NumArgs;
    if (usec < 0 || usec >= 1000000LL) {
        ReturnValue->Val->Integer = -1;
        errno = EINVAL;
        return;
    }
    {
        int gil = BabycGuestGIL_UnlockIfHeld();
#ifdef WIN32
        if (usec > 0) {
            /* Windows Sleep has millisecond resolution; round up rather than
             * returning earlier than requested. */
            DWORD ms = (DWORD)((usec + 999LL) / 1000LL);
            Sleep(ms);
        }
        ReturnValue->Val->Integer = 0;
#else
        {
            struct timespec req;
            req.tv_sec = 0;
            req.tv_nsec = (long)(usec * 1000LL);
            ReturnValue->Val->Integer = nanosleep(&req, NULL);
        }
#endif
        BabycGuestGIL_RelockIf(gil);
    }
}

static void BabyPosixUnlinkCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        ReturnValue->Val->Integer = -1;
        errno = EFAULT;
        return;
    }
    ReturnValue->Val->Integer = babyposix_native_unlink(
        (const char *)Param[0]->Val->Pointer);
}

static void BabyPosixRmdirCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        ReturnValue->Val->Integer = -1;
        errno = EFAULT;
        return;
    }
    ReturnValue->Val->Integer = babyposix_native_rmdir(
        (const char *)Param[0]->Val->Pointer);
}

static int BabyPosixDirFd(int dirfd)
{
    if (dirfd == -100) {
#ifdef AT_FDCWD
        return AT_FDCWD;
#else
        return -100;
#endif
    }
    return BabyPosixFdNativeOf(dirfd);
}

static void BabyPosixUnlinkatCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[1]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    int native_dir;
    native_dir = BabyPosixDirFd((int)Param[0]->Val->Integer);
    if (native_dir < 0 && (int)Param[0]->Val->Integer != -100) {
        ReturnValue->Val->Integer = -1; return;
    }
    ReturnValue->Val->Integer = unlinkat(native_dir,
        (const char *)Param[1]->Val->Pointer, (int)Param[2]->Val->Integer);
#endif
}

static void BabyPosixFaccessatCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[1]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    int native_dir;
    native_dir = BabyPosixDirFd((int)Param[0]->Val->Integer);
    if (native_dir < 0 && (int)Param[0]->Val->Integer != -100) {
        ReturnValue->Val->Integer = -1; return;
    }
    ReturnValue->Val->Integer = faccessat(native_dir,
        (const char *)Param[1]->Val->Pointer, (int)Param[2]->Val->Integer,
        (int)Param[3]->Val->Integer);
#endif
}

static void BabyPosixReadlinkatCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[1]->Val->Pointer == NULL ||
        (Param[3]->Val->Integer > 0 && Param[2]->Val->Pointer == NULL)) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    int native_dir;
    native_dir = BabyPosixDirFd((int)Param[0]->Val->Integer);
    if (native_dir < 0 && (int)Param[0]->Val->Integer != -100) {
        ReturnValue->Val->Integer = -1; return;
    }
    ReturnValue->Val->Integer = (long)readlinkat(native_dir,
        (const char *)Param[1]->Val->Pointer, (char *)Param[2]->Val->Pointer,
        (size_t)Param[3]->Val->Integer);
#endif
}

static void BabyPosixSymlinkatCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL || Param[2]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    int native_dir;
    native_dir = BabyPosixDirFd((int)Param[1]->Val->Integer);
    if (native_dir < 0 && (int)Param[1]->Val->Integer != -100) {
        ReturnValue->Val->Integer = -1; return;
    }
    ReturnValue->Val->Integer = symlinkat((const char *)Param[0]->Val->Pointer,
        native_dir, (const char *)Param[2]->Val->Pointer);
#endif
}

static void BabyPosixLinkatCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[1]->Val->Pointer == NULL || Param[3]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    int olddir, newdir;
    olddir = BabyPosixDirFd((int)Param[0]->Val->Integer);
    newdir = BabyPosixDirFd((int)Param[2]->Val->Integer);
    if ((olddir < 0 && (int)Param[0]->Val->Integer != -100) ||
        (newdir < 0 && (int)Param[2]->Val->Integer != -100)) {
        ReturnValue->Val->Integer = -1; return;
    }
    ReturnValue->Val->Integer = linkat(olddir,
        (const char *)Param[1]->Val->Pointer, newdir,
        (const char *)Param[3]->Val->Pointer, (int)Param[4]->Val->Integer);
#endif
}

static void BabyPosixSetenvCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        errno = EINVAL; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = setenv((const char *)Param[0]->Val->Pointer,
        Param[1]->Val->Pointer ? (const char *)Param[1]->Val->Pointer : "",
        (int)Param[2]->Val->Integer);
#endif
}

static void BabyPosixUnsetenvCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        errno = EINVAL; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = unsetenv((const char *)Param[0]->Val->Pointer);
#endif
}

static void BabyPosixPutenvCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        errno = EINVAL; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = putenv((char *)Param[0]->Val->Pointer);
#endif
}

static void BabyPosixRenameatCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[1]->Val->Pointer == NULL || Param[3]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    int olddir, newdir;
    olddir = BabyPosixDirFd((int)Param[0]->Val->Integer);
    newdir = BabyPosixDirFd((int)Param[2]->Val->Integer);
    if ((olddir < 0 && (int)Param[0]->Val->Integer != -100) ||
        (newdir < 0 && (int)Param[2]->Val->Integer != -100)) {
        ReturnValue->Val->Integer = -1; return;
    }
    ReturnValue->Val->Integer = renameat(olddir,
        (const char *)Param[1]->Val->Pointer, newdir,
        (const char *)Param[3]->Val->Pointer);
#endif
}

static void BabyPosixGetppidCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)Param; (void)NumArgs;
#ifdef WIN32
    ReturnValue->Val->Integer = 0;
#else
    ReturnValue->Val->Integer = (long)getppid();
#endif
}

static void BabyPosixGetuidCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)Param; (void)NumArgs;
#ifdef WIN32
    ReturnValue->Val->Integer = 0;
#else
    ReturnValue->Val->Integer = (long)getuid();
#endif
}

static void BabyPosixGeteuidCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)Param; (void)NumArgs;
#ifdef WIN32
    ReturnValue->Val->Integer = 0;
#else
    ReturnValue->Val->Integer = (long)geteuid();
#endif
}

static void BabyPosixGetgidCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)Param; (void)NumArgs;
#ifdef WIN32
    ReturnValue->Val->Integer = 0;
#else
    ReturnValue->Val->Integer = (long)getgid();
#endif
}

static void BabyPosixGetegidCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)Param; (void)NumArgs;
#ifdef WIN32
    ReturnValue->Val->Integer = 0;
#else
    ReturnValue->Val->Integer = (long)getegid();
#endif
}

static void BabyPosixSetuidCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = setuid((uid_t)Param[0]->Val->Integer);
#endif
}

static void BabyPosixSetgidCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = setgid((gid_t)Param[0]->Val->Integer);
#endif
}

static void BabyPosixSetsidCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)Param; (void)NumArgs;
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = (long)setsid();
#endif
}

static void BabyPosixExecveCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = execve((const char *)Param[0]->Val->Pointer,
        (char *const *)Param[1]->Val->Pointer,
        (char *const *)Param[2]->Val->Pointer);
#endif
}

static void BabyPosixIsattyCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int native;
    (void)Parser; (void)NumArgs;
    native = BabyPosixFdNativeOf((int)Param[0]->Val->Integer);
    if (native < 0) { ReturnValue->Val->Integer = 0; return; }
#ifdef WIN32
    ReturnValue->Val->Integer = _isatty(native);
#else
    ReturnValue->Val->Integer = isatty(native);
#endif
}

static void BabyPosixTtynameCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int native;
    (void)Parser; (void)NumArgs;
    native = BabyPosixFdNativeOf((int)Param[0]->Val->Integer);
    if (native < 0) { ReturnValue->Val->Pointer = NULL; return; }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Pointer = NULL;
#else
    ReturnValue->Val->Pointer = ttyname(native);
#endif
}

static void BabyPosixLinkCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL || Param[1]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = link((const char *)Param[0]->Val->Pointer,
        (const char *)Param[1]->Val->Pointer);
#endif
}

static void BabyPosixSymlinkCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL || Param[1]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = symlink((const char *)Param[0]->Val->Pointer,
        (const char *)Param[1]->Val->Pointer);
#endif
}

static void BabyPosixReadlinkCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL ||
        (Param[2]->Val->Integer > 0 && Param[1]->Val->Pointer == NULL)) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
    if (Param[2]->Val->Integer < 0) {
        errno = EINVAL; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = (long)readlink((const char *)Param[0]->Val->Pointer,
        (char *)Param[1]->Val->Pointer, (size_t)Param[2]->Val->Integer);
#endif
}

static void BabyPosixFsyncCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int native;
    (void)Parser; (void)NumArgs;
    native = BabyPosixFdNativeOf((int)Param[0]->Val->Integer);
    if (native < 0) { ReturnValue->Val->Integer = -1; return; }
#ifdef WIN32
    ReturnValue->Val->Integer = _commit(native);
#else
    ReturnValue->Val->Integer = fsync(native);
#endif
}

static void BabyPosixFtruncateCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int native;
    (void)Parser; (void)NumArgs;
    native = BabyPosixFdNativeOf((int)Param[0]->Val->Integer);
    if (native < 0) { ReturnValue->Val->Integer = -1; return; }
#ifdef WIN32
    ReturnValue->Val->Integer = _chsize(native, (long)Param[1]->Val->Integer);
#else
    ReturnValue->Val->Integer = ftruncate(native, (off_t)Param[1]->Val->Integer);
#endif
}

static void BabyPosixTruncateCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = truncate((const char *)Param[0]->Val->Pointer,
        (off_t)Param[1]->Val->Integer);
#endif
}

static void BabyPosixFchdirCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int native;
    (void)Parser; (void)NumArgs;
    native = BabyPosixFdNativeOf((int)Param[0]->Val->Integer);
    if (native < 0) { ReturnValue->Val->Integer = -1; return; }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = fchdir(native);
#endif
}

static void BabyPosixChownCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = chown((const char *)Param[0]->Val->Pointer,
        (uid_t)Param[1]->Val->Integer, (gid_t)Param[2]->Val->Integer);
#endif
}

static void BabyPosixLchownCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = lchown((const char *)Param[0]->Val->Pointer,
        (uid_t)Param[1]->Val->Integer, (gid_t)Param[2]->Val->Integer);
#endif
}

static void BabyPosixFchownCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int native;
    (void)Parser; (void)NumArgs;
    native = BabyPosixFdNativeOf((int)Param[0]->Val->Integer);
    if (native < 0) { ReturnValue->Val->Integer = -1; return; }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = fchown(native, (uid_t)Param[1]->Val->Integer,
        (gid_t)Param[2]->Val->Integer);
#endif
}

static void BabyPosixExitUnderscoreCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)ReturnValue; (void)NumArgs;
    (void)BabycGuestGIL_UnlockIfHeld();
#ifdef WIN32
    _exit((int)Param[0]->Val->Integer);
#else
    _exit((int)Param[0]->Val->Integer);
#endif
}

static void BabyPosixForkCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)Param; (void)NumArgs;
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    int gil;
    gil = BabycGuestGIL_UnlockIfHeld();
    ReturnValue->Val->Integer = (long)fork();
    if (ReturnValue->Val->Integer != 0)
        BabycGuestGIL_RelockIf(gil);
    else
        BabycGuestGIL_RelockIf(gil);
#endif
}

static void BabyPosixExecvCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = execv((const char *)Param[0]->Val->Pointer,
        (char *const *)Param[1]->Val->Pointer);
#endif
}

static void BabyPosixExecvpCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = execvp((const char *)Param[0]->Val->Pointer,
        (char *const *)Param[1]->Val->Pointer);
#endif
}

static void BabyPosixGetpagesizeCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)Param; (void)NumArgs;
#ifdef WIN32
    ReturnValue->Val->Integer = 4096;
#else
    ReturnValue->Val->Integer = getpagesize();
#endif
}

static void BabyPosixSysconfCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = (long)sysconf((int)Param[0]->Val->Integer);
#endif
}

static void BabyPosixGethostnameCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
    if (Param[1]->Val->Integer <= 0) {
        errno = EINVAL; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = gethostname((char *)Param[0]->Val->Pointer,
        (size_t)Param[1]->Val->Integer);
#endif
}

static void BabyPosixGetloginCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)Param; (void)NumArgs;
#ifdef WIN32
    ReturnValue->Val->Pointer = getenv("USERNAME");
#else
    ReturnValue->Val->Pointer = getlogin();
#endif
}

static void BabyPosixMkstempCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    char *tmpl = (char *)Param[0]->Val->Pointer;
    (void)Parser; (void)NumArgs;
    if (tmpl == NULL) { errno = EFAULT; ReturnValue->Val->Integer = -1; return; }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1; return;
#else
    int native, guest, access;
    native = mkstemp(tmpl);
    if (native < 0) { ReturnValue->Val->Integer = -1; return; }
    access = BP_FD_READ | BP_FD_WRITE;
    guest = BabyPosixFdAllocate(native, access, 1);
    if (guest < 0) {
        int saved = errno;
        close(native);
        errno = saved;
        ReturnValue->Val->Integer = -1;
        return;
    }
    ReturnValue->Val->Integer = guest;
#endif
}

static void BabyPosixMkdtempCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    char *tmpl = (char *)Param[0]->Val->Pointer;
    (void)Parser; (void)NumArgs;
    if (tmpl == NULL) { errno = EFAULT; ReturnValue->Val->Pointer = NULL; return; }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Pointer = NULL;
#else
    ReturnValue->Val->Pointer = mkdtemp(tmpl);
#endif
}

static void BabyPosixRealpathCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Pointer = NULL; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Pointer = NULL;
#else
    ReturnValue->Val->Pointer = realpath((const char *)Param[0]->Val->Pointer,
        (char *)Param[1]->Val->Pointer);
#endif
}

static void BabyPosixSyncCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)Param; (void)NumArgs; (void)ReturnValue;
#ifndef WIN32
    sync();
#endif
}

static void BabyPosixFdatasyncCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int native;
    (void)Parser; (void)NumArgs;
    native = BabyPosixFdNativeOf((int)Param[0]->Val->Integer);
    if (native < 0) { ReturnValue->Val->Integer = -1; return; }
#ifdef WIN32
    ReturnValue->Val->Integer = _commit(native);
#else
    ReturnValue->Val->Integer = fdatasync(native);
#endif
}

static void BabyPosixGetpgrpCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)Param; (void)NumArgs;
#ifdef WIN32
    ReturnValue->Val->Integer = (long)babyposix_native_getpid();
#else
    ReturnValue->Val->Integer = (long)getpgrp();
#endif
}

static void BabyPosixGetpgidCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
#ifdef WIN32
    (void)Param;
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = (long)getpgid((pid_t)Param[0]->Val->Integer);
#endif
}

static void BabyPosixSetpgidCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
#ifdef WIN32
    (void)Param;
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = setpgid((pid_t)Param[0]->Val->Integer,
        (pid_t)Param[1]->Val->Integer);
#endif
}

static void BabyPosixGetgroupsCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int size = (int)Param[0]->Val->Integer;
    int *list = (int *)Param[1]->Val->Pointer;
    (void)Parser; (void)NumArgs;
    if (size < 0) {
        errno = EINVAL; ReturnValue->Val->Integer = -1; return;
    }
    if (size > 0 && list == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    if (size == 0) { ReturnValue->Val->Integer = 1; return; }
    list[0] = 0;
    ReturnValue->Val->Integer = 1;
#else
    ReturnValue->Val->Integer = getgroups(size, (gid_t *)list);
#endif
}

static void BabyPosixNiceCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
#ifdef WIN32
    (void)Param;
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = nice((int)Param[0]->Val->Integer);
#endif
}

static void BabyPosixPathconfCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    if (Param[0]->Val->Pointer == NULL) {
        errno = EFAULT; ReturnValue->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = (long)pathconf((const char *)Param[0]->Val->Pointer,
        (int)Param[1]->Val->Integer);
#endif
}

static void BabyPosixGetdtablesizeCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)Param; (void)NumArgs;
    ReturnValue->Val->Integer = BABYPOSIX_FD_CAPACITY;
}

static void BabyPosixFpathconfCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int native;
    (void)Parser; (void)NumArgs;
    native = BabyPosixFdNativeOf((int)Param[0]->Val->Integer);
    if (native < 0) { ReturnValue->Val->Integer = -1; return; }
#ifdef WIN32
    errno = ENOSYS; ReturnValue->Val->Integer = -1;
#else
    ReturnValue->Val->Integer = (long)fpathconf(native, (int)Param[1]->Val->Integer);
#endif
}

const char BabyPosixUnistdDefs[] =
    "#ifndef _BABYPOSIX_PID_T\n"
    "#define _BABYPOSIX_PID_T\n"
    "typedef int pid_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_OFF_T\n"
    "#define _BABYPOSIX_OFF_T\n"
    "typedef int off_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_UID_T\n"
    "#define _BABYPOSIX_UID_T\n"
    "typedef int uid_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_GID_T\n"
    "#define _BABYPOSIX_GID_T\n"
    "typedef int gid_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_USECONDS_T\n"
    "#define _BABYPOSIX_USECONDS_T\n"
    "typedef unsigned int useconds_t;\n"
    "#endif\n"
    "#define F_OK 0\n"
    "#define X_OK 1\n"
    "#define W_OK 2\n"
    "#define R_OK 4\n"
    "#define _CS_PATH 0\n"
    "#define STDIN_FILENO 0\n"
    "#define STDOUT_FILENO 1\n"
    "#define STDERR_FILENO 2\n"
    "#ifndef SEEK_SET\n"
    "#define SEEK_SET 0\n"
    "#endif\n"
    "#ifndef SEEK_CUR\n"
    "#define SEEK_CUR 1\n"
    "#endif\n"
    "#ifndef SEEK_END\n"
    "#define SEEK_END 2\n"
    "#endif\n"
    "#ifndef PATH_MAX\n"
    "#define PATH_MAX 4096\n"
    "#endif\n"
    "#define _SC_PAGESIZE 30\n"
    "#define _SC_PAGE_SIZE 30\n"
    "#define _SC_CLK_TCK 2\n"
    "#define _SC_ARG_MAX 0\n"
    "#define _SC_OPEN_MAX 4\n"
    "#define _SC_NPROCESSORS_ONLN 84\n"
    "#define _PC_LINK_MAX 0\n"
    "#define _PC_MAX_CANON 1\n"
    "#define _PC_MAX_INPUT 2\n"
    "#define _PC_NAME_MAX 3\n"
    "#define _PC_PATH_MAX 4\n"
    "#define _PC_PIPE_BUF 5\n"
    "#define _PC_CHOWN_RESTRICTED 6\n"
    "#define _PC_NO_TRUNC 7\n"
    "#define _PC_VDISABLE 8\n"
    "#ifndef AT_FDCWD\n"
    "#define AT_FDCWD (-100)\n"
    "#endif\n"
    "#ifndef AT_SYMLINK_NOFOLLOW\n"
    "#define AT_SYMLINK_NOFOLLOW 256\n"
    "#endif\n"
    "#ifndef AT_REMOVEDIR\n"
    "#define AT_REMOVEDIR 512\n"
    "#endif\n"
    "#ifndef AT_SYMLINK_FOLLOW\n"
    "#define AT_SYMLINK_FOLLOW 1024\n"
    "#endif\n"
    "#ifndef AT_EACCESS\n"
    "#define AT_EACCESS 512\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_MODE_T\n"
    "#define _BABYPOSIX_MODE_T\n"
    "typedef unsigned int mode_t;\n"
    "#endif\n";

struct LibraryFunction BabyPosixUnistdFunctions[] = {
    { BabyPosixAccessCall, "int access(char *, int);" },
    { BabyPosixEaccessCall, "int eaccess(char *, int);" },
    { BabyPosixConfstrCall, "size_t confstr(int, char *, size_t);" },
    { BabyPosixChdirCall, "int chdir(char *);" },
    { BabyPosixGetcwdCall, "char *getcwd(char *, size_t);" },
    { BabyPosixGetpidCall, "pid_t getpid(void);" },
    { BabyPosixGetppidCall, "pid_t getppid(void);" },
    { BabyPosixGetuidCall, "uid_t getuid(void);" },
    { BabyPosixGeteuidCall, "uid_t geteuid(void);" },
    { BabyPosixGetgidCall, "gid_t getgid(void);" },
    { BabyPosixGetegidCall, "gid_t getegid(void);" },
    { BabyPosixSetuidCall, "int setuid(uid_t);" },
    { BabyPosixSetgidCall, "int setgid(gid_t);" },
    { BabyPosixSetsidCall, "pid_t setsid(void);" },
    { BabyPosixExecveCall, "int execve(char *, char **, char **);" },
    { BabyPosixIsattyCall, "int isatty(int);" },
    { BabyPosixTtynameCall, "char *ttyname(int);" },
    { BabyPosixLinkCall, "int link(char *, char *);" },
    { BabyPosixSymlinkCall, "int symlink(char *, char *);" },
    { BabyPosixReadlinkCall, "ssize_t readlink(char *, char *, size_t);" },
    { BabyPosixFsyncCall, "int fsync(int);" },
    { BabyPosixFtruncateCall, "int ftruncate(int, off_t);" },
    { BabyPosixTruncateCall, "int truncate(char *, off_t);" },
    { BabyPosixFchdirCall, "int fchdir(int);" },
    { BabyPosixChownCall, "int chown(char *, uid_t, gid_t);" },
    { BabyPosixLchownCall, "int lchown(char *, uid_t, gid_t);" },
    { BabyPosixFchownCall, "int fchown(int, uid_t, gid_t);" },
    { BabyPosixExitUnderscoreCall, "void _exit(int);" },
    { BabyPosixForkCall, "pid_t fork(void);" },
    { BabyPosixExecvCall, "int execv(char *, char **);" },
    { BabyPosixExecvpCall, "int execvp(char *, char **);" },
    { BabyPosixGetpagesizeCall, "int getpagesize(void);" },
    { BabyPosixSysconfCall, "long sysconf(int);" },
    { BabyPosixGethostnameCall, "int gethostname(char *, size_t);" },
    { BabyPosixGetloginCall, "char *getlogin(void);" },
    { BabyPosixMkstempCall, "int mkstemp(char *);" },
    { BabyPosixMkdtempCall, "char *mkdtemp(char *);" },
    { BabyPosixRealpathCall, "char *realpath(char *, char *);" },
    { BabyPosixSyncCall, "void sync(void);" },
    { BabyPosixFdatasyncCall, "int fdatasync(int);" },
    { BabyPosixGetpgrpCall, "pid_t getpgrp(void);" },
    { BabyPosixGetpgidCall, "pid_t getpgid(pid_t);" },
    { BabyPosixSetpgidCall, "int setpgid(pid_t, pid_t);" },
    { BabyPosixGetgroupsCall, "int getgroups(int, gid_t *);" },
    { BabyPosixNiceCall, "int nice(int);" },
    { BabyPosixPathconfCall, "long pathconf(char *, int);" },
    { BabyPosixFpathconfCall, "long fpathconf(int, int);" },
    { BabyPosixGetdtablesizeCall, "int getdtablesize(void);" },
    { BabyPosixSleepCall, "unsigned int sleep(unsigned int);" },
    { BabyPosixUsleepCall, "int usleep(unsigned int);" },
    { BabyPosixUnlinkCall, "int unlink(char *);" },
    { BabyPosixRmdirCall, "int rmdir(char *);" },
    { BabyPosixUnlinkatCall, "int unlinkat(int, char *, int);" },
    { BabyPosixFaccessatCall, "int faccessat(int, char *, int, int);" },
    { BabyPosixReadlinkatCall, "ssize_t readlinkat(int, char *, char *, size_t);" },
    { BabyPosixSymlinkatCall, "int symlinkat(char *, int, char *);" },
    { BabyPosixLinkatCall, "int linkat(int, char *, int, char *, int);" },
    { BabyPosixRenameatCall, "int renameat(int, char *, int, char *);" },
    { BabyPosixSetenvCall, "int setenv(char *, char *, int);" },
    { BabyPosixUnsetenvCall, "int unsetenv(char *);" },
    { BabyPosixPutenvCall, "int putenv(char *);" },
    { BabyPosixReadCall, "ssize_t read(int, void *, size_t);" },
    { BabyPosixWriteCall, "ssize_t write(int, void *, size_t);" },
    { BabyPosixCloseCall, "int close(int);" },
    { BabyPosixDupCall, "int dup(int);" },
    { BabyPosixDup2Call, "int dup2(int, int);" },
    { BabyPosixDup3Call, "int dup3(int, int, int);" },
    { BabyPosixLseekCall, "off_t lseek(int, off_t, int);" },
    { BabyPosixPipeCall, "int pipe(int *);" },
    { BabyPosixPipe2Call, "int pipe2(int *, int);" },
    { BabyPosixPreadCall, "ssize_t pread(int, void *, size_t, off_t);" },
    { BabyPosixPwriteCall, "ssize_t pwrite(int, void *, size_t, off_t);" },
    { NULL, NULL }
};

void BabyPosixUnistdSetupFunc(Picoc *pc)
{
    (void)pc;
}
