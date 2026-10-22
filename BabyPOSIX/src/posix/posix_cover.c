/* BabyPOSIX extra headers used by coreutils-style programs. */
#include "posix_cover.h"
#include "posix_fd.h"
#include "../platform/guest_thread.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#ifdef WIN32
#else
# include <glob.h>
# include <sys/mman.h>
# include <sys/uio.h>
# include <sys/statvfs.h>
# include <sys/sendfile.h>
# include <syslog.h>
# include <langinfo.h>
# include <unistd.h>
#endif

static void BabyPosixNlLanginfoCall(struct ParseState *p, struct Value *r,
                                    struct Value **a, int n)
{
    (void)p; (void)n;
#ifdef WIN32
    (void)a;
    r->Val->Pointer = (void *)"UTF-8";
#else
    {
        char *s = nl_langinfo((nl_item)a[0]->Val->Integer);
        r->Val->Pointer = s ? (void *)s : (void *)"";
    }
#endif
}

const char BabyPosixLanginfoDefs[] =
    "typedef int nl_item;\n"
    "#define CODESET 14\n"
    "#define D_T_FMT 131113\n"
    "#define D_FMT 131112\n"
    "#define T_FMT 131112\n";

struct LibraryFunction BabyPosixLanginfoFunctions[] = {
    { BabyPosixNlLanginfoCall, "char *nl_langinfo(int);" },
    { NULL, NULL }
};

void BabyPosixLanginfoSetupFunc(Picoc *pc) { (void)pc; }

const char BabyPosixPathsDefs[] =
    "#define _PATH_DEVNULL \"/dev/null\"\n"
    "#define _PATH_TTY \"/dev/tty\"\n"
    "#define _PATH_TMP \"/tmp/\"\n"
    "#define _PATH_BSHELL \"/bin/sh\"\n"
    "#define _PATH_DEFPATH \"/usr/bin:/bin\"\n"
    "#define _PATH_STDPATH \"/usr/bin:/bin:/usr/sbin:/sbin\"\n";

struct LibraryFunction BabyPosixPathsFunctions[] = {
    { NULL, NULL }
};

void BabyPosixPathsSetupFunc(Picoc *pc) { (void)pc; }

const char BabyPosixSysexitsDefs[] =
    "#define EX_OK 0\n"
    "#define EX_USAGE 64\n"
    "#define EX_DATAERR 65\n"
    "#define EX_NOINPUT 66\n"
    "#define EX_NOUSER 67\n"
    "#define EX_NOHOST 68\n"
    "#define EX_UNAVAILABLE 69\n"
    "#define EX_SOFTWARE 70\n"
    "#define EX_OSERR 71\n"
    "#define EX_OSFILE 72\n"
    "#define EX_CANTCREAT 73\n"
    "#define EX_IOERR 74\n"
    "#define EX_TEMPFAIL 75\n"
    "#define EX_PROTOCOL 76\n"
    "#define EX_NOPERM 77\n"
    "#define EX_CONFIG 78\n";

struct LibraryFunction BabyPosixSysexitsFunctions[] = {
    { NULL, NULL }
};

void BabyPosixSysexitsSetupFunc(Picoc *pc) { (void)pc; }

static void BabyPosixOpenlogCall(struct ParseState *p, struct Value *r,
                                 struct Value **a, int n)
{
    const char *ident = (const char *)a[0]->Val->Pointer;
    (void)p; (void)n; (void)r;
#ifdef WIN32
    (void)ident; (void)a;
#else
    openlog(ident, (int)a[1]->Val->Integer, (int)a[2]->Val->Integer);
#endif
}

static void BabyPosixSyslogCall(struct ParseState *p, struct Value *r,
                                struct Value **a, int n)
{
    const char *msg = (const char *)a[1]->Val->Pointer;
    (void)p; (void)n; (void)r;
#ifdef WIN32
    (void)msg; (void)a;
#else
    if (msg == NULL) msg = "";
    syslog((int)a[0]->Val->Integer, "%s", msg);
#endif
}

static void BabyPosixCloselogCall(struct ParseState *p, struct Value *r,
                                  struct Value **a, int n)
{
    (void)p; (void)r; (void)a; (void)n;
#ifndef WIN32
    closelog();
#endif
}

const char BabyPosixSyslogDefs[] =
    "#define LOG_PID 0x01\n"
    "#define LOG_CONS 0x02\n"
    "#define LOG_NDELAY 0x08\n"
    "#define LOG_ODELAY 0x04\n"
    "#define LOG_NOWAIT 0x10\n"
    "#define LOG_PERROR 0x20\n"
    "#define LOG_EMERG 0\n"
    "#define LOG_ALERT 1\n"
    "#define LOG_CRIT 2\n"
    "#define LOG_ERR 3\n"
    "#define LOG_WARNING 4\n"
    "#define LOG_NOTICE 5\n"
    "#define LOG_INFO 6\n"
    "#define LOG_DEBUG 7\n"
    "#define LOG_KERN 0\n"
    "#define LOG_USER 8\n"
    "#define LOG_MAIL 16\n"
    "#define LOG_DAEMON 24\n"
    "#define LOG_AUTH 32\n"
    "#define LOG_SYSLOG 40\n"
    "#define LOG_LPR 48\n"
    "#define LOG_NEWS 56\n"
    "#define LOG_UUCP 64\n"
    "#define LOG_CRON 72\n"
    "#define LOG_AUTHPRIV 80\n"
    "#define LOG_LOCAL0 128\n"
    "#define LOG_LOCAL1 136\n"
    "#define LOG_LOCAL2 144\n"
    "#define LOG_LOCAL3 152\n"
    "#define LOG_LOCAL4 160\n"
    "#define LOG_LOCAL5 168\n"
    "#define LOG_LOCAL6 176\n"
    "#define LOG_LOCAL7 184\n";

struct LibraryFunction BabyPosixSyslogFunctions[] = {
    { BabyPosixOpenlogCall, "void openlog(char *, int, int);" },
    { BabyPosixSyslogCall, "void syslog(int, char *);" },
    { BabyPosixCloselogCall, "void closelog(void);" },
    { NULL, NULL }
};

void BabyPosixSyslogSetupFunc(Picoc *pc) { (void)pc; }

struct BabyPosixGlob {
    unsigned int gl_pathc;
    char **gl_pathv;
    unsigned int gl_offs;
};

static void BabyPosixGlobCall(struct ParseState *p, struct Value *r,
                              struct Value **a, int n)
{
    const char *pattern = (const char *)a[0]->Val->Pointer;
    struct BabyPosixGlob *g = (struct BabyPosixGlob *)a[3]->Val->Pointer;
    (void)p; (void)n;
    if (pattern == NULL || g == NULL) {
        errno = EFAULT; r->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; r->Val->Integer = -1;
#else
    {
        glob_t hg;
        int rc, flags;
        unsigned int i, offs, nmatch, total;
        char **copy;
        flags = (int)a[1]->Val->Integer;
        memset(&hg, 0, sizeof(hg));
        offs = (flags & 8) ? g->gl_offs : 0;
        hg.gl_offs = offs;
        rc = glob(pattern, flags & ~32, NULL, &hg);
        if (rc != 0) {
            globfree(&hg);
            r->Val->Integer = rc;
            return;
        }
        nmatch = (unsigned int)hg.gl_pathc;
        total = offs + nmatch;
        copy = (char **)malloc(sizeof(char *) * (total + 1));
        if (copy == NULL) {
            globfree(&hg);
            errno = ENOMEM;
            r->Val->Integer = -1;
            return;
        }
        for (i = 0; i < offs; i++) copy[i] = NULL;
        for (i = 0; i < nmatch; i++) {
            char *src = hg.gl_pathv[offs + i];
            size_t n;
            if (src == NULL) { copy[offs + i] = NULL; continue; }
            n = strlen(src) + 1;
            copy[offs + i] = (char *)malloc(n);
            if (copy[offs + i] == NULL) {
                unsigned int j;
                for (j = 0; j < offs + i; j++) free(copy[j]);
                free(copy);
                globfree(&hg);
                errno = ENOMEM;
                r->Val->Integer = -1;
                return;
            }
            memcpy(copy[offs + i], src, n);
        }
        copy[total] = NULL;
        globfree(&hg);
        if ((flags & 32) && g->gl_pathv != NULL) {
            unsigned int old_off = g->gl_offs;
            unsigned int old_c = g->gl_pathc;
            unsigned int new_c = old_c + nmatch;
            char **nv = (char **)malloc(sizeof(char *) * (old_off + new_c + 1));
            if (nv == NULL) {
                for (i = 0; i < total; i++) free(copy[i]);
                free(copy);
                errno = ENOMEM;
                r->Val->Integer = -1;
                return;
            }
            for (i = 0; i < old_off; i++) nv[i] = NULL;
            for (i = 0; i < old_c; i++)
                nv[old_off + i] = g->gl_pathv[old_off + i];
            for (i = 0; i < nmatch; i++)
                nv[old_off + old_c + i] = copy[offs + i];
            nv[old_off + new_c] = NULL;
            free(g->gl_pathv);
            free(copy);
            g->gl_pathv = nv;
            g->gl_pathc = new_c;
            g->gl_offs = old_off;
        } else {
            if (g->gl_pathv != NULL) {
                unsigned int n = g->gl_pathc + g->gl_offs;
                for (i = 0; i < n; i++) free(g->gl_pathv[i]);
                free(g->gl_pathv);
            }
            g->gl_pathc = nmatch;
            g->gl_pathv = copy;
            g->gl_offs = offs;
        }
        r->Val->Integer = rc;
    }
#endif
}

static void BabyPosixGlobfreeCall(struct ParseState *p, struct Value *r,
                                  struct Value **a, int n)
{
    struct BabyPosixGlob *g = (struct BabyPosixGlob *)a[0]->Val->Pointer;
    (void)p; (void)n; (void)r;
    if (g == NULL) return;
    if (g->gl_pathv != NULL) {
        unsigned int i, n = g->gl_pathc + g->gl_offs;
        for (i = 0; i < n; i++)
            free(g->gl_pathv[i]);
        free(g->gl_pathv);
    }
    g->gl_pathc = 0;
    g->gl_pathv = NULL;
}

const char BabyPosixGlobDefs[] =
    "typedef struct { unsigned int gl_pathc; char **gl_pathv; unsigned int gl_offs; } glob_t;\n"
    "#define GLOB_ERR 1\n"
    "#define GLOB_MARK 2\n"
    "#define GLOB_NOSORT 4\n"
    "#define GLOB_DOOFFS 8\n"
    "#define GLOB_NOCHECK 16\n"
    "#define GLOB_APPEND 32\n"
    "#define GLOB_NOESCAPE 64\n"
    "#define GLOB_NOSPACE 1\n"
    "#define GLOB_ABORTED 2\n"
    "#define GLOB_NOMATCH 3\n";

struct LibraryFunction BabyPosixGlobFunctions[] = {
    { BabyPosixGlobCall, "int glob(char *, int, void *, glob_t *);" },
    { BabyPosixGlobfreeCall, "void globfree(glob_t *);" },
    { NULL, NULL }
};

void BabyPosixGlobSetupFunc(Picoc *pc) { (void)pc; }

static void BabyPosixMmapCall(struct ParseState *p, struct Value *r,
                              struct Value **a, int n)
{
    void *addr = a[0]->Val->Pointer;
    unsigned int length = (unsigned int)a[1]->Val->Integer;
    int prot = (int)a[2]->Val->Integer;
    int flags = (int)a[3]->Val->Integer;
    int fd = (int)a[4]->Val->Integer;
    int off = (int)a[5]->Val->Integer;
    (void)p; (void)n;
#ifdef WIN32
    (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)off;
    errno = ENOSYS; r->Val->Pointer = (void *)(long)-1;
#else
    {
        int native = -1;
        if (fd >= 0) {
            native = BabyPosixFdNativeOf(fd);
            if (native < 0) { r->Val->Pointer = (void *)(long)-1; return; }
        }
        r->Val->Pointer = mmap(addr, (size_t)length, prot, flags, native,
            (off_t)off);
    }
#endif
}

static void BabyPosixMunmapCall(struct ParseState *p, struct Value *r,
                                struct Value **a, int n)
{
    void *addr = a[0]->Val->Pointer;
    unsigned int length = (unsigned int)a[1]->Val->Integer;
    (void)p; (void)n;
#ifdef WIN32
    (void)addr; (void)length;
    errno = ENOSYS; r->Val->Integer = -1;
#else
    if (addr == NULL || addr == (void *)(long)-1) {
        errno = EINVAL; r->Val->Integer = -1; return;
    }
    r->Val->Integer = munmap(addr, (size_t)length);
#endif
}

static void BabyPosixMsyncCall(struct ParseState *p, struct Value *r,
                               struct Value **a, int n)
{
    void *addr = a[0]->Val->Pointer;
    unsigned int length = (unsigned int)a[1]->Val->Integer;
    int flags = (int)a[2]->Val->Integer;
    (void)p; (void)n;
#ifdef WIN32
    (void)addr; (void)length; (void)flags;
    errno = ENOSYS; r->Val->Integer = -1;
#else
    if (addr == NULL || addr == (void *)(long)-1) {
        errno = EINVAL; r->Val->Integer = -1; return;
    }
    r->Val->Integer = msync(addr, (size_t)length, flags);
#endif
}

static void BabyPosixMprotectCall(struct ParseState *p, struct Value *r,
                                  struct Value **a, int n)
{
    void *addr = a[0]->Val->Pointer;
    unsigned int length = (unsigned int)a[1]->Val->Integer;
    int prot = (int)a[2]->Val->Integer;
    (void)p; (void)n;
#ifdef WIN32
    (void)addr; (void)length; (void)prot;
    errno = ENOSYS; r->Val->Integer = -1;
#else
    if (addr == NULL || addr == (void *)(long)-1) {
        errno = EINVAL; r->Val->Integer = -1; return;
    }
    r->Val->Integer = mprotect(addr, (size_t)length, prot);
#endif
}

const char BabyPosixMmanDefs[] =
    "#define PROT_NONE 0\n"
    "#define PROT_READ 1\n"
    "#define PROT_WRITE 2\n"
    "#define PROT_EXEC 4\n"
    "#define MAP_SHARED 1\n"
    "#define MAP_PRIVATE 2\n"
    "#define MAP_FIXED 16\n"
    "#define MAP_ANON 32\n"
    "#define MAP_ANONYMOUS 32\n"
    "#define MS_ASYNC 1\n"
    "#define MS_INVALIDATE 2\n"
    "#define MS_SYNC 4\n";

static void *BabyPosixMapFailed = (void *)(long)-1;

struct LibraryFunction BabyPosixMmanFunctions[] = {
    { BabyPosixMmapCall, "void *mmap(void *, size_t, int, int, int, int);" },
    { BabyPosixMunmapCall, "int munmap(void *, size_t);" },
    { BabyPosixMsyncCall, "int msync(void *, size_t, int);" },
    { BabyPosixMprotectCall, "int mprotect(void *, size_t, int);" },
    { NULL, NULL }
};

void BabyPosixMmanSetupFunc(Picoc *pc)
{
    VariableDefinePlatformVar(pc, NULL, "MAP_FAILED", pc->VoidPtrType,
        (union AnyValue *)&BabyPosixMapFailed, FALSE);
}

struct BabyPosixIovec {
    void *iov_base;
    unsigned int iov_len;
};

static void BabyPosixReadvCall(struct ParseState *p, struct Value *r,
                               struct Value **a, int n)
{
    int fd = (int)a[0]->Val->Integer;
    struct BabyPosixIovec *giov = (struct BabyPosixIovec *)a[1]->Val->Pointer;
    int iovcnt = (int)a[2]->Val->Integer;
    int native, i;
    (void)p; (void)n;
    if (giov == NULL && iovcnt != 0) {
        errno = EFAULT; r->Val->Integer = -1; return;
    }
#ifdef WIN32
    (void)fd; (void)native; (void)i;
    errno = ENOSYS; r->Val->Integer = -1;
#else
    native = BabyPosixFdNativeOf(fd);
    if (native < 0) { r->Val->Integer = -1; return; }
    if (iovcnt < 0) { errno = EINVAL; r->Val->Integer = -1; return; }
    {
        struct iovec *hv = NULL;
        int gil;
        ssize_t rc;
        if (iovcnt > 0) {
            hv = (struct iovec *)malloc(sizeof(*hv) * (size_t)iovcnt);
            if (hv == NULL) { errno = ENOMEM; r->Val->Integer = -1; return; }
            for (i = 0; i < iovcnt; i++) {
                hv[i].iov_base = giov[i].iov_base;
                hv[i].iov_len = (size_t)giov[i].iov_len;
            }
        }
        gil = BabycGuestGIL_UnlockIfHeld();
        rc = readv(native, hv, iovcnt);
        BabycGuestGIL_RelockIf(gil);
        free(hv);
        r->Val->Integer = (int)rc;
    }
#endif
}

static void BabyPosixWritevCall(struct ParseState *p, struct Value *r,
                                struct Value **a, int n)
{
    int fd = (int)a[0]->Val->Integer;
    struct BabyPosixIovec *giov = (struct BabyPosixIovec *)a[1]->Val->Pointer;
    int iovcnt = (int)a[2]->Val->Integer;
    int native, i;
    (void)p; (void)n;
    if (giov == NULL && iovcnt != 0) {
        errno = EFAULT; r->Val->Integer = -1; return;
    }
#ifdef WIN32
    (void)fd; (void)native; (void)i;
    errno = ENOSYS; r->Val->Integer = -1;
#else
    native = BabyPosixFdNativeOf(fd);
    if (native < 0) { r->Val->Integer = -1; return; }
    if (iovcnt < 0) { errno = EINVAL; r->Val->Integer = -1; return; }
    {
        struct iovec *hv = NULL;
        int gil;
        ssize_t rc;
        if (iovcnt > 0) {
            hv = (struct iovec *)malloc(sizeof(*hv) * (size_t)iovcnt);
            if (hv == NULL) { errno = ENOMEM; r->Val->Integer = -1; return; }
            for (i = 0; i < iovcnt; i++) {
                hv[i].iov_base = giov[i].iov_base;
                hv[i].iov_len = (size_t)giov[i].iov_len;
            }
        }
        gil = BabycGuestGIL_UnlockIfHeld();
        rc = writev(native, hv, iovcnt);
        BabycGuestGIL_RelockIf(gil);
        free(hv);
        r->Val->Integer = (int)rc;
    }
#endif
}

const char BabyPosixUioDefs[] =
    "struct iovec { void *iov_base; unsigned int iov_len; };\n";

struct LibraryFunction BabyPosixUioFunctions[] = {
    { BabyPosixReadvCall, "ssize_t readv(int, struct iovec *, int);" },
    { BabyPosixWritevCall, "ssize_t writev(int, struct iovec *, int);" },
    { NULL, NULL }
};

void BabyPosixUioSetupFunc(Picoc *pc) { (void)pc; }

struct BabyPosixStatvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    unsigned long f_blocks;
    unsigned long f_bfree;
    unsigned long f_bavail;
    unsigned long f_files;
    unsigned long f_ffree;
    unsigned long f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

#ifdef WIN32
static void BabyPosixFillStatvfs(struct BabyPosixStatvfs *out)
{
    memset(out, 0, sizeof(*out));
}
#else
static void BabyPosixFillStatvfs(struct BabyPosixStatvfs *out,
                                 const struct statvfs *st)
{
    memset(out, 0, sizeof(*out));
    out->f_bsize = (unsigned long)st->f_bsize;
    out->f_frsize = (unsigned long)st->f_frsize;
    out->f_blocks = (unsigned long)st->f_blocks;
    out->f_bfree = (unsigned long)st->f_bfree;
    out->f_bavail = (unsigned long)st->f_bavail;
    out->f_files = (unsigned long)st->f_files;
    out->f_ffree = (unsigned long)st->f_ffree;
    out->f_favail = (unsigned long)st->f_favail;
    out->f_fsid = (unsigned long)st->f_fsid;
    out->f_flag = (unsigned long)st->f_flag;
    out->f_namemax = (unsigned long)st->f_namemax;
}
#endif

static void BabyPosixStatvfsCall(struct ParseState *p, struct Value *r,
                                 struct Value **a, int n)
{
    const char *path = (const char *)a[0]->Val->Pointer;
    struct BabyPosixStatvfs *out = (struct BabyPosixStatvfs *)a[1]->Val->Pointer;
    (void)p; (void)n;
    if (path == NULL || out == NULL) {
        errno = EFAULT; r->Val->Integer = -1; return;
    }
#ifdef WIN32
    errno = ENOSYS; r->Val->Integer = -1;
#else
    {
        struct statvfs st;
        if (statvfs(path, &st) != 0) { r->Val->Integer = -1; return; }
        BabyPosixFillStatvfs(out, &st);
        r->Val->Integer = 0;
    }
#endif
}

static void BabyPosixFstatvfsCall(struct ParseState *p, struct Value *r,
                                  struct Value **a, int n)
{
    int fd = (int)a[0]->Val->Integer;
    struct BabyPosixStatvfs *out = (struct BabyPosixStatvfs *)a[1]->Val->Pointer;
    int native;
    (void)p; (void)n;
    if (out == NULL) { errno = EFAULT; r->Val->Integer = -1; return; }
#ifdef WIN32
    (void)fd; (void)native;
    errno = ENOSYS; r->Val->Integer = -1;
#else
    native = BabyPosixFdNativeOf(fd);
    if (native < 0) { r->Val->Integer = -1; return; }
    {
        struct statvfs st;
        if (fstatvfs(native, &st) != 0) { r->Val->Integer = -1; return; }
        BabyPosixFillStatvfs(out, &st);
        r->Val->Integer = 0;
    }
#endif
}

const char BabyPosixStatvfsDefs[] =
    "struct statvfs {\n"
    "    unsigned long f_bsize;\n"
    "    unsigned long f_frsize;\n"
    "    unsigned long f_blocks;\n"
    "    unsigned long f_bfree;\n"
    "    unsigned long f_bavail;\n"
    "    unsigned long f_files;\n"
    "    unsigned long f_ffree;\n"
    "    unsigned long f_favail;\n"
    "    unsigned long f_fsid;\n"
    "    unsigned long f_flag;\n"
    "    unsigned long f_namemax;\n"
    "};\n"
    "#define ST_RDONLY 1\n"
    "#define ST_NOSUID 2\n";

struct LibraryFunction BabyPosixStatvfsFunctions[] = {
    { BabyPosixStatvfsCall, "int statvfs(char *, struct statvfs *);" },
    { BabyPosixFstatvfsCall, "int fstatvfs(int, struct statvfs *);" },
    { NULL, NULL }
};

void BabyPosixStatvfsSetupFunc(Picoc *pc) { (void)pc; }

static void BabyPosixSetjmpCall(struct ParseState *p, struct Value *r,
                                struct Value **a, int n)
{
    (void)p; (void)n; (void)a;
    r->Val->Integer = 0;
}

static void BabyPosixLongjmpCall(struct ParseState *p, struct Value *r,
                                 struct Value **a, int n)
{
    int v = (int)a[1]->Val->Integer;
    (void)p; (void)r; (void)n;
    if (v == 0) v = 1;
#ifdef WIN32
    exit(v);
#else
    _exit(v);
#endif
}

const char BabyPosixSetjmpDefs[] =
    "typedef long jmp_buf[8];\n"
    "#define setjmp(env) _babyposix_setjmp(env)\n";

struct LibraryFunction BabyPosixSetjmpFunctions[] = {
    { BabyPosixSetjmpCall, "int _babyposix_setjmp(jmp_buf);" },
    { BabyPosixLongjmpCall, "void longjmp(jmp_buf, int);" },
    { NULL, NULL }
};

void BabyPosixSetjmpSetupFunc(Picoc *pc) { (void)pc; }

static void BabyPosixWcslenCall(struct ParseState *p, struct Value *r,
                                struct Value **a, int n)
{
    const int *s = (const int *)a[0]->Val->Pointer;
    unsigned int len = 0;
    (void)p; (void)n;
    if (s == NULL) { r->Val->Integer = 0; return; }
    while (s[len] != 0) len++;
    r->Val->Integer = (int)len;
}

static void BabyPosixWcscmpCall(struct ParseState *p, struct Value *r,
                                struct Value **a, int n)
{
    const int *x = (const int *)a[0]->Val->Pointer;
    const int *y = (const int *)a[1]->Val->Pointer;
    (void)p; (void)n;
    if (x == NULL || y == NULL) { r->Val->Integer = 0; return; }
    while (*x && *x == *y) { x++; y++; }
    r->Val->Integer = *x - *y;
}

static void BabyPosixWcscpyCall(struct ParseState *p, struct Value *r,
                                struct Value **a, int n)
{
    int *d = (int *)a[0]->Val->Pointer;
    const int *s = (const int *)a[1]->Val->Pointer;
    int *orig = d;
    (void)p; (void)n;
    if (d == NULL || s == NULL) { r->Val->Pointer = d; return; }
    while ((*d++ = *s++) != 0) {}
    r->Val->Pointer = orig;
}

const char BabyPosixWcharDefs[] =
    "#ifndef _BABYPOSIX_WCHAR_T\n"
    "#define _BABYPOSIX_WCHAR_T\n"
    "typedef int wchar_t;\n"
    "#endif\n"
    "#define WEOF (-1)\n";

struct LibraryFunction BabyPosixWcharFunctions[] = {
    { BabyPosixWcslenCall, "size_t wcslen(wchar_t *);" },
    { BabyPosixWcscmpCall, "int wcscmp(wchar_t *, wchar_t *);" },
    { BabyPosixWcscpyCall, "wchar_t *wcscpy(wchar_t *, wchar_t *);" },
    { NULL, NULL }
};

void BabyPosixWcharSetupFunc(Picoc *pc) { (void)pc; }

static void BabyPosixSendfileCall(struct ParseState *p, struct Value *r,
                                  struct Value **a, int n)
{
    int out_fd = (int)a[0]->Val->Integer;
    int in_fd = (int)a[1]->Val->Integer;
    int *offp = (int *)a[2]->Val->Pointer;
    unsigned int count = (unsigned int)a[3]->Val->Integer;
    int nout, nin;
    (void)p; (void)n;
#ifdef WIN32
    (void)out_fd; (void)in_fd; (void)offp; (void)count; (void)nout; (void)nin;
    errno = ENOSYS; r->Val->Integer = -1;
#else
    nout = BabyPosixFdNativeOf(out_fd);
    nin = BabyPosixFdNativeOf(in_fd);
    if (nout < 0 || nin < 0) { r->Val->Integer = -1; return; }
    {
        off_t hoff = 0;
        off_t *hp = NULL;
        ssize_t sent;
        int gil;
        if (offp != NULL) {
            hoff = (off_t)*offp;
            hp = &hoff;
        }
        gil = BabycGuestGIL_UnlockIfHeld();
        sent = sendfile(nout, nin, hp, (size_t)count);
        BabycGuestGIL_RelockIf(gil);
        if (offp != NULL)
            *offp = (int)hoff;
        r->Val->Integer = (int)sent;
    }
#endif
}

const char BabyPosixSendfileDefs[] =
    "#ifndef _BABYPOSIX_OFF_T\n"
    "#define _BABYPOSIX_OFF_T\n"
    "typedef int off_t;\n"
    "#endif\n";

struct LibraryFunction BabyPosixSendfileFunctions[] = {
    { BabyPosixSendfileCall, "ssize_t sendfile(int, int, off_t *, size_t);" },
    { NULL, NULL }
};

void BabyPosixSendfileSetupFunc(Picoc *pc) { (void)pc; }
