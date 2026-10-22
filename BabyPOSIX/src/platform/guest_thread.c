#include "guest_thread.h"
#include "../picoc.h"
#include "../ir_runtime.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef WIN32
#include <windows.h>
static Picoc * volatile BabycGuestPC;
#else
#include <pthread.h>
#include <stdatomic.h>
static _Atomic(Picoc *) BabycGuestPC;
#endif

/* The GIL is process-wide, but ownership is thread-local. Keeping the owner
 * and held flag in ordinary globals creates a data race: a second thread can
 * overwrite them between the first thread's lock and unlock, and
 * UnlockIfHeld() can then release a lock it does not own. TLS also makes the
 * lock/unlock contract independent of the host thread-ID representation. */
#ifdef WIN32
static CRITICAL_SECTION BabycGuestGIL;
static INIT_ONCE BabycGuestGILOnce = INIT_ONCE_STATIC_INIT;
static __declspec(thread) int BabycGuestGILHeld;

static BOOL CALLBACK BabycGuestGILInitialize(PINIT_ONCE once, PVOID parameter,
                                              PVOID *context)
{
    (void)once;
    (void)parameter;
    (void)context;
    InitializeCriticalSection(&BabycGuestGIL);
    return TRUE;
}

void BabycGuestGIL_Lock(void)
{
    if (!InitOnceExecuteOnce(&BabycGuestGILOnce, BabycGuestGILInitialize,
                             NULL, NULL))
        abort(); /* Never execute guest code without its interpreter lock. */
    EnterCriticalSection(&BabycGuestGIL);
    BabycGuestGILHeld = 1;
}
void BabycGuestGIL_Unlock(void)
{
    if (!BabycGuestGILHeld)
        return;
    BabycGuestGILHeld = 0;
    LeaveCriticalSection(&BabycGuestGIL);
}
int BabycGuestGIL_UnlockIfHeld(void)
{
    if (!BabycGuestGILHeld)
        return 0;
    BabycGuestGILHeld = 0;
    LeaveCriticalSection(&BabycGuestGIL);
    return 1;
}
void BabycGuestGIL_RelockIf(int was_held)
{
    if (was_held)
        BabycGuestGIL_Lock();
}
#else
static pthread_mutex_t BabycGuestGIL = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int BabycGuestGILHeld;
void BabycGuestGIL_Lock(void)
{
    if (pthread_mutex_lock(&BabycGuestGIL) != 0)
        abort(); /* Never execute guest code without its interpreter lock. */
    BabycGuestGILHeld = 1;
}
void BabycGuestGIL_Unlock(void)
{
    if (!BabycGuestGILHeld)
        return;
    BabycGuestGILHeld = 0;
    (void)pthread_mutex_unlock(&BabycGuestGIL);
}
int BabycGuestGIL_UnlockIfHeld(void)
{
    if (!BabycGuestGILHeld)
        return 0;
    BabycGuestGILHeld = 0;
    (void)pthread_mutex_unlock(&BabycGuestGIL);
    return 1;
}
void BabycGuestGIL_RelockIf(int was_held)
{
    if (was_held)
        BabycGuestGIL_Lock();
}
#endif

void BabycGuestSetPC(Picoc *pc)
{
#ifdef WIN32
    (void)InterlockedExchangePointer((PVOID volatile *)&BabycGuestPC, pc);
#else
    atomic_store_explicit(&BabycGuestPC, pc, memory_order_release);
#endif
}

Picoc *BabycGuestGetPC(void)
{
#ifdef WIN32
    return (Picoc *)InterlockedCompareExchangePointer(
        (PVOID volatile *)&BabycGuestPC, NULL, NULL);
#else
    return atomic_load_explicit(&BabycGuestPC, memory_order_acquire);
#endif
}

void *BabycGuestValueAsPointer(struct Value *value, int reject_low_addresses)
{
    void *p;
    uintptr_t integer_value;
    if (value == NULL || value->Val == NULL || value->Val->Integer == 0)
        return NULL;
    integer_value = (uintptr_t)(unsigned long)value->Val->Integer;
    /* Small explicit integer-to-pointer casts (e.g. (void *)31) share the
     * union with stale high pointer bits. Resolve them from the integer member
     * before consulting Pointer; real object pointers are not in this range. */
    if (integer_value < 4096u)
        return reject_low_addresses ? NULL : (void *)integer_value;
    p = value->Val->Pointer;
    if (p == NULL)
        p = (void *)integer_value;
    if (reject_low_addresses && p != NULL && (uintptr_t)p < 4096u)
        return NULL;
    return p;
}

struct Value *BabycGuestResolveFuncValue(struct Value *StartParam)
{
    struct Value *fv;
    if (StartParam == NULL || StartParam->Val == NULL)
        return NULL;
    if (StartParam->Typ != NULL && StartParam->Typ->Base == TypeFunction)
        return StartParam;
    if (StartParam->Typ != NULL && StartParam->Typ->Base == TypePointer
        && StartParam->Val->Pointer != NULL) {
        fv = (struct Value *)StartParam->Val->Pointer;
        if (fv != NULL && fv->Typ != NULL && fv->Typ->Base == TypeFunction)
            return fv;
        if (fv != NULL && fv->Typ != NULL && fv->Typ->Base == TypePointer
            && fv->Typ->FromType != NULL && fv->Typ->FromType->Base == TypeFunction
            && fv->Val != NULL && fv->Val->Pointer != NULL)
            return (struct Value *)fv->Val->Pointer;
    }
    if (StartParam->Val->Integer != 0) {
        fv = (struct Value *)(uintptr_t)(unsigned long)StartParam->Val->Integer;
        if (fv != NULL && fv->Typ != NULL && fv->Typ->Base == TypeFunction)
            return fv;
    }
    if (StartParam->Val->LongInteger != 0) {
        fv = (struct Value *)(uintptr_t)(unsigned long)StartParam->Val->LongInteger;
        if (fv != NULL && fv->Typ != NULL && fv->Typ->Base == TypeFunction)
            return fv;
    }
    return NULL;
}

struct BabycGuestSnap {
    char *heap_stack_top;
    void *stack_frame;
    struct StackFrame *top_stack_frame;
    int ir_call_depth;
    int lin_frame_depth;
    IrLinThreadContext lin_context;
    jmp_buf exit_buf;
    int exit_point_set;
};

static void BabycGuestSnapCapture(Picoc *pc, struct BabycGuestSnap *s)
{
    s->heap_stack_top = pc->HeapStackTop;
    s->stack_frame = pc->StackFrame;
    s->top_stack_frame = pc->TopStackFrame;
    s->ir_call_depth = IrCallDepth;
    s->lin_frame_depth = LinFrameDepth();
    IrLinThreadContextCapture(&s->lin_context);
    memcpy(s->exit_buf, pc->PicocExitBuf, sizeof(jmp_buf));
    s->exit_point_set = pc->PicocExitPointSet;
}

static void BabycGuestSnapUnwind(Picoc *pc, const struct BabycGuestSnap *s)
{
    struct ValueProxyEntry **link;
    LinFrameRestoreDepth(s->lin_frame_depth);
    while (IrCallDepth > s->ir_call_depth)
        IrFramePop();
    link = &pc->ValueProxyList;
    while (*link != NULL) {
        struct ValueProxyEntry *entry = *link;
        if ((char *)entry->Owner >= s->heap_stack_top) {
            *link = entry->Next;
            HeapFreeMem(pc, entry);
        } else
            link = &entry->Next;
    }
    pc->HeapStackTop = s->heap_stack_top;
    pc->StackFrame = s->stack_frame;
    pc->TopStackFrame = s->top_stack_frame;
    IrLinThreadContextRestore(&s->lin_context);
}

static void BabycGuestSnapRestoreExit(Picoc *pc, const struct BabycGuestSnap *s)
{
    memcpy(pc->PicocExitBuf, s->exit_buf, sizeof(jmp_buf));
    pc->PicocExitPointSet = s->exit_point_set;
}

void *BabycGuestCall(struct Value *FuncValue, void *arg)
{
    Picoc *pc = BabycGuestGetPC();
    struct ParseState Parser;
    struct Value *ParamArray[2];
    struct Value ArgVal;
    struct Value RetVal;
    union AnyValue ArgAny;
    union AnyValue RetAny;
    int nparams;
    int count;
    int oldScope;
    struct BabycGuestSnap snap;
    void *result = NULL;

    if (pc == NULL || FuncValue == NULL || FuncValue->Val == NULL)
        return NULL;
    if (FuncValue->Val->FuncDef.Body.Pos == NULL
        && FuncValue->Val->FuncDef.BodyIr == NULL
        && FuncValue->Val->FuncDef.Intrinsic == NULL)
        return NULL;

    memset(&Parser, 0, sizeof(Parser));
    LexInitParser(&Parser, pc,
        FuncValue->Val->FuncDef.Body.SourceText
            ? FuncValue->Val->FuncDef.Body.SourceText : "",
        (void *)FuncValue->Val->FuncDef.Body.Pos,
        FuncValue->Val->FuncDef.Body.FileName
            ? FuncValue->Val->FuncDef.Body.FileName : (char *)"guest_thread",
        TRUE, FALSE);
    Parser.Mode = RunModeRun;

    memset(&ArgAny, 0, sizeof(ArgAny));
    ArgAny.Pointer = arg;
    memset(&ArgVal, 0, sizeof(ArgVal));
    ArgVal.Typ = pc->VoidPtrType ? pc->VoidPtrType : pc->CharPtrType;
    ArgVal.Val = &ArgAny;
    ArgVal.IsLValue = FALSE;
    ArgVal.IsInitialized = TRUE;

    memset(&RetAny, 0, sizeof(RetAny));
    memset(&RetVal, 0, sizeof(RetVal));
    RetVal.Typ = FuncValue->Val->FuncDef.ReturnType
        ? FuncValue->Val->FuncDef.ReturnType : &pc->VoidType;
    RetVal.Val = &RetAny;

    nparams = FuncValue->Val->FuncDef.NumParams;
    ParamArray[0] = &ArgVal;
    ParamArray[1] = NULL;

    BabycGuestSnapCapture(pc, &snap);
    if (PicocPlatformSetExitPoint(pc) != 0) {
        int ev = pc->PicocExitValue;
        BabycGuestSnapUnwind(pc, &snap);
        BabycGuestSnapRestoreExit(pc, &snap);
        if (ev == 0)
            return NULL;
        pc->PicocExitPointSet = 0;
        PlatformExit(pc, ev);
    }

    if (FuncValue->Val->FuncDef.Intrinsic != NULL) {
        PicocCallIntrinsic(&Parser, FuncValue->Val->FuncDef.Intrinsic,
            &RetVal, ParamArray, nparams > 0 ? 1 : 0, "guest_start");
        BabycGuestSnapRestoreExit(pc, &snap);
        return RetVal.Val->Pointer;
    }

    oldScope = Parser.ScopeID;
    VariableStackFrameAdd(&Parser, "guest_start", 0);
    if (pc->TopStackFrame != NULL) {
        pc->TopStackFrame->NumParams = nparams > 0 ? 1 : 0;
        pc->TopStackFrame->ReturnValue = &RetVal;
    }
    for (count = 0; count < nparams; count++) {
        struct Value *src = (count == 0) ? &ArgVal : NULL;
        if (src != NULL && FuncValue->Val->FuncDef.ParamName != NULL
            && FuncValue->Val->FuncDef.ParamName[count] != NULL)
            VariableDefine(pc, &Parser, FuncValue->Val->FuncDef.ParamName[count],
                src, NULL, TRUE);
    }
    Parser.ScopeID = oldScope + 1;
    Parser.Mode = RunModeRun;

    if (FuncValue->Val->FuncDef.BodyIr != NULL) {
        IrExecuteFunctionBody(&Parser, FuncValue, ParamArray, nparams > 0 ? 1 : 0);
    } else if (FuncValue->Val->FuncDef.Body.Pos != NULL) {
        struct ParseState FuncParser;
        ParserCopy(&FuncParser, &FuncValue->Val->FuncDef.Body);
        FuncParser.Mode = RunModeRun;
        if (ParseStatement(&FuncParser, TRUE) != ParseResultOk) {
            VariableStackFramePop(&Parser);
            BabycGuestSnapRestoreExit(pc, &snap);
            return NULL;
        }
        Parser.Mode = FuncParser.Mode;
    }
    VariableStackFramePop(&Parser);
    if (RetVal.Typ != NULL && RetVal.Typ->Base == TypePointer)
        result = RetVal.Val->Pointer;
    else if (RetVal.Typ != NULL && RetVal.Typ->Base >= TypeInt
             && RetVal.Typ->Base <= TypeUnsignedLongLong)
        result = (void *)(uintptr_t)(unsigned long)RetVal.Val->Integer;
    BabycGuestSnapRestoreExit(pc, &snap);
    return result;
}

int BabycGuestCallInt2(struct Value *FuncValue, void *a, void *b)
{
    Picoc *pc = BabycGuestGetPC();
    struct ParseState Parser;
    struct Value *ParamArray[3];
    struct Value ArgA, ArgB;
    struct Value RetVal;
    union AnyValue AnyA, AnyB, RetAny;
    int nparams;
    int count;
    int oldScope;
    int result = 0;
    struct ValueType *PtrTyp;
    struct BabycGuestSnap snap;

    if (pc == NULL || FuncValue == NULL || FuncValue->Val == NULL)
        return 0;
    if (FuncValue->Val->FuncDef.Body.Pos == NULL
        && FuncValue->Val->FuncDef.BodyIr == NULL
        && FuncValue->Val->FuncDef.Intrinsic == NULL)
        return 0;

    PtrTyp = pc->VoidPtrType ? pc->VoidPtrType : pc->CharPtrType;

    memset(&Parser, 0, sizeof(Parser));
    LexInitParser(&Parser, pc,
        FuncValue->Val->FuncDef.Body.SourceText
            ? FuncValue->Val->FuncDef.Body.SourceText : "",
        (void *)FuncValue->Val->FuncDef.Body.Pos,
        FuncValue->Val->FuncDef.Body.FileName
            ? FuncValue->Val->FuncDef.Body.FileName : (char *)"guest_compar",
        TRUE, FALSE);
    Parser.Mode = RunModeRun;

    memset(&AnyA, 0, sizeof(AnyA));
    AnyA.Pointer = a;
    memset(&ArgA, 0, sizeof(ArgA));
    ArgA.Typ = PtrTyp;
    ArgA.Val = &AnyA;
    ArgA.IsLValue = FALSE;
    ArgA.IsInitialized = TRUE;

    memset(&AnyB, 0, sizeof(AnyB));
    AnyB.Pointer = b;
    memset(&ArgB, 0, sizeof(ArgB));
    ArgB.Typ = PtrTyp;
    ArgB.Val = &AnyB;
    ArgB.IsLValue = FALSE;
    ArgB.IsInitialized = TRUE;

    memset(&RetAny, 0, sizeof(RetAny));
    memset(&RetVal, 0, sizeof(RetVal));
    RetVal.Typ = FuncValue->Val->FuncDef.ReturnType
        ? FuncValue->Val->FuncDef.ReturnType : &pc->IntType;
    RetVal.Val = &RetAny;

    nparams = FuncValue->Val->FuncDef.NumParams;
    ParamArray[0] = &ArgA;
    ParamArray[1] = &ArgB;
    ParamArray[2] = NULL;

    BabycGuestSnapCapture(pc, &snap);
    if (PicocPlatformSetExitPoint(pc) != 0) {
        int ev = pc->PicocExitValue;
        BabycGuestSnapUnwind(pc, &snap);
        BabycGuestSnapRestoreExit(pc, &snap);
        if (ev == 0)
            return 0;
        PlatformExit(pc, ev);
    }

    if (FuncValue->Val->FuncDef.Intrinsic != NULL) {
        PicocCallIntrinsic(&Parser, FuncValue->Val->FuncDef.Intrinsic,
            &RetVal, ParamArray, nparams >= 2 ? 2 : nparams, "guest_compar");
        BabycGuestSnapRestoreExit(pc, &snap);
        return RetVal.Val->Integer;
    }

    oldScope = Parser.ScopeID;
    VariableStackFrameAdd(&Parser, "guest_compar", 0);
    if (pc->TopStackFrame != NULL) {
        pc->TopStackFrame->NumParams = nparams >= 2 ? 2 : nparams;
        pc->TopStackFrame->ReturnValue = &RetVal;
    }
    for (count = 0; count < nparams && count < 2; count++) {
        struct Value *src = (count == 0) ? &ArgA : &ArgB;
        if (FuncValue->Val->FuncDef.ParamName != NULL
            && FuncValue->Val->FuncDef.ParamName[count] != NULL)
            VariableDefine(pc, &Parser, FuncValue->Val->FuncDef.ParamName[count],
                src, NULL, TRUE);
    }
    Parser.ScopeID = oldScope + 1;
    Parser.Mode = RunModeRun;

    if (FuncValue->Val->FuncDef.BodyIr != NULL) {
        IrExecuteFunctionBody(&Parser, FuncValue, ParamArray,
            nparams >= 2 ? 2 : nparams);
    } else if (FuncValue->Val->FuncDef.Body.Pos != NULL) {
        struct ParseState FuncParser;
        ParserCopy(&FuncParser, &FuncValue->Val->FuncDef.Body);
        FuncParser.Mode = RunModeRun;
        if (ParseStatement(&FuncParser, TRUE) != ParseResultOk) {
            VariableStackFramePop(&Parser);
            BabycGuestSnapRestoreExit(pc, &snap);
            return 0;
        }
        Parser.Mode = FuncParser.Mode;
    }
    VariableStackFramePop(&Parser);

    if (RetVal.Typ != NULL && RetVal.Typ->Base >= TypeInt
        && RetVal.Typ->Base <= TypeUnsignedLongLong)
        result = RetVal.Val->Integer;
    else if (RetVal.Typ != NULL && RetVal.Typ->Base == TypeLongLong)
        result = (int)RetVal.Val->LongLongInteger;
    BabycGuestSnapRestoreExit(pc, &snap);
    return result;
}
