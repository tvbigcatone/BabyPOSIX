/* BabyPOSIX: pwd.h / grp.h — passwd/group lookups with a guest-stable layout. */
#include "posix_pwd.h"
#include <errno.h>
#include <string.h>
#ifdef WIN32
#else
# include <pwd.h>
# include <grp.h>
#endif

struct BabyPosixPasswd {
    char *pw_name;
    char *pw_passwd;
    int pw_uid;
    int pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
};

struct BabyPosixGroup {
    char *gr_name;
    char *gr_passwd;
    int gr_gid;
    char **gr_mem;
};

static struct BabyPosixPasswd BabyPosixPw;
static struct BabyPosixGroup BabyPosixGr;
static char *BabyPosixEmptyMem[1];

#ifdef WIN32
static void BabyPosixFillWinPasswd(void)
{
    static char name[] = "user";
    static char empty[] = "";
    static char dir[] = ".";
    static char shell[] = "cmd.exe";
    BabyPosixPw.pw_name = name;
    BabyPosixPw.pw_passwd = empty;
    BabyPosixPw.pw_uid = 0;
    BabyPosixPw.pw_gid = 0;
    BabyPosixPw.pw_gecos = empty;
    BabyPosixPw.pw_dir = dir;
    BabyPosixPw.pw_shell = shell;
}
static void BabyPosixFillWinGroup(void)
{
    static char name[] = "user";
    static char empty[] = "";
    BabyPosixEmptyMem[0] = 0;
    BabyPosixGr.gr_name = name;
    BabyPosixGr.gr_passwd = empty;
    BabyPosixGr.gr_gid = 0;
    BabyPosixGr.gr_mem = BabyPosixEmptyMem;
}
#else
static void BabyPosixFillPasswd(struct passwd *pw)
{
    if (pw == NULL) return;
    BabyPosixPw.pw_name = pw->pw_name;
    BabyPosixPw.pw_passwd = pw->pw_passwd;
    BabyPosixPw.pw_uid = (int)pw->pw_uid;
    BabyPosixPw.pw_gid = (int)pw->pw_gid;
    BabyPosixPw.pw_gecos = pw->pw_gecos;
    BabyPosixPw.pw_dir = pw->pw_dir;
    BabyPosixPw.pw_shell = pw->pw_shell;
}
static void BabyPosixFillGroup(struct group *gr)
{
    if (gr == NULL) return;
    BabyPosixGr.gr_name = gr->gr_name;
    BabyPosixGr.gr_passwd = gr->gr_passwd;
    BabyPosixGr.gr_gid = (int)gr->gr_gid;
    BabyPosixGr.gr_mem = gr->gr_mem;
}
#endif

static void BabyPosixGetpwuidCall(struct ParseState *p, struct Value *r,
                                  struct Value **a, int n)
{
    (void)p; (void)n;
#ifdef WIN32
    BabyPosixFillWinPasswd();
    r->Val->Pointer = &BabyPosixPw;
#else
    {
        struct passwd *pw = getpwuid((uid_t)a[0]->Val->Integer);
        if (pw == NULL) { r->Val->Pointer = NULL; return; }
        BabyPosixFillPasswd(pw);
        r->Val->Pointer = &BabyPosixPw;
    }
#endif
}

static void BabyPosixGetpwnamCall(struct ParseState *p, struct Value *r,
                                  struct Value **a, int n)
{
    const char *name = (const char *)a[0]->Val->Pointer;
    (void)p; (void)n;
    if (name == NULL) { errno = EFAULT; r->Val->Pointer = NULL; return; }
#ifdef WIN32
    BabyPosixFillWinPasswd();
    r->Val->Pointer = &BabyPosixPw;
#else
    {
        struct passwd *pw = getpwnam(name);
        if (pw == NULL) { r->Val->Pointer = NULL; return; }
        BabyPosixFillPasswd(pw);
        r->Val->Pointer = &BabyPosixPw;
    }
#endif
}

static void BabyPosixGetgrgidCall(struct ParseState *p, struct Value *r,
                                  struct Value **a, int n)
{
    (void)p; (void)n;
#ifdef WIN32
    BabyPosixFillWinGroup();
    r->Val->Pointer = &BabyPosixGr;
#else
    {
        struct group *gr = getgrgid((gid_t)a[0]->Val->Integer);
        if (gr == NULL) { r->Val->Pointer = NULL; return; }
        BabyPosixFillGroup(gr);
        r->Val->Pointer = &BabyPosixGr;
    }
#endif
}

static void BabyPosixGetgrnamCall(struct ParseState *p, struct Value *r,
                                  struct Value **a, int n)
{
    const char *name = (const char *)a[0]->Val->Pointer;
    (void)p; (void)n;
    if (name == NULL) { errno = EFAULT; r->Val->Pointer = NULL; return; }
#ifdef WIN32
    BabyPosixFillWinGroup();
    r->Val->Pointer = &BabyPosixGr;
#else
    {
        struct group *gr = getgrnam(name);
        if (gr == NULL) { r->Val->Pointer = NULL; return; }
        BabyPosixFillGroup(gr);
        r->Val->Pointer = &BabyPosixGr;
    }
#endif
}

const char BabyPosixPwdDefs[] =
    "#ifndef _BABYPOSIX_UID_T\n"
    "#define _BABYPOSIX_UID_T\n"
    "typedef int uid_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_GID_T\n"
    "#define _BABYPOSIX_GID_T\n"
    "typedef int gid_t;\n"
    "#endif\n"
    "struct passwd {\n"
    "    char *pw_name;\n"
    "    char *pw_passwd;\n"
    "    int pw_uid;\n"
    "    int pw_gid;\n"
    "    char *pw_gecos;\n"
    "    char *pw_dir;\n"
    "    char *pw_shell;\n"
    "};\n";

struct LibraryFunction BabyPosixPwdFunctions[] = {
    { BabyPosixGetpwuidCall, "struct passwd *getpwuid(int);" },
    { BabyPosixGetpwnamCall, "struct passwd *getpwnam(char *);" },
    { NULL, NULL }
};

void BabyPosixPwdSetupFunc(Picoc *pc) { (void)pc; }

const char BabyPosixGrpDefs[] =
    "#ifndef _BABYPOSIX_GID_T\n"
    "#define _BABYPOSIX_GID_T\n"
    "typedef int gid_t;\n"
    "#endif\n"
    "struct group {\n"
    "    char *gr_name;\n"
    "    char *gr_passwd;\n"
    "    int gr_gid;\n"
    "    char **gr_mem;\n"
    "};\n";

struct LibraryFunction BabyPosixGrpFunctions[] = {
    { BabyPosixGetgrgidCall, "struct group *getgrgid(int);" },
    { BabyPosixGetgrnamCall, "struct group *getgrnam(char *);" },
    { NULL, NULL }
};

void BabyPosixGrpSetupFunc(Picoc *pc) { (void)pc; }
