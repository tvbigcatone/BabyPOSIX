/* BabyPOSIX file descriptors.
 * Guest descriptor numbers are managed by one table and never expose the
 * host CRT/POSIX descriptor directly. This keeps the public ABI stable and
 * supports dup()/dup2()/pipe() and leaves room for other descriptor-backed objects.
 */
#include "posix_fd.h"
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#ifdef WIN32
# include <io.h>
# include <fcntl.h>
# define bp_native_open _open
# define bp_native_read _read
# define bp_native_write _write
# define bp_native_close _close
# define bp_native_dup _dup
# define bp_native_lseek _lseeki64
#else
# include <unistd.h>
# include <fcntl.h>
# define bp_native_open open
# define bp_native_read read
# define bp_native_write write
# define bp_native_close close
# define bp_native_dup dup
# define bp_native_lseek lseek
#endif

/* The descriptor table is shared by all interpreter contexts in this process.
 * Use a tiny platform-specific spin lock so this module does not add a pthread
 * link dependency. Never hold it while performing potentially blocking I/O. */
#ifdef WIN32
# include <windows.h>
static volatile LONG BabyPosixFdLockWord;
static void BabyPosixLock(void) { while (InterlockedExchange(&BabyPosixFdLockWord, 1) != 0) Sleep(0); }
static void BabyPosixUnlock(void) { InterlockedExchange(&BabyPosixFdLockWord, 0); }
#else
static volatile int BabyPosixFdLockWord;
static void BabyPosixLock(void) { while (__sync_lock_test_and_set(&BabyPosixFdLockWord, 1)) { while (BabyPosixFdLockWord) { } } }
static void BabyPosixUnlock(void) { __sync_lock_release(&BabyPosixFdLockWord); }
#endif

#define BP_O_RDONLY 0
#define BP_O_WRONLY 1
#define BP_O_RDWR   2
#define BP_O_ACCMODE 3
#define BP_O_CREAT  0x0100
#define BP_O_EXCL   0x0200
#define BP_O_TRUNC  0x0400
#define BP_O_APPEND 0x0800
#define BP_O_NONBLOCK 0x1000
#define BP_O_CLOEXEC  0x2000
#define BP_O_DIRECTORY 0x4000
#define BP_O_NOFOLLOW 0x8000
#define BP_O_NOCTTY 0x10000
#define BP_O_KNOWN (BP_O_ACCMODE | BP_O_CREAT | BP_O_EXCL | BP_O_TRUNC | BP_O_APPEND \
    | BP_O_NONBLOCK | BP_O_CLOEXEC | BP_O_DIRECTORY | BP_O_NOFOLLOW | BP_O_NOCTTY)

struct BabyPosixFdEntry {
    int in_use;
    int native_fd;
    int access;
    int owns_native_fd;
    int fd_flags;
};

static struct BabyPosixFdEntry BabyPosixFdTable[BABYPOSIX_FD_CAPACITY];
static int BabyPosixFdTableInitialized;

static void BabyPosixFdInitLocked(void)
{
    if (BabyPosixFdTableInitialized) return;
    memset(BabyPosixFdTable, 0, sizeof(BabyPosixFdTable));
    BabyPosixFdTable[0].in_use = 1;
    BabyPosixFdTable[0].native_fd = 0;
    BabyPosixFdTable[0].access = BP_FD_READ;
    BabyPosixFdTable[0].owns_native_fd = 0;
    BabyPosixFdTable[1].in_use = 1;
    BabyPosixFdTable[1].native_fd = 1;
    BabyPosixFdTable[1].access = BP_FD_WRITE;
    BabyPosixFdTable[1].owns_native_fd = 0;
    BabyPosixFdTable[2].in_use = 1;
    BabyPosixFdTable[2].native_fd = 2;
    BabyPosixFdTable[2].access = BP_FD_WRITE;
    BabyPosixFdTable[2].owns_native_fd = 0;
    BabyPosixFdTable[0].fd_flags = 0;
    BabyPosixFdTable[1].fd_flags = 0;
    BabyPosixFdTable[2].fd_flags = 0;
    BabyPosixFdTableInitialized = 1;
}

/* Return a private native duplicate while holding the table lock. The
 * duplicate pins the underlying open-file description across close()/dup2()
 * races; the lock is released before any possibly blocking host operation. */
static int BabyPosixFdAcquireNative(int fd, int required_access, int *access_out)
{
    int native_fd = -1;
    BabyPosixLock();
    BabyPosixFdInitLocked();
    if (fd < 0 || fd >= BABYPOSIX_FD_CAPACITY ||
        !BabyPosixFdTable[fd].in_use ||
        (required_access != 0 &&
         (BabyPosixFdTable[fd].access & required_access) != required_access)) {
        errno = EBADF;
    } else {
        native_fd = bp_native_dup(BabyPosixFdTable[fd].native_fd);
        if (native_fd >= 0 && access_out != NULL)
            *access_out = BabyPosixFdTable[fd].access;
    }
    BabyPosixUnlock();
    return native_fd;
}

int BabyPosixFdNativeOf(int guest_fd)
{
    int native_fd = -1;
    BabyPosixLock();
    BabyPosixFdInitLocked();
    if (guest_fd < 0 || guest_fd >= BABYPOSIX_FD_CAPACITY ||
        !BabyPosixFdTable[guest_fd].in_use) {
        errno = EBADF;
    } else
        native_fd = BabyPosixFdTable[guest_fd].native_fd;
    BabyPosixUnlock();
    return native_fd;
}

int BabyPosixFdNativeOfDir(int dirfd)
{
#ifdef AT_FDCWD
    if (dirfd == -100) return AT_FDCWD;
#endif
    return BabyPosixFdNativeOf(dirfd);
}

static int BabyPosixFdAllocateFromLocked(int minfd, int native_fd, int access,
                                         int owns_native_fd, int fd_flags)
{
    int i;
    if (minfd < 0) minfd = 0;
    for (i = minfd; i < BABYPOSIX_FD_CAPACITY; ++i) {
        if (!BabyPosixFdTable[i].in_use) {
            BabyPosixFdTable[i].in_use = 1;
            BabyPosixFdTable[i].native_fd = native_fd;
            BabyPosixFdTable[i].access = access;
            BabyPosixFdTable[i].owns_native_fd = owns_native_fd;
            BabyPosixFdTable[i].fd_flags = fd_flags;
            return i;
        }
    }
    errno = EMFILE;
    return -1;
}

int BabyPosixFdAllocate(int native_fd, int access, int owns_native_fd)
{
    int result;
    BabyPosixLock();
    BabyPosixFdInitLocked();
    result = BabyPosixFdAllocateFromLocked(0, native_fd, access, owns_native_fd, 0);
    BabyPosixUnlock();
    return result;
}

void BabyPosixFdRelease(int guest_fd)
{
    BabyPosixLock();
    BabyPosixFdInitLocked();
    if (guest_fd >= 0 && guest_fd < BABYPOSIX_FD_CAPACITY &&
        BabyPosixFdTable[guest_fd].in_use) {
        BabyPosixFdTable[guest_fd].in_use = 0;
        BabyPosixFdTable[guest_fd].native_fd = -1;
        BabyPosixFdTable[guest_fd].access = 0;
        BabyPosixFdTable[guest_fd].owns_native_fd = 0;
        BabyPosixFdTable[guest_fd].fd_flags = 0;
    }
    BabyPosixUnlock();
}

/* Release a guest slot only if it still names the native descriptor that
 * the caller originally borrowed. DIR objects and similar wrappers can outlive
 * a guest descriptor number; that slot may have been closed and reused for an
 * unrelated native descriptor in the meantime. */
void BabyPosixFdReleaseIfNative(int guest_fd, int expected_native_fd)
{
    BabyPosixLock();
    BabyPosixFdInitLocked();
    if (guest_fd >= 0 && guest_fd < BABYPOSIX_FD_CAPACITY &&
        BabyPosixFdTable[guest_fd].in_use &&
        BabyPosixFdTable[guest_fd].native_fd == expected_native_fd) {
        BabyPosixFdTable[guest_fd].in_use = 0;
        BabyPosixFdTable[guest_fd].native_fd = -1;
        BabyPosixFdTable[guest_fd].access = 0;
        BabyPosixFdTable[guest_fd].owns_native_fd = 0;
        BabyPosixFdTable[guest_fd].fd_flags = 0;
    }
    BabyPosixUnlock();
}

void BabyPosixFdDisown(int guest_fd)
{
    BabyPosixLock();
    BabyPosixFdInitLocked();
    if (guest_fd >= 0 && guest_fd < BABYPOSIX_FD_CAPACITY &&
        BabyPosixFdTable[guest_fd].in_use)
        BabyPosixFdTable[guest_fd].owns_native_fd = 0;
    BabyPosixUnlock();
}

static int BabyPosixFdAllocateFrom(int minfd, int native_fd, int access,
                                   int owns_native_fd, int fd_flags)
{
    int result;
    BabyPosixLock();
    BabyPosixFdInitLocked();
    result = BabyPosixFdAllocateFromLocked(minfd, native_fd, access,
        owns_native_fd, fd_flags);
    BabyPosixUnlock();
    return result;
}

static int BabyPosixNativeOpenFlags(int flags)
{
    int result;
    if ((flags & ~BP_O_KNOWN) != 0) { errno = EINVAL; return -1; }
    switch (flags & BP_O_ACCMODE) {
    case BP_O_RDONLY: result = O_RDONLY; break;
    case BP_O_WRONLY: result = O_WRONLY; break;
    case BP_O_RDWR: result = O_RDWR; break;
    default: errno = EINVAL; return -1;
    }
    if (flags & BP_O_CREAT) result |= O_CREAT;
    if (flags & BP_O_EXCL) result |= O_EXCL;
    if (flags & BP_O_TRUNC) result |= O_TRUNC;
    if (flags & BP_O_APPEND) result |= O_APPEND;
#ifndef WIN32
#ifdef O_NONBLOCK
    if (flags & BP_O_NONBLOCK) result |= O_NONBLOCK;
#endif
#ifdef O_CLOEXEC
    if (flags & BP_O_CLOEXEC) result |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    if (flags & BP_O_DIRECTORY) result |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    if (flags & BP_O_NOFOLLOW) result |= O_NOFOLLOW;
#endif
#ifdef O_NOCTTY
    if (flags & BP_O_NOCTTY) result |= O_NOCTTY;
#endif
#endif
#ifdef WIN32
    result |= _O_BINARY;
#endif
    return result;
}

static void BabyPosixOpenCall(struct ParseState *p, struct Value *r,
                              struct Value **a, int n)
{
    const char *path = (const char *)a[0]->Val->Pointer;
    int flags = (int)a[1]->Val->Integer;
    int mode = n >= 3 ? (int)a[2]->Val->Integer : 0666;
    int nativeFlags, native_fd, guest_fd, access;
    (void)p;
    if (path == NULL) { errno = EFAULT; r->Val->Integer = -1; return; }
    nativeFlags = BabyPosixNativeOpenFlags(flags);
    if (nativeFlags < 0) { r->Val->Integer = -1; return; }
#ifdef WIN32
    native_fd = bp_native_open(path, nativeFlags, mode);
#else
    native_fd = bp_native_open(path, nativeFlags, (mode_t)mode);
#endif
    if (native_fd < 0) { r->Val->Integer = -1; return; }
    switch (flags & BP_O_ACCMODE) {
    case BP_O_RDONLY: access = BP_FD_READ; break;
    case BP_O_WRONLY: access = BP_FD_WRITE; break;
    default: access = BP_FD_READ | BP_FD_WRITE; break;
    }
    guest_fd = BabyPosixFdAllocate(native_fd, access, 1);
    if (guest_fd < 0) {
        int saved_errno = errno;
        (void)bp_native_close(native_fd);
        errno = saved_errno;
        r->Val->Integer = -1;
        return;
    }
    if (flags & BP_O_CLOEXEC) {
        BabyPosixLock();
        BabyPosixFdTable[guest_fd].fd_flags = 1;
        BabyPosixUnlock();
    }
    r->Val->Integer = guest_fd;
}

static void BabyPosixOpenatCall(struct ParseState *p, struct Value *r,
                                struct Value **a, int n)
{
    int dirfd = (int)a[0]->Val->Integer;
    const char *path = (const char *)a[1]->Val->Pointer;
    int flags = (int)a[2]->Val->Integer;
    int mode = n >= 4 ? (int)a[3]->Val->Integer : 0666;
    (void)p;
    if (path == NULL) { errno = EFAULT; r->Val->Integer = -1; return; }
#ifdef WIN32
    (void)dirfd; (void)mode;
    errno = ENOSYS; r->Val->Integer = -1; return;
#else
    int nativeFlags, native_fd, guest_fd, access, native_dir;
    nativeFlags = BabyPosixNativeOpenFlags(flags);
    if (nativeFlags < 0) { r->Val->Integer = -1; return; }
    native_dir = BabyPosixFdNativeOfDir(dirfd);
    if (native_dir < 0 && dirfd != -100) { r->Val->Integer = -1; return; }
    native_fd = openat(native_dir, path, nativeFlags, (mode_t)mode);
    if (native_fd < 0) { r->Val->Integer = -1; return; }
    switch (flags & BP_O_ACCMODE) {
    case BP_O_RDONLY: access = BP_FD_READ; break;
    case BP_O_WRONLY: access = BP_FD_WRITE; break;
    default: access = BP_FD_READ | BP_FD_WRITE; break;
    }
    guest_fd = BabyPosixFdAllocate(native_fd, access, 1);
    if (guest_fd < 0) {
        int saved_errno = errno;
        (void)bp_native_close(native_fd);
        errno = saved_errno;
        r->Val->Integer = -1;
        return;
    }
    if (flags & BP_O_CLOEXEC) {
        BabyPosixLock();
        BabyPosixFdTable[guest_fd].fd_flags = 1;
        BabyPosixUnlock();
    }
    r->Val->Integer = guest_fd;
#endif
}

void BabyPosixReadCall(struct ParseState *p, struct Value *r,
                       struct Value **a, int n)
{
    int fd = (int)a[0]->Val->Integer;
    long long count = (long long)a[2]->Val->Integer;
    int native_fd, saved_errno;
    (void)p; (void)n;
    if (count < 0 || count > INT_MAX) {
        errno = EINVAL; r->Val->Integer = -1; return;
    }
    if (count > 0 && a[1]->Val->Pointer == NULL) {
        errno = EFAULT; r->Val->Integer = -1; return;
    }
    native_fd = BabyPosixFdAcquireNative(fd, BP_FD_READ, NULL);
    if (native_fd < 0) { r->Val->Integer = -1; return; }
    if (count == 0) {
        (void)bp_native_close(native_fd);
        r->Val->Integer = 0;
        return;
    }
    r->Val->Integer = (long long)bp_native_read(native_fd,
        a[1]->Val->Pointer, (unsigned int)count);
    saved_errno = errno;
    (void)bp_native_close(native_fd);
    errno = saved_errno;
}

void BabyPosixWriteCall(struct ParseState *p, struct Value *r,
                        struct Value **a, int n)
{
    int fd = (int)a[0]->Val->Integer;
    long long count = (long long)a[2]->Val->Integer;
    int native_fd, saved_errno;
    (void)p; (void)n;
    if (count < 0 || count > INT_MAX) {
        errno = EINVAL; r->Val->Integer = -1; return;
    }
    if (count > 0 && a[1]->Val->Pointer == NULL) {
        errno = EFAULT; r->Val->Integer = -1; return;
    }
    native_fd = BabyPosixFdAcquireNative(fd, BP_FD_WRITE, NULL);
    if (native_fd < 0) { r->Val->Integer = -1; return; }
    if (count == 0) {
        (void)bp_native_close(native_fd);
        r->Val->Integer = 0;
        return;
    }
    r->Val->Integer = (long long)bp_native_write(native_fd,
        a[1]->Val->Pointer, (unsigned int)count);
    saved_errno = errno;
    (void)bp_native_close(native_fd);
    errno = saved_errno;
}

void BabyPosixLseekCall(struct ParseState *p, struct Value *r,
                        struct Value **a, int n)
{
    int fd = (int)a[0]->Val->Integer;
    long long offset = (long long)a[1]->Val->Integer;
    int whence = (int)a[2]->Val->Integer;
    int native_fd, saved_errno;
    long long result;
    (void)p; (void)n;
    if (whence != 0 && whence != 1 && whence != 2) {
        errno = EINVAL; r->Val->Integer = -1; return;
    }
    native_fd = BabyPosixFdAcquireNative(fd, 0, NULL);
    if (native_fd < 0) { r->Val->Integer = -1; return; }
#ifdef WIN32
    result = (long long)bp_native_lseek(native_fd, offset, whence);
#else
    result = (long long)bp_native_lseek(native_fd, (off_t)offset, whence);
#endif
    saved_errno = errno;
    (void)bp_native_close(native_fd);
    errno = saved_errno;
    /* The virtual unistd.h currently declares off_t as int. Never silently
     * truncate a host offset returned by _lseeki64/lseek. */
    if (result != -1 && (result < INT_MIN || result > INT_MAX)) {
#ifdef EOVERFLOW
        errno = EOVERFLOW;
#else
        errno = ERANGE;
#endif
        r->Val->Integer = -1;
        return;
    }
    r->Val->Integer = (int)result;
}

void BabyPosixDupCall(struct ParseState *p, struct Value *r,
                      struct Value **a, int n)
{
    int oldfd = (int)a[0]->Val->Integer;
    int access = 0;
    int native_fd = BabyPosixFdAcquireNative(oldfd, 0, &access);
    int guest_fd;
    (void)p; (void)n;
    if (native_fd < 0) { r->Val->Integer = -1; return; }
    guest_fd = BabyPosixFdAllocate(native_fd, access, 1);
    if (guest_fd < 0) {
        int saved_errno = errno;
        (void)bp_native_close(native_fd);
        errno = saved_errno;
        r->Val->Integer = -1;
        return;
    }
    r->Val->Integer = guest_fd;
}

void BabyPosixDup2Call(struct ParseState *p, struct Value *r,
                       struct Value **a, int n)
{
    int oldfd = (int)a[0]->Val->Integer;
    int newfd = (int)a[1]->Val->Integer;
    int access = 0, native_fd, old_native = -1, old_owned = 0;
    (void)p; (void)n;
    if (newfd < 0 || newfd >= BABYPOSIX_FD_CAPACITY) {
        errno = EBADF; r->Val->Integer = -1; return;
    }
    if (oldfd == newfd) {
        BabyPosixLock(); BabyPosixFdInitLocked();
        if (oldfd >= 0 && oldfd < BABYPOSIX_FD_CAPACITY && BabyPosixFdTable[oldfd].in_use)
            r->Val->Integer = newfd;
        else { errno = EBADF; r->Val->Integer = -1; }
        BabyPosixUnlock();
        return;
    }
    native_fd = BabyPosixFdAcquireNative(oldfd, 0, &access);
    if (native_fd < 0) { r->Val->Integer = -1; return; }
    BabyPosixLock();
    BabyPosixFdInitLocked();
    if (BabyPosixFdTable[newfd].in_use) {
        old_native = BabyPosixFdTable[newfd].native_fd;
        old_owned = BabyPosixFdTable[newfd].owns_native_fd;
    }
    BabyPosixFdTable[newfd].in_use = 1;
    BabyPosixFdTable[newfd].native_fd = native_fd;
    BabyPosixFdTable[newfd].access = access;
    BabyPosixFdTable[newfd].owns_native_fd = 1;
    /* dup2 clears FD_CLOEXEC on the resulting descriptor. */
    BabyPosixFdTable[newfd].fd_flags = 0;
    BabyPosixUnlock();
    if (old_owned) (void)bp_native_close(old_native);
    r->Val->Integer = newfd;
}

void BabyPosixDup3Call(struct ParseState *p, struct Value *r,
                       struct Value **a, int n)
{
    int oldfd = (int)a[0]->Val->Integer;
    int newfd = (int)a[1]->Val->Integer;
    int flags = n >= 3 ? (int)a[2]->Val->Integer : 0;
    int access = 0, native_fd, old_native = -1, old_owned = 0;
    (void)p;
    if (flags != 0 && flags != BP_O_CLOEXEC) {
        errno = EINVAL; r->Val->Integer = -1; return;
    }
    if (oldfd == newfd) {
        errno = EINVAL; r->Val->Integer = -1; return;
    }
    if (newfd < 0 || newfd >= BABYPOSIX_FD_CAPACITY) {
        errno = EBADF; r->Val->Integer = -1; return;
    }
    native_fd = BabyPosixFdAcquireNative(oldfd, 0, &access);
    if (native_fd < 0) { r->Val->Integer = -1; return; }
    BabyPosixLock();
    BabyPosixFdInitLocked();
    if (BabyPosixFdTable[newfd].in_use) {
        old_native = BabyPosixFdTable[newfd].native_fd;
        old_owned = BabyPosixFdTable[newfd].owns_native_fd;
    }
    BabyPosixFdTable[newfd].in_use = 1;
    BabyPosixFdTable[newfd].native_fd = native_fd;
    BabyPosixFdTable[newfd].access = access;
    BabyPosixFdTable[newfd].owns_native_fd = 1;
    BabyPosixFdTable[newfd].fd_flags = (flags & BP_O_CLOEXEC) ? 1 : 0;
    BabyPosixUnlock();
    if (old_owned) (void)bp_native_close(old_native);
    r->Val->Integer = newfd;
}

void BabyPosixPreadCall(struct ParseState *p, struct Value *r,
                        struct Value **a, int n)
{
    int fd = (int)a[0]->Val->Integer;
    long long count = (long long)a[2]->Val->Integer;
    long long offset = (long long)a[3]->Val->Integer;
    int native_fd;
    (void)p; (void)n;
    if (count < 0 || count > INT_MAX || offset < 0) {
        errno = EINVAL; r->Val->Integer = -1; return;
    }
    if (count > 0 && a[1]->Val->Pointer == NULL) {
        errno = EFAULT; r->Val->Integer = -1; return;
    }
    native_fd = BabyPosixFdAcquireNative(fd, BP_FD_READ, NULL);
    if (native_fd < 0) { r->Val->Integer = -1; return; }
#ifdef WIN32
    (void)bp_native_close(native_fd);
    errno = ENOSYS;
    r->Val->Integer = -1;
#else
    int saved_errno;
    r->Val->Integer = (long long)pread(native_fd, a[1]->Val->Pointer,
        (size_t)count, (off_t)offset);
    saved_errno = errno;
    (void)bp_native_close(native_fd);
    errno = saved_errno;
#endif
}

void BabyPosixPwriteCall(struct ParseState *p, struct Value *r,
                         struct Value **a, int n)
{
    int fd = (int)a[0]->Val->Integer;
    long long count = (long long)a[2]->Val->Integer;
    long long offset = (long long)a[3]->Val->Integer;
    int native_fd;
    (void)p; (void)n;
    if (count < 0 || count > INT_MAX || offset < 0) {
        errno = EINVAL; r->Val->Integer = -1; return;
    }
    if (count > 0 && a[1]->Val->Pointer == NULL) {
        errno = EFAULT; r->Val->Integer = -1; return;
    }
    native_fd = BabyPosixFdAcquireNative(fd, BP_FD_WRITE, NULL);
    if (native_fd < 0) { r->Val->Integer = -1; return; }
#ifdef WIN32
    (void)bp_native_close(native_fd);
    errno = ENOSYS;
    r->Val->Integer = -1;
#else
    int saved_errno;
    r->Val->Integer = (long long)pwrite(native_fd, a[1]->Val->Pointer,
        (size_t)count, (off_t)offset);
    saved_errno = errno;
    (void)bp_native_close(native_fd);
    errno = saved_errno;
#endif
}

void BabyPosixPipeCall(struct ParseState *p, struct Value *r,
                       struct Value **a, int n)
{
    int native_pipe[2];
    int guest_read, guest_write;
    int *guest_pair;
    int saved_errno;
    (void)p; (void)n;

    guest_pair = (int *)a[0]->Val->Pointer;
    if (guest_pair == NULL) {
        errno = EFAULT;
        r->Val->Integer = -1;
        return;
    }

#ifdef WIN32
    if (_pipe(native_pipe, 4096U, _O_BINARY) != 0) {
#else
    if (pipe(native_pipe) != 0) {
#endif
        r->Val->Integer = -1;
        return;
    }

    guest_read = BabyPosixFdAllocate(native_pipe[0], BP_FD_READ, 1);
    if (guest_read < 0) {
        saved_errno = errno;
        (void)bp_native_close(native_pipe[0]);
        (void)bp_native_close(native_pipe[1]);
        errno = saved_errno;
        r->Val->Integer = -1;
        return;
    }

    guest_write = BabyPosixFdAllocate(native_pipe[1], BP_FD_WRITE, 1);
    if (guest_write < 0) {
        saved_errno = errno;
        /* Roll back the first guest slot and close both native endpoints. */
        BabyPosixLock();
        BabyPosixFdTable[guest_read].in_use = 0;
        BabyPosixFdTable[guest_read].native_fd = -1;
        BabyPosixFdTable[guest_read].access = 0;
        BabyPosixFdTable[guest_read].owns_native_fd = 0;
        BabyPosixFdTable[guest_read].fd_flags = 0;
        BabyPosixUnlock();
        (void)bp_native_close(native_pipe[0]);
        (void)bp_native_close(native_pipe[1]);
        errno = saved_errno;
        r->Val->Integer = -1;
        return;
    }

    guest_pair[0] = guest_read;
    guest_pair[1] = guest_write;
    r->Val->Integer = 0;
}

void BabyPosixPipe2Call(struct ParseState *p, struct Value *r,
                        struct Value **a, int n)
{
    int flags = n >= 2 ? (int)a[1]->Val->Integer : 0;
    int cloexec;
    (void)p;
    if (flags != 0 && flags != BP_O_CLOEXEC && flags != BP_O_NONBLOCK &&
        flags != (BP_O_CLOEXEC | BP_O_NONBLOCK)) {
        errno = EINVAL;
        r->Val->Integer = -1;
        return;
    }
    BabyPosixPipeCall(p, r, a, 1);
    if (r->Val->Integer != 0)
        return;
    cloexec = (flags & BP_O_CLOEXEC) ? 1 : 0;
    if (cloexec) {
        int *guest_pair = (int *)a[0]->Val->Pointer;
        BabyPosixLock();
        BabyPosixFdTable[guest_pair[0]].fd_flags = 1;
        BabyPosixFdTable[guest_pair[1]].fd_flags = 1;
        BabyPosixUnlock();
    }
#ifndef WIN32
    if (flags & BP_O_NONBLOCK) {
        int *guest_pair = (int *)a[0]->Val->Pointer;
        int i;
        for (i = 0; i < 2; i++) {
            int native = BabyPosixFdNativeOf(guest_pair[i]);
            int fl;
            if (native < 0) continue;
            fl = fcntl(native, F_GETFL);
            if (fl >= 0)
                (void)fcntl(native, F_SETFL, fl | O_NONBLOCK);
        }
    }
#endif
}

void BabyPosixCloseCall(struct ParseState *p, struct Value *r,
                        struct Value **a, int n)
{
    int fd = (int)a[0]->Val->Integer;
    int native_fd = -1, owns_native_fd = 0, rc = 0;
    (void)p; (void)n;
    BabyPosixLock();
    BabyPosixFdInitLocked();
    if (fd < 0 || fd >= BABYPOSIX_FD_CAPACITY || !BabyPosixFdTable[fd].in_use) {
        errno = EBADF;
        rc = -1;
    } else {
        native_fd = BabyPosixFdTable[fd].native_fd;
        owns_native_fd = BabyPosixFdTable[fd].owns_native_fd;
        BabyPosixFdTable[fd].in_use = 0;
        BabyPosixFdTable[fd].native_fd = -1;
        BabyPosixFdTable[fd].access = 0;
        BabyPosixFdTable[fd].owns_native_fd = 0;
        BabyPosixFdTable[fd].fd_flags = 0;
    }
    BabyPosixUnlock();
    if (rc == 0 && owns_native_fd) rc = bp_native_close(native_fd);
    r->Val->Integer = rc;
}


/* fcntl subset: F_GETFD/F_SETFD and F_GETFL/F_SETFL. Descriptor flags live
 * on each guest table slot; file status flags live on the shared native open
 * file description. The guest flag values are stable and
 * deliberately independent of host constants. POSIX F_SETFL changes O_APPEND
 * and O_NONBLOCK on the shared open-file description; access-mode bits in
 * arg are ignored. The MSVC CRT has no equivalent portable operation for
 * toggling those flags after open, so that command reports ENOSYS on Windows
 * rather than silently pretending it worked. */
void BabyPosixFcntlCall(struct ParseState *p, struct Value *r,
                        struct Value **a, int n)
{
    int fd = (int)a[0]->Val->Integer;
    int cmd = (int)a[1]->Val->Integer;
    int native_fd, access = 0, saved_errno;
    (void)p;

    if (cmd == 1 /* F_GETFD */ || cmd == 2 /* F_SETFD */) {
        int requested;
        BabyPosixLock();
        BabyPosixFdInitLocked();
        if (fd < 0 || fd >= BABYPOSIX_FD_CAPACITY ||
            !BabyPosixFdTable[fd].in_use) {
            BabyPosixUnlock();
            errno = EBADF;
            r->Val->Integer = -1;
            return;
        }
        if (cmd == 1) {
            r->Val->Integer = BabyPosixFdTable[fd].fd_flags;
            BabyPosixUnlock();
            return;
        }
        if (n < 3) {
            BabyPosixUnlock();
            errno = EINVAL;
            r->Val->Integer = -1;
            return;
        }
        requested = (int)a[2]->Val->Integer;
        if ((requested & ~1) != 0) {
            BabyPosixUnlock();
            errno = EINVAL;
            r->Val->Integer = -1;
            return;
        }
        BabyPosixFdTable[fd].fd_flags = requested;
        BabyPosixUnlock();
        r->Val->Integer = 0;
        return;
    }
    if (cmd == 0 /* F_DUPFD */ || cmd == 1030 /* F_DUPFD_CLOEXEC */) {
        int minfd, guest_fd, cloexec;
        if (n < 3) {
            errno = EINVAL;
            r->Val->Integer = -1;
            return;
        }
        native_fd = BabyPosixFdAcquireNative(fd, 0, &access);
        if (native_fd < 0) {
            r->Val->Integer = -1;
            return;
        }
        minfd = (int)a[2]->Val->Integer;
        if (minfd < 0 || minfd >= BABYPOSIX_FD_CAPACITY) {
            saved_errno = EINVAL;
            (void)bp_native_close(native_fd);
            errno = saved_errno;
            r->Val->Integer = -1;
            return;
        }
        cloexec = (cmd == 1030) ? 1 : 0;
        guest_fd = BabyPosixFdAllocateFrom(minfd, native_fd, access, 1, cloexec);
        if (guest_fd < 0) {
            saved_errno = errno;
            (void)bp_native_close(native_fd);
            errno = saved_errno;
            r->Val->Integer = -1;
            return;
        }
        r->Val->Integer = guest_fd;
        return;
    }
    if (cmd != 3 /* F_GETFL */ && cmd != 4 /* F_SETFL */) {
        errno = EINVAL;
        r->Val->Integer = -1;
        return;
    }
    if (cmd == 4 && n < 3) {
        errno = EINVAL;
        r->Val->Integer = -1;
        return;
    }
    native_fd = BabyPosixFdAcquireNative(fd, 0, &access);
    if (native_fd < 0) {
        r->Val->Integer = -1;
        return;
    }

    if (cmd == 3) {
#ifdef WIN32
        /* The CRT does not expose a portable runtime F_GETFL. Report the
         * access mode tracked by BabyPOSIX; O_APPEND is not introspectable. */
        r->Val->Integer = (access == BP_FD_READ) ? BP_O_RDONLY :
                          (access == BP_FD_WRITE) ? BP_O_WRONLY : BP_O_RDWR;
#else
        {
            int native_flags = fcntl(native_fd, F_GETFL);
            saved_errno = errno;
            if (native_flags < 0) {
                (void)bp_native_close(native_fd);
                errno = saved_errno;
                r->Val->Integer = -1;
                return;
            }
            r->Val->Integer = (access == BP_FD_READ) ? BP_O_RDONLY :
                              (access == BP_FD_WRITE) ? BP_O_WRONLY : BP_O_RDWR;
            if (native_flags & O_APPEND) r->Val->Integer |= BP_O_APPEND;
#ifdef O_NONBLOCK
            if (native_flags & O_NONBLOCK) r->Val->Integer |= BP_O_NONBLOCK;
#endif
        }
#endif
        saved_errno = errno;
        (void)bp_native_close(native_fd);
        errno = saved_errno;
        return;
    }

#ifdef WIN32
    (void)bp_native_close(native_fd);
    errno = ENOSYS;
    r->Val->Integer = -1;
#else
    {
        int requested = (int)a[2]->Val->Integer;
        int allowed = BP_O_ACCMODE | BP_O_APPEND | BP_O_NONBLOCK;
        int native_flags, current_flags, result;
        /* POSIX F_SETFL ignores access-mode bits; O_APPEND and O_NONBLOCK
         * live on the shared open-file description. */
        if ((requested & ~allowed) != 0) {
            (void)bp_native_close(native_fd);
            errno = EINVAL;
            r->Val->Integer = -1;
            return;
        }
        current_flags = fcntl(native_fd, F_GETFL);
        if (current_flags < 0) {
            saved_errno = errno;
            (void)bp_native_close(native_fd);
            errno = saved_errno;
            r->Val->Integer = -1;
            return;
        }
        native_flags = current_flags;
        if (requested & BP_O_APPEND) native_flags |= O_APPEND;
        else native_flags &= ~O_APPEND;
#ifdef O_NONBLOCK
        if (requested & BP_O_NONBLOCK) native_flags |= O_NONBLOCK;
        else native_flags &= ~O_NONBLOCK;
#endif
        result = fcntl(native_fd, F_SETFL, native_flags);
        saved_errno = errno;
        (void)bp_native_close(native_fd);
        errno = saved_errno;
        r->Val->Integer = result;
    }
#endif
}

const char BabyPosixFcntlDefs[] =
    "#define O_RDONLY 0\n"
    "#define O_WRONLY 1\n"
    "#define O_RDWR 2\n"
    "#define O_CREAT 256\n"
    "#define O_EXCL 512\n"
    "#define O_TRUNC 1024\n"
    "#define O_APPEND 2048\n"
    "#define O_NONBLOCK 4096\n"
    "#define O_CLOEXEC 8192\n"
    "#define O_DIRECTORY 16384\n"
    "#define O_NOFOLLOW 32768\n"
    "#define O_NOCTTY 65536\n"
    "#define O_ACCMODE 3\n"
    "#define F_DUPFD 0\n"
    "#define F_GETFD 1\n"
    "#define F_SETFD 2\n"
    "#define FD_CLOEXEC 1\n"
    "#define F_GETFL 3\n"
    "#define F_SETFL 4\n"
    "#define F_DUPFD_CLOEXEC 1030\n"
#ifdef WIN32
    "#define BABYPOSIX_FCNTL_SETFL_SUPPORTED 0\n"
#else
    "#define BABYPOSIX_FCNTL_SETFL_SUPPORTED 1\n"
#endif
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
    "#endif\n";

struct LibraryFunction BabyPosixFcntlFunctions[] = {
    { BabyPosixOpenCall, "int open(char *, int, ...);" },
    { BabyPosixOpenatCall, "int openat(int, char *, int, ...);" },
    { BabyPosixFcntlCall, "int fcntl(int, int, ...);" },
    { NULL, NULL }
};

void BabyPosixFcntlSetupFunc(Picoc *pc)
{
    (void)pc;
}
