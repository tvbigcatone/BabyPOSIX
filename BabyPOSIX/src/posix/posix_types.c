/* BabyPOSIX: sys/types.h — shared integer typedefs for the guest ABI. */
#include "posix_types.h"

const char BabyPosixTypesDefs[] =
    "#ifndef _BABYPOSIX_PID_T\n"
    "#define _BABYPOSIX_PID_T\n"
    "typedef int pid_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_UID_T\n"
    "#define _BABYPOSIX_UID_T\n"
    "typedef int uid_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_GID_T\n"
    "#define _BABYPOSIX_GID_T\n"
    "typedef int gid_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_OFF_T\n"
    "#define _BABYPOSIX_OFF_T\n"
    "typedef int off_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_MODE_T\n"
    "#define _BABYPOSIX_MODE_T\n"
    "typedef unsigned int mode_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_NLINK_T\n"
    "#define _BABYPOSIX_NLINK_T\n"
    "typedef unsigned int nlink_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_DEV_T\n"
    "#define _BABYPOSIX_DEV_T\n"
    "typedef unsigned long dev_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_INO_T\n"
    "#define _BABYPOSIX_INO_T\n"
    "typedef unsigned long ino_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_BLKSIZE_T\n"
    "#define _BABYPOSIX_BLKSIZE_T\n"
    "typedef long blksize_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_BLKCNT_T\n"
    "#define _BABYPOSIX_BLKCNT_T\n"
    "typedef long blkcnt_t;\n"
    "#endif\n"
    "#ifndef _BABYPOSIX_USECONDS_T\n"
    "#define _BABYPOSIX_USECONDS_T\n"
    "typedef unsigned int useconds_t;\n"
    "#endif\n";

struct LibraryFunction BabyPosixTypesFunctions[] = {
    { NULL, NULL }
};

void BabyPosixTypesSetupFunc(Picoc *pc) { (void)pc; }
