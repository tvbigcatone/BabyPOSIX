/* BabyPOSIX: portable pthread.h subset.
 *
 * Design goals match the rest of BabyPOSIX:
 *   - one interpreted ABI on every host;
 *   - host primitives selected here, never exposed as guest handles;
 *   - keep the educational surface small and documented.
 *
 * Thread-creation policy mirrors the WINAPI bridge: a real OS thread is
 * started only when the start routine is a native Intrinsic. Interpreted
 * function pointers use the shared GIL trampoline in platform/guest_thread.c
 * (same mechanism as CreateThread). Blocking mutex/cond/join drop the GIL.
 */
#include "posix_pthread.h"
#include "../picoc.h"
#include "../platform/guest_thread.h"
#include "../ir_runtime.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#ifdef WIN32
# include <windows.h>
# include <process.h>
#else
# include <pthread.h>
# include <time.h>
#endif

/* Guest ABI handle types — independent from the host's pthread_t layout.
 * On UNIX these may coincide in size with the system types; on WIN32 the
 * system has no pthread_t at all, so we always use our own aliases. */
typedef unsigned long bp_pthread_t;
typedef unsigned long bp_mutex_t;
typedef unsigned long bp_cond_t;

/* ---- Guest-visible opaque handles (stable size on all hosts) ---- */

#define BP_PTHREAD_MAX          64
#define BP_MUTEX_MAX            128
#define BP_COND_MAX             64
#define BP_KEY_MAX              32
#define BP_PTHREAD_MUTEX_NORMAL     0
#define BP_PTHREAD_MUTEX_RECURSIVE  1
#define BP_PTHREAD_MUTEX_ERRORCHECK 2

#define BP_PTHREAD_CREATE_JOINABLE 0
#define BP_PTHREAD_CREATE_DETACHED 1

struct BabyPosixThread {
    int in_use;
    int detached;
    int finished;
    void *exit_value;
#ifdef WIN32
    HANDLE handle;
    unsigned thread_id;
#else
    pthread_t native;
#endif
    void *(*start_routine)(void *); /* native C only */
    struct Value *guest_func;       /* interpreted start_routine */
    void *start_arg;
    void *saved_heap_stack_top;
    void *saved_stack_frame;
    struct StackFrame *saved_top_stack_frame;
    int saved_ir_call_depth;
    int saved_lin_frame_depth;
    IrLinThreadContext saved_lin_context;
    jmp_buf saved_exit_buf;
    int saved_exit_point_set;
    int saved_interp;
};

struct BabyPosixMutex {
    int in_use;
    int recursive;
#ifdef WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t native;
#endif
};

struct BabyPosixCond {
    int in_use;
#ifdef WIN32
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t native;
#endif
};

struct BabyPosixKey {
    int in_use;
    struct Value *dtor;
    void *value[BP_PTHREAD_MAX];
};

static struct BabyPosixThread BabyPosixThreads[BP_PTHREAD_MAX];
static struct BabyPosixMutex  BabyPosixMutexes[BP_MUTEX_MAX];
static struct BabyPosixCond   BabyPosixConds[BP_COND_MAX];
static struct BabyPosixKey    BabyPosixKeys[BP_KEY_MAX];

/* Slot 0 reserved as "main / invalid" sentinel for pthread_t == 0. */
static int BabyPosixPthreadInited;

#ifdef WIN32
static CRITICAL_SECTION BabyPosixPthreadTableLock;
static void BabyPosixPthreadLock(void)   { EnterCriticalSection(&BabyPosixPthreadTableLock); }
static void BabyPosixPthreadUnlock(void) { LeaveCriticalSection(&BabyPosixPthreadTableLock); }
static __declspec(thread) unsigned long BabyPosixTlsSlot;
#else
static pthread_mutex_t BabyPosixPthreadTableLock = PTHREAD_MUTEX_INITIALIZER;
static void BabyPosixPthreadLock(void)   { (void)pthread_mutex_lock(&BabyPosixPthreadTableLock); }
static void BabyPosixPthreadUnlock(void) { (void)pthread_mutex_unlock(&BabyPosixPthreadTableLock); }
static __thread unsigned long BabyPosixTlsSlot;
#endif


/* GIL + guest invoke: shared with libWINAPI CreateThread (platform/guest_thread.c). */

static void *BabyPosixGuestPtr(struct Value *V)
{
    /* Shared normalization handles nulls and explicit integer-to-pointer
     * values consistently across pthread and libWINAPI guest callbacks. */
    return BabycGuestValueAsPointer(V, 0);
}

static void BabyPosixPthreadEnsureInit(void)
{
    if (BabyPosixPthreadInited) return;
#ifdef WIN32
    InitializeCriticalSection(&BabyPosixPthreadTableLock);
#endif
    memset(BabyPosixThreads, 0, sizeof(BabyPosixThreads));
    memset(BabyPosixMutexes, 0, sizeof(BabyPosixMutexes));
    memset(BabyPosixConds, 0, sizeof(BabyPosixConds));
    memset(BabyPosixKeys, 0, sizeof(BabyPosixKeys));
    /* Slot 0 is never allocated: guest pthread_t 0 means "no thread". */
    BabyPosixThreads[0].in_use = 1;
    BabyPosixPthreadInited = 1;
}

/* Recycle a detached slot once the native thread has finished. Joinable
 * slots stay allocated until pthread_join / pthread_detach. */
static void BabyPosixThreadComplete(struct BabyPosixThread *t, void *ret)
{
    BabyPosixPthreadLock();
    t->exit_value = ret;
    t->finished = 1;
    if (t->detached)
        t->in_use = 0;
    BabyPosixPthreadUnlock();
}

/* pthread_exit() is a native host exit: it never returns through
 * BabycGuestCall, so guest heap/IR frames would otherwise leak into the
 * next interpreted statement on the shared Picoc instance. */
static void BabyPosixGuestHeapCapture(struct BabyPosixThread *t)
{
    Picoc *pc = BabycGuestGetPC();
    t->saved_interp = 0;
    t->saved_heap_stack_top = NULL;
    t->saved_stack_frame = NULL;
    t->saved_top_stack_frame = NULL;
    t->saved_ir_call_depth = 0;
    t->saved_lin_frame_depth = 0;
    memset(&t->saved_lin_context, 0, sizeof(t->saved_lin_context));
    if (pc == NULL)
        return;
    t->saved_heap_stack_top = pc->HeapStackTop;
    t->saved_stack_frame = pc->StackFrame;
    t->saved_top_stack_frame = pc->TopStackFrame;
    t->saved_ir_call_depth = IrCallDepth;
    t->saved_lin_frame_depth = LinFrameDepth();
    IrLinThreadContextCapture(&t->saved_lin_context);
    memcpy(t->saved_exit_buf, pc->PicocExitBuf, sizeof(jmp_buf));
    t->saved_exit_point_set = pc->PicocExitPointSet;
    t->saved_interp = 1;
}

static void BabyPosixGuestHeapRestore(struct BabyPosixThread *t)
{
    Picoc *pc = BabycGuestGetPC();
    struct ValueProxyEntry **link;
    if (!t->saved_interp || pc == NULL)
        return;
    /* pthread_exit bypasses FnCodeRun's normal linear-frame unwind. */
    LinFrameRestoreDepth(t->saved_lin_frame_depth);
    while (IrCallDepth > t->saved_ir_call_depth)
        IrFramePop();
    link = &pc->ValueProxyList;
    while (*link != NULL) {
        struct ValueProxyEntry *entry = *link;
        if ((char *)entry->Owner >= (char *)t->saved_heap_stack_top) {
            *link = entry->Next;
            HeapFreeMem(pc, entry);
        } else
            link = &entry->Next;
    }
    pc->HeapStackTop = t->saved_heap_stack_top;
    pc->StackFrame = t->saved_stack_frame;
    pc->TopStackFrame = t->saved_top_stack_frame;
    IrLinThreadContextRestore(&t->saved_lin_context);
    memcpy(pc->PicocExitBuf, t->saved_exit_buf, sizeof(jmp_buf));
    pc->PicocExitPointSet = t->saved_exit_point_set;
}

static void BabyPosixRunKeyDtors(unsigned long tslot);

/* ---- Thread entry (native Intrinsic only) ---- */

#ifdef WIN32
static unsigned __stdcall BabyPosixThreadEntry(void *arg)
{
    struct BabyPosixThread *t = (struct BabyPosixThread *)arg;
    void *ret = NULL;
    struct Value *fv = t->guest_func;
    BabyPosixTlsSlot = (unsigned long)(t - BabyPosixThreads);
    BabycGuestGIL_Lock();
    BabyPosixGuestHeapCapture(t);
    if (fv != NULL && fv->Typ != NULL && fv->Typ->Base == TypeFunction)
        ret = BabycGuestCall(fv, t->start_arg);
    else if (t->start_routine != NULL)
        ret = t->start_routine(t->start_arg);
    BabyPosixRunKeyDtors((unsigned long)(t - BabyPosixThreads));
    BabycGuestGIL_Unlock();
    BabyPosixThreadComplete(t, ret);
    return 0;
}
#else
static void *BabyPosixThreadEntry(void *arg)
{
    struct BabyPosixThread *t = (struct BabyPosixThread *)arg;
    void *ret = NULL;
    struct Value *fv = t->guest_func;
    BabyPosixTlsSlot = (unsigned long)(t - BabyPosixThreads);
    BabycGuestGIL_Lock();
    BabyPosixGuestHeapCapture(t);
    if (fv != NULL && fv->Typ != NULL && fv->Typ->Base == TypeFunction)
        ret = BabycGuestCall(fv, t->start_arg);
    else if (t->start_routine != NULL)
        ret = t->start_routine(t->start_arg);
    BabyPosixRunKeyDtors((unsigned long)(t - BabyPosixThreads));
    BabycGuestGIL_Unlock();
    BabyPosixThreadComplete(t, ret);
    return ret;
}
#endif

/* ---- pthread_create / join / detach / exit / self / equal ---- */

static void BabyPosixPthreadCreateCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    bp_pthread_t *thread_out;
    struct Value *func_value;
    void *arg;
    int slot;
    struct BabyPosixThread *t;
    int rc;
    int i;
    int detached = 0;
    (void)NumArgs;

    BabyPosixPthreadEnsureInit();
    if (Parser != NULL && Parser->pc != NULL)
        BabycGuestSetPC(Parser->pc);

    thread_out = (bp_pthread_t *)Param[0]->Val->Pointer;
    {
        int *ap = (int *)BabyPosixGuestPtr(Param[1]);
        if (ap != NULL && *ap == BP_PTHREAD_CREATE_DETACHED)
            detached = 1;
    }
    func_value = BabycGuestResolveFuncValue(Param[2]);
    arg = BabyPosixGuestPtr(Param[3]);

    if (thread_out == NULL) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    if (func_value == NULL) {
        ReturnValue->Val->Integer = ENOSYS;
        return;
    }

    BabyPosixPthreadLock();
    slot = -1;
    for (i = 1; i < BP_PTHREAD_MAX; i++) {
        if (!BabyPosixThreads[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        BabyPosixPthreadUnlock();
        ReturnValue->Val->Integer = EAGAIN;
        return;
    }
    t = &BabyPosixThreads[slot];
    memset(t, 0, sizeof(*t));
    t->in_use = 1;
    t->detached = detached;
    t->start_routine = NULL;
    t->guest_func = func_value;
    t->start_arg = arg;
    BabyPosixPthreadUnlock();

#ifdef WIN32
    {
        uintptr_t h = _beginthreadex(NULL, 0, BabyPosixThreadEntry, t, 0, &t->thread_id);
        if (h == 0) {
            BabyPosixPthreadLock();
            t->in_use = 0;
            BabyPosixPthreadUnlock();
            ReturnValue->Val->Integer = EAGAIN;
            return;
        }
        t->handle = (HANDLE)h;
        if (detached) {
            CloseHandle(t->handle);
            t->handle = NULL;
        }
        rc = 0;
    }
#else
    {
        pthread_attr_t nattr;
        pthread_attr_t *nattrp = NULL;
        if (detached) {
            (void)pthread_attr_init(&nattr);
            (void)pthread_attr_setdetachstate(&nattr, PTHREAD_CREATE_DETACHED);
            nattrp = &nattr;
        }
        rc = pthread_create(&t->native, nattrp, BabyPosixThreadEntry, t);
        if (nattrp != NULL)
            (void)pthread_attr_destroy(&nattr);
        if (rc != 0) {
            BabyPosixPthreadLock();
            t->in_use = 0;
            BabyPosixPthreadUnlock();
            ReturnValue->Val->Integer = rc;
            return;
        }
    }
#endif
    *thread_out = (bp_pthread_t)(uintptr_t)slot;
    ReturnValue->Val->Integer = 0;
}



static void BabyPosixPthreadJoinCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    uintptr_t slot = (uintptr_t)Param[0]->Val->Integer;
    void **retval = (void **)BabyPosixGuestPtr(Param[1]);
    struct BabyPosixThread *t;
    (void)Parser; (void)NumArgs;

    BabyPosixPthreadEnsureInit();
    if (slot == 0 || slot >= BP_PTHREAD_MAX) {
        ReturnValue->Val->Integer = ESRCH;
        return;
    }
    BabyPosixPthreadLock();
    t = &BabyPosixThreads[slot];
    if (!t->in_use) {
        BabyPosixPthreadUnlock();
        ReturnValue->Val->Integer = ESRCH;
        return;
    }
    if (t->detached) {
        BabyPosixPthreadUnlock();
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    BabyPosixPthreadUnlock();

    {
        int gil = BabycGuestGIL_UnlockIfHeld();
        int rc = 0;
#ifdef WIN32
        if (t->handle != NULL) {
            WaitForSingleObject(t->handle, INFINITE);
            CloseHandle(t->handle);
            t->handle = NULL;
        }
#else
        rc = pthread_join(t->native, NULL);
#endif
        BabycGuestGIL_RelockIf(gil);
        if (rc != 0) {
            ReturnValue->Val->Integer = rc;
            return;
        }
    }
    if (retval != NULL)
        *retval = t->exit_value;
    BabyPosixPthreadLock();
    t->in_use = 0;
    BabyPosixPthreadUnlock();
    ReturnValue->Val->Integer = 0;
}

static void BabyPosixPthreadDetachCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    uintptr_t slot = (uintptr_t)Param[0]->Val->Integer;
    struct BabyPosixThread *t;
    (void)Parser; (void)NumArgs;

    BabyPosixPthreadEnsureInit();
    if (slot == 0 || slot >= BP_PTHREAD_MAX) {
        ReturnValue->Val->Integer = ESRCH;
        return;
    }
    BabyPosixPthreadLock();
    t = &BabyPosixThreads[slot];
    if (!t->in_use) {
        BabyPosixPthreadUnlock();
        ReturnValue->Val->Integer = ESRCH;
        return;
    }
    t->detached = 1;
#ifdef WIN32
    if (t->handle != NULL) {
        CloseHandle(t->handle);
        t->handle = NULL;
    }
    if (t->finished)
        t->in_use = 0;
    BabyPosixPthreadUnlock();
    ReturnValue->Val->Integer = 0;
#else
    {
        pthread_t native = t->native;
        int finished = t->finished;
        int rc;
        BabyPosixPthreadUnlock();
        rc = pthread_detach(native);
        if (rc == 0 && finished) {
            BabyPosixPthreadLock();
            if (t->in_use && t->finished && t->detached)
                t->in_use = 0;
            BabyPosixPthreadUnlock();
        }
        ReturnValue->Val->Integer = rc;
    }
#endif
}

static void BabyPosixPthreadExitCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    unsigned long slot;
    void *retval;
    (void)Parser; (void)ReturnValue; (void)NumArgs;
    retval = BabyPosixGuestPtr(Param[0]);
    BabyPosixPthreadEnsureInit();
    slot = BabyPosixTlsSlot;
    if (slot != 0 && slot < (unsigned long)BP_PTHREAD_MAX) {
        struct BabyPosixThread *t = &BabyPosixThreads[slot];
        if (t->in_use) {
            BabyPosixRunKeyDtors(slot);
            BabyPosixGuestHeapRestore(t);
            BabyPosixThreadComplete(t, retval);
        }
    }
    (void)BabycGuestGIL_UnlockIfHeld();
#ifdef WIN32
    _endthreadex(0);
#else
    pthread_exit(retval);
#endif
}

static void BabyPosixPthreadSelfCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)Param; (void)NumArgs;
    BabyPosixPthreadEnsureInit();
    ReturnValue->Val->Integer = (long)BabyPosixTlsSlot;
}

static void BabyPosixPthreadEqualCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    ReturnValue->Val->Integer =
        (Param[0]->Val->Integer == Param[1]->Val->Integer) ? 1 : 0;
}

/* ---- Mutex ---- */

static int BabyPosixMutexAlloc(int recursive)
{
    int i;
    BabyPosixPthreadEnsureInit();
    BabyPosixPthreadLock();
    for (i = 1; i < BP_MUTEX_MAX; i++) {
        if (!BabyPosixMutexes[i].in_use) {
            memset(&BabyPosixMutexes[i], 0, sizeof(BabyPosixMutexes[i]));
            BabyPosixMutexes[i].in_use = 1;
            BabyPosixMutexes[i].recursive = recursive ? 1 : 0;
#ifdef WIN32
            InitializeCriticalSection(&BabyPosixMutexes[i].cs);
#else
            {
                pthread_mutexattr_t nattr;
                pthread_mutexattr_t *ap = NULL;
                int rc;
                if (recursive) {
                    if (pthread_mutexattr_init(&nattr) != 0) {
                        BabyPosixMutexes[i].in_use = 0;
                        BabyPosixPthreadUnlock();
                        return -1;
                    }
                    if (pthread_mutexattr_settype(&nattr, PTHREAD_MUTEX_RECURSIVE) != 0) {
                        (void)pthread_mutexattr_destroy(&nattr);
                        BabyPosixMutexes[i].in_use = 0;
                        BabyPosixPthreadUnlock();
                        return -1;
                    }
                    ap = &nattr;
                }
                rc = pthread_mutex_init(&BabyPosixMutexes[i].native, ap);
                if (ap != NULL) (void)pthread_mutexattr_destroy(&nattr);
                if (rc != 0) {
                    BabyPosixMutexes[i].in_use = 0;
                    BabyPosixPthreadUnlock();
                    return -1;
                }
            }
#endif
            BabyPosixPthreadUnlock();
            return i;
        }
    }
    BabyPosixPthreadUnlock();
    return -1;
}

static void BabyPosixPthreadMutexInitCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    bp_mutex_t *mp = (bp_mutex_t *)Param[0]->Val->Pointer;
    int slot;
    int recursive = 0;
    int *attr = (int *)Param[1]->Val->Pointer;
    (void)Parser; (void)NumArgs;
    if (mp == NULL) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    if (attr != NULL && *attr == BP_PTHREAD_MUTEX_RECURSIVE)
        recursive = 1;
    slot = BabyPosixMutexAlloc(recursive);
    if (slot < 0) {
        ReturnValue->Val->Integer = EAGAIN;
        return;
    }
    *mp = (bp_mutex_t)(uintptr_t)slot;
    ReturnValue->Val->Integer = 0;
}

static void BabyPosixPthreadMutexDestroyCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    bp_mutex_t *mp = (bp_mutex_t *)Param[0]->Val->Pointer;
    uintptr_t slot;
    (void)Parser; (void)NumArgs;
    if (mp == NULL) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    slot = (uintptr_t)(*mp);
    /* Destroying a never-used INITIALIZER (handle 0) is a no-op. */
    if (slot == 0) {
        ReturnValue->Val->Integer = 0;
        return;
    }
    if (slot >= BP_MUTEX_MAX || !BabyPosixMutexes[slot].in_use) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
#ifdef WIN32
    DeleteCriticalSection(&BabyPosixMutexes[slot].cs);
#else
    (void)pthread_mutex_destroy(&BabyPosixMutexes[slot].native);
#endif
    BabyPosixMutexes[slot].in_use = 0;
    *mp = (bp_mutex_t)(uintptr_t)-1;
    ReturnValue->Val->Integer = 0;
}

/* PTHREAD_MUTEX_INITIALIZER is 0 in the guest ABI. First lock/trylock on a
 * zero handle allocates a real mutex (common educational pattern). */
static int BabyPosixMutexEnsure(bp_mutex_t *mp)
{
    int slot;
    if (mp == NULL) return EINVAL;
    if (*mp != 0) {
        slot = (int)(uintptr_t)(*mp);
        if (slot <= 0 || slot >= BP_MUTEX_MAX || !BabyPosixMutexes[slot].in_use)
            return EINVAL;
        return 0;
    }
    slot = BabyPosixMutexAlloc(0);
    if (slot < 0) return EAGAIN;
    *mp = (bp_mutex_t)(uintptr_t)slot;
    return 0;
}

static void BabyPosixPthreadMutexLockCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    bp_mutex_t *mp = (bp_mutex_t *)Param[0]->Val->Pointer;
    uintptr_t slot;
    int er;
    (void)Parser; (void)NumArgs;
    er = BabyPosixMutexEnsure(mp);
    if (er != 0) {
        ReturnValue->Val->Integer = er;
        return;
    }
    slot = (uintptr_t)(*mp);
    {
        int gil = BabycGuestGIL_UnlockIfHeld();
#ifdef WIN32
        EnterCriticalSection(&BabyPosixMutexes[slot].cs);
        ReturnValue->Val->Integer = 0;
#else
        ReturnValue->Val->Integer = pthread_mutex_lock(&BabyPosixMutexes[slot].native);
#endif
        BabycGuestGIL_RelockIf(gil);
    }
}

static void BabyPosixPthreadMutexUnlockCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    bp_mutex_t *mp = (bp_mutex_t *)Param[0]->Val->Pointer;
    uintptr_t slot;
    (void)Parser; (void)NumArgs;
    if (mp == NULL) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    slot = (uintptr_t)(*mp);
    /* Unlock never auto-inits: unlocking an INITIALIZER handle is invalid. */
    if (slot == 0 || slot >= BP_MUTEX_MAX || !BabyPosixMutexes[slot].in_use) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
#ifdef WIN32
    LeaveCriticalSection(&BabyPosixMutexes[slot].cs);
    ReturnValue->Val->Integer = 0;
#else
    ReturnValue->Val->Integer = pthread_mutex_unlock(&BabyPosixMutexes[slot].native);
#endif
}

static void BabyPosixPthreadMutexTrylockCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    bp_mutex_t *mp = (bp_mutex_t *)Param[0]->Val->Pointer;
    uintptr_t slot;
    int er;
    (void)Parser; (void)NumArgs;
    er = BabyPosixMutexEnsure(mp);
    if (er != 0) {
        ReturnValue->Val->Integer = er;
        return;
    }
    slot = (uintptr_t)(*mp);
#ifdef WIN32
    if (TryEnterCriticalSection(&BabyPosixMutexes[slot].cs))
        ReturnValue->Val->Integer = 0;
    else
        ReturnValue->Val->Integer = EBUSY;
#else
    ReturnValue->Val->Integer = pthread_mutex_trylock(&BabyPosixMutexes[slot].native);
#endif
}

/* ---- Mutex attributes (guest ABI values, independent of host layout) ---- */

static void BabyPosixPthreadMutexattrInitCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int *ap = (int *)Param[0]->Val->Pointer;
    (void)Parser; (void)NumArgs;
    if (ap == NULL) { ReturnValue->Val->Integer = EINVAL; return; }
    *ap = BP_PTHREAD_MUTEX_NORMAL;
    ReturnValue->Val->Integer = 0;
}

static void BabyPosixPthreadMutexattrDestroyCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    (void)Parser; (void)NumArgs;
    ReturnValue->Val->Integer = Param[0]->Val->Pointer != NULL ? 0 : EINVAL;
}

static void BabyPosixPthreadMutexattrSettypeCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int *ap = (int *)Param[0]->Val->Pointer;
    int type = (int)Param[1]->Val->Integer;
    (void)Parser; (void)NumArgs;
    if (ap == NULL || (type != BP_PTHREAD_MUTEX_NORMAL &&
        type != BP_PTHREAD_MUTEX_RECURSIVE && type != BP_PTHREAD_MUTEX_ERRORCHECK)) {
        ReturnValue->Val->Integer = EINVAL; return;
    }
    *ap = type;
    ReturnValue->Val->Integer = 0;
}

static void BabyPosixPthreadMutexattrGettypeCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int *ap = (int *)Param[0]->Val->Pointer;
    int *out = (int *)Param[1]->Val->Pointer;
    (void)Parser; (void)NumArgs;
    if (ap == NULL || out == NULL) { ReturnValue->Val->Integer = EINVAL; return; }
    *out = *ap;
    ReturnValue->Val->Integer = 0;
}

/* ---- Condition variables ---- */

static int BabyPosixCondAlloc(void)
{
    int i;
    BabyPosixPthreadEnsureInit();
    BabyPosixPthreadLock();
    for (i = 1; i < BP_COND_MAX; i++) {
        if (!BabyPosixConds[i].in_use) {
            memset(&BabyPosixConds[i], 0, sizeof(BabyPosixConds[i]));
            BabyPosixConds[i].in_use = 1;
#ifdef WIN32
            InitializeConditionVariable(&BabyPosixConds[i].cv);
#else
            if (pthread_cond_init(&BabyPosixConds[i].native, NULL) != 0) {
                BabyPosixConds[i].in_use = 0;
                BabyPosixPthreadUnlock();
                return -1;
            }
#endif
            BabyPosixPthreadUnlock();
            return i;
        }
    }
    BabyPosixPthreadUnlock();
    return -1;
}

static void BabyPosixPthreadCondInitCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    bp_cond_t *cp = (bp_cond_t *)Param[0]->Val->Pointer;
    int slot;
    (void)Parser; (void)NumArgs;
    if (cp == NULL) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    slot = BabyPosixCondAlloc();
    if (slot < 0) {
        ReturnValue->Val->Integer = EAGAIN;
        return;
    }
    *cp = (bp_cond_t)(uintptr_t)slot;
    ReturnValue->Val->Integer = 0;
}

static void BabyPosixPthreadCondDestroyCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    bp_cond_t *cp = (bp_cond_t *)Param[0]->Val->Pointer;
    uintptr_t slot;
    (void)Parser; (void)NumArgs;
    if (cp == NULL) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    slot = (uintptr_t)(*cp);
    /* Destroying a never-used INITIALIZER (handle 0) is a no-op. */
    if (slot == 0) {
        ReturnValue->Val->Integer = 0;
        return;
    }
    if (slot >= BP_COND_MAX || !BabyPosixConds[slot].in_use) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
#ifndef WIN32
    (void)pthread_cond_destroy(&BabyPosixConds[slot].native);
#endif
    /* Win32 CONDITION_VARIABLE has no destroy. */
    BabyPosixConds[slot].in_use = 0;
    *cp = (bp_cond_t)(uintptr_t)-1;
    ReturnValue->Val->Integer = 0;
}

static int BabyPosixCondEnsure(bp_cond_t *cp)
{
    int slot;
    if (cp == NULL) return EINVAL;
    if (*cp != 0) {
        slot = (int)(uintptr_t)(*cp);
        if (slot <= 0 || slot >= BP_COND_MAX || !BabyPosixConds[slot].in_use)
            return EINVAL;
        return 0;
    }
    slot = BabyPosixCondAlloc();
    if (slot < 0) return EAGAIN;
    *cp = (bp_cond_t)(uintptr_t)slot;
    return 0;
}

static void BabyPosixPthreadCondWaitCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    bp_cond_t *cp = (bp_cond_t *)Param[0]->Val->Pointer;
    bp_mutex_t *mp = (bp_mutex_t *)Param[1]->Val->Pointer;
    uintptr_t cslot, mslot;
    int er;
    (void)Parser; (void)NumArgs;
    if (cp == NULL || mp == NULL) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    er = BabyPosixCondEnsure(cp);
    if (er != 0) {
        ReturnValue->Val->Integer = er;
        return;
    }
    er = BabyPosixMutexEnsure(mp);
    if (er != 0) {
        ReturnValue->Val->Integer = er;
        return;
    }
    cslot = (uintptr_t)(*cp);
    mslot = (uintptr_t)(*mp);
    if (cslot == 0 || cslot >= BP_COND_MAX || !BabyPosixConds[cslot].in_use ||
        mslot == 0 || mslot >= BP_MUTEX_MAX || !BabyPosixMutexes[mslot].in_use) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    {
        int gil = BabycGuestGIL_UnlockIfHeld();
#ifdef WIN32
        if (!SleepConditionVariableCS(&BabyPosixConds[cslot].cv,
                &BabyPosixMutexes[mslot].cs, INFINITE)) {
            BabycGuestGIL_RelockIf(gil);
            ReturnValue->Val->Integer = EINTR;
            return;
        }
        ReturnValue->Val->Integer = 0;
#else
        ReturnValue->Val->Integer = pthread_cond_wait(
            &BabyPosixConds[cslot].native, &BabyPosixMutexes[mslot].native);
#endif
        BabycGuestGIL_RelockIf(gil);
    }
}

static void BabyPosixPthreadCondTimedwaitCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    bp_cond_t *cp = (bp_cond_t *)Param[0]->Val->Pointer;
    bp_mutex_t *mp = (bp_mutex_t *)Param[1]->Val->Pointer;
    struct BabyPosixTimespec { long long tv_sec; long tv_nsec; };
    struct BabyPosixTimespec *ts = (struct BabyPosixTimespec *)Param[2]->Val->Pointer;
    uintptr_t cslot, mslot;
    int er;
    (void)Parser; (void)NumArgs;
    if (cp == NULL || mp == NULL || ts == NULL || ts->tv_nsec < 0 ||
        ts->tv_nsec >= 1000000000L) {
        ReturnValue->Val->Integer = EINVAL; return;
    }
    er = BabyPosixCondEnsure(cp);
    if (er != 0) { ReturnValue->Val->Integer = er; return; }
    er = BabyPosixMutexEnsure(mp);
    if (er != 0) { ReturnValue->Val->Integer = er; return; }
    cslot = (uintptr_t)(*cp); mslot = (uintptr_t)(*mp);
    if (cslot == 0 || cslot >= BP_COND_MAX || !BabyPosixConds[cslot].in_use ||
        mslot == 0 || mslot >= BP_MUTEX_MAX || !BabyPosixMutexes[mslot].in_use) {
        ReturnValue->Val->Integer = EINVAL; return;
    }
    {
        int gil = BabycGuestGIL_UnlockIfHeld();
#ifdef WIN32
        FILETIME ft;
        ULARGE_INTEGER ticks;
        const unsigned long long Epoch = 116444736000000000ULL;
        unsigned long long now, deadline;
        long long ms;
        DWORD timeout;
        GetSystemTimeAsFileTime(&ft);
        ticks.LowPart = ft.dwLowDateTime; ticks.HighPart = ft.dwHighDateTime;
        now = ticks.QuadPart;
        deadline = ts->tv_sec < 0 ? 0ULL : Epoch +
            (unsigned long long)ts->tv_sec * 10000000ULL +
            (unsigned long long)ts->tv_nsec / 100ULL;
        ms = deadline <= now ? 0 : (long long)((deadline - now) / 10000ULL);
        if (ms > 0x7fffffffLL) ms = 0x7fffffffLL;
        timeout = (DWORD)ms;
        if (!SleepConditionVariableCS(&BabyPosixConds[cslot].cv,
                &BabyPosixMutexes[mslot].cs, timeout))
            ReturnValue->Val->Integer = ETIMEDOUT;
        else ReturnValue->Val->Integer = 0;
#else
        {
            struct timespec nts;
            nts.tv_sec = (time_t)ts->tv_sec;
            nts.tv_nsec = ts->tv_nsec;
            ReturnValue->Val->Integer = pthread_cond_timedwait(
                &BabyPosixConds[cslot].native, &BabyPosixMutexes[mslot].native, &nts);
        }
#endif
        BabycGuestGIL_RelockIf(gil);
    }
}

static void BabyPosixPthreadCondSignalCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    bp_cond_t *cp = (bp_cond_t *)Param[0]->Val->Pointer;
    uintptr_t slot;
    int er;
    (void)Parser; (void)NumArgs;
    er = BabyPosixCondEnsure(cp);
    if (er != 0) {
        ReturnValue->Val->Integer = er;
        return;
    }
    slot = (uintptr_t)(*cp);
    if (slot == 0 || slot >= BP_COND_MAX || !BabyPosixConds[slot].in_use) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
#ifdef WIN32
    WakeConditionVariable(&BabyPosixConds[slot].cv);
    ReturnValue->Val->Integer = 0;
#else
    ReturnValue->Val->Integer = pthread_cond_signal(&BabyPosixConds[slot].native);
#endif
}

static void BabyPosixPthreadCondBroadcastCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    bp_cond_t *cp = (bp_cond_t *)Param[0]->Val->Pointer;
    uintptr_t slot;
    int er;
    (void)Parser; (void)NumArgs;
    er = BabyPosixCondEnsure(cp);
    if (er != 0) {
        ReturnValue->Val->Integer = er;
        return;
    }
    slot = (uintptr_t)(*cp);
    if (slot == 0 || slot >= BP_COND_MAX || !BabyPosixConds[slot].in_use) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
#ifdef WIN32
    WakeAllConditionVariable(&BabyPosixConds[slot].cv);
    ReturnValue->Val->Integer = 0;
#else
    ReturnValue->Val->Integer = pthread_cond_broadcast(&BabyPosixConds[slot].native);
#endif
}

/* ---- pthread_once ---- */

struct BabyPosixOnceControl {
    int in_use;
    int done;
#ifdef WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t mu;
#endif
};

#define BP_ONCE_MAX 32
static struct BabyPosixOnceControl BabyPosixOnces[BP_ONCE_MAX];

static void BabyPosixPthreadOnceCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    /* Guest: int pthread_once(pthread_once_t *once_control, void (*init)(void));
     * once_control is an int* that starts at 0; we promote the first use to a
     * table slot stored back into *once_control. Interpreted init uses the
     * shared GIL trampoline (same as pthread_create start_routine). */
    int *once_p = (int *)Param[0]->Val->Pointer;
    struct Value *init_fv;
    int slot;
    if (Parser != NULL && Parser->pc != NULL)
        BabycGuestSetPC(Parser->pc);
    init_fv = BabycGuestResolveFuncValue(Param[1]);
    (void)NumArgs;

    BabyPosixPthreadEnsureInit();
    if (once_p == NULL) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }

    if (*once_p == 0) {
        BabyPosixPthreadLock();
        if (*once_p == 0) {
            for (slot = 1; slot < BP_ONCE_MAX; slot++) {
                if (!BabyPosixOnces[slot].in_use) break;
            }
            if (slot >= BP_ONCE_MAX) {
                BabyPosixPthreadUnlock();
                ReturnValue->Val->Integer = EAGAIN;
                return;
            }
            memset(&BabyPosixOnces[slot], 0, sizeof(BabyPosixOnces[slot]));
            BabyPosixOnces[slot].in_use = 1;
#ifdef WIN32
            InitializeCriticalSection(&BabyPosixOnces[slot].cs);
#else
            (void)pthread_mutex_init(&BabyPosixOnces[slot].mu, NULL);
#endif
            *once_p = slot;
        }
        BabyPosixPthreadUnlock();
    }
    slot = *once_p;
    if (slot <= 0 || slot >= BP_ONCE_MAX || !BabyPosixOnces[slot].in_use) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    if (init_fv == NULL) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }

    {
        int gil = BabycGuestGIL_UnlockIfHeld();
#ifdef WIN32
        EnterCriticalSection(&BabyPosixOnces[slot].cs);
#else
        (void)pthread_mutex_lock(&BabyPosixOnces[slot].mu);
#endif
        if (!BabyPosixOnces[slot].done) {
            BabycGuestGIL_Lock();
            (void)BabycGuestCall(init_fv, NULL);
            BabycGuestGIL_Unlock();
            BabyPosixOnces[slot].done = 1;
        }
#ifdef WIN32
        LeaveCriticalSection(&BabyPosixOnces[slot].cs);
#else
        (void)pthread_mutex_unlock(&BabyPosixOnces[slot].mu);
#endif
        BabycGuestGIL_RelockIf(gil);
    }
    ReturnValue->Val->Integer = 0;
}

/* ---- attr (minimal: only detachstate stored; create ignores for now) ---- */

static void BabyPosixPthreadAttrInitCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int *ap = (int *)Param[0]->Val->Pointer;
    (void)Parser; (void)NumArgs;
    if (ap == NULL) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    *ap = BP_PTHREAD_CREATE_JOINABLE;
    ReturnValue->Val->Integer = 0;
}

static void BabyPosixPthreadAttrDestroyCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int *ap = (int *)Param[0]->Val->Pointer;
    (void)Parser; (void)NumArgs;
    if (ap == NULL) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    ReturnValue->Val->Integer = 0;
}

static void BabyPosixPthreadAttrSetdetachstateCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int *ap = (int *)Param[0]->Val->Pointer;
    int state = (int)Param[1]->Val->Integer;
    (void)Parser; (void)NumArgs;
    if (ap == NULL ||
        (state != BP_PTHREAD_CREATE_JOINABLE && state != BP_PTHREAD_CREATE_DETACHED)) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    *ap = state;
    ReturnValue->Val->Integer = 0;
}

static void BabyPosixPthreadAttrGetdetachstateCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    int *ap = (int *)Param[0]->Val->Pointer;
    int *out = (int *)Param[1]->Val->Pointer;
    (void)Parser; (void)NumArgs;
    if (ap == NULL || out == NULL) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    *out = *ap;
    ReturnValue->Val->Integer = 0;
}

/* ---- pthread thread-specific data ---- */

static void BabyPosixRunKeyDtors(unsigned long tslot)
{
    int k;
    if (tslot >= (unsigned long)BP_PTHREAD_MAX)
        return;
    for (k = 1; k < BP_KEY_MAX; k++) {
        struct Value *dtor;
        void *val;
        if (!BabyPosixKeys[k].in_use)
            continue;
        dtor = BabyPosixKeys[k].dtor;
        val = BabyPosixKeys[k].value[tslot];
        BabyPosixKeys[k].value[tslot] = NULL;
        if (dtor != NULL && val != NULL)
            (void)BabycGuestCall(dtor, val);
    }
}

static void BabyPosixPthreadKeyCreateCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    unsigned int *kp = (unsigned int *)Param[0]->Val->Pointer;
    struct Value *dtor;
    int i;
    (void)NumArgs;
    if (Parser != NULL && Parser->pc != NULL)
        BabycGuestSetPC(Parser->pc);
    if (kp == NULL) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    dtor = BabycGuestResolveFuncValue(Param[1]);
    BabyPosixPthreadEnsureInit();
    BabyPosixPthreadLock();
    for (i = 1; i < BP_KEY_MAX; i++) {
        if (!BabyPosixKeys[i].in_use) {
            memset(&BabyPosixKeys[i], 0, sizeof(BabyPosixKeys[i]));
            BabyPosixKeys[i].in_use = 1;
            BabyPosixKeys[i].dtor = dtor;
            *kp = (unsigned int)i;
            BabyPosixPthreadUnlock();
            ReturnValue->Val->Integer = 0;
            return;
        }
    }
    BabyPosixPthreadUnlock();
    ReturnValue->Val->Integer = EAGAIN;
}

static void BabyPosixPthreadKeyDeleteCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    unsigned int key = (unsigned int)Param[0]->Val->Integer;
    (void)Parser; (void)NumArgs;
    BabyPosixPthreadEnsureInit();
    if (key == 0 || key >= BP_KEY_MAX || !BabyPosixKeys[key].in_use) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    BabyPosixPthreadLock();
    BabyPosixKeys[key].in_use = 0;
    BabyPosixKeys[key].dtor = NULL;
    memset(BabyPosixKeys[key].value, 0, sizeof(BabyPosixKeys[key].value));
    BabyPosixPthreadUnlock();
    ReturnValue->Val->Integer = 0;
}

static void BabyPosixPthreadSetspecificCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    unsigned int key = (unsigned int)Param[0]->Val->Integer;
    void *val;
    unsigned long slot;
    (void)Parser; (void)NumArgs;
    BabyPosixPthreadEnsureInit();
    if (key == 0 || key >= BP_KEY_MAX || !BabyPosixKeys[key].in_use) {
        ReturnValue->Val->Integer = EINVAL;
        return;
    }
    val = BabyPosixGuestPtr(Param[1]);
    slot = BabyPosixTlsSlot;
    if (slot >= (unsigned long)BP_PTHREAD_MAX)
        slot = 0;
    BabyPosixKeys[key].value[slot] = val;
    ReturnValue->Val->Integer = 0;
}

static void BabyPosixPthreadGetspecificCall(struct ParseState *Parser,
    struct Value *ReturnValue, struct Value **Param, int NumArgs)
{
    unsigned int key = (unsigned int)Param[0]->Val->Integer;
    unsigned long slot;
    (void)Parser; (void)NumArgs;
    BabyPosixPthreadEnsureInit();
    if (key == 0 || key >= BP_KEY_MAX || !BabyPosixKeys[key].in_use) {
        ReturnValue->Val->Pointer = NULL;
        return;
    }
    slot = BabyPosixTlsSlot;
    if (slot >= (unsigned long)BP_PTHREAD_MAX)
        slot = 0;
    ReturnValue->Val->Pointer = BabyPosixKeys[key].value[slot];
}

/* ---- defs + registration table ---- */

const char BabyPosixPthreadDefs[] =
    "typedef unsigned long pthread_t;\n"
    "typedef unsigned long pthread_mutex_t;\n"
    "typedef unsigned long pthread_cond_t;\n"
    "typedef int pthread_once_t;\n"
    "typedef int pthread_attr_t;\n"
    "typedef int pthread_mutexattr_t;\n"
    "typedef int pthread_condattr_t;\n"
    "typedef unsigned int pthread_key_t;\n"
    "typedef void *(*babyposix_thread_fn)(void *);\n"
    "#include <sys/time.h>\n"
    "#define PTHREAD_ONCE_INIT 0\n"
    "#define PTHREAD_CREATE_JOINABLE 0\n"
    "#define PTHREAD_CREATE_DETACHED 1\n"
    "#define PTHREAD_MUTEX_INITIALIZER 0\n"
    "#define PTHREAD_MUTEX_NORMAL 0\n"
    "#define PTHREAD_MUTEX_RECURSIVE 1\n"
    "#define PTHREAD_MUTEX_ERRORCHECK 2\n"
    "#define PTHREAD_MUTEX_DEFAULT 0\n"
    "#define PTHREAD_COND_INITIALIZER 0\n";

struct LibraryFunction BabyPosixPthreadFunctions[] = {
    { BabyPosixPthreadCreateCall,
      "int pthread_create(pthread_t *, void *, void *, void *);" },
    { BabyPosixPthreadJoinCall,
      "int pthread_join(pthread_t, void **);" },
    { BabyPosixPthreadDetachCall,
      "int pthread_detach(pthread_t);" },
    { BabyPosixPthreadExitCall,
      "void pthread_exit(void *);" },
    { BabyPosixPthreadSelfCall,
      "pthread_t pthread_self(void);" },
    { BabyPosixPthreadEqualCall,
      "int pthread_equal(pthread_t, pthread_t);" },

    { BabyPosixPthreadMutexInitCall,
      "int pthread_mutex_init(pthread_mutex_t *, pthread_mutexattr_t *);" },
    { BabyPosixPthreadMutexDestroyCall,
      "int pthread_mutex_destroy(pthread_mutex_t *);" },
    { BabyPosixPthreadMutexLockCall,
      "int pthread_mutex_lock(pthread_mutex_t *);" },
    { BabyPosixPthreadMutexUnlockCall,
      "int pthread_mutex_unlock(pthread_mutex_t *);" },
    { BabyPosixPthreadMutexTrylockCall,
      "int pthread_mutex_trylock(pthread_mutex_t *);" },
    { BabyPosixPthreadMutexattrInitCall,
      "int pthread_mutexattr_init(pthread_mutexattr_t *);" },
    { BabyPosixPthreadMutexattrDestroyCall,
      "int pthread_mutexattr_destroy(pthread_mutexattr_t *);" },
    { BabyPosixPthreadMutexattrSettypeCall,
      "int pthread_mutexattr_settype(pthread_mutexattr_t *, int);" },
    { BabyPosixPthreadMutexattrGettypeCall,
      "int pthread_mutexattr_gettype(pthread_mutexattr_t *, int *);" },

    { BabyPosixPthreadCondInitCall,
      "int pthread_cond_init(pthread_cond_t *, pthread_condattr_t *);" },
    { BabyPosixPthreadCondDestroyCall,
      "int pthread_cond_destroy(pthread_cond_t *);" },
    { BabyPosixPthreadCondWaitCall,
      "int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);" },
    { BabyPosixPthreadCondTimedwaitCall,
      "int pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, struct timespec *);" },
    { BabyPosixPthreadCondSignalCall,
      "int pthread_cond_signal(pthread_cond_t *);" },
    { BabyPosixPthreadCondBroadcastCall,
      "int pthread_cond_broadcast(pthread_cond_t *);" },

    { BabyPosixPthreadOnceCall,
      "int pthread_once(pthread_once_t *, void *);" },

    { BabyPosixPthreadKeyCreateCall,
      "int pthread_key_create(pthread_key_t *, void *);" },
    { BabyPosixPthreadKeyDeleteCall,
      "int pthread_key_delete(pthread_key_t);" },
    { BabyPosixPthreadSetspecificCall,
      "int pthread_setspecific(pthread_key_t, void *);" },
    { BabyPosixPthreadGetspecificCall,
      "void *pthread_getspecific(pthread_key_t);" },

    { BabyPosixPthreadAttrInitCall,
      "int pthread_attr_init(pthread_attr_t *);" },
    { BabyPosixPthreadAttrDestroyCall,
      "int pthread_attr_destroy(pthread_attr_t *);" },
    { BabyPosixPthreadAttrSetdetachstateCall,
      "int pthread_attr_setdetachstate(pthread_attr_t *, int);" },
    { BabyPosixPthreadAttrGetdetachstateCall,
      "int pthread_attr_getdetachstate(pthread_attr_t *, int *);" },

    { NULL, NULL }
};

void BabyPosixPthreadSetupFunc(Picoc *pc)
{
    BabycGuestSetPC(pc);
    BabyPosixPthreadEnsureInit();
}
