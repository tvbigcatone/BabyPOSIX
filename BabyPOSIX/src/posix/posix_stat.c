/* BabyPOSIX: sys/stat.h — guest struct stat independent of host layout. */
#include "posix_stat.h"
#include "posix_fd.h"
#include <errno.h>
#include <string.h>
#ifdef WIN32
# include <sys/stat.h>
# include <direct.h>
# include <io.h>
#else
# include <sys/stat.h>
# include <sys/types.h>
# include <unistd.h>
# include <fcntl.h>
# include <time.h>
#endif

/* Guest layout matches BabyPosixStatDefs. Field names avoid glibc
 * st_atime / st_mtime / st_ctime macros (st_atim.tv_sec). */
struct BabyPosixStat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    int st_uid;
    int st_gid;
    unsigned long st_rdev;
    int st_size;
    long bp_atime;
    long bp_mtime;
    long bp_ctime;
    long st_blksize;
    long st_blocks;
};

static void BabyPosixFillStat(struct BabyPosixStat *out, const struct stat *st)
{
    memset(out, 0, sizeof(*out));
    out->st_dev = (unsigned long)st->st_dev;
    out->st_ino = (unsigned long)st->st_ino;
    out->st_mode = (unsigned int)st->st_mode;
    out->st_nlink = (unsigned int)st->st_nlink;
#ifdef WIN32
    out->st_uid = 0;
    out->st_gid = 0;
    out->st_rdev = 0;
    out->st_blksize = 4096;
    out->st_blocks = (long)((st->st_size + 511) / 512);
#else
    out->st_uid = (int)st->st_uid;
    out->st_gid = (int)st->st_gid;
    out->st_rdev = (unsigned long)st->st_rdev;
    out->st_blksize = (long)st->st_blksize;
    out->st_blocks = (long)st->st_blocks;
#endif
    out->st_size = (int)st->st_size;
    out->bp_atime = (long)st->st_atime;
    out->bp_mtime = (long)st->st_mtime;
    out->bp_ctime = (long)st->st_ctime;
}

static void BabyPosixStatCall(struct ParseState *p, struct Value *r,
                              struct Value **a, int n)
{
    const char *path = (const char *)a[0]->Val->Pointer;
    struct BabyPosixStat *out = (struct BabyPosixStat *)a[1]->Val->Pointer;
    struct stat st;
    int rc;
    (void)p; (void)n;
    if (path == NULL || out == NULL) {
        errno = EFAULT; r->Val->Integer = -1; return;
    }
    rc = stat(path, &st);
    if (rc != 0) { r->Val->Integer = -1; return; }
    BabyPosixFillStat(out, &st);
    r->Val->Integer = 0;
}

static void BabyPosixLstatCall(struct ParseState *p, struct Value *r,
                               struct Value **a, int n)
{
    const char *path = (const char *)a[0]->Val->Pointer;
    struct BabyPosixStat *out = (struct BabyPosixStat *)a[1]->Val->Pointer;
    struct stat st;
    int rc;
    (void)p; (void)n;
    if (path == NULL || out == NULL) {
        errno = EFAULT; r->Val->Integer = -1; return;
    }
#ifdef WIN32
    rc = stat(path, &st);
#else
    rc = lstat(path, &st);
#endif
    if (rc != 0) { r->Val->Integer = -1; return; }
    BabyPosixFillStat(out, &st);
    r->Val->Integer = 0;
}

static void BabyPosixFstatCall(struct ParseState *p, struct Value *r,
                               struct Value **a, int n)
{
    int fd = (int)a[0]->Val->Integer;
    struct BabyPosixStat *out = (struct BabyPosixStat *)a[1]->Val->Pointer;
    int native;
    struct stat st;
    int rc;
    (void)p; (void)n;
    if (out == NULL) {
        errno = EFAULT; r->Val->Integer = -1; return;
    }
    native = BabyPosixFdNativeOf(fd);
    if (native < 0) { r->Val->Integer = -1; return; }
    rc = fstat(native, &st);
    if (rc != 0) { r->Val->Integer = -1; return; }
    BabyPosixFillStat(out, &st);
    r->Val->Integer = 0;
}

static void BabyPosixChmodCall(struct ParseState *p, struct Value *r,
                               struct Value **a, int n)
{
    const char *path = (const char *)a[0]->Val->Pointer;
    unsigned int mode = (unsigned int)a[1]->Val->Integer;
    (void)p; (void)n;
    if (path == NULL) { errno = EFAULT; r->Val->Integer = -1; return; }
#ifdef WIN32
    r->Val->Integer = _chmod(path, (int)mode);
#else
    r->Val->Integer = chmod(path, (mode_t)mode);
#endif
}

static void BabyPosixFchmodCall(struct ParseState *p, struct Value *r,
                                struct Value **a, int n)
{
    int native;
    (void)p; (void)n;
    native = BabyPosixFdNativeOf((int)a[0]->Val->Integer);
    if (native < 0) { r->Val->Integer = -1; return; }
#ifdef WIN32
    errno = ENOSYS; r->Val->Integer = -1;
#else
    r->Val->Integer = fchmod(native, (mode_t)a[1]->Val->Integer);
#endif
}

static void BabyPosixMkdirCall(struct ParseState *p, struct Value *r,
                               struct Value **a, int n)
{
    const char *path = (const char *)a[0]->Val->Pointer;
    (void)p; (void)n;
    if (path == NULL) { errno = EFAULT; r->Val->Integer = -1; return; }
#ifdef WIN32
    r->Val->Integer = _mkdir(path);
#else
    r->Val->Integer = mkdir(path, (mode_t)a[1]->Val->Integer);
#endif
}

static void BabyPosixUmaskCall(struct ParseState *p, struct Value *r,
                               struct Value **a, int n)
{
    (void)p; (void)n;
#ifdef WIN32
    r->Val->Integer = _umask((int)a[0]->Val->Integer);
#else
    r->Val->Integer = (int)umask((mode_t)a[0]->Val->Integer);
#endif
}

static void BabyPosixMknodCall(struct ParseState *p, struct Value *r,
                               struct Value **a, int n)
{
    (void)p; (void)n;
#ifdef WIN32
    errno = ENOSYS; r->Val->Integer = -1;
#else
    {
        const char *path = (const char *)a[0]->Val->Pointer;
        if (path == NULL) { errno = EFAULT; r->Val->Integer = -1; return; }
        r->Val->Integer = mknod(path, (mode_t)a[1]->Val->Integer,
            (dev_t)a[2]->Val->Integer);
    }
#endif
}

static void BabyPosixMkfifoCall(struct ParseState *p, struct Value *r,
                                struct Value **a, int n)
{
    const char *path = (const char *)a[0]->Val->Pointer;
    (void)p; (void)n;
    if (path == NULL) { errno = EFAULT; r->Val->Integer = -1; return; }
#ifdef WIN32
    errno = ENOSYS; r->Val->Integer = -1;
#else
    r->Val->Integer = mkfifo(path, (mode_t)a[1]->Val->Integer);
#endif
}

static void BabyPosixFstatatCall(struct ParseState *p, struct Value *r,
                                 struct Value **a, int n)
{
    int dirfd = (int)a[0]->Val->Integer;
    const char *path = (const char *)a[1]->Val->Pointer;
    struct BabyPosixStat *out = (struct BabyPosixStat *)a[2]->Val->Pointer;
    int flags = (int)a[3]->Val->Integer;
    (void)p; (void)n;
    if (path == NULL || out == NULL) {
        errno = EFAULT; r->Val->Integer = -1; return;
    }
#ifdef WIN32
    (void)dirfd; (void)flags;
    errno = ENOSYS; r->Val->Integer = -1;
#else
    int native_dir;
    struct stat st;
    if (dirfd == -100) native_dir = AT_FDCWD;
    else native_dir = BabyPosixFdNativeOf(dirfd);
    if (native_dir < 0 && dirfd != -100) { r->Val->Integer = -1; return; }
    if (fstatat(native_dir, path, &st, flags) != 0) {
        r->Val->Integer = -1; return;
    }
    BabyPosixFillStat(out, &st);
    r->Val->Integer = 0;
#endif
}

static void BabyPosixMkdiratCall(struct ParseState *p, struct Value *r,
                                 struct Value **a, int n)
{
    int dirfd = (int)a[0]->Val->Integer;
    const char *path = (const char *)a[1]->Val->Pointer;
    (void)p; (void)n;
    if (path == NULL) { errno = EFAULT; r->Val->Integer = -1; return; }
#ifdef WIN32
    (void)dirfd;
    errno = ENOSYS; r->Val->Integer = -1;
#else
    int native_dir;
    if (dirfd == -100) native_dir = AT_FDCWD;
    else native_dir = BabyPosixFdNativeOf(dirfd);
    if (native_dir < 0 && dirfd != -100) { r->Val->Integer = -1; return; }
    r->Val->Integer = mkdirat(native_dir, path, (mode_t)a[2]->Val->Integer);
#endif
}

static void BabyPosixFchmodatCall(struct ParseState *p, struct Value *r,
                                  struct Value **a, int n)
{
    int dirfd = (int)a[0]->Val->Integer;
    const char *path = (const char *)a[1]->Val->Pointer;
    (void)p; (void)n;
    if (path == NULL) { errno = EFAULT; r->Val->Integer = -1; return; }
#ifdef WIN32
    (void)dirfd;
    errno = ENOSYS; r->Val->Integer = -1;
#else
    int native_dir;
    if (dirfd == -100) native_dir = AT_FDCWD;
    else native_dir = BabyPosixFdNativeOf(dirfd);
    if (native_dir < 0 && dirfd != -100) { r->Val->Integer = -1; return; }
     r->Val->Integer = fchmodat(native_dir, path, (mode_t)a[2]->Val->Integer,
        (int)a[3]->Val->Integer);
#endif
}

struct BabyPosixTimespecArg {
    long long tv_sec;
    long tv_nsec;
};

static void BabyPosixUtimensatCall(struct ParseState *p, struct Value *r,
                                   struct Value **a, int n)
{
    int dirfd = (int)a[0]->Val->Integer;
    const char *path = (const char *)a[1]->Val->Pointer;
    struct BabyPosixTimespecArg *ts = (struct BabyPosixTimespecArg *)a[2]->Val->Pointer;
    int flags = (int)a[3]->Val->Integer;
    (void)p; (void)n;
    if (path == NULL) { errno = EFAULT; r->Val->Integer = -1; return; }
#ifdef WIN32
    (void)dirfd; (void)ts; (void)flags;
    errno = ENOSYS; r->Val->Integer = -1;
#else
    int native_dir;
    if (dirfd == -100) native_dir = AT_FDCWD;
    else native_dir = BabyPosixFdNativeOf(dirfd);
    if (native_dir < 0 && dirfd != -100) { r->Val->Integer = -1; return; }
    if (ts == NULL) {
        r->Val->Integer = utimensat(native_dir, path, NULL, flags);
    } else {
        struct timespec nts[2];
        nts[0].tv_sec = (time_t)ts[0].tv_sec;
        nts[0].tv_nsec = ts[0].tv_nsec;
        nts[1].tv_sec = (time_t)ts[1].tv_sec;
        nts[1].tv_nsec = ts[1].tv_nsec;
        r->Val->Integer = utimensat(native_dir, path, nts, flags);
    }
#endif
}

static void BabyPosixFutimensCall(struct ParseState *p, struct Value *r,
                                  struct Value **a, int n)
{
    int native;
    struct BabyPosixTimespecArg *ts = (struct BabyPosixTimespecArg *)a[1]->Val->Pointer;
    (void)p; (void)n;
    native = BabyPosixFdNativeOf((int)a[0]->Val->Integer);
    if (native < 0) { r->Val->Integer = -1; return; }
#ifdef WIN32
    (void)ts;
    errno = ENOSYS; r->Val->Integer = -1;
#else
    if (ts == NULL) {
        r->Val->Integer = futimens(native, NULL);
    } else {
        struct timespec nts[2];
        nts[0].tv_sec = (time_t)ts[0].tv_sec;
        nts[0].tv_nsec = ts[0].tv_nsec;
        nts[1].tv_sec = (time_t)ts[1].tv_sec;
        nts[1].tv_nsec = ts[1].tv_nsec;
        r->Val->Integer = futimens(native, nts);
    }
#endif
}

const char BabyPosixStatDefs[] =
    "#ifndef AT_FDCWD\n"
    "#define AT_FDCWD (-100)\n"
    "#endif\n"
    "struct stat {\n"
    "    unsigned long st_dev;\n"
    "    unsigned long st_ino;\n"
    "    unsigned int st_mode;\n"
    "    unsigned int st_nlink;\n"
    "    int st_uid;\n"
    "    int st_gid;\n"
    "    unsigned long st_rdev;\n"
    "    int st_size;\n"
    "    long st_atime;\n"
    "    long st_mtime;\n"
    "    long st_ctime;\n"
    "    long st_blksize;\n"
    "    long st_blocks;\n"
    "};\n"
    "#define S_IFMT 0170000\n"
    "#define S_IFSOCK 0140000\n"
    "#define S_IFLNK 0120000\n"
    "#define S_IFREG 0100000\n"
    "#define S_IFBLK 0060000\n"
    "#define S_IFDIR 0040000\n"
    "#define S_IFCHR 0020000\n"
    "#define S_IFIFO 0010000\n"
    "#define S_ISUID 0004000\n"
    "#define S_ISGID 0002000\n"
    "#define S_ISVTX 0001000\n"
    "#define S_IRWXU 00700\n"
    "#define S_IRUSR 00400\n"
    "#define S_IWUSR 00200\n"
    "#define S_IXUSR 00100\n"
    "#define S_IRWXG 00070\n"
    "#define S_IRGRP 00040\n"
    "#define S_IWGRP 00020\n"
    "#define S_IXGRP 00010\n"
    "#define S_IRWXO 00007\n"
    "#define S_IROTH 00004\n"
    "#define S_IWOTH 00002\n"
    "#define S_IXOTH 00001\n"
    "#define S_ISDIR(m) (((m) & 0170000) == 0040000)\n"
    "#define S_ISCHR(m) (((m) & 0170000) == 0020000)\n"
    "#define S_ISBLK(m) (((m) & 0170000) == 0060000)\n"
    "#define S_ISREG(m) (((m) & 0170000) == 0100000)\n"
    "#define S_ISFIFO(m) (((m) & 0170000) == 0010000)\n"
    "#define S_ISLNK(m) (((m) & 0170000) == 0120000)\n"
    "#define S_ISSOCK(m) (((m) & 0170000) == 0140000)\n"
    "#include <sys/time.h>\n"
    "#define UTIME_NOW 1073741823\n"
    "#define UTIME_OMIT 1073741822\n";

struct LibraryFunction BabyPosixStatFunctions[] = {
    { BabyPosixStatCall, "int stat(char *, struct stat *);" },
    { BabyPosixLstatCall, "int lstat(char *, struct stat *);" },
    { BabyPosixFstatCall, "int fstat(int, struct stat *);" },
    { BabyPosixChmodCall, "int chmod(char *, unsigned int);" },
    { BabyPosixFchmodCall, "int fchmod(int, unsigned int);" },
    { BabyPosixMkdirCall, "int mkdir(char *, unsigned int);" },
    { BabyPosixUmaskCall, "unsigned int umask(unsigned int);" },
    { BabyPosixMknodCall, "int mknod(char *, unsigned int, unsigned long);" },
    { BabyPosixMkfifoCall, "int mkfifo(char *, unsigned int);" },
    { BabyPosixFstatatCall, "int fstatat(int, char *, struct stat *, int);" },
    { BabyPosixMkdiratCall, "int mkdirat(int, char *, unsigned int);" },
    { BabyPosixFchmodatCall, "int fchmodat(int, char *, unsigned int, int);" },
    { BabyPosixUtimensatCall, "int utimensat(int, char *, struct timespec *, int);" },
    { BabyPosixFutimensCall, "int futimens(int, struct timespec *);" },
    { NULL, NULL }
};

void BabyPosixStatSetupFunc(Picoc *pc) { (void)pc; }
