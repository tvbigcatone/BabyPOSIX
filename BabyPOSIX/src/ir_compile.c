/* ir_compile.c — Body-IR compilation (parser → IR), separated from parse.c / ir_exec.c */
#include "interpreter.h"
#include "ir.h"
#include "ir_private.h"
void ParseTypedef(struct ParseState *Parser);
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cstdlib/printf_format.h"

/* Track allocations made while compiling a Body-IR function.  Compilation can
 * reject a construct after building only part of its IR tree; recursive tree
 * destructors cannot reach every unattached temporary in that case.  The
 * tracker records each allocation independently, so failure cleanup can free
 * exactly the allocations from the failed attempt without traversing possibly
 * incomplete graphs.  Successful compilation merely drops the records. */
typedef struct IrCompileAllocRecord
{
    void *Ptr;
    struct IrCompileAllocRecord *Next;
} IrCompileAllocRecord;
static IrCompileAllocRecord *IrCompileAllocTop = NULL;
static int IrCompileTrackActive = 0;

static void *IrCompileTrackAllocation(void *Ptr)
{
    IrCompileAllocRecord *R;
    if (Ptr == NULL || !IrCompileTrackActive)
        return Ptr;
    R = (IrCompileAllocRecord *)malloc(sizeof(*R));
    if (R == NULL)
    {
        free(Ptr);
        return NULL;
    }
    R->Ptr = Ptr;
    R->Next = IrCompileAllocTop;
    IrCompileAllocTop = R;
    return Ptr;
}

static void *IrCompileTrackedMalloc(size_t Size)
{
    return IrCompileTrackAllocation(malloc(Size));
}
static void *IrCompileTrackedCalloc(size_t Count, size_t Size)
{
    return IrCompileTrackAllocation(calloc(Count, Size));
}
static void *IrCompileTrackedRealloc(void *Ptr, size_t Size)
{
    IrCompileAllocRecord *R;
    void *NewPtr;
    if (Ptr == NULL)
        return IrCompileTrackedMalloc(Size);
    for (R = IrCompileAllocTop; R != NULL; R = R->Next)
        if (R->Ptr == Ptr)
            break;
    NewPtr = realloc(Ptr, Size);
    if (NewPtr != NULL && R != NULL)
        R->Ptr = NewPtr;
    return NewPtr;
}
static void IrCompileTrackedFree(void *Ptr)
{
    IrCompileAllocRecord **Link = &IrCompileAllocTop;
    while (*Link != NULL)
    {
        IrCompileAllocRecord *R = *Link;
        if (R->Ptr == Ptr)
        {
            *Link = R->Next;
            free(R);
            break;
        }
        Link = &R->Next;
    }
    free(Ptr);
}
static void IrCompileTrackEnd(IrCompileAllocRecord *Mark, int KeepAllocations)
{
    while (IrCompileAllocTop != Mark)
    {
        IrCompileAllocRecord *R = IrCompileAllocTop;
        if (R == NULL)
            break; /* defensive: never walk beyond the saved checkpoint */
        IrCompileAllocTop = R->Next;
        if (!KeepAllocations)
            free(R->Ptr);
        free(R);
    }
}

/* Keep all IR compiler allocations in the same tracked domain.  The helper
 * implementations above intentionally call the C allocator directly. */
#define malloc(Size) IrCompileTrackedMalloc(Size)
#define calloc(Count, Size) IrCompileTrackedCalloc((Count), (Size))
#define realloc(Ptr, Size) IrCompileTrackedRealloc((Ptr), (Size))
#define free(Ptr) IrCompileTrackedFree(Ptr)

extern int IrInLoopBody;
void ParseMacroDefinition(struct ParseState *Parser);

/* Diagnostics: BodyIr success vs token fallback */
int PicocIrBodyCompiled = 0;
int PicocIrBodyTokenFallback = 0;
/* Last compile-failure reason (for --analyze --why) */
static const char *IrCompileWhy = NULL;
static int IrCompileLeadingConst = 0;
static char **IrCompileMacroArgNames = NULL;
static IrExpr **IrCompileMacroArgExprs = NULL;
static int IrCompileMacroArgCount = 0;
static int IrCompileMacroDepth = 0;
void IrCompileClearWhy(void) { IrCompileWhy = NULL; }
const char *IrCompileGetWhy(void) { return IrCompileWhy ? IrCompileWhy : ""; }
#define IR_FAIL(msg) do { IrCompileWhy = (msg); return NULL; } while (0)

/* Analyze log: last few compiled/fallback functions (name + line + IR? + why) */
#define BABYC_ANALYZE_MAX 256
typedef struct {
    char Name[64];
    const char *File;
    int Line;
    int IsIr;
    char Why[96];
} BabycAnalyzeEntry;
static BabycAnalyzeEntry BabycAnalyzeLog[BABYC_ANALYZE_MAX];
static int BabycAnalyzeCount = 0;

void BabycAnalyzeReset(void) { BabycAnalyzeCount = 0; }
int BabycAnalyzeGetCount(void) { return BabycAnalyzeCount; }
void BabycAnalyzeGet(int i, const char **Name, const char **File, int *Line, int *IsIr)
{
    if (i < 0 || i >= BabycAnalyzeCount) { *Name = "?"; *File = "?"; *Line = 0; *IsIr = 0; return; }
    *Name = BabycAnalyzeLog[i].Name;
    *File = BabycAnalyzeLog[i].File ? BabycAnalyzeLog[i].File : "?";
    *Line = BabycAnalyzeLog[i].Line;
    *IsIr = BabycAnalyzeLog[i].IsIr;
}
void BabycAnalyzeGetWhy(int i, const char **Why)
{
    if (i < 0 || i >= BabycAnalyzeCount) { *Why = ""; return; }
    *Why = BabycAnalyzeLog[i].Why;
}
void BabycAnalyzeNote(const char *Name, const char *File, int Line, int IsIr)
{
    if (BabycAnalyzeCount >= BABYC_ANALYZE_MAX) return;
    {
        BabycAnalyzeEntry *e = &BabycAnalyzeLog[BabycAnalyzeCount++];
        if (Name) {
            strncpy(e->Name, Name, sizeof(e->Name)-1);
            e->Name[sizeof(e->Name)-1] = 0;
        } else {
            e->Name[0] = '?'; e->Name[1] = 0;
        }
        e->File = File;
        e->Line = Line;
        e->IsIr = IsIr;
        e->Why[0] = 0;
        if (!IsIr && IrCompileWhy != NULL) {
            strncpy(e->Why, IrCompileWhy, sizeof(e->Why)-1);
            e->Why[sizeof(e->Why)-1] = 0;
        }
        IrCompileWhy = NULL;
    }
}

static struct ParseState *IrAllocParserPos = NULL;

/* Active function params while compiling BodyIr (compile-time name → slot). */
static char **IrCompileParamNames = NULL;
static struct ValueType **IrCompileParamTypes = NULL;
static int IrCompileNumParams = 0;
static struct ValueType *IrCompileReturnType = NULL;
#define IR_COMPILE_MAX_LOCALS 128
#define IR_COMPILE_SCOPE_MAX 32
static int IrCompileScopeStack[IR_COMPILE_SCOPE_MAX];
static int IrCompileScopeDepth;
static char *IrCompileLocalNames[IR_COMPILE_MAX_LOCALS];
static int IrCompileLocalElemSize[IR_COMPILE_MAX_LOCALS];
static int IrCompileLocalArraySize[IR_COMPILE_MAX_LOCALS];
static int IrCompileLocalScale[IR_COMPILE_MAX_LOCALS];
/* T *a[N]: pointee type T of each element (Decl DeclElemPointee). */
static struct ValueType *IrCompileLocalElemPointee[IR_COMPILE_MAX_LOCALS];
/* One setter for every T *a[N] declaration path (int/char, typedef, double). */
static void IrCompileSetElemPointee(IrStmt *One, int Slot, struct ValueType *PT)
{
    if (One == NULL || One->DeclNDims > 1)
        return; /* T *a[N][M]: rows are arrays, not pointer elements */
    One->DeclElemPointee = PT;
    if (Slot >= 0 && Slot < IR_COMPILE_MAX_LOCALS)
        IrCompileLocalElemPointee[Slot] = PT;
}
static int IrCompileLocalIsConst[IR_COMPILE_MAX_LOCALS];
static unsigned char IrCompileLocalKind[IR_COMPILE_MAX_LOCALS];
static unsigned char IrCompileLocalElemIsPtr[IR_COMPILE_MAX_LOCALS];
static unsigned char IrCompileLocalUnsigned[IR_COMPILE_MAX_LOCALS];
static struct ValueType *IrCompileLocalFromType[IR_COMPILE_MAX_LOCALS];
static struct ValueType *IrCompileLocalArrayType[IR_COMPILE_MAX_LOCALS];
/* Full declared type of a multi-level struct/union pointer local (T **p).
 * The slot model keeps the aggregate pointee in LocalFromType for member
 * resolution, which loses the pointer depth; the canonical annotation reads
 * this exact type instead so *p of T **p is a pointer, not a T object. */
static struct ValueType *IrCompileLocalCanonType[IR_COMPILE_MAX_LOCALS];
static int IrCompileNumLocals = 0;
/* Front-end type of a non-array pointer local: its full declared type (Base
 * wrapped in Stars pointer layers), the same representation the integer
 * declaration path records. DeclFromType keeps the lowering convention
 * (pointee / first-level pointee) and is not changed. Without this, the
 * dereference annotation had only the pointee metadata and guessed the
 * pointer depth from the slot stride, so *p of a double * (stride 8 ==
 * sizeof(void *) on 64-bit) or of a pointer typedef was typed as a pointer. */
static void IrCompileStampLocalPointerType(struct ParseState *P, int Slot,
                                           struct ValueType *Base, int Stars)
{
    struct ValueType *T = Base;
    int i;
    if (P == NULL || P->pc == NULL || T == NULL
        || Slot < 0 || Slot >= IR_COMPILE_MAX_LOCALS)
        return;
    for (i = 0; i < Stars && T != NULL; i++)
        T = TypeGetMatching(P->pc, P, T, TypePointer, 0, P->pc->StrEmpty, TRUE);
    if (T != NULL && T->Base == TypePointer)
        IrCompileLocalFromType[Slot] = T;
}
static int IrCompilingFunctionBody = 0;
static Picoc *IrCompilePc = NULL;


static int IrInternLocal(const char *Name)
{
    if (Name == NULL || IrCompileNumLocals >= IR_COMPILE_MAX_LOCALS)
        return -1;
    /* Allow same name again: nested-block shadow gets a fresh slot.
     * IrBindIdentSlots resolves Ident → newest active slot (scan from end).
     * Block exit clears LocalNames[slot] so outer code rebinds to outer slot. */
    IrCompileLocalNames[IrCompileNumLocals] = (char *)Name;
    IrCompileLocalElemSize[IrCompileNumLocals] = 0;
    IrCompileLocalArraySize[IrCompileNumLocals] = 0;
    IrCompileLocalScale[IrCompileNumLocals] = 0;
    IrCompileLocalElemPointee[IrCompileNumLocals] = NULL;
    IrCompileLocalIsConst[IrCompileNumLocals] = 0;
    IrCompileLocalKind[IrCompileNumLocals] = 0;
    IrCompileLocalElemIsPtr[IrCompileNumLocals] = 0;
    IrCompileLocalUnsigned[IrCompileNumLocals] = 0;
    IrCompileLocalFromType[IrCompileNumLocals] = NULL;
    IrCompileLocalArrayType[IrCompileNumLocals] = NULL;
    IrCompileLocalCanonType[IrCompileNumLocals] = NULL;
    return IrCompileNumLocals++;
}

/* B.1 local/param slots + Fase C global bind: CachedVal / CachedFunc at compile
 * when the symbol is already in GlobalTable. Miss stays NULL → runtime fill. */
static void IrBindIdentSlots(struct ParseState *P, IrExpr *E)
{
    int i;
    if (E == NULL)
        return;
    E->ParamSlot = -1;
    E->LocalSlot = -1;
    if (E->Ident == NULL)
        return;
    for (i = IrCompileNumLocals - 1; i >= 0; i--)
        if (IrCompileLocalNames[i] != NULL && strcmp(IrCompileLocalNames[i], E->Ident) == 0)
        {
            E->LocalSlot = i;
            return;
        }
    if (IrCompileParamNames != NULL)
    {
        for (i = 0; i < IrCompileNumParams; i++)
            if (IrCompileParamNames[i] != NULL &&
                strcmp(IrCompileParamNames[i], E->Ident) == 0)
            {
                E->ParamSlot = i;
                return;
            }
    }
    /* Fase C: bind data globals (array/scalar/ptr).
     * B.3: also bind Intrinsic callees (printf/scanf/...) — stable Value*,
     * never redefined. User functions stay unbound (mutual-rec / forward). */
    if (P != NULL && P->pc != NULL && VariableDefined(P->pc, E->Ident))
    {
        struct Value *GV = NULL;
        if (TableGet(&P->pc->GlobalTable, E->Ident, &GV, NULL, NULL, NULL)
            && GV != NULL && !GV->OutOfScope && GV->Typ != NULL)
        {
            if (GV->Typ->Base != TypeFunction)
                E->CachedVal = GV;
            else
            {
                /* User or intrinsic function: fptr-table Ident is PTR.
                 * CachedFunc only for Intrinsic (stable Value*); user functions
                 * stay unbound for call resolution (forward/rec). */
                if (GV->Val != NULL && GV->Val->FuncDef.Intrinsic != NULL)
                {
                    E->CachedVal = GV;
                    E->CachedFunc = GV;
                }
                /* A bare designator (qsort(.., cmp), fp = f) is a pointer
                 * value. The same binder runs on IrExCall nodes (E->Ident =
                 * callee): the call's result type is stamped by
                 * IrAnnotateExpr from the return type, so never mark a call
                 * PTR here (0 ? bump(99) : 1 was packed as %s pointer). */
                if (E->Kind != IrExCall)
                    E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
            }
        }
    }
}


/* Deep-copy expression tree (no shared subnodes). Needed for compound-assign
 * rewrite  s += i  →  s = s + i  so free does not double-free Left/Right. */

static int IrCompilePeekIntConst(struct ParseState *P, int *Out); /* fwd */

/* Fold simple constant expressions for array/struct initializers.
 * Handles Const, 0 - Const (unary minus), and Unary Minus on Const.
 * Returns 1 if folded, 0 otherwise. */
static int IrTryFoldInitConst(IrExpr *Ex, long long *Out)
{
    if (Ex == NULL || Out == NULL) return 0;
    if (Ex->Kind == IrExConst)
    {
        *Out = Ex->ConstVal;
        return 1;
    }
    if (Ex->Kind == IrExBinOp && Ex->Op == TokenMinus
        && Ex->Left != NULL && Ex->Left->Kind == IrExConst
        && Ex->Left->ConstVal == 0
        && Ex->Right != NULL && Ex->Right->Kind == IrExConst)
    {
        *Out = -Ex->Right->ConstVal;
        return 1;
    }
    if (Ex->Kind == IrExUnary && Ex->Op == TokenMinus
        && Ex->Left != NULL && Ex->Left->Kind == IrExConst)
    {
        *Out = -Ex->Left->ConstVal;
        return 1;
    }
    return 0;
}


static int IrCompilePeekIntConstPrimary(struct ParseState *P, int *Out)
{
    enum LexToken T = LexGetToken(P, NULL, FALSE);
    struct Value *Lv = NULL;
    if (T == TokenIntegerConstant) {
        LexGetToken(P, &Lv, TRUE);
        if (Lv == NULL) return 0;
        *Out = (int)ExpressionCoerceInteger(Lv);
        return 1;
    }
    if (T == TokenMinus) {
        int v;
        LexGetToken(P, NULL, TRUE);
        if (!IrCompilePeekIntConstPrimary(P, &v)) return 0;
        *Out = -v;
        return 1;
    }
    if (T == TokenOpenBracket) {
        int v;
        LexGetToken(P, NULL, TRUE);
        if (!IrCompilePeekIntConst(P, &v)) return 0;
        if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket) return 0;
        *Out = v;
        return 1;
    }
    if (T == TokenIdentifier) {
        struct Value *MV = NULL; struct ParseState MP, Save;
        char *Name;
        /* Save position: only commit consume if ident is a parameterless macro
         * that expands to a const. Locals (VLA size) must stay put. */
        ParserCopy(&Save, P);
        LexGetToken(P, &Lv, TRUE);
        if (Lv == NULL || Lv->Val == NULL) { ParserCopy(P, &Save); return 0; }
        Name = Lv->Val->Identifier;
        if (!VariableDefined(P->pc, Name) || !TableGet(&P->pc->GlobalTable, Name, &MV, NULL, NULL, NULL) ||
            MV == NULL || MV->Typ == NULL || MV->Typ->Base != TypeMacro ||
            MV->Val == NULL || MV->Val->MacroDef.NumParams != 0)
        {
            ParserCopy(P, &Save); /* restore: not a usable macro */
            return 0;
        }
        /* Macro body may be 10 or (10) or 1+2 — parse as const expr */
        ParserCopy(&MP, &MV->Val->MacroDef.Body);
        MP.pc = P->pc;
        MP.Mode = P->Mode;
        if (!IrCompilePeekIntConst(&MP, Out))
        {
            ParserCopy(P, &Save);
            return 0;
        }
        return 1; /* name consumed; body folded into *Out */
    }
    return 0;
}

static int IrCompilePeekIntConst(struct ParseState *P, int *Out)
{
    int left, right;
    enum LexToken Op;
    if (!IrCompilePeekIntConstPrimary(P, &left))
        return 0;
    for (;;) {
        Op = LexGetToken(P, NULL, FALSE);
        if (Op != TokenAsterisk && Op != TokenSlash && Op != TokenPlus
            && Op != TokenMinus && Op != TokenModulus
            && Op != TokenShiftLeft && Op != TokenShiftRight)
            break;
        LexGetToken(P, NULL, TRUE);
        if (!IrCompilePeekIntConstPrimary(P, &right))
            return 0;
        switch (Op) {
            case TokenAsterisk: left = left * right; break;
            case TokenSlash:
                if (right == 0) return 0;
                left = left / right; break;
            case TokenModulus:
                if (right == 0) return 0;
                left = left % right; break;
            case TokenPlus: left = left + right; break;
            case TokenMinus: left = left - right; break;
            case TokenShiftLeft: left = left << right; break;
            case TokenShiftRight: left = left >> right; break;
            default: return 0;
        }
    }
    *Out = left;
    return 1;
}

static void IrFreeExpr(IrExpr *E);

static IrExpr *IrCloneExpr(IrExpr *E)
{
    IrExpr *C;
    int i;
    if (E == NULL) return NULL;
    C = (IrExpr *)calloc(1, sizeof(IrExpr));
    if (C == NULL) return NULL;
    *C = *E;
    C->Left = NULL;
    C->Right = NULL;
    C->Args = NULL;
    C->ArgCount = 0;
    /* Joined string literals are owned by the source expression's Ident.
     * A clone must own a separate copy before the original can be released. */
    if (E->Kind == IrExConst && E->Op == TokenStringConstant && E->Ident != NULL)
    {
        size_t n = strlen(E->Ident) + 1;
        C->Ident = (char *)malloc(n);
        if (C->Ident == NULL) { free(C); return NULL; }
        memcpy(C->Ident, E->Ident, n);
    }
    C->Left = IrCloneExpr(E->Left);
    if (E->Left != NULL && C->Left == NULL) { IrFreeExpr(C); return NULL; }
    C->Right = IrCloneExpr(E->Right);
    if (E->Right != NULL && C->Right == NULL) { IrFreeExpr(C); return NULL; }
    if (E->Args != NULL && E->ArgCount > 0)
    {
        C->Args = (IrExpr **)calloc((size_t)E->ArgCount, sizeof(IrExpr *));
        if (C->Args == NULL) { IrFreeExpr(C); return NULL; }
        C->ArgCount = E->ArgCount;
        for (i = 0; i < E->ArgCount; i++)
        {
            C->Args[i] = IrCloneExpr(E->Args[i]);
            if (E->Args[i] != NULL && C->Args[i] == NULL)
            { IrFreeExpr(C); return NULL; }
        }
    }
    /* Non-owned Ident / CachedFunc are borrowed from the symbol table or fn. */
    return C;
}

static void IrFreeExpr(IrExpr *E)
{
    int i;
    if (E == NULL) return;
    IrFreeExpr(E->Left);
    IrFreeExpr(E->Right);
    if (E->Args != NULL)
    {
        for (i = 0; i < E->ArgCount; i++)
            IrFreeExpr(E->Args[i]);
        free(E->Args);
    }
    /* Owned joined string constants (see string-concat in IrCompilePrimary). */
    if (E->Kind == IrExConst && E->Op == TokenStringConstant && E->Ident != NULL)
    {
        free(E->Ident);
        E->Ident = NULL;
    }
    free(E);
}

static void IrFreeExprVector(IrExpr **Exprs, int Count)
{
    int i;
    if (Exprs == NULL) return;
    for (i = 0; i < Count; i++)
        IrFreeExpr(Exprs[i]);
    free(Exprs);
}

static void IrFreeStmt(IrStmt *S)
{
    int i;
    if (S == NULL) return;
    IrFreeExpr(S->Expr);
    IrFreeStmt(S->ThenStmt);
    IrFreeStmt(S->ElseStmt);
    IrFreeStmt(S->BodyStmt);
    IrFreeStmt(S->InitStmt);
    IrFreeStmt(S->IncStmt);
    if (S->Stmts != NULL)
    {
        for (i = 0; i < S->StmtCount; i++)
            IrFreeStmt(S->Stmts[i]);
        free(S->Stmts);
    }
        if (S->DeclInitVals != NULL)
        free(S->DeclInitVals);
    if (S->DeclInitExprs != NULL)
    {
        int di;
        for (di = 0; di < S->DeclInitCount; di++)
            if (S->DeclInitExprs[di] != NULL)
                IrFreeExpr(S->DeclInitExprs[di]);
        free(S->DeclInitExprs);
    }
    if (S->DeclInitNames != NULL)
    {
        int di;
        for (di = 0; di < S->DeclInitCount; di++)
            free(S->DeclInitNames[di]);
        free(S->DeclInitNames);
    }

    free(S);
}

void IrFreeFunctionBody(void *BodyIr)
{
    IrFreeStmt((IrStmt *)BodyIr);
}



/* True if Typ is IR-layout-friendly: scalar/BF, fixed scalar arrays,
 * or nested struct/union with the same constraints (depth-limited). */
static int IrStructLayoutOk(struct ValueType *Typ, int Depth)
{
    int hi;
    struct TableEntry *Ent;
    if (Typ == NULL || Depth < 0)
        return 0;
    if (Typ->Base != TypeStruct && Typ->Base != TypeUnion)
        return 0;
    if (Typ->Members == NULL)
        return 0;
    for (hi = 0; hi < Typ->Members->Size; hi++)
    {
        for (Ent = Typ->Members->HashTable[hi]; Ent != NULL; Ent = Ent->Next)
        {
            struct Value *MV = Ent->p.v.Val;
            enum BaseType B;
            struct ValueType *MT;
            if (MV == NULL || MV->Typ == NULL)
                continue;
            MT = MV->Typ;
            B = MT->Base;
            /* TypeFunction as value member is rare; fptr is TypePointer.
             * TypeFP is allowed as opaque bits (struct with float/double). */
            if (B == TypeFunction)
                return 0;
            if (B == TypeArray)
            {
                struct ValueType *ET = MT->FromType;
                /* Float/double/char/int arrays as members are opaque blobs.
                 * Only recurse for array-of-struct; reject array-of-function. */
                if (ET != NULL && (ET->Base == TypeStruct || ET->Base == TypeUnion))
                {
                    if (!IrStructLayoutOk(ET, Depth - 1))
                        return 0;
                }
                else if (ET != NULL && ET->Base == TypeFunction)
                    return 0;
                else if (ET != NULL && ET->Base == TypeArray)
                    return 0;
                /* else: scalar/FP/pointer element — OK */
            }
            else if (B == TypeStruct || B == TypeUnion)
            {
                if (!IrStructLayoutOk(MT, Depth - 1))
                    return 0;
            }
        }
    }
    return 1;
}

static IrExpr *IrAllocExpr(Picoc *pc, IrExprKind Kind)
{
    IrExpr *E = (IrExpr *)calloc(1, sizeof(IrExpr));
    (void)pc;
    if (E == NULL) return NULL;
    E->Kind = Kind;
    /* calloc zeroes slots to 0 — but 0 is a valid local/param index.
     * Unbound must be -1 or member/index nodes steal LocalVal[0]. */
    E->LocalSlot = -1;
    E->ParamSlot = -1;
    E->BindSlot = 0; /* 0 = unbound; -2 used for cached member offset */
    if (IrAllocParserPos != NULL)
    {
        E->SourceLine = IrAllocParserPos->Line;
        E->CharacterPos = IrAllocParserPos->CharacterPos;
    }
    return E;
}

static IrStmt *IrAllocStmt(Picoc *pc, IrStmtKind Kind)
{
    IrStmt *S = (IrStmt *)calloc(1, sizeof(IrStmt));
    (void)pc;
    if (S == NULL) return NULL;
    S->Kind = Kind;
    S->LocalSlot = -1;
    if (Kind == IrStDecl)
    {
        int di;
        for (di = 0; di < IR_MAX_ARRAY_DIMS; di++)
            S->DeclVlaDimRegs[di] = -1;
    }
    if (IrAllocParserPos != NULL)
    {
        S->SourceLine = IrAllocParserPos->Line;
        S->CharacterPos = IrAllocParserPos->CharacterPos;
        S->SourceFile = IrAllocParserPos->FileName;
    }
    return S;
}

static IrExpr *IrCompileExpr(struct ParseState *P);
static IrExpr *IrCompileAssignExpr(struct ParseState *P);

static inline int IrCompileVlaIndexSlot(IrExpr *E)
{
    IrExpr *Root = E;
    while (Root != NULL && Root->Kind == IrExIndex)
        Root = Root->Left;
    if (Root == NULL || Root->Kind != IrExIdent
        || Root->LocalSlot < 0 || Root->LocalSlot >= IR_COMPILE_MAX_LOCALS)
        return -1;
    return IrCompileLocalArraySize[Root->LocalSlot] == -2 ? Root->LocalSlot : -1;
}

/* Point2: structural ForFpKind at compile (no frame). Pure-shape only. */
static void IrClassifyForFpCompile(IrStmt *S)
{
    IrStmt *Body;
    IrExpr *E, *Add, *Dest, *A, *B, *Idx;
    if (S == NULL || S->Kind != IrStFor)
        return;
    /* BoundIsVar allowed — FastArrayAccum/Tight resolve bound at run. */
    if (!S->FastCond.Active || S->FastCond.Active != 1)
        return;
    if (!S->FastInc.Active || S->FastInc.VarName == NULL)
        return;
    if (S->FastCond.VarName == NULL
        || strcmp(S->FastCond.VarName, S->FastInc.VarName) != 0)
        return;
    Body = S->BodyStmt;
    if (Body != NULL && Body->Kind == IrStBlock && Body->StmtCount == 1)
        Body = Body->Stmts[0];
    if (Body == NULL || Body->Kind != IrStExpr || Body->Expr == NULL)
        return;
    E = Body->Expr;
    if (E->Kind != IrExAssign || E->Left == NULL || E->Right == NULL)
        return;
    Dest = E->Left;
    Add = E->Right;
    if (Dest->Kind != IrExIdent || Add->Kind != IrExBinOp || Add->Op != TokenPlus)
        return;
    A = Add->Left;
    B = Add->Right;
    if (A == NULL || B == NULL)
        return;
    /* s += a[i] / a[i] + s  → ARRAY_ACCUM (array_sum) */
    Idx = NULL;
    if (A->Kind == IrExIndex && B->Kind == IrExIdent && Dest->Ident
        && B->Ident && strcmp(Dest->Ident, B->Ident) == 0)
        Idx = A;
    else if (B->Kind == IrExIndex && A->Kind == IrExIdent && Dest->Ident
             && A->Ident && strcmp(Dest->Ident, A->Ident) == 0)
        Idx = B;
    if (Idx != NULL && Idx->Right != NULL && Idx->Right->Kind == IrExIdent
        && Idx->Right->Ident != NULL
        && strcmp(Idx->Right->Ident, S->FastInc.VarName) == 0)
    {
        S->ForFpKind = IR_FP_ARRAY_ACCUM;
        return;
    }
    if (A->Kind == IrExIdent && Dest->Ident != NULL && A->Ident != NULL
        && strcmp(Dest->Ident, A->Ident) == 0 && B->Kind == IrExIdent
        && B->Ident != NULL && strcmp(B->Ident, S->FastInc.VarName) == 0)
        S->ForFpKind = IR_FP_TIGHT_FOR;
    else if (B->Kind == IrExIdent && Dest->Ident != NULL && B->Ident != NULL
        && strcmp(Dest->Ident, B->Ident) == 0 && A->Kind == IrExIdent
        && A->Ident != NULL && strcmp(A->Ident, S->FastInc.VarName) == 0)
        S->ForFpKind = IR_FP_TIGHT_FOR;
}

/* Bind FastCond operands to the compile-time local slots of the compiled
 * condition expression (same idea as FastInc.VarSlot). Name-only lookup at run
 * time is newest-first and scope-blind: with two sibling `for (int i...)` loops
 * the first loop's condition found the second (still uninitialised) `i`. */
static void IrBindFastCondSlots(IrStmt *S)
{
    IrExpr *E;
    if (S == NULL || S->Expr == NULL || !S->FastCond.Active)
        return;
    E = S->Expr;
    if (S->FastCond.Active == 3)
    {
        if (E->Kind == IrExIdent && E->Ident != NULL && S->FastCond.VarName != NULL
            && strcmp(E->Ident, S->FastCond.VarName) == 0 && E->LocalSlot >= 0)
            S->FastCond.VarSlot = E->LocalSlot;
        return;
    }
    if (S->FastCond.Active != 1 || E->Kind != IrExBinOp)
        return;
    if (E->Left != NULL && E->Left->Kind == IrExIdent && E->Left->Ident != NULL
        && S->FastCond.VarName != NULL
        && strcmp(E->Left->Ident, S->FastCond.VarName) == 0
        && E->Left->LocalSlot >= 0)
        S->FastCond.VarSlot = E->Left->LocalSlot;
    if (S->FastCond.BoundIsVar && E->Right != NULL && E->Right->Kind == IrExIdent
        && E->Right->Ident != NULL && S->FastCond.BoundVarName != NULL
        && strcmp(E->Right->Ident, S->FastCond.BoundVarName) == 0
        && E->Right->LocalSlot >= 0)
        S->FastCond.BoundSlot = E->Right->LocalSlot;
}

static IrStmt *IrCompileStmt(struct ParseState *P);
static void IrAnnotateExpr(IrExpr *E);
static void IrAnnotateStmt(IrStmt *S);
static void IrDaFunction(IrStmt *Body);

static IrExpr *IrCompilePrimary(struct ParseState *P)
{
    struct Value *LexVal;
    enum LexToken Tok;
    IrExpr *E;

    Tok = LexGetToken(P, &LexVal, TRUE);
    /* Parentheses: ( expr )  OR  C cast (type)expr */
    if (Tok == TokenOpenBracket)
    {
        enum LexToken Peek = LexGetToken(P, NULL, FALSE);
        /* (int)x / (long)x / ... / (typedef_name)x when typedef is integer/pointer */
        int IsTypedefCast = 0;
        if (Peek == TokenIdentifier)
        {
            struct Value *TLex = NULL;
            struct Value *TV = NULL;
            struct ParseState Look;
            ParserCopy(&Look, P);
            if (LexGetToken(&Look, &TLex, TRUE) == TokenIdentifier
                && TLex != NULL && TLex->Val != NULL
                && VariableDefined(P->pc, TLex->Val->Identifier))
            {
                VariableGet(P->pc, P, TLex->Val->Identifier, &TV);
                if (TV != NULL && TV->Typ != NULL && TV->Typ->Base == Type_Type
                    && TV->Val != NULL && TV->Val->Typ != NULL)
                {
                    struct ValueType *BT = TV->Val->Typ;
                    if (BT->Base == TypeInt || BT->Base == TypeShort || BT->Base == TypeChar
                        || BT->Base == TypeLong || BT->Base == TypeLongLong
                        || BT->Base == TypeUnsignedInt || BT->Base == TypeUnsignedShort
                        || BT->Base == TypeUnsignedChar || BT->Base == TypeUnsignedLong
                        || BT->Base == TypeUnsignedLongLong || BT->Base == TypePointer
                        || BT->Base == TypeVoid || BT->Base == TypeEnum
                        || BT->Base == TypeStruct || BT->Base == TypeUnion)
                        IsTypedefCast = 1;
                }
            }
        }
        /* Skip leading const/volatile so (const char*), (volatile int), etc. are casts */
        {
            struct ParseState LookCV;
            ParserCopy(&LookCV, P);
            while (LexGetToken(&LookCV, NULL, FALSE) == TokenConstType
                || LexGetToken(&LookCV, NULL, FALSE) == TokenVolatileType)
                LexGetToken(&LookCV, NULL, TRUE);
            Peek = LexGetToken(&LookCV, NULL, FALSE);
            if (Peek == TokenIdentifier && !IsTypedefCast)
            {
                struct Value *TLex = NULL;
                struct Value *TV = NULL;
                if (LexGetToken(&LookCV, &TLex, TRUE) == TokenIdentifier
                    && TLex != NULL && TLex->Val != NULL
                    && VariableDefined(P->pc, TLex->Val->Identifier))
                {
                    VariableGet(P->pc, P, TLex->Val->Identifier, &TV);
                    if (TV != NULL && TV->Typ != NULL && TV->Typ->Base == Type_Type
                        && TV->Val != NULL && TV->Val->Typ != NULL)
                    {
                        struct ValueType *BT = TV->Val->Typ;
                        if (BT->Base == TypeInt || BT->Base == TypeShort || BT->Base == TypeChar
                            || BT->Base == TypeLong || BT->Base == TypeLongLong
                            || BT->Base == TypeUnsignedInt || BT->Base == TypeUnsignedShort
                            || BT->Base == TypeUnsignedChar || BT->Base == TypeUnsignedLong
                            || BT->Base == TypeUnsignedLongLong || BT->Base == TypePointer
                            || BT->Base == TypeVoid || BT->Base == TypeEnum
                            || BT->Base == TypeStruct || BT->Base == TypeUnion)
                            IsTypedefCast = 1;
                    }
                }
            }
        }
        if (Peek == TokenIntType || Peek == TokenCharType || Peek == TokenShortType
            || Peek == TokenLongType || Peek == TokenUnsignedType
#ifndef NO_FP
            || Peek == TokenFloatType || Peek == TokenDoubleType
#endif
            || Peek == TokenVoidType || Peek == TokenStructType || Peek == TokenUnionType
            || Peek == TokenEnumType
            || IsTypedefCast)
        {
            int IsVoidCast = (Peek == TokenVoidType);
            int PtrDepth = 0;
            int PointeeBase = TypeInt;
            int PointeeSize = (int)sizeof(int);
            struct ValueType *PointeeTyp = NULL; /* full type for struct/union cast->member */
            /* consume leading const/volatile of the cast type */
            while (LexGetToken(P, NULL, FALSE) == TokenConstType
                || LexGetToken(P, NULL, FALSE) == TokenVolatileType)
                LexGetToken(P, NULL, TRUE);
            /* Determine pointee from first type token / typedef */
            if (IsTypedefCast && Peek == TokenIdentifier)
            {
                struct Value *TLex = NULL, *TV = NULL;
                struct ParseState Look;
                ParserCopy(&Look, P);
                if (LexGetToken(&Look, &TLex, TRUE) == TokenIdentifier
                    && TLex != NULL && TLex->Val != NULL
                    && VariableDefined(P->pc, TLex->Val->Identifier))
                {
                    VariableGet(P->pc, P, TLex->Val->Identifier, &TV);
                    if (TV != NULL && TV->Val != NULL && TV->Val->Typ != NULL)
                    {
                        struct ValueType *BT = TV->Val->Typ;
                        PointeeBase = BT->Base;
                        PointeeSize = BT->Sizeof > 0 ? BT->Sizeof : (int)sizeof(int);
                        PointeeTyp = BT;
                    }
                }
            }
            else if (Peek == TokenStructType || Peek == TokenUnionType)
            {
                /* (struct tag *) / (union tag *) — capture full type for -> member */
                struct ParseState Look;
                struct ValueType *ST = NULL;
                ParserCopy(&Look, P);
                TypeParseFront(&Look, &ST, NULL);
                if (ST != NULL && (ST->Base == TypeStruct || ST->Base == TypeUnion))
                {
                    PointeeTyp = ST;
                    PointeeBase = ST->Base;
                    PointeeSize = ST->Sizeof > 0 ? ST->Sizeof : (int)sizeof(int);
                }
                else
                {
                    PointeeBase = (Peek == TokenStructType) ? TypeStruct : TypeUnion;
                    PointeeSize = (int)sizeof(int);
                }
            }
            else if (Peek == TokenCharType) { PointeeBase = TypeChar; PointeeSize = 1; }
            else if (Peek == TokenShortType) { PointeeBase = TypeShort; PointeeSize = 2; }
            else if (Peek == TokenLongType) { PointeeBase = TypeLong; PointeeSize = (int)sizeof(long); }
            else if (Peek == TokenIntType) { PointeeBase = TypeInt; PointeeSize = (int)sizeof(int); }
            else if (Peek == TokenUnsignedType) { PointeeBase = TypeUnsignedInt; PointeeSize = (int)sizeof(unsigned); }
#ifndef NO_FP
            else if (Peek == TokenFloatType || Peek == TokenDoubleType) {
                PointeeBase = TypeFP; PointeeSize = (int)sizeof(double);
            }
#endif
            /* consume type tokens until ')' */
            while (LexGetToken(P, NULL, FALSE) != TokenCloseBracket
                   && LexGetToken(P, NULL, FALSE) != TokenEOF)
            {
                enum LexToken T = LexGetToken(P, NULL, TRUE);
                if (T == TokenOpenBracket)
                    IR_FAIL("cast to function type not in Body-IR");
                if (T == TokenAsterisk)
                {
                    PtrDepth++;
                    continue;
                }
                /* refine pointee as more type tokens appear: unsigned char, long long, ... */
                if (T == TokenCharType)
                {
                    if (PointeeBase == TypeUnsignedInt || Peek == TokenUnsignedType)
                    {
                        PointeeBase = TypeUnsignedChar;
                        PointeeSize = 1;
                    }
                    else
                    {
                        PointeeBase = TypeChar;
                        PointeeSize = 1;
                    }
                }
                else if (T == TokenShortType)
                {
                    if (PointeeBase == TypeUnsignedInt || Peek == TokenUnsignedType)
                    {
                        PointeeBase = TypeUnsignedShort;
                        PointeeSize = 2;
                    }
                    else
                    {
                        PointeeBase = TypeShort;
                        PointeeSize = 2;
                    }
                }
                else if (T == TokenLongType)
                {
                    if (PointeeBase == TypeLong || PointeeBase == TypeLongLong
                        || PointeeBase == TypeUnsignedLong)
                    {
                        /* long long / unsigned long long */
                        if (Peek == TokenUnsignedType || PointeeBase == TypeUnsignedLong
                            || PointeeBase == TypeUnsignedInt)
                        {
                            PointeeBase = TypeUnsignedLongLong;
                            PointeeSize = (int)sizeof(unsigned long long);
                        }
                        else
                        {
                            PointeeBase = TypeLongLong;
                            PointeeSize = (int)sizeof(long long);
                        }
                    }
                    else if (Peek == TokenUnsignedType || PointeeBase == TypeUnsignedInt)
                    {
                        PointeeBase = TypeUnsignedLong;
                        PointeeSize = (int)sizeof(unsigned long);
                    }
                    else
                    {
                        PointeeBase = TypeLong;
                        PointeeSize = (int)sizeof(long);
                    }
                }
                else if (T == TokenIntType)
                {
                    if (Peek == TokenUnsignedType || PointeeBase == TypeUnsignedInt
                        || PointeeBase == TypeUnsignedLong)
                    {
                        /* keep unsigned, sizeof int unless long already */
                        if (PointeeBase != TypeUnsignedLong && PointeeBase != TypeUnsignedLongLong)
                        {
                            PointeeBase = TypeUnsignedInt;
                            PointeeSize = (int)sizeof(unsigned int);
                        }
                    }
                    else if (PointeeBase != TypeLong && PointeeBase != TypeLongLong)
                    {
                        PointeeBase = TypeInt;
                        PointeeSize = (int)sizeof(int);
                    }
                }
            }
            if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket)
                IR_FAIL("cast: expected ')' after type");
            /* (void)expr — discard value; (void*)p is a pointer cast below */
            if (IsVoidCast && PtrDepth == 0)
            {
                IrExpr *Discard = IrCompilePrimary(P);
                if (Discard == NULL) return NULL;
                IrFreeExpr(Discard);
                E = IrAllocExpr(P->pc, IrExConst);
                if (E == NULL) return NULL;
                E->ConstVal = 0;
                return E;
            }
            if (IsVoidCast && PtrDepth > 0)
            {
                PointeeBase = TypeVoid;
                PointeeSize = (int)sizeof(void *);
            }
            E = IrCompilePrimary(P);
            if (E == NULL) return NULL;
            /* Wrap all casts: pointer (ConstVal=size>0) or value (ConstVal=0, I32=target base) */
            {
                IrExpr *C = IrAllocExpr(P->pc, IrExUnary);
                if (C == NULL) return NULL;
                C->Op = TokenCast;
                C->Left = E;
                if (PtrDepth > 0)
                {
                    C->ConstVal = PointeeSize; /* bytes: * uses this */
                    C->I32 = PointeeBase;
                    /* CachedAddr holds pointee ValueType* for struct/union (cast)->member */
                    if (PointeeTyp != NULL
                        && (PointeeTyp->Base == TypeStruct || PointeeTyp->Base == TypeUnion
                            || PointeeTyp->Base == TypeArray))
                        C->CachedAddr = (void *)PointeeTyp;
                }
                else
                {
                    C->ConstVal = 0; /* value cast: convert on eval */
                    C->I32 = PointeeBase; /* target BaseType */
                }
                E = C;
            }
            /* Postfix after cast: ((T*)p)[i], ((T*)p)->m, ((T*)p)++ */
            for (;;)
            {
                enum LexToken PeekC = LexGetToken(P, NULL, FALSE);
                if (PeekC == TokenLeftSquareBracket)
                {
                    IrExpr *Idx, *Ix;
                    LexGetToken(P, NULL, TRUE);
                    Idx = IrCompileExpr(P);
                    if (Idx == NULL) return NULL;
                    if (LexGetToken(P, NULL, TRUE) != TokenRightSquareBracket)
                        return NULL;
                    Ix = IrAllocExpr(P->pc, IrExIndex);
                    if (Ix == NULL) return NULL;
                Ix->SourceLine = P->Line;
                Ix->CharacterPos = P->CharacterPos;
                    Ix->Left = E;
                    Ix->Right = Idx;
                    E = Ix;
                    continue;
                }
                if (PeekC == TokenDot || PeekC == TokenArrow)
                {
                    struct Value *MLex = NULL;
                    IrExpr *M;
                    LexGetToken(P, NULL, TRUE);
                    if (LexGetToken(P, &MLex, TRUE) != TokenIdentifier)
                        return NULL;
                    M = IrAllocExpr(P->pc, IrExMember);
                    if (M == NULL) return NULL;
                M->SourceLine = P->Line;
                    M->Left = E;
                    M->Ident = MLex->Val->Identifier;
                    /* LexGetToken has advanced beyond the identifier. Store
                     * its 1-based start column, not the post-token cursor. */
                    M->CharacterPos = P->CharacterPos - (int)strlen(M->Ident) - 1;
                    if (M->CharacterPos < 1) M->CharacterPos = 1;
                    M->Op = PeekC;
                    E = M;
                    continue;
                }
                if (PeekC == TokenIncrement || PeekC == TokenDecrement)
                {
                    IrExpr *U;
                    LexGetToken(P, NULL, TRUE);
                    U = IrAllocExpr(P->pc, IrExUnary);
                    if (U == NULL) return NULL;
                    U->Op = PeekC;
                    U->Left = E;
                    U->ConstVal = 1;
                    return U;
                }
                break;
            }
            return E;
        }
        E = IrCompileExpr(P);
        if (E == NULL) return NULL;
        if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket)
            IR_FAIL("expected ')' after parenthesized expression");
        /* Postfix after (expr): (*f)(args), (p)[i], (x)++ */
        for (;;)
        {
            enum LexToken Peek2 = LexGetToken(P, NULL, FALSE);
            if (Peek2 == TokenOpenBracket
                && (E->Kind == IrExUnary || E->Kind == IrExIdent
                    || E->Kind == IrExMember || E->Kind == IrExIndex))
            {
                IrExpr *Call;
                IrExpr **Args;
                int Cap = 8, N = 0;
                LexGetToken(P, NULL, TRUE); /* ( */
                Args = (IrExpr **)calloc(Cap, sizeof(IrExpr *));
                if (Args == NULL) IR_FAIL("OOM call args");
                if (LexGetToken(P, NULL, FALSE) != TokenCloseBracket)
                {
                    for (;;)
                    {
                        IrExpr *A = IrCompileAssignExpr(P);
                        if (A == NULL) { free(Args); return NULL; }
                        if (N >= Cap)
                        {
                            Cap *= 2;
                            Args = (IrExpr **)realloc(Args, Cap * sizeof(IrExpr *));
                            if (Args == NULL) IR_FAIL("OOM call args");
                        }
                        Args[N++] = A;
                        if (LexGetToken(P, NULL, FALSE) == TokenComma)
                            LexGetToken(P, NULL, TRUE);
                        else
                            break;
                    }
                }
                if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket)
                    IR_FAIL("expected ')' after call args");
                Call = IrAllocExpr(P->pc, IrExCall);
                if (Call == NULL) return NULL;
                Call->Left = E;
                Call->Ident = NULL;
                Call->Args = Args;
                Call->ArgCount = N;
                E = Call;
                continue;
            }
            if (Peek2 == TokenLeftSquareBracket)
            {
                IrExpr *Idx, *Ix;
                LexGetToken(P, NULL, TRUE);
                Idx = IrCompileExpr(P);
                if (Idx == NULL) return NULL;
                if (LexGetToken(P, NULL, TRUE) != TokenRightSquareBracket)
                    IR_FAIL("expected ']'");
                Ix = IrAllocExpr(P->pc, IrExIndex);
                if (Ix == NULL) return NULL;
                Ix->SourceLine = P->Line;
                Ix->CharacterPos = P->CharacterPos;
                Ix->Left = E;
                Ix->Right = Idx;
                E = Ix;
                continue;
            }
            if (Peek2 == TokenDot || Peek2 == TokenArrow)
            {
                struct Value *MLex = NULL;
                IrExpr *M;
                LexGetToken(P, NULL, TRUE); /* . or -> */
                if (LexGetToken(P, &MLex, TRUE) != TokenIdentifier)
                    return NULL;
                /* (*p).m / (p)->m / (s).m / (f())->m */
                if (E->Kind != IrExIdent && E->Kind != IrExIndex
                    && E->Kind != IrExMember && E->Kind != IrExUnary
                    && E->Kind != IrExCall && E->Kind != IrExVaArg)
                    return NULL;
                M = IrAllocExpr(P->pc, IrExMember);
                if (M == NULL) return NULL;
                M->SourceLine = P->Line;
                M->Left = E;
                M->Ident = MLex->Val->Identifier;
                M->CharacterPos = P->CharacterPos - (int)strlen(M->Ident) - 1;
                if (M->CharacterPos < 1) M->CharacterPos = 1;
                M->Op = Peek2;
                E = M;
                continue;
            }
            if (Peek2 == TokenIncrement || Peek2 == TokenDecrement)
            {
                IrExpr *U;
                LexGetToken(P, NULL, TRUE);
                U = IrAllocExpr(P->pc, IrExUnary);
                if (U == NULL) return NULL;
                U->Op = Peek2;
                U->Left = E;
                U->ConstVal = 1;
                return U;
            }
            break;
        }
        return E;
    }
    /* sizeof expr / sizeof (type) → compile-time constant when possible */
    if (Tok == TokenOffsetof)
    {
        struct ValueType *Typ = NULL;
        char *Ident = NULL;
        int IsStatic = 0;
        struct Value *MemLex = NULL;
        int Off = 0;
        if (LexGetToken(P, NULL, TRUE) != TokenOpenBracket)
            IR_FAIL("offsetof: expected '('");
        TypeParse(P, &Typ, &Ident, &IsStatic);
        if (Typ == NULL)
            IR_FAIL("offsetof: type expected");
        if (LexGetToken(P, NULL, TRUE) != TokenComma)
            IR_FAIL("offsetof: expected ','");
        if (LexGetToken(P, &MemLex, TRUE) != TokenIdentifier || MemLex == NULL)
            IR_FAIL("offsetof: member name expected");
        if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket)
            IR_FAIL("offsetof: expected ')'");
        if (Typ->Base != TypeStruct && Typ->Base != TypeUnion)
            IR_FAIL("offsetof: not a struct/union");
        {
            struct Value *MemVal = NULL;
            if (Typ->Members == NULL
                || !TableGet(Typ->Members, MemLex->Val->Identifier, &MemVal, NULL, NULL, NULL)
                || MemVal == NULL)
                IR_FAIL("offsetof: unknown member");
            Off = MemVal->Val->Integer;
        }
        E = IrAllocExpr(P->pc, IrExConst);
        if (E == NULL) return NULL;
        E->ConstVal = Off;
        return E;
    }
    if (Tok == TokenSizeof)
    {
        enum LexToken Peek = LexGetToken(P, NULL, FALSE);
        int sz = 0;
        if (Peek == TokenOpenBracket)
        {
            struct ParseState Look;
            enum LexToken T2;
            LexGetToken(P, NULL, TRUE); /* ( */
            ParserCopy(&Look, P);
            T2 = LexGetToken(&Look, NULL, FALSE);
            /* typedef-name counts as a type in sizeof(T) / sizeof(T*) */
            int IsTypedefSizeof = 0;
            if (T2 == TokenIdentifier)
            {
                struct Value *TLex = NULL, *TV = NULL;
                struct ParseState Look2;
                ParserCopy(&Look2, P);
                if (LexGetToken(&Look2, &TLex, TRUE) == TokenIdentifier
                    && TLex != NULL && TLex->Val != NULL
                    && VariableDefined(P->pc, TLex->Val->Identifier))
                {
                    VariableGet(P->pc, P, TLex->Val->Identifier, &TV);
                    if (TV != NULL && TV->Typ != NULL && TV->Typ->Base == Type_Type
                        && TV->Val != NULL && TV->Val->Typ != NULL)
                        IsTypedefSizeof = 1;
                }
            }
            /* Skip leading const/volatile so sizeof(const int) works */
            while (T2 == TokenConstType || T2 == TokenVolatileType)
            {
                LexGetToken(P, NULL, TRUE);
                ParserCopy(&Look, P);
                T2 = LexGetToken(&Look, NULL, FALSE);
            }
            if (T2 == TokenIntType || T2 == TokenCharType || T2 == TokenShortType
                || T2 == TokenLongType || T2 == TokenUnsignedType
                || T2 == TokenSignedType
                || T2 == TokenStructType || T2 == TokenUnionType
#ifndef NO_FP
                || T2 == TokenFloatType || T2 == TokenDoubleType
#endif
                || T2 == TokenVoidType
                || IsTypedefSizeof)
            {
                struct ValueType *Typ = NULL;
                char *IgnoreIdent = NULL;
                int IsStatic = 0;
#ifndef NO_FP
                /* sizeof(long double): TypeParse would eat only "long" */
                if (T2 == TokenLongType)
                {
                    struct ParseState LookLD;
                    ParserCopy(&LookLD, P);
                    LexGetToken(&LookLD, NULL, TRUE); /* long */
                    if (LexGetToken(&LookLD, NULL, FALSE) == TokenDoubleType)
                    {
                        LexGetToken(P, NULL, TRUE); /* long */
                        LexGetToken(P, NULL, TRUE); /* double */
                        sz = (int)sizeof(double);
                        while (LexGetToken(P, NULL, FALSE) == TokenAsterisk)
                        {
                            LexGetToken(P, NULL, TRUE);
                            sz = (int)sizeof(void *);
                        }
                        if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket)
                            return NULL;
                        E = IrAllocExpr(P->pc, IrExConst);
                        if (E == NULL) return NULL;
                        E->ConstVal = sz;
                        return E;
                    }
                }
#endif
                TypeParse(P, &Typ, &IgnoreIdent, &IsStatic);
                if (Typ == NULL)
                {
                    /* fallback manual sizes */
                    if (T2 == TokenCharType) { LexGetToken(P, NULL, TRUE); sz = 1; }
                    else if (T2 == TokenShortType) { LexGetToken(P, NULL, TRUE); sz = 2; }
                    else if (T2 == TokenIntType) { LexGetToken(P, NULL, TRUE); sz = (int)sizeof(int); }
                    else if (T2 == TokenSignedType)
                    {
                        /* signed [char|short|int|long [long]] */
                        LexGetToken(P, NULL, TRUE); /* signed */
                        if (LexGetToken(P, NULL, FALSE) == TokenCharType)
                        { LexGetToken(P, NULL, TRUE); sz = 1; }
                        else if (LexGetToken(P, NULL, FALSE) == TokenShortType)
                        { LexGetToken(P, NULL, TRUE); sz = 2; }
                        else if (LexGetToken(P, NULL, FALSE) == TokenLongType)
                        {
                            LexGetToken(P, NULL, TRUE);
                            if (LexGetToken(P, NULL, FALSE) == TokenLongType)
                            { LexGetToken(P, NULL, TRUE); sz = (int)sizeof(long long); }
                            else sz = (int)sizeof(long);
                        }
                        else if (LexGetToken(P, NULL, FALSE) == TokenIntType)
                        { LexGetToken(P, NULL, TRUE); sz = (int)sizeof(int); }
                        else
                            sz = (int)sizeof(int); /* bare "signed" */
                    }
                    else if (T2 == TokenLongType)
                    {
                        LexGetToken(P, NULL, TRUE); /* long */
#ifndef NO_FP
                        if (LexGetToken(P, NULL, FALSE) == TokenDoubleType)
                        { LexGetToken(P, NULL, TRUE); sz = (int)sizeof(double); } /* long double → double */
                        else
#endif
                        if (LexGetToken(P, NULL, FALSE) == TokenLongType)
                        { LexGetToken(P, NULL, TRUE); sz = (int)sizeof(long long); }
                        else sz = (int)sizeof(long);
                    }
                    else if (T2 == TokenUnsignedType)
                    {
                        LexGetToken(P, NULL, TRUE);
                        if (LexGetToken(P, NULL, FALSE) == TokenLongType)
                        {
                            LexGetToken(P, NULL, TRUE);
                            if (LexGetToken(P, NULL, FALSE) == TokenLongType)
                            { LexGetToken(P, NULL, TRUE); sz = (int)sizeof(unsigned long long); }
                            else sz = (int)sizeof(unsigned long);
                        }
                        else if (LexGetToken(P, NULL, FALSE) == TokenIntType)
                        { LexGetToken(P, NULL, TRUE); sz = (int)sizeof(unsigned int); }
                        else if (LexGetToken(P, NULL, FALSE) == TokenCharType)
                        { LexGetToken(P, NULL, TRUE); sz = 1; }
                        else if (LexGetToken(P, NULL, FALSE) == TokenShortType)
                        { LexGetToken(P, NULL, TRUE); sz = 2; }
                        else sz = (int)sizeof(unsigned int);
                    }
#ifndef NO_FP
                    else if (T2 == TokenFloatType)
                    { LexGetToken(P, NULL, TRUE); sz = (int)sizeof(float); }
                    else if (T2 == TokenDoubleType)
                    { LexGetToken(P, NULL, TRUE); sz = (int)sizeof(double); }
#endif
                    else if (T2 == TokenVoidType)
                    { LexGetToken(P, NULL, TRUE); sz = 1; }
                    else return NULL;
                }
                else
                    sz = Typ->Sizeof > 0 ? Typ->Sizeof : 1;
#ifndef NO_FP
                /* TypeParse currently maps both float and double to FPType
                 * (the interpreter's double-sized working representation).
                 * sizeof must report the declared C object size instead. */
                if (T2 == TokenFloatType && Typ->Base != TypePointer)
                    sz = (int)sizeof(float);
                else if (T2 == TokenDoubleType && Typ->Base != TypePointer)
                    sz = (int)sizeof(double);
#endif
                /* consume trailing * if any for sizeof(int*) */
                while (LexGetToken(P, NULL, FALSE) == TokenAsterisk)
                {
                    LexGetToken(P, NULL, TRUE);
                    sz = (int)sizeof(void *);
                }
                if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket)
                    return NULL;
            }
            else
            {
                /* sizeof (expr) */
                IrExpr *Inner = IrCompileExpr(P);
                if (Inner == NULL) return NULL;
                if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket)
                    return NULL;
                if (Inner->Kind == IrExConst && Inner->Op == TokenStringConstant
                    && Inner->ConstVal != 0)
                {
                    const char *s = (const char *)(intptr_t)Inner->ConstVal;
                    sz = (int)strlen(s) + 1;
                    /* sizeof folds this expression to a constant; it is not
                     * attached to the returned node, so release it here. */
                    IrFreeExpr(Inner);
                }
                else if (Inner->Kind == IrExIdent && Inner->LocalSlot >= 0
                         && Inner->LocalSlot < IR_COMPILE_MAX_LOCALS
                         && IrCompileLocalElemSize[Inner->LocalSlot] > 0)
                {
                    if (IrCompileLocalArraySize[Inner->LocalSlot] == -2)
                    {
                        E = IrAllocExpr(P->pc, IrExUnary);
                        if (E == NULL) return NULL;
                        E->Op = TokenSizeof;
                        E->Left = Inner;
                        E->IsVLAByteSize = 1;
                        return E;
                    }
                    sz = IrCompileLocalElemSize[Inner->LocalSlot];
                    E = IrAllocExpr(P->pc, IrExConst);
                    if (E == NULL) return NULL;
                    E->ConstVal = sz;
                    IrFreeExpr(Inner);
                    return E;
                }
                else if (Inner->Kind == IrExIndex && IrCompileVlaIndexSlot(Inner) >= 0)
                {
                    E = IrAllocExpr(P->pc, IrExUnary);
                    if (E == NULL) return NULL;
                    E->Op = TokenSizeof;
                    E->Left = Inner;
                    E->IsVLAByteSize = 1;
                    return E;
                }
                else if (Inner->Kind == IrExIdent && Inner->LocalSlot < 0
                         && Inner->Ident != NULL
                         && VariableDefined(P->pc, Inner->Ident))
                {
                    /* sizeof(global_object), including fixed/multidimensional
                     * arrays, must fold from the declared object type.  If it
                     * stays as runtime sizeof(expr), ConstVal is zero and the
                     * linear emitter silently treats the object size as zero. */
                    struct Value *SV = NULL;
                    VariableGet(P->pc, P, Inner->Ident, &SV);
                    if (SV != NULL && SV->Typ != NULL)
                        sz = TypeSize(SV->Typ, SV->Typ->ArraySize, TRUE);
                    if (sz > 0)
                    {
                        E = IrAllocExpr(P->pc, IrExConst);
                        if (E == NULL) return NULL;
                        E->ConstVal = sz;
                        IrFreeExpr(Inner);
                        return E;
                    }
                    E = IrAllocExpr(P->pc, IrExUnary);
                    if (E == NULL) return NULL;
                    E->Op = TokenSizeof;
                    E->Left = Inner;
                    return E;
                }
                else if (Inner->Kind == IrExIndex && Inner->Left != NULL
                         && Inner->Left->Kind == IrExIdent
                         && Inner->Left->LocalSlot >= 0
                         && Inner->Left->LocalSlot < IR_COMPILE_MAX_LOCALS)
                {
                    /* sizeof(a[i]): element size (Scale, or total/ArraySize). */
                    int slot = Inner->Left->LocalSlot;
                    int total = IrCompileLocalElemSize[slot];
                    int an = IrCompileLocalArraySize[slot];
                    int es = 0;
                    if (IrCompileLocalScale[slot] > 0)
                        es = IrCompileLocalScale[slot];
                    else if (Inner->ElemSize > 0)
                        es = Inner->ElemSize;
                    else if (an > 0 && total > 0 && (total % an) == 0)
                        es = total / an;
                    else if (total > 0)
                        es = 1; /* last resort: char-sized */
                    if (es <= 0) es = 1;
                    sz = es;
                    E = IrAllocExpr(P->pc, IrExConst);
                    if (E == NULL) return NULL;
                    E->ConstVal = sz;
                    IrFreeExpr(Inner);
                    return E;
                }
                else if (Inner->Kind == IrExMember)
                {
                    /* sizeof(s.m): prefer compile-time member Sizeof if known */
                    if (Inner->Ident != NULL && Inner->Left != NULL
                        && Inner->Left->Kind == IrExIdent)
                    {
                        struct Value *SV = NULL, *MV = NULL;
                        struct ValueType *ST = NULL;
                        /* Annotated ElemSize on Member (from parse) */
                        if (Inner->ElemSize > 0 && Inner->BitWidth == 0)
                        {
                            E = IrAllocExpr(P->pc, IrExConst);
                            if (E == NULL) return NULL;
                            E->ConstVal = Inner->ElemSize;
                            IrFreeExpr(Inner);
                            return E;
                        }
                        /* Local first: Body-IR Decl may not be in Variable table yet */
                        if (Inner->Left->LocalSlot >= 0
                            && Inner->Left->LocalSlot < IR_COMPILE_MAX_LOCALS)
                        {
                            ST = IrCompileLocalFromType[Inner->Left->LocalSlot];
                        }
                        /* Fallback: name → LocalFromType via name table */
                        if (ST == NULL && Inner->Left->Ident != NULL)
                        {
                            int li;
                            for (li = 0; li < IrCompileNumLocals; li++)
                            {
                                if (IrCompileLocalNames[li] != NULL
                                    && strcmp(IrCompileLocalNames[li], Inner->Left->Ident) == 0)
                                {
                                    ST = IrCompileLocalFromType[li];
                                    break;
                                }
                            }
                        }
                        if (ST == NULL && Inner->Left->Ident != NULL
                            && VariableDefined(P->pc, Inner->Left->Ident))
                        {
                            VariableGet(P->pc, P, Inner->Left->Ident, &SV);
                            if (SV != NULL) ST = SV->Typ;
                        }
                        if (ST != NULL)
                        {
                            if (ST->Base == TypePointer && ST->FromType != NULL)
                                ST = ST->FromType;
                            if (ST != NULL && ST->Members != NULL
                                && TableGet(ST->Members, Inner->Ident, &MV, NULL, NULL, NULL)
                                && MV != NULL && MV->Typ != NULL
                                && TypeSize(MV->Typ, MV->Typ->ArraySize, TRUE) > 0
                                && MV->Val != NULL && MV->Val->Integer >= 0
                                && MV->BitWidth == 0)
                            {
                                /* Use the shared semantic size helper instead of the
                                 * cached Sizeof field: FP members can have binary32
                                 * object storage while execution values use double slots,
                                 * and array/member metadata may need the derived size. */
                                E = IrAllocExpr(P->pc, IrExConst);
                                if (E == NULL) return NULL;
                                E->ConstVal = TypeSize(MV->Typ, MV->Typ->ArraySize, TRUE);
                                IrFreeExpr(Inner);
                                return E;
                            }
                        }
                    }
                    /* runtime sizeof unary */
                    E = IrAllocExpr(P->pc, IrExUnary);
                    if (E == NULL) return NULL;
                    E->Op = TokenSizeof;
                    E->Left = Inner;
                    return E;
                }
                else
                {
                    E = IrAllocExpr(P->pc, IrExUnary);
                    if (E == NULL) return NULL;
                    E->Op = TokenSizeof;
                    E->Left = Inner;
                    return E;
                }
            }
        }
        else
        {
            IrExpr *Inner = IrCompilePrimary(P);
            if (Inner == NULL) return NULL;
            if (Inner->Kind == IrExConst && Inner->Op == TokenStringConstant && Inner->ConstVal != 0)
            {
                const char *s = (const char *)(intptr_t)Inner->ConstVal;
                sz = (int)strlen(s) + 1;
            }
            else if (Inner->Kind == IrExIdent && Inner->LocalSlot >= 0
                     && Inner->LocalSlot < IR_COMPILE_MAX_LOCALS
                     && IrCompileLocalElemSize[Inner->LocalSlot] > 0)
            {
                if (IrCompileLocalArraySize[Inner->LocalSlot] == -2)
                {
                    E = IrAllocExpr(P->pc, IrExUnary);
                    if (E == NULL) return NULL;
                    E->Op = TokenSizeof;
                    E->Left = Inner;
                    E->IsVLAByteSize = 1;
                    return E;
                }
                sz = IrCompileLocalElemSize[Inner->LocalSlot];
            }
            else if (Inner->Kind == IrExIndex && IrCompileVlaIndexSlot(Inner) >= 0)
            {
                E = IrAllocExpr(P->pc, IrExUnary);
                if (E == NULL) return NULL;
                E->Op = TokenSizeof;
                E->Left = Inner;
                E->IsVLAByteSize = 1;
                return E;
            }
            else if (Inner->Kind == IrExIndex && Inner->Left != NULL
                     && Inner->Left->Kind == IrExIdent
                     && Inner->Left->LocalSlot >= 0
                     && Inner->Left->LocalSlot < IR_COMPILE_MAX_LOCALS)
            {
                int slot = Inner->Left->LocalSlot;
                int total = IrCompileLocalElemSize[slot];
                int an = IrCompileLocalArraySize[slot];
                int es = 0;
                if (IrCompileLocalScale[slot] > 0)
                    es = IrCompileLocalScale[slot];
                else if (Inner->ElemSize > 0)
                    es = Inner->ElemSize;
                else if (an > 0 && total > 0 && (total % an) == 0)
                    es = total / an;
                else if (total > 0)
                    es = 1;
                if (es <= 0) es = 1;
                sz = es;
            }
            else if (Inner->Kind == IrExIdent && Inner->Ident != NULL
                     && VariableDefined(P->pc, Inner->Ident))
            {
                /* sizeof(global_object) is a compile-time query just like
                 * sizeof(local_array).  Keeping it as a runtime unary node
                 * leaves ConstVal zero; Linear then emits sizeof(a)==0 and
                 * loops over global arrays silently skip or index invalidly. */
                struct Value *SV = NULL;
                VariableGet(P->pc, P, Inner->Ident, &SV);
                if (SV != NULL && SV->Typ != NULL)
                    sz = TypeSize(SV->Typ, SV->Typ->ArraySize, TRUE);
                if (sz <= 0)
                {
                    E = IrAllocExpr(P->pc, IrExUnary);
                    if (E == NULL) return NULL;
                    E->Op = TokenSizeof;
                    E->Left = Inner;
                    return E;
                }
            }
            else
            {
                E = IrAllocExpr(P->pc, IrExUnary);
                if (E == NULL) return NULL;
                E->Op = TokenSizeof;
                E->Left = Inner;
                return E;
            }
            IrFreeExpr(Inner);
        }
        E = IrAllocExpr(P->pc, IrExConst);
        if (E == NULL) return NULL;
        E->ConstVal = sz;
        return E;
    }
    /* Unary + - ! ~ * (deref) & (addr-of) prefix ++/-- */
    if (Tok == TokenMinus || Tok == TokenPlus
        || Tok == TokenUnaryNot || Tok == TokenUnaryExor
        || Tok == TokenAsterisk
        || Tok == TokenAmpersand
        || Tok == TokenIncrement || Tok == TokenDecrement)
    {
        IrExpr *Inner = IrCompilePrimary(P);
        if (Inner == NULL) return NULL;
        if (Tok == TokenPlus)
            return Inner;
        if (Tok == TokenMinus)
        {
            E = IrAllocExpr(P->pc, IrExBinOp);
            if (E == NULL) return NULL;
            E->Op = TokenMinus;
            E->Left = IrAllocExpr(P->pc, IrExConst);
            if (E->Left == NULL) return NULL;
            E->Left->ConstVal = 0;
            E->Right = Inner;
            return E;
        }
        if (Tok == TokenAsterisk)
        {
            /* * on pointer ident (incl. global fptr/void*), index, member, unary chain */
            if (Inner->Kind != IrExIdent && Inner->Kind != IrExIndex
                && Inner->Kind != IrExBinOp && Inner->Kind != IrExUnary
                && Inner->Kind != IrExMember && Inner->Kind != IrExCall
                && Inner->Kind != IrExVaArg)
                IR_FAIL("deref form not in Body-IR");
        }
        if (Tok == TokenAmpersand)
        {
            /* &x / &a[i] / &s.m / &p->m (bitfield still may fail at runtime) */
            if (Inner->Kind != IrExIdent && Inner->Kind != IrExIndex
                && Inner->Kind != IrExMember)
                IR_FAIL("addr-of form not in Body-IR");
        }
        E = IrAllocExpr(P->pc, IrExUnary);
        if (E == NULL) return NULL;
        E->Op = Tok;
        E->Left = Inner;
        E->ConstVal = 0; /* prefix; TokenAsterisk = dereference; TokenAmpersand = addr-of */
        /* Stamp ElemSize/TF_PTR on *p / **pp so linear lower loads the right
         * cell width (sizeof(void*) for intermediate pointer cells on LP64).
         * Walk to the root Ident and peel FromType once per effective * level.
         * &x adds a pointer level that the first * cancels. */
        if (Tok == TokenAsterisk)
        {
            IrExpr *Root = Inner;
            int stars = 1; /* this Unary* */
            int saw_amp = 0;
            struct ValueType *PT = NULL;
            int es = 0;
            int result_is_ptr = 0;
            while (Root != NULL && Root->Kind == IrExUnary
                   && (Root->Op == TokenAsterisk || Root->Op == TokenAmpersand))
            {
                if (Root->Op == TokenAmpersand)
                {
                    saw_amp = 1;
                    Root = Root->Left;
                    break;
                }
                stars++;
                Root = Root->Left;
            }
            if (Root != NULL && Root->Kind == IrExIdent
                && Root->LocalSlot >= 0 && Root->LocalSlot < IR_COMPILE_MAX_LOCALS)
                PT = IrCompileLocalFromType[Root->LocalSlot];
            if (PT != NULL)
            {
                int i, peels;
                struct ValueType *T = PT;
                /* *&x → type of x; ****&pppx with pppx:int*** → peel 3 from int*** */
                peels = saw_amp ? (stars - 1) : stars;
                if (peels < 0) peels = 0;
                for (i = 0; i < peels && T != NULL; i++)
                {
                    if (T->Base == TypePointer && T->FromType != NULL)
                        T = T->FromType;
                    else if (T->Base == TypeArray && T->FromType != NULL)
                        T = T->FromType;
                    else
                        break;
                }
                if (T != NULL)
                {
                    if (T->Base == TypePointer || T->Base == TypeArray)
                    {
                        es = (int)sizeof(void *);
                        result_is_ptr = 1;
                    }
                    else if (T->Sizeof > 0)
                        es = (int)T->Sizeof;
                }
            }
            if (es <= 0 && Inner->ElemSize > 0)
                es = Inner->ElemSize;
            if (es <= 0 && (Inner->TypeFlags & IR_TF_PTR))
                es = (int)sizeof(void *);
            if (es > 0)
                E->ElemSize = es;
            if (result_is_ptr)
                E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
            else
                E->TypeFlags |= IR_TF_RESOLVED;
        }
        return E;
    }
    if (Tok == TokenIntegerConstant)
    {
        E = IrAllocExpr(P->pc, IrExConst);
        if (E == NULL) return NULL;
        E->ConstVal = ExpressionCoerceInteger(LexVal);
        /* Keep the lex type (u/ul suffix) so binops wrap to that width. */
        if (LexVal != NULL && LexVal->Typ != NULL)
            E->I32 = (int)LexVal->Typ->Base;
        /* 4[a] postfix: share ident postfix path (swaps const/array) */
        goto ident_or_call_postfix;
    }
#ifndef NO_FP
    if (Tok == TokenFPConstant)
    {
        union { double d; long long l; } u;
        E = IrAllocExpr(P->pc, IrExConst);
        if (E == NULL) return NULL;
        u.d = LexVal->Val->FP;
        E->ConstVal = u.l;
        E->Op = TokenFPConstant; /* IEEE bits in ConstVal */
        return E;
    }
#endif
    if (Tok == TokenCharacterConstant)
    {
        E = IrAllocExpr(P->pc, IrExConst);
        if (E == NULL) return NULL;
        E->ConstVal = (long long)(unsigned char)LexVal->Val->Character;
        /* 'A'[s] rare; still allow index postfix */
        goto ident_or_call_postfix;
    }
    if (Tok == TokenStringConstant)
    {
        E = IrAllocExpr(P->pc, IrExConst);
        if (E == NULL) return NULL;
        E->ConstVal = (long long)(intptr_t)LexVal->Val->Pointer;
        E->Op = TokenStringConstant;
    string_const_concat:
        for (;;) {
            enum LexToken Pk = LexGetToken(P, NULL, FALSE);
            const char *s1, *s2; char *joined; size_t n1, n2;
            if (Pk == TokenStringConstant) {
                struct Value *Lv2 = NULL;
                LexGetToken(P, &Lv2, TRUE);
                if (Lv2 == NULL || Lv2->Val == NULL) break;
                s1 = (const char *)(intptr_t)E->ConstVal;
                s2 = (const char *)Lv2->Val->Pointer;
            } else if (Pk == TokenIdentifier) {
                struct Value *Lv = NULL, *MV = NULL; struct ParseState Look; enum LexToken Mt; struct Value *MLex = NULL; char *Id;
                ParserCopy(&Look, P); LexGetToken(&Look, &Lv, TRUE);
                if (Lv == NULL) break;
                Id = Lv->Val->Identifier;
                if (!VariableDefined(P->pc, Id) || !TableGet(&P->pc->GlobalTable, Id, &MV, NULL, NULL, NULL) ||
                    MV == NULL || MV->Typ->Base != TypeMacro || MV->Val->MacroDef.NumParams != 0) break;
                ParserCopy(&Look, &MV->Val->MacroDef.Body);
                Mt = LexGetToken(&Look, &MLex, TRUE);
                if (Mt != TokenStringConstant || MLex == NULL) break;
                LexGetToken(P, NULL, TRUE);
                s1 = (const char *)(intptr_t)E->ConstVal;
                s2 = (const char *)MLex->Val->Pointer;
            } else break;
            if (s1 == NULL) s1 = "";
            if (s2 == NULL) s2 = "";
            n1 = strlen(s1); n2 = strlen(s2);
            /* Own via Ident so IrFreeExpr can free (HeapAllocMem not on expr free path). */
            joined = (char *)malloc((size_t)n1 + (size_t)n2 + 1);
            if (joined == NULL) return NULL;
            memcpy(joined, s1, n1); memcpy(joined + n1, s2, n2); joined[n1 + n2] = 0;
            /* Drop previous owned join buffer if we chain adjacent strings. */
            if (E->Ident != NULL)
                free(E->Ident);
            E->ConstVal = (long long)(intptr_t)joined;
            E->Ident = joined;
        }
        return E;
    }
    if (Tok == TokenIdentifier)
    {
        char *Name = LexVal->Val->Identifier;
        if (IrCompileMacroArgNames != NULL && IrCompileMacroArgCount > 0) {
            int mi;
            for (mi = 0; mi < IrCompileMacroArgCount; mi++)
                if (IrCompileMacroArgNames[mi] && strcmp(IrCompileMacroArgNames[mi], Name) == 0
                    && IrCompileMacroArgExprs && IrCompileMacroArgExprs[mi]) {
                    E = IrCloneExpr(IrCompileMacroArgExprs[mi]);
                    if (E == NULL) return NULL;
                    return E;
                }
        }
        if (LexGetToken(P, NULL, FALSE) != TokenOpenBracket) {
            struct Value *MV = NULL;
            if (VariableDefined(P->pc, Name) && TableGet(&P->pc->GlobalTable, Name, &MV, NULL, NULL, NULL)
                && MV && MV->Typ->Base == TypeMacro && MV->Val->MacroDef.NumParams == 0) {
                struct ParseState MP; enum LexToken Mt; struct Value *MLex = NULL;
                ParserCopy(&MP, P); MP.Pos = MV->Val->MacroDef.Body.Pos;
                Mt = LexGetToken(&MP, &MLex, TRUE);
                if (Mt == TokenStringConstant && MLex
                    && LexGetToken(&MP, NULL, FALSE) == TokenEndOfFunction) {
                    E = IrAllocExpr(P->pc, IrExConst); if (!E) return NULL;
                    E->ConstVal = (long long)(intptr_t)MLex->Val->Pointer;
                    E->Op = TokenStringConstant; goto string_const_concat;
                }
                {
                    IrExpr *ME;
                    ParserCopy(&MP, P);
                    MP.Pos = MV->Val->MacroDef.Body.Pos;
                    ME = IrCompileExpr(&MP);
                    if (ME != NULL)
                        return ME;
                }
            }
        }
        {
            extern int StdargMacroIntrinsicKind(Picoc *, struct MacroDef *);
            struct Value *StdargMacro = NULL;
            int IsTypedVaArg = VariableDefined(P->pc, Name)
                && TableGet(&P->pc->GlobalTable, Name, &StdargMacro, NULL, NULL, NULL)
                && StdargMacro != NULL && StdargMacro->Typ != NULL
                && StdargMacro->Typ->Base == TypeMacro
                && StdargMacroIntrinsicKind(P->pc, &StdargMacro->Val->MacroDef) == 2;
            if (IsTypedVaArg && LexGetToken(P, NULL, FALSE) == TokenOpenBracket)
            {
            struct ValueType *RequestedType = NULL;
            char *TypeName = NULL;
            IrExpr *ListExpr;
            /* The second operand is a C type-name, not an expression. Keep it
             * typed in IR instead of forcing it through generic macro args. */
            LexGetToken(P, NULL, TRUE); /* '(' */
            ListExpr = IrCompileAssignExpr(P);
            if (ListExpr == NULL || LexGetToken(P, NULL, TRUE) != TokenComma)
            {
                if (ListExpr != NULL) IrFreeExpr(ListExpr);
                return NULL;
            }
            TypeParse(P, &RequestedType, &TypeName, NULL);
            if (RequestedType == NULL || LexGetToken(P, NULL, TRUE) != TokenCloseBracket)
            {
                IrFreeExpr(ListExpr);
                return NULL;
            }
            E = IrAllocExpr(P->pc, IrExVaArg);
            if (E == NULL) { IrFreeExpr(ListExpr); return NULL; }
            E->Left = ListExpr;
            E->VaType = RequestedType;
            E->CanonicalBase = (int)RequestedType->Base;
            E->CanonicalFlags = (unsigned char)(IR_CF_VALID
                | (RequestedType->Base == TypeFP ? IR_CF_FP : 0)
                | ((RequestedType->Base == TypePointer || RequestedType->Base == TypeArray) ? IR_CF_PTR : 0)
                | (IrTypeIsUnsignedBase(RequestedType->Base) ? IR_CF_UNSIGNED : 0));
            E->TypeFlags = (unsigned char)(IR_TF_RESOLVED
                | (RequestedType->Base == TypeFP ? IR_TF_FP : 0)
                | ((RequestedType->Base == TypePointer || RequestedType->Base == TypeArray) ? IR_TF_PTR : 0)
                | (IrTypeIsUnsignedBase(RequestedType->Base) ? IR_TF_UNSIGNED : 0));
            E->ValWidth = (RequestedType->Sizeof >= 8) ? IR_VW_I64
                : (RequestedType->Sizeof == 1) ? IR_VW_I8
                : (RequestedType->Sizeof == 2) ? IR_VW_I16 : IR_VW_I32;
            E->SourceLine = P->Line;
            E->CharacterPos = P->CharacterPos;
            if (RequestedType->Base == TypePointer && RequestedType->FromType != NULL)
                E->ElemSize = (RequestedType->FromType->Base == TypePointer)
                    ? (int)sizeof(void *) : (int)RequestedType->FromType->Sizeof;
            goto ident_or_call_postfix; /* va_arg(ap, T *)[i], ->m, ++ */
            }
        }
        if (LexGetToken(P, NULL, FALSE) == TokenOpenBracket) {
            struct Value *MVCall = NULL;
            if (VariableDefined(P->pc, Name) && TableGet(&P->pc->GlobalTable, Name, &MVCall, NULL, NULL, NULL)
                && MVCall && MVCall->Typ->Base == TypeMacro) {
                struct MacroDef *MD = &MVCall->Val->MacroDef;
                IrExpr **MArgs; int MCap = MD->NumParams > 0 ? MD->NumParams : 1, MN = 0;
                IrExpr *Expanded; struct ParseState BodyP;
                char **SaveNames; IrExpr **SaveExprs; int SaveCount, SaveDepth;
                LexGetToken(P, NULL, TRUE);
                MArgs = (IrExpr **)calloc((size_t)MCap, sizeof(IrExpr *));
                if (!MArgs) IR_FAIL("OOM macro args");
                if (LexGetToken(P, NULL, FALSE) != TokenCloseBracket) {
                    for (;;) {
                        IrExpr *A = IrCompileAssignExpr(P);
                        if (!A) { IrFreeExprVector(MArgs, MN); return NULL; }
                        if (MN >= MCap) {
                            int nc = MCap*2; IrExpr **nb = (IrExpr **)realloc(MArgs, (size_t)nc*sizeof(IrExpr *));
                            if (!nb) { IrFreeExpr(A); IrFreeExprVector(MArgs, MN); IR_FAIL("OOM macro grow"); }
                            MArgs = nb; MCap = nc;
                        }
                        MArgs[MN++] = A;
                        if (LexGetToken(P, NULL, FALSE) == TokenComma) LexGetToken(P, NULL, TRUE); else break;
                    }
                }
                if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket) { IrFreeExprVector(MArgs, MN); return NULL; }
                if (MN != MD->NumParams) { IrFreeExprVector(MArgs, MN); IR_FAIL("macro argument count mismatch"); }
                if (IrCompileMacroDepth > 8) { IrFreeExprVector(MArgs, MN); IR_FAIL("macro expansion depth"); }
                if (!MD->Body.Pos) { IrFreeExprVector(MArgs, MN); IR_FAIL("empty macro body"); }
                SaveNames = IrCompileMacroArgNames; SaveExprs = IrCompileMacroArgExprs;
                SaveCount = IrCompileMacroArgCount; SaveDepth = IrCompileMacroDepth;
                IrCompileMacroArgNames = MD->ParamName; IrCompileMacroArgExprs = MArgs;
                IrCompileMacroArgCount = MN; IrCompileMacroDepth = SaveDepth + 1;
                ParserCopy(&BodyP, &MD->Body); BodyP.pc = P->pc; BodyP.Mode = P->Mode;
                Expanded = IrCompileExpr(&BodyP);
                IrCompileMacroArgNames = SaveNames; IrCompileMacroArgExprs = SaveExprs;
                IrCompileMacroArgCount = SaveCount; IrCompileMacroDepth = SaveDepth;
                /* Parameter references are cloned into the expansion; the
                 * parsed argument trees are now detached and must be freed. */
                IrFreeExprVector(MArgs, MN);
                if (!Expanded) return NULL;
                return Expanded;
            }
        }
        /* call? */
        if (LexGetToken(P, NULL, FALSE) == TokenOpenBracket)
        {
            IrExpr **Args;
            int Cap = 16, N = 0;
            LexGetToken(P, NULL, TRUE); /* ( */
            Args = (IrExpr **)calloc(Cap, sizeof(IrExpr *));
            if (Args == NULL) return NULL;
            if (LexGetToken(P, NULL, FALSE) != TokenCloseBracket)
            {
                for (;;)
                {
                    IrExpr *A = IrCompileAssignExpr(P);
                    if (A == NULL) { free(Args); return NULL; }
                    if (N >= Cap)
                    {
                        int ncap = Cap * 2;
                        IrExpr **na = (IrExpr **)realloc(Args, (size_t)ncap * sizeof(IrExpr *));
                        if (na == NULL) { free(Args); return NULL; }
                        Args = na; Cap = ncap;
                    }
                    Args[N++] = A;
                    Tok = LexGetToken(P, NULL, TRUE);
                    if (Tok == TokenCloseBracket) break;
                    if (Tok != TokenComma) { free(Args); return NULL; }
                }
            }
            else
                LexGetToken(P, NULL, TRUE);

            /* Grey list (relaxed): printf/scanf family accept:
             *  - string literal format (checked IR-safe when known)
             *  - ternary of two string literals
             *  - pointer/ident format (runtime; Intrinsic packs by value kind)
             * fgets/fputs/getchar always OK. */
            if (Name != NULL) {
                int is_printf = (strcmp(Name, "printf") == 0);
                int is_sprintf = (strcmp(Name, "sprintf") == 0);
                int is_snprintf = (strcmp(Name, "snprintf") == 0);
                int is_fprintf = (strcmp(Name, "fprintf") == 0);
                int is_scanf = (strcmp(Name, "scanf") == 0);
                int is_fscanf = (strcmp(Name, "fscanf") == 0);
                int is_sscanf = (strcmp(Name, "sscanf") == 0);
                if (is_printf || is_sprintf || is_snprintf || is_fprintf) {
                    int fmtIndex = is_printf ? 0 : (is_snprintf ? 2 : (is_fprintf ? 1 : 1));
                    const char *fmt = NULL;
                    IrExpr *FmtE;
                    if (N <= fmtIndex) IR_FAIL("printf/fprintf/sprintf: missing format argument");
                    FmtE = Args[fmtIndex];
                    if (FmtE == NULL)
                        IR_FAIL("printf: null format");
                    if (FmtE->Kind == IrExConst && FmtE->Op == TokenStringConstant)
                    {
                        fmt = (const char *)(intptr_t)FmtE->ConstVal;
                        if (fmt != NULL && !PrintfFormatIsIrSafe(fmt))
                            IR_FAIL("printf/fprintf/sprintf: format not IR-safe");
                    }
                    else if (FmtE->Kind == IrExTernary
                             && FmtE->Right != NULL
                             && FmtE->Args != NULL && FmtE->ArgCount >= 1
                             && FmtE->Right->Kind == IrExConst && FmtE->Right->Op == TokenStringConstant
                             && FmtE->Args[0] != NULL
                             && FmtE->Args[0]->Kind == IrExConst && FmtE->Args[0]->Op == TokenStringConstant)
                    {
                        if (!PrintfFormatIsIrSafe((const char *)(intptr_t)FmtE->Right->ConstVal)
                            || !PrintfFormatIsIrSafe((const char *)(intptr_t)FmtE->Args[0]->ConstVal))
                            IR_FAIL("printf: ternary format arms not IR-safe");
                    }
                    else if (FmtE->Kind == IrExIdent
                             || FmtE->Kind == IrExIndex
                             || FmtE->Kind == IrExMember
                             || (FmtE->Kind == IrExUnary && FmtE->Op == TokenAmpersand)
                             || (FmtE->Kind == IrExUnary && FmtE->Op == TokenAsterisk)
                             || (FmtE->Kind == IrExUnary && FmtE->Op == TokenCast)
                             || FmtE->Kind == IrExCall
                             || FmtE->Kind == IrExTernary)
                    {
                        /* variable / computed format pointer — allow (runtime) */
                    }
                    else
                        IR_FAIL("printf/fprintf/sprintf: format not usable in Body-IR");
                }
                /* scanf family: literal format preferred; ident/pointer format OK */
                if (is_scanf || is_fscanf || is_sscanf) {
                    int fmtIndex = is_fscanf ? 1 : 0;
                    IrExpr *FmtE;
                    if (N <= fmtIndex)
                        IR_FAIL("scanf: missing format argument");
                    FmtE = Args[fmtIndex];
                    if (FmtE == NULL)
                        IR_FAIL("scanf: null format");
                    if (FmtE->Kind == IrExConst && FmtE->Op == TokenStringConstant)
                        ; /* ok */
                    else if (FmtE->Kind == IrExIdent
                             || FmtE->Kind == IrExIndex
                             || FmtE->Kind == IrExMember
                             || (FmtE->Kind == IrExUnary && FmtE->Op == TokenCast)
                             || FmtE->Kind == IrExCall
                             || FmtE->Kind == IrExTernary)
                        ; /* variable format OK */
                    else
                        IR_FAIL("scanf: format must be string or pointer in Body-IR");
                }
            }

            E = IrAllocExpr(P->pc, IrExCall);
            if (E == NULL) return NULL;
            /* Param/local fptr: call through Value*, not global function name */
            {
                int IsFptrVar = 0;
                int si;
                if (IrCompileParamNames != NULL)
                {
                    for (si = 0; si < IrCompileNumParams; si++)
                        if (IrCompileParamNames[si] != NULL
                            && strcmp(IrCompileParamNames[si], Name) == 0)
                        { IsFptrVar = 1; break; }
                }
                for (si = 0; !IsFptrVar && si < IrCompileNumLocals; si++)
                    if (IrCompileLocalNames[si] != NULL
                        && strcmp(IrCompileLocalNames[si], Name) == 0)
                    { IsFptrVar = 1; break; }
                if (IsFptrVar)
                {
                    IrExpr *Callee = IrAllocExpr(P->pc, IrExIdent);
                    if (Callee == NULL) return NULL;
                    Callee->Ident = Name;
                    IrBindIdentSlots(P, Callee);
                    E->Left = Callee;
                    E->Ident = NULL;
                }
                else
                {
                    E->Ident = Name;
                    IrBindIdentSlots(P, E);
                }
            }
            E->Args = Args;
            E->ArgCount = N;
            /* fall through to shared postfix (f()->m, f()[i], ...) */
            goto ident_or_call_postfix;
        }
        E = IrAllocExpr(P->pc, IrExIdent);
        if (E == NULL) return NULL;
        E->Ident = Name;
        IrBindIdentSlots(P, E);
        /* Root: fold true rvalue constants (enum: IsLValue=0).
         * Reuse CachedVal from IrBindIdentSlots — no second TableGet. */
        {
            struct Value *GV = E->CachedVal;
            enum LexToken PeekPost;
            if (E->LocalSlot < 0 && E->ParamSlot < 0
                && GV != NULL && GV->Typ != NULL && GV->Val != NULL
                && !GV->IsLValue
                && (GV->Typ->Base == TypeInt || GV->Typ->Base == TypeChar
                    || GV->Typ->Base == TypeShort || GV->Typ->Base == TypeLong
                    || GV->Typ->Base == TypeLongLong
                    || GV->Typ->Base == TypeUnsignedInt
                    || GV->Typ->Base == TypeUnsignedChar
                    || GV->Typ->Base == TypeUnsignedShort
                    || GV->Typ->Base == TypeUnsignedLong
                    || GV->Typ->Base == TypeUnsignedLongLong
                    || GV->Typ->Base == TypeEnum))
            {
                PeekPost = LexGetToken(P, NULL, FALSE);
                if (PeekPost != TokenOpenBracket
                    && PeekPost != TokenLeftSquareBracket
                    && PeekPost != TokenDot
                    && PeekPost != TokenArrow
                    && PeekPost != TokenIncrement
                    && PeekPost != TokenDecrement)
                {
                    long long cv = ExpressionCoerceInteger(GV);
                    free(E);
                    E = IrAllocExpr(P->pc, IrExConst);
                    if (E == NULL) return NULL;
                    E->ConstVal = cv;
                    return E;
                }
            }
        }
        /* Postfix: ++/-- or [index] (param/local pointer or array) */
        ident_or_call_postfix:
        for (;;)
        {
            enum LexToken Peek = LexGetToken(P, NULL, FALSE);
            if (Peek == TokenIncrement || Peek == TokenDecrement)
            {
                IrExpr *U;
                LexGetToken(P, NULL, TRUE);
                U = IrAllocExpr(P->pc, IrExUnary);
                if (U == NULL) return NULL;
                U->Op = Peek;
                U->Left = E;
                U->ConstVal = 1; /* postfix */
                return U;
            }
            if (Peek == TokenLeftSquareBracket)
            {
                IrExpr *Idx, *Ix;
                /* Index: param/local/global 1D or multi-dim, member array, nested Index.
                 * Runtime IrResolvePtr already scales multi-dim globals via FromType->Sizeof.
                 * C also allows 4[a] (commutative with a[4]): const/int base + array index. */
                if (E->Kind == IrExIndex)
                    ; /* a[i][j] */
                else if (E->Kind == IrExMember)
                    ;
                else if (E->Kind == IrExCall)
                    ; /* f()[i] */
                else if (E->Kind == IrExUnary)
                    ; /* (*p)[i] */
                else if (E->Kind == IrExVaArg)
                    ; /* va_arg(ap, T *)[i] */
                else if (E->Kind == IrExConst)
                    ; /* 4[a] — swap below */
                else if (E->Kind != IrExIdent)
                    return NULL;
                /* globals (incl. multi-dim int A[N][N]) allowed — do not refuse */
                LexGetToken(P, NULL, TRUE); /* [ */
                Idx = IrCompileExpr(P);
                if (Idx == NULL) return NULL;
                if (LexGetToken(P, NULL, TRUE) != TokenRightSquareBracket)
                    return NULL;
                Ix = IrAllocExpr(P->pc, IrExIndex);
                if (Ix == NULL) return NULL;
                Ix->SourceLine = P->Line;
                Ix->CharacterPos = P->CharacterPos;
                /* 4[a]: Left must be array/pointer, Right the integer index */
                if (E->Kind == IrExConst
                    && (Idx->Kind == IrExIdent || Idx->Kind == IrExIndex
                        || Idx->Kind == IrExMember || Idx->Kind == IrExUnary
                        || Idx->Kind == IrExCall))
                {
                    Ix->Left = Idx;
                    Ix->Right = E;
                }
                else
                {
                    Ix->Left = E;
                    Ix->Right = Idx;
                }
                E = Ix;
                continue; /* allow a[i][j] */
            }
            if (Peek == TokenDot || Peek == TokenArrow)
            {
                struct Value *MLex = NULL;
                IrExpr *M;
                LexGetToken(P, NULL, TRUE); /* . or -> */
                if (LexGetToken(P, &MLex, TRUE) != TokenIdentifier)
                    return NULL;
                /* Base: ident/index/member/unary-deref/call (f()->m) */
                if (E->Kind != IrExIdent && E->Kind != IrExIndex
                    && E->Kind != IrExMember && E->Kind != IrExUnary
                    && E->Kind != IrExCall && E->Kind != IrExVaArg)
                    return NULL;
                M = IrAllocExpr(P->pc, IrExMember);
                if (M == NULL) return NULL;
                M->SourceLine = P->Line;
                M->Left = E;
                M->Ident = MLex->Val->Identifier;
                M->CharacterPos = P->CharacterPos - (int)strlen(M->Ident) - 1;
                if (M->CharacterPos < 1) M->CharacterPos = 1;
                M->Op = Peek; /* TokenDot or TokenArrow */
                E = M;
                continue; /* allow p->a.b / s.a.b / (*p).m */
            }
            /* p->fp(args) / s.fp(args) / fp(args) after postfix chain */
            if (Peek == TokenOpenBracket
                && (E->Kind == IrExMember
                    || (E->Kind == IrExUnary && E->Op == TokenAsterisk)
                    || E->Kind == IrExIndex
                    || E->Kind == IrExIdent))
            {
                IrExpr *Call;
                IrExpr **Args;
                int Cap = 8, N = 0;
                LexGetToken(P, NULL, TRUE); /* ( */
                Args = (IrExpr **)calloc(Cap, sizeof(IrExpr *));
                if (Args == NULL) return NULL;
                if (LexGetToken(P, NULL, FALSE) != TokenCloseBracket)
                {
                    for (;;)
                    {
                        IrExpr *A = IrCompileAssignExpr(P);
                        if (A == NULL) { free(Args); return NULL; }
                        if (N >= Cap)
                        {
                            Cap *= 2;
                            Args = (IrExpr **)realloc(Args, Cap * sizeof(IrExpr *));
                            if (Args == NULL) return NULL;
                        }
                        Args[N++] = A;
                        if (LexGetToken(P, NULL, FALSE) == TokenComma)
                            LexGetToken(P, NULL, TRUE);
                        else
                            break;
                    }
                }
                if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket)
                    return NULL;
                Call = IrAllocExpr(P->pc, IrExCall);
                if (Call == NULL) return NULL;
                Call->Left = E; /* callee: member / *fp / local fptr */
                Call->Ident = NULL;
                Call->Args = Args;
                Call->ArgCount = N;
                E = Call;
                continue;
            }
            break;
        }
        return E;
    }
    IR_FAIL("expression form not supported in Body-IR");
}

/* expression with + - * / % and relational < > <= >= == != (all left-assoc, flat) */
/* C operator precedence for IR (subset). Higher = binds tighter. */
int IrOpPrecedence(enum LexToken Op)
{
    switch (Op)
    {
        case TokenAsterisk:
        case TokenSlash:
        case TokenModulus:
            return 10;
        case TokenPlus:
        case TokenMinus:
            return 9;
        case TokenShiftLeft:
        case TokenShiftRight:
            return 8;
        case TokenLessThan:
        case TokenGreaterThan:
        case TokenLessEqual:
        case TokenGreaterEqual:
            return 7;
        case TokenEqual:
        case TokenNotEqual:
            return 6;
        case TokenAmpersand:
            return 5;
        case TokenArithmeticExor:
            return 4;
        case TokenArithmeticOr:
            return 3;
        case TokenLogicalAnd:
            return 3;
        case TokenLogicalOr:
            return 2;
        case TokenQuestionMark:
            return 1; /* ternary; special-cased */
        case TokenAssign:
        case TokenAddAssign:
        case TokenSubtractAssign:
        case TokenMultiplyAssign:
        case TokenDivideAssign:
        case TokenModulusAssign:
        case TokenShiftLeftAssign:
        case TokenShiftRightAssign:
        case TokenArithmeticAndAssign:
        case TokenArithmeticOrAssign:
        case TokenArithmeticExorAssign:
            return 1; /* above comma */
        case TokenComma:
            return 0; /* lowest of real operators */
        default:
            return -1;
    }
}

static IrExpr *IrCompileExprPrec(struct ParseState *P, int MinPrec)
{
    IrExpr *Left, *Right, *Bin;
    enum LexToken Op;
    int Prec;

    Left = IrCompilePrimary(P);
    if (Left == NULL) return NULL;

    for (;;)
    {
        Op = LexGetToken(P, NULL, FALSE);
        /* comma below assign: only when MinPrec allows full expression */
        if (Op == TokenComma && MinPrec <= 0)
        {
            IrExpr *Bin;
            LexGetToken(P, NULL, TRUE); /* , */
            Right = IrCompileExprPrec(P, 0); /* include assign on right */
            if (Right == NULL) return NULL;
            Bin = IrAllocExpr(P->pc, IrExBinOp);
            if (Bin == NULL) return NULL;
            Bin->Op = TokenComma;
            Bin->Left = Left;
            Bin->Right = Right;
            Left = Bin;
            continue;
        }
        Prec = IrOpPrecedence(Op);
        /* Prec < 0: not a binary operator (semicolon, brace, etc.) */
        if (Prec < 0 || Prec < MinPrec)
            break;
        /* cond ? then : else  (right-associative) */
        if (Op == TokenQuestionMark)
        {
            IrExpr *ThenE, *ElseE, *T;
            IrExpr **Args;
            LexGetToken(P, NULL, TRUE); /* ? */
            /* then-branch: full expression (C allows comma here) */
            ThenE = IrCompileExprPrec(P, 0);
            if (ThenE == NULL) return NULL;
            if (LexGetToken(P, NULL, TRUE) != TokenColon)
                return NULL;
            /* else-branch: conditional-expression only — must NOT swallow
             * a top-level comma (C: a?b:c, d is (a?b:c), d). MinPrec=1
             * excludes comma (prec 0) while keeping assign/ternary.
             * Fixes printf("%d %d", p<q?1:0, p==q?1:0) under Body-IR (406). */
            ElseE = IrCompileExprPrec(P, 1);
            if (ElseE == NULL) return NULL;
            T = IrAllocExpr(P->pc, IrExTernary);
            if (T == NULL) return NULL;
            T->Left = Left;
            T->Right = ThenE;
            Args = (IrExpr **)malloc(sizeof(IrExpr *));
            if (Args == NULL) return NULL;
            Args[0] = ElseE;
            T->Args = Args;
            T->ArgCount = 1;
            Left = T;
            continue;
        }
        LexGetToken(P, NULL, TRUE);
        {
            int EqCol = P->CharacterPos > 0 ? P->CharacterPos - 1 : 0;
            int IsAssign = (Op == TokenAssign || Op == TokenAddAssign
                || Op == TokenSubtractAssign || Op == TokenMultiplyAssign
                || Op == TokenDivideAssign || Op == TokenModulusAssign
                || Op == TokenShiftLeftAssign || Op == TokenShiftRightAssign
                || Op == TokenArithmeticAndAssign || Op == TokenArithmeticOrAssign
                || Op == TokenArithmeticExorAssign);
            Right = IrCompileExprPrec(P, IsAssign ? Prec : Prec + 1);
            if (Right == NULL) return NULL;
            if (Op == TokenAssign)
            {
                Bin = IrAllocExpr(P->pc, IrExAssign);
                if (Bin == NULL) return NULL;
                if (EqCol > 0)
                    Bin->CharacterPos = EqCol;
                Bin->Left = Left;
                Bin->Right = Right;
            }
            else if (IsAssign)
            {
                /* s += i  →  s = s + i  (and ^= <<= >>= &= |= likewise) */
                IrExpr *LhsCopy;
                IrExpr *Add;
                enum LexToken BinOp = TokenPlus;
                if (Op == TokenSubtractAssign) BinOp = TokenMinus;
                else if (Op == TokenMultiplyAssign) BinOp = TokenAsterisk;
                else if (Op == TokenDivideAssign) BinOp = TokenSlash;
                else if (Op == TokenModulusAssign) BinOp = TokenModulus;
                else if (Op == TokenShiftLeftAssign) BinOp = TokenShiftLeft;
                else if (Op == TokenShiftRightAssign) BinOp = TokenShiftRight;
                else if (Op == TokenArithmeticAndAssign) BinOp = TokenAmpersand;
                else if (Op == TokenArithmeticOrAssign) BinOp = TokenArithmeticOr;
                else if (Op == TokenArithmeticExorAssign) BinOp = TokenArithmeticExor;
                LhsCopy = IrCloneExpr(Left);
                if (LhsCopy == NULL) return NULL;
                Add = IrAllocExpr(P->pc, IrExBinOp);
                if (Add == NULL) return NULL;
                Add->Op = BinOp;
                Add->Left = LhsCopy;
                Add->Right = Right;
                Bin = IrAllocExpr(P->pc, IrExAssign);
                if (Bin == NULL) return NULL;
                Bin->Left = Left;
                Bin->Right = Add;
            }
            else
            {
                Bin = IrAllocExpr(P->pc, IrExBinOp);
                if (Bin == NULL) return NULL;
                Bin->Op = Op;
                Bin->Left = Left;
                Bin->Right = Right;
                /* Error caret: point at right operand (divisor for / %, etc.) */
                if (Right != NULL && Right->CharacterPos)
                    Bin->CharacterPos = Right->CharacterPos;
                if (Right != NULL && Right->SourceLine)
                    Bin->SourceLine = Right->SourceLine;
            }
            Left = Bin;
        }
    }
    return Left;
}

static IrExpr *IrCompileExpr(struct ParseState *P)
{
    return IrCompileExprPrec(P, 0); /* includes comma (prec 0) */
}
/* Assignment-expression (no comma) — for call args, initializers, etc. */
static IrExpr *IrCompileAssignExpr(struct ParseState *P)
{
    return IrCompileExprPrec(P, 1); /* assign and above, no comma */
}

static IrStmt *IrCompileStmt(struct ParseState *P)
{
    enum LexToken Tok;
    IrStmt *S;
    struct ParseState Save;

    IrAllocParserPos = P;
    Tok = LexGetToken(P, NULL, FALSE);

    /* empty statement: while (x);  for (...);  label: ; */
    if (Tok == TokenSemicolon)
    {
        LexGetToken(P, NULL, TRUE);
        S = IrAllocStmt(P->pc, IrStBlock);
        if (S == NULL) return NULL;
        S->Stmts = NULL;
        S->StmtCount = 0;
        return S;
    }

    /* Local typedef in function body: typedef int MyInt;
     * Register the type alias at compile-time (VariableDefine) so subsequent
     * MyInt x; is a typedef-name local (659 token_fn → Body-IR). Runtime
     * is a no-op block. */
    if (Tok == TokenTypedef)
    {
        enum RunMode SavedMode = P->Mode;
        LexGetToken(P, NULL, TRUE); /* typedef */
        P->Mode = RunModeRun;
        ParseTypedef(P);
        P->Mode = SavedMode;
        S = IrAllocStmt(P->pc, IrStBlock);
        if (S == NULL) return NULL;
        S->Stmts = NULL;
        S->StmtCount = 0;
        return S;
    }

    /* Skip leading const/volatile for all declarators (Body-IR).
     * const is recorded via LeadingConst → DeclIsConst for assign checks.
     * volatile is accepted as a pure qualifier (no extra runtime). */
    {
        int LeadingConst = 0;
        while (Tok == TokenConstType || Tok == TokenVolatileType)
        {
            if (Tok == TokenConstType)
                LeadingConst = 1;
            LexGetToken(P, NULL, TRUE);
            Tok = LexGetToken(P, NULL, FALSE);
        }
        /* stash for decl handlers below (thread-local style static) */
        IrCompileLeadingConst = LeadingConst;
    }

    if (Tok == TokenLeftBrace)
    {
        IrStmt **List;
        int Cap = 16, Count = 0;
        int scopeBase;
        int si;
        LexGetToken(P, NULL, TRUE);
        /* push scope: slots allocated inside this block are unbound on exit so
         * outer Ident "n" rebinds to the outer slot (nested shadowing). */
        if (IrCompileScopeDepth + 1 >= IR_COMPILE_SCOPE_MAX)
            return NULL;
        IrCompileScopeDepth++;
        IrCompileScopeStack[IrCompileScopeDepth] = IrCompileNumLocals;
        scopeBase = IrCompileNumLocals;
        List = (IrStmt **)calloc(Cap, sizeof(IrStmt *));
        if (List == NULL) IR_FAIL("OOM block stmts");
        while (LexGetToken(P, NULL, FALSE) != TokenRightBrace)
        {
            IrStmt *One = IrCompileStmt(P);
            if (One == NULL)
            {
                int li;
                /* This block was never attached to a parent IR node. Release
                 * completed children and the temporary vector on failure. */
                for (li = 0; li < Count; li++)
                    IrFreeStmt(List[li]);
                free(List);
                /* pop scope on failure */
                for (si = scopeBase; si < IrCompileNumLocals; si++)
                    IrCompileLocalNames[si] = NULL;
                IrCompileScopeDepth--;
                return NULL;
            }
            if (Count >= Cap)
            {
                IrStmt **Bigger = (IrStmt **)realloc(List, (size_t)Cap * 2 * sizeof(IrStmt *));
                if (Bigger == NULL)
                {
                    int li;
                    for (li = 0; li < Count; li++) IrFreeStmt(List[li]);
                    free(List);
                    IrCompileScopeDepth--;
                    return NULL;
                }
                List = Bigger;
                Cap *= 2;
            }
            List[Count++] = One;
        }
        LexGetToken(P, NULL, TRUE); /* } */
        /* pop scope: hide names introduced in this block */
        for (si = scopeBase; si < IrCompileNumLocals; si++)
            IrCompileLocalNames[si] = NULL;
        IrCompileScopeDepth--;
        S = IrAllocStmt(P->pc, IrStBlock);
        if (S == NULL)
        {
            int li;
            for (li = 0; li < Count; li++) IrFreeStmt(List[li]);
            free(List);
            return NULL;
        }
        S->Stmts = List;
        S->StmtCount = Count;
        return S;
    }

    if (Tok == TokenReturn)
    {
        LexGetToken(P, NULL, TRUE);
        S = IrAllocStmt(P->pc, IrStReturn);
        if (S == NULL) return NULL;
        S->DeclFromType = IrCompileReturnType;
        if (LexGetToken(P, NULL, FALSE) != TokenSemicolon)
        {
            S->Expr = IrCompileExpr(P);
            if (S->Expr == NULL) return NULL;
#ifndef NO_FP
            if (IrCompileReturnType != NULL && IrCompileReturnType->Base == TypeFP)
            {
                IrExpr *C = IrAllocExpr(P->pc, IrExUnary);
                if (C == NULL) return NULL;
                C->Op = TokenCast;
                C->Left = S->Expr;
                C->ConstVal = 0;
                C->I32 = TypeFP;
                C->TypeFlags = IR_TF_RESOLVED | IR_TF_FP; /* 954: visible before annotate */
                S->Expr = C;
            }
#endif
        }
        if (LexGetToken(P, NULL, TRUE) != TokenSemicolon)
            return NULL;
        return S;
    }

    /* #define inside function body (ParseMacroDefinition expects #define already consumed) */
    if (Tok == TokenHashDefine)
    {
        LexGetToken(P, NULL, TRUE); /* #define */
        ParseMacroDefinition(P);
        S = IrAllocStmt(P->pc, IrStBlock);
        if (S == NULL) return NULL;
        S->Stmts = NULL;
        S->StmtCount = 0;
        return S;
    }
    if (Tok == TokenBreak)
    {
        LexGetToken(P, NULL, TRUE);
        if (LexGetToken(P, NULL, TRUE) != TokenSemicolon) return NULL;
        return IrAllocStmt(P->pc, IrStBreak);
    }
    if (Tok == TokenContinue)
    {
        LexGetToken(P, NULL, TRUE);
        if (LexGetToken(P, NULL, TRUE) != TokenSemicolon) return NULL;
        return IrAllocStmt(P->pc, IrStContinue);
    }

    /* switch (expr) { ... } */
    if (Tok == TokenSwitch)
    {
        IrStmt *Body;
        LexGetToken(P, NULL, TRUE); /* switch */
        if (LexGetToken(P, NULL, TRUE) != TokenOpenBracket) return NULL;
        S = IrAllocStmt(P->pc, IrStSwitch);
        if (S == NULL) return NULL;
        S->Expr = IrCompileExpr(P);
        if (S->Expr == NULL) return NULL;
        if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket) return NULL;
        Body = IrCompileStmt(P); /* expects { ... } */
        if (Body == NULL) return NULL;
        S->BodyStmt = Body;
        return S;
    }

    /* case const-expr : */
    if (Tok == TokenCase)
    {
        LexGetToken(P, NULL, TRUE);
        S = IrAllocStmt(P->pc, IrStCase);
        if (S == NULL) return NULL;
        S->Expr = IrCompileExpr(P);
        if (S->Expr == NULL) return NULL;
        if (LexGetToken(P, NULL, TRUE) != TokenColon) return NULL;
        return S;
    }

    /* default : */
    if (Tok == TokenDefault)
    {
        LexGetToken(P, NULL, TRUE);
        if (LexGetToken(P, NULL, TRUE) != TokenColon) return NULL;
        return IrAllocStmt(P->pc, IrStDefault);
    }

    /* goto label ; */
    if (Tok == TokenGoto)
    {
        struct Value *LexVal;
        LexGetToken(P, NULL, TRUE);
        if (LexGetToken(P, &LexVal, TRUE) != TokenIdentifier) return NULL;
        if (LexGetToken(P, NULL, TRUE) != TokenSemicolon) return NULL;
        S = IrAllocStmt(P->pc, IrStGoto);
        if (S == NULL) return NULL;
        S->DeclIdent = LexVal->Val->Identifier;
        return S;
    }

    /* label:  (identifier followed by colon) */
    if (Tok == TokenIdentifier)
    {
        struct ParseState Look = *P;
        struct Value *LexVal;
        LexGetToken(&Look, &LexVal, TRUE); /* ident on copy */
        if (LexGetToken(&Look, NULL, FALSE) == TokenColon)
        {
            LexGetToken(P, &LexVal, TRUE); /* ident */
            LexGetToken(P, NULL, TRUE);    /* : */
            S = IrAllocStmt(P->pc, IrStLabel);
            if (S == NULL) return NULL;
            S->DeclIdent = LexVal->Val->Identifier;
            return S;
        }
    }

    /* int/float (*fp)(...); int (*tab[N])(void); float (*math[])(float,float)={...}
     * function-pointer local (scalar or array) for Body IR */
    if ((Tok == TokenIntType || Tok == TokenShortType || Tok == TokenCharType
        || Tok == TokenLongType || Tok == TokenVoidType
#ifndef NO_FP
        || Tok == TokenFloatType || Tok == TokenDoubleType
#endif
        ) && IrCompilingFunctionBody)
    {
        struct ParseState Look;
        enum LexToken T1, T2, T3, T4;
        ParserCopy(&Look, P);
        LexGetToken(&Look, NULL, TRUE); /* type */
        T1 = LexGetToken(&Look, NULL, TRUE);
        /* optional * for pointer return type: int *(*fp)(void) */
        if (T1 == TokenAsterisk)
            T1 = LexGetToken(&Look, NULL, TRUE);
        if (T1 == TokenOpenBracket)
        {
            T2 = LexGetToken(&Look, NULL, TRUE);
            if (T2 == TokenAsterisk)
            {
                struct Value *Lv = NULL;
                T3 = LexGetToken(&Look, &Lv, TRUE);
                if (T3 == TokenIdentifier)
                {
                    int FptrArrN = 0;
                    int FptrArrInfer = 0;
                    T4 = LexGetToken(&Look, NULL, TRUE);
                    /* int (*fp)(  OR  int (*tab[N])(  OR  int (*tab[])( */
                    if (T4 == TokenLeftSquareBracket)
                    {
                        struct Value *Nv = NULL;
                        if (LexGetToken(&Look, NULL, FALSE) == TokenRightSquareBracket)
                        {
                            LexGetToken(&Look, NULL, TRUE);
                            FptrArrInfer = 1;
                            FptrArrN = -1;
                            T4 = LexGetToken(&Look, NULL, TRUE); /* ) */
                        }
                        else if (LexGetToken(&Look, &Nv, TRUE) == TokenIntegerConstant && Nv != NULL)
                        {
                            FptrArrN = (int)Nv->Val->Integer;
                            if (FptrArrN > 0 && FptrArrN <= 256
                                && LexGetToken(&Look, NULL, TRUE) == TokenRightSquareBracket)
                                T4 = LexGetToken(&Look, NULL, TRUE); /* expect ) */
                            else
                                FptrArrN = 0;
                        }
                    }
                    if (T4 == TokenCloseBracket
                        && LexGetToken(&Look, NULL, TRUE) == TokenOpenBracket)
                    {
                        /* consume full decl from P */
                        char *DName;
                        int Slot, depth;
                        IrStmt *One;
                        LexGetToken(P, NULL, TRUE); /* type */
                        if (LexGetToken(P, NULL, FALSE) == TokenAsterisk)
                            LexGetToken(P, NULL, TRUE); /* optional * for ptr return */
                        LexGetToken(P, NULL, TRUE); /* ( */
                        LexGetToken(P, NULL, TRUE); /* * */
                        LexGetToken(P, &Lv, TRUE);  /* name */
                        DName = Lv->Val->Identifier;
                        if (FptrArrN != 0)
                        {
                            /* [N] or [] */
                            LexGetToken(P, NULL, TRUE); /* [ */
                            if (FptrArrInfer)
                                LexGetToken(P, NULL, TRUE); /* ] */
                            else
                            {
                                LexGetToken(P, NULL, TRUE); /* N */
                                LexGetToken(P, NULL, TRUE); /* ] */
                            }
                        }
                        LexGetToken(P, NULL, TRUE); /* ) */
                        LexGetToken(P, NULL, TRUE); /* ( */
                        depth = 1;
                        while (depth > 0)
                        {
                            enum LexToken Tk = LexGetToken(P, NULL, TRUE);
                            if (Tk == TokenOpenBracket) depth++;
                            else if (Tk == TokenCloseBracket) depth--;
                            else if (Tk == TokenEOF) return NULL;
                        }
                        Slot = IrInternLocal(DName);
                        if (Slot < 0) return NULL;
                        One = IrAllocStmt(P->pc, IrStDecl);
                        if (One == NULL) return NULL;
                        One->DeclIdent = DName;
                        One->LocalSlot = Slot;
                        One->DeclIsFptr = 1;
                        One->DeclIsPtr = (FptrArrN != 0) ? 1 : 0;
                        One->DeclElemSize = (FptrArrN != 0) ? (int)sizeof(void *) : 0;
                        One->DeclArraySize = (FptrArrN > 0) ? FptrArrN : 0;
                        One->DeclInitVals = NULL;
                        One->DeclInitCount = 0;
                        One->DeclInitExprs = NULL;
                        /* optional = init or = { f0, f1, ... } */
                        if (LexGetToken(P, NULL, FALSE) == TokenAssign)
                        {
                            LexGetToken(P, NULL, TRUE); /* = */
                            if (LexGetToken(P, NULL, FALSE) == TokenLeftBrace)
                            {
                                long long *Vals;
                                IrExpr **Exprs = NULL;
                                int ni = 0, cap = 8;
                                LexGetToken(P, NULL, TRUE); /* { */
                                Vals = (long long *)calloc((size_t)cap, sizeof(long long));
                                if (Vals == NULL) return NULL;
                                while (LexGetToken(P, NULL, FALSE) != TokenRightBrace)
                                {
                                    IrExpr *Ex = IrCompileAssignExpr(P);
                                    if (Ex == NULL) { free(Vals); free(Exprs); return NULL; }
                                    if (ni >= cap)
                                    {
                                        int ncap = cap * 2;
                                        long long *nv;
                                        IrExpr **ne;
                                        if (ncap > 256) { free(Vals); free(Exprs); return NULL; }
                                        nv = (long long *)realloc(Vals, (size_t)ncap * sizeof(long long));
                                        if (nv == NULL) { free(Vals); free(Exprs); return NULL; }
                                        Vals = nv;
                                        if (Exprs != NULL)
                                        {
                                            ne = (IrExpr **)realloc(Exprs, (size_t)ncap * sizeof(IrExpr *));
                                            if (ne == NULL) { free(Vals); free(Exprs); return NULL; }
                                            { int z; for (z = ni; z < ncap; z++) ne[z] = NULL; }
                                            Exprs = ne;
                                        }
                                        cap = ncap;
                                    }
                                    long long folded;
                                    if (IrTryFoldInitConst(Ex, &folded))
                                    {
                                        Vals[ni] = folded;
                                        IrFreeExpr(Ex);
                                    }
                                    else
                                    {
                                        if (Exprs == NULL)
                                        {
                                            Exprs = (IrExpr **)calloc((size_t)cap, sizeof(IrExpr *));
                                            if (Exprs == NULL) { free(Vals); return NULL; }
                                        }
                                        Exprs[ni] = Ex;
                                        Vals[ni] = 0;
                                    }
                                    ni++;
                                    if (LexGetToken(P, NULL, FALSE) == TokenComma)
                                        LexGetToken(P, NULL, TRUE);
                                }
                                LexGetToken(P, NULL, TRUE); /* } */
                                if (FptrArrInfer)
                                {
                                    if (ni < 1) { free(Vals); free(Exprs); return NULL; }
                                    FptrArrN = ni;
                                    One->DeclArraySize = ni;
                                    One->DeclIsPtr = 1;
                                    One->DeclElemSize = (int)sizeof(void *);
                                }
                                else if (FptrArrN > 0 && ni > FptrArrN)
                                { free(Vals); free(Exprs); return NULL; }
                                One->DeclInitVals = Vals;
                                One->DeclInitCount = ni;
                                One->DeclInitExprs = Exprs;
                            }
                            else
                            {
                                /* scalar fptr: int (*fp)(...) = sum; */
                                if (FptrArrN != 0) return NULL;
                                One->Expr = IrCompileAssignExpr(P);
                                if (One->Expr == NULL) return NULL;
                            }
                        }
                        else if (FptrArrInfer)
                            return NULL; /* [] needs brace init */
                        if (LexGetToken(P, NULL, TRUE) != TokenSemicolon)
                            return NULL;
                        /* Element stride is always a machine word for fptr slots */
                        if (Slot < IR_COMPILE_MAX_LOCALS)
                            IrCompileLocalElemSize[Slot] = (int)sizeof(void *);
                        return One;
                    }
                }
            }
        }
    }

    /* typedef-name local: int8_t x; uint64_t u; char *p via typedef-to-scalar only.
     * Struct typedefs and arrays fall back (ParserCopy + fall through). */
    if (Tok == TokenIdentifier && IrCompilingFunctionBody)
    {
        struct Value *LexVal = NULL;
        struct Value *TV = NULL;
        struct ValueType *BT = NULL;
        ParserCopy(&Save, P);
        LexGetToken(P, &LexVal, TRUE); /* typedef name */
        if (LexVal != NULL && LexVal->Val != NULL
            && VariableDefined(P->pc, LexVal->Val->Identifier))
        {
            VariableGet(P->pc, P, LexVal->Val->Identifier, &TV);
            if (TV != NULL && TV->Typ != NULL && TV->Typ->Base == Type_Type
                && TV->Val != NULL && TV->Val->Typ != NULL)
                BT = TV->Val->Typ;
        }
        if (BT != NULL && (BT->Base == TypeInt || BT->Base == TypeShort
            || BT->Base == TypeChar || BT->Base == TypeLong
            || BT->Base == TypeLongLong
            || BT->Base == TypeUnsignedInt || BT->Base == TypeUnsignedShort
            || BT->Base == TypeUnsignedChar || BT->Base == TypeUnsignedLong
            || BT->Base == TypeUnsignedLongLong
#ifndef NO_FP
            || BT->Base == TypeFP
#endif
            || BT->Base == TypePointer
            || BT->Base == TypeArray
            || ((BT->Base == TypeStruct || BT->Base == TypeUnion)
                && IrStructLayoutOk(BT, 4))))
        {
            IrStmt **List = NULL;
            int Cap = 0, Count = 0;
            int IsStructVal = (BT->Base == TypeStruct || BT->Base == TypeUnion);
            int BaseElem = (BT->Base == TypePointer)
                ? (int)sizeof(void *)
                : (BT->Sizeof > 0 ? BT->Sizeof : (int)sizeof(int));
            struct ValueType *ScalarFromType = (BT->Base == TypePointer) ? NULL : BT;
            int BaseIsPtr = (BT->Base == TypePointer);
            int TypedefArrSize = 0;
            /* typedef float T[3]; → array local with leaf elem size */
            if (BT->Base == TypeArray)
            {
                struct ValueType *ET = BT;
                long long tot = 1;
                int nd = 0;
                while (ET != NULL && ET->Base == TypeArray && nd < IR_MAX_ARRAY_DIMS)
                {
                    int d = ET->ArraySize > 0 ? ET->ArraySize : 0;
                    if (d <= 0) break;
                    tot *= d;
                    nd++;
                    ET = ET->FromType;
                }
                if (ET != NULL && tot > 0 && tot <= 65536)
                {
                    BaseElem = ET->Sizeof > 0 ? ET->Sizeof : (int)sizeof(int);
                    TypedefArrSize = (int)tot;
                    ScalarFromType = ET;
                }
            }

            for (;;)
            {
                struct Value *IdV = NULL;
                char *DName;
                int Slot, ArrSize = TypedefArrSize, IsPtr = BaseIsPtr, TdStars = 0;
                IrStmt *One;

                /* optional * / ** / *** (typedef already pointer, or pointer-to-T) */
                {
                    int stars = 0;
                    while (LexGetToken(P, NULL, FALSE) == TokenAsterisk)
                    {
                        LexGetToken(P, NULL, TRUE);
                        stars++;
                    }
                    if (stars > 0)
                        IsPtr = 1;
                    TdStars = stars;
                    /* multi-level pointers still DeclIsPtr with elem=void*;
                     * LocalFromType keeps scalar BT for rare punning. */
                    (void)stars;
                }
                if (LexGetToken(P, &IdV, TRUE) != TokenIdentifier)
                {
                    ParserCopy(P, &Save);
                    List = NULL;
                    break;
                }
                DName = IdV->Val->Identifier;
                /* optional [N][M]... fixed array of typedef scalar/struct/pointers */
                int TdNDims = 0;
                int TdDims[IR_MAX_ARRAY_DIMS];
                {
                    int tdi;
                    for (tdi = 0; tdi < IR_MAX_ARRAY_DIMS; tdi++) TdDims[tdi] = 0;
                }
                {
                    int dim_fail = 0;
                    while (LexGetToken(P, NULL, FALSE) == TokenLeftSquareBracket)
                    {
                        int dim;
                        if (TdNDims >= IR_MAX_ARRAY_DIMS)
                        {
                            dim_fail = 1;
                            break;
                        }
                        LexGetToken(P, NULL, TRUE); /* [ */
                        if (!IrCompilePeekIntConst(P, &dim)
                            || dim <= 0 || dim > 16 * 1024 * 1024
                            || LexGetToken(P, NULL, TRUE) != TokenRightSquareBracket)
                        {
                            dim_fail = 1;
                            break;
                        }
                        TdDims[TdNDims++] = dim;
                    }
                    if (dim_fail)
                    {
                        ParserCopy(P, &Save);
                        List = NULL;
                        break;
                    }
                    if (TdNDims > 0)
                    {
                        long long tot = 1;
                        int tdi;
                        for (tdi = 0; tdi < TdNDims; tdi++)
                        {
                            tot *= TdDims[tdi];
                            if (tot > 16 * 1024 * 1024)
                            {
                                dim_fail = 1;
                                break;
                            }
                        }
                        if (dim_fail)
                        {
                            ParserCopy(P, &Save);
                            List = NULL;
                            break;
                        }
                        ArrSize = (int)tot;
                    }
                }
                Slot = IrInternLocal(DName);
                if (Slot < 0)
                {
                    ParserCopy(P, &Save);
                    List = NULL;
                    break;
                }
                One = IrAllocStmt(P->pc, IrStDecl);
                if (One == NULL) IR_FAIL("OOM IrStDecl typedef local");
                One->DeclIdent = DName;
                One->LocalSlot = Slot;
                One->DeclIsConst = IrCompileLeadingConst && !IsPtr;
                if (IsStructVal && !IsPtr && ArrSize == 0)
                {
                    /* single struct/union value local (same shape as struct Tag x;) */
                    One->DeclIsPtr = 0;
                    One->DeclElemSize = BaseElem;
                    One->DeclArraySize = 1;
                    One->DeclFromType = BT;
                }
                else if (IsStructVal && !IsPtr && ArrSize > 0)
                {
                    /* point pts[N]; — array of struct values */
                    One->DeclIsPtr = 0;
                    One->DeclElemSize = BaseElem;
                    One->DeclArraySize = ArrSize;
                    One->DeclFromType = BT;
                    One->DeclNDims = 1;
                    One->DeclDims[0] = ArrSize;
                    if (Slot < IR_COMPILE_MAX_LOCALS)
                    {
                        IrCompileLocalElemSize[Slot] = BaseElem * ArrSize;
                        IrCompileLocalArraySize[Slot] = ArrSize;
                        if (BaseElem > 0)
                            IrCompileLocalScale[Slot] = BaseElem;
                    }
                }
                else if (IsStructVal && IsPtr)
                {
                    /* typedef struct S; S *p; — pointee layout for p->m
                     * (also S a, *p, *q multi-decl second/third). */
                    One->DeclIsPtr = 1;
                    One->DeclElemSize = (int)sizeof(void *);
                    One->DeclArraySize = 0;
                    One->DeclFromType = BT;
                    One->DeclPointeeSize = BaseElem > 0 ? BaseElem
                        : (BT != NULL && BT->Sizeof > 0 ? BT->Sizeof : (int)sizeof(int));
                    if (Slot < IR_COMPILE_MAX_LOCALS)
                    {
                        IrCompileLocalElemSize[Slot] = (int)sizeof(void *);
                        IrCompileLocalScale[Slot] = One->DeclPointeeSize > 0
                            ? One->DeclPointeeSize : (int)sizeof(void *);
                    }
                }
                else if (ArrSize > 0)
                {
                    /* int32_t a[N]; int32_t *a[N][M]; OR fn_t tab[N] */
                    One->DeclIsPtr = 1;
                    One->DeclArraySize = ArrSize;
                    if (TdNDims > 0)
                    {
                        int tdi;
                        One->DeclNDims = TdNDims;
                        for (tdi = 0; tdi < TdNDims && tdi < IR_MAX_ARRAY_DIMS; tdi++)
                            One->DeclDims[tdi] = TdDims[tdi];
                    }
                    else
                    {
                        One->DeclNDims = 1;
                        One->DeclDims[0] = ArrSize;
                    }
                    if (IsPtr || (BT != NULL && BT->Base == TypePointer))
                    {
                        /* T *a[N] or ptr_typedef a[N]: elements are pointer words */
                        One->DeclElemIsPtr = 1;
                        One->DeclElemSize = (int)sizeof(void *);
                        One->DeclFromType = (BT != NULL && BT->Base != TypePointer) ? BT : NULL;
                        /* int32_t *a[N]: typed pointee for a[i][j] */
                        if (TdStars == 1 && BT != NULL && BT->Base != TypePointer
                            && (IS_INTEGER_NUMERIC_TYPE(BT) || BT->Base == TypeFP))
                            IrCompileSetElemPointee(One, Slot, BT);
                        /* B.2: pointee of element pointers */
                        One->DeclPointeeSize = (BT != NULL && BT->Base != TypePointer && BT->Sizeof > 0)
                            ? BT->Sizeof
                            : (BT != NULL && BT->Base == TypePointer && BT->FromType != NULL && BT->FromType->Sizeof > 0
                               ? BT->FromType->Sizeof : (int)sizeof(int));
                        if (BT != NULL && BT->Base == TypePointer
                            && BT->FromType != NULL
                            && (BT->FromType->Base == TypeFunction
                                || BT->FromType->Base == TypePointer))
                            One->DeclIsFptr = 1;
                    }
                    else
                    {
                        One->DeclElemSize = BaseElem;
                        One->DeclFromType = ScalarFromType;
                    }
                    if (Slot < IR_COMPILE_MAX_LOCALS)
                    {
                        IrCompileLocalElemSize[Slot] = One->DeclElemSize * ArrSize;
                        IrCompileLocalArraySize[Slot] = ArrSize;
                        if (One->DeclElemSize > 0)
                            IrCompileLocalScale[Slot] = One->DeclElemSize;
                    }
                }
                else
                {
                    One->DeclIsPtr = IsPtr;
                    /* Scalar typedef (int8_t x): elem size is the typedef'd
                     * type width, not 0. sizeof(x) reads IrCompileLocalElemSize. */
                    One->DeclElemSize = IsPtr ? BaseElem : (BaseElem > 0 ? BaseElem : 0);
                    if (!IsPtr && BaseElem > 0 && Slot < IR_COMPILE_MAX_LOCALS)
                        IrCompileLocalElemSize[Slot] = BaseElem;
                    if (IsPtr && BT != NULL && BT->Base == TypePointer
                        && BT->FromType != NULL
                        && BT->FromType->Base == TypeFunction)
                        One->DeclIsFptr = 1;
                    if (!IsPtr && ScalarFromType != NULL)
                        One->DeclFromType = ScalarFromType;
                    /* T *p / ptr_typedef p / ptr_typedef *p: full type for
                     * front-end dereference typing. Function pointers and
                     * binary32 pointees keep their existing paths. */
                    if (IsPtr && !One->DeclIsFptr && BT != NULL
                        && !(BT->Base == TypePointer && BT->FromType != NULL
                             && BT->FromType->Base == TypeFunction)
                        && !(BT->Base == TypeFP
                             && TypeFPStorageSize(BT) != (int)sizeof(double)))
                    {
                        IrCompileStampLocalPointerType(P, Slot, BT, TdStars);
                        /* Lowering convention (as the integer path): DeclFromType
                         * is the pointee of the slot (first-level pointee for
                         * T **p), so Linear can type *p. */
                        if (Slot >= 0 && Slot < IR_COMPILE_MAX_LOCALS
                            && IrCompileLocalFromType[Slot] != NULL
                            && IrCompileLocalFromType[Slot]->Base == TypePointer
                            && IrCompileLocalFromType[Slot]->FromType != NULL)
                        {
                            struct ValueType *PT = IrCompileLocalFromType[Slot]->FromType;
                            int ps = (PT->Base == TypePointer) ? (int)sizeof(void *)
                                : (PT->Base == TypeFP) ? TypeFPStorageSize(PT)
                                : (int)PT->Sizeof;
                            One->DeclFromType = PT;
                            /* Stride/pointee of the slot is sizeof(*p): for a
                             * pointer typedef (IP q) BaseElem is the pointer
                             * width, not the pointee size. */
                            if (ps > 0)
                            {
                                One->DeclElemSize = ps;
                                One->DeclPointeeSize = ps;
                            }
                        }
                    }
                    One->DeclArraySize = 0;
                }

                if (LexGetToken(P, NULL, FALSE) == TokenAssign)
                {
                    /* struct/union value brace-init on typedef-name: parse as
                     * sequential/designated constants (same DeclInitVals shape
                     * as struct Tag x = {...};). */
                    if (IsStructVal && !IsPtr && ArrSize == 0
                        && LexGetToken(P, NULL, FALSE) == TokenLeftBrace)
                    {
                        long long *Vals = NULL;
                        IrExpr **Exprs = NULL;
                        char **Names = NULL;
                        int ni = 0, cap = 0, depth = 0, anyExpr = 0;
                        LexGetToken(P, NULL, TRUE); /* = */
                        LexGetToken(P, NULL, TRUE); /* { */
                        depth = 0;
                        do {
                            enum LexToken Ct;
                            struct Value *Lv = NULL;
                            int neg = 0;
                            char *desig = NULL;
                            long long cval = 0;
                            IrExpr *ex = NULL;
                            Ct = LexGetToken(P, NULL, FALSE);
                            if (Ct == TokenLeftBrace)
                            { LexGetToken(P, NULL, TRUE); depth++; continue; }
                            if (Ct == TokenRightBrace)
                            {
                                LexGetToken(P, NULL, TRUE);
                                if (depth > 0) depth--;
                                if (depth == 0) break;
                                if (LexGetToken(P, NULL, FALSE) == TokenComma)
                                    LexGetToken(P, NULL, TRUE);
                                continue;
                            }
                            if (Ct == TokenDot)
                            {
                                LexGetToken(P, NULL, TRUE);
                                if (LexGetToken(P, &Lv, TRUE) != TokenIdentifier || Lv == NULL)
                                { free(Vals); free(Exprs); free(Names); return NULL; }
                                desig = strdup(Lv->Val->Identifier);
                                if (desig == NULL)
                                { free(Vals); free(Exprs); free(Names); return NULL; }
                                if (LexGetToken(P, NULL, TRUE) != TokenAssign)
                                { free(desig); free(Vals); free(Exprs); free(Names); return NULL; }
                                Ct = LexGetToken(P, NULL, FALSE);
                            }
                            if (Ct == TokenMinus)
                            { LexGetToken(P, NULL, TRUE); neg = 1; Ct = LexGetToken(P, NULL, FALSE); }
                            if (Ct == TokenIntegerConstant)
                            {
                                LexGetToken(P, &Lv, TRUE);
                                if (Lv == NULL) { free(desig); free(Vals); free(Exprs); free(Names); return NULL; }
                                cval = (long long)Lv->Val->Integer;
                                if (neg) cval = -cval;
                            }
#ifndef NO_FP
                            else if (Ct == TokenFPConstant)
                            {
                                double d;
                                LexGetToken(P, &Lv, TRUE);
                                if (Lv == NULL) { free(desig); free(Vals); free(Exprs); free(Names); return NULL; }
                                d = Lv->Val->FP; if (neg) d = -d;
                                { union { double d; long long l; } u; u.d = d; cval = u.l; }
                            }
#endif
                            else
                            {
                                if (neg) { free(desig); free(Vals); free(Exprs); free(Names); return NULL; }
                                ex = IrCompileAssignExpr(P);
                                if (ex == NULL) { free(desig); free(Vals); free(Exprs); free(Names); return NULL; }
                                anyExpr = 1;
                            }
                            if (ni >= cap)
                            {
                                int nc = cap ? cap * 2 : 8;
                                long long *nv = (long long *)realloc(Vals, (size_t)nc * sizeof(long long));
                                IrExpr **ne = (IrExpr **)realloc(Exprs, (size_t)nc * sizeof(IrExpr *));
                                char **nn = (char **)realloc(Names, (size_t)nc * sizeof(char *));
                                if (!nv || !ne || !nn) { free(desig); free(Vals); free(Exprs); free(Names); return NULL; }
                                Vals = nv; Exprs = ne; Names = nn; cap = nc;
                                for (int zi = ni; zi < cap; zi++) { Exprs[zi] = NULL; Names[zi] = NULL; }
                            }
                            Vals[ni] = cval;
                            Exprs[ni] = ex;
                            Names[ni] = desig;
                            ni++;
                            if (LexGetToken(P, NULL, FALSE) == TokenComma)
                                LexGetToken(P, NULL, TRUE);
                        } while (1);
                        One->DeclInitVals = Vals;
                        One->DeclInitExprs = anyExpr ? Exprs : NULL;
                        if (!anyExpr && Exprs) free(Exprs);
                        One->DeclInitNames = Names;
                        One->DeclInitCount = ni;
                        One->Expr = NULL;
                    }
                    else
                    LexGetToken(P, NULL, TRUE);
                    /* T *a[N] = {0}; or T a[N] = {0}; — zero-init via empty Expr */
                    if (!(IsStructVal && !IsPtr && ArrSize == 0 && One->DeclInitCount > 0)
                        && ArrSize > 0
                        && LexGetToken(P, NULL, FALSE) == TokenLeftBrace)
                    {
                        int depth = 0;
                        int ok = 1;
                        do {
                            enum LexToken Bt = LexGetToken(P, NULL, TRUE);
                            if (Bt == TokenLeftBrace) depth++;
                            else if (Bt == TokenRightBrace) depth--;
                            else if (Bt == TokenEOF) { ok = 0; break; }
                        } while (depth > 0);
                        if (!ok) {
                            ParserCopy(P, &Save);
                            List = NULL;
                            break;
                        }
                        /* mark as full zero-init (same as int a[N] = {0}) */
                        {
                            long long *Z = (long long *)calloc((size_t)ArrSize, sizeof(long long));
                            if (Z == NULL) IR_FAIL("OOM zero-init");
                            One->DeclInitVals = Z;
                            One->DeclInitCount = ArrSize;
                        }
                        One->Expr = NULL;
                    }
                    else
                    {
                        One->Expr = IrCompileAssignExpr(P);
                        if (One->Expr == NULL)
                        {
                            ParserCopy(P, &Save);
                            List = NULL;
                            break;
                        }
                    }
                }
                if (List == NULL)
                {
                    Cap = 4;
                    List = (IrStmt **)calloc(Cap, sizeof(IrStmt *));
                    if (List == NULL) IR_FAIL("OOM typedef decl list");
                }
                if (Count >= Cap)
                {
                    IrStmt **Bigger = (IrStmt **)realloc(List, (size_t)Cap * 2 * sizeof(IrStmt *));
                    if (Bigger == NULL) IR_FAIL("OOM typedef decl grow");
                    List = Bigger;
                    Cap *= 2;
                }
                List[Count++] = One;
                if (LexGetToken(P, NULL, FALSE) == TokenComma)
                {
                    LexGetToken(P, NULL, TRUE);
                    continue;
                }
                if (LexGetToken(P, NULL, TRUE) != TokenSemicolon)
                {
                    ParserCopy(P, &Save);
                    List = NULL;
                    break;
                }
                break;
            }
            if (List != NULL && Count > 0)
            {
                if (Count == 1)
                {
                    /* The single statement does not take ownership of the
                     * temporary declaration vector. Keep the statement alive,
                     * but release the vector before returning it. */
                    IrStmt *Only = List[0];
                    free(List);
                    return Only;
                }
                {
                    IrStmt *Blk = IrAllocStmt(P->pc, IrStBlock);
                    if (Blk == NULL) IR_FAIL("OOM typedef decl block");
                    Blk->Stmts = List;
                    Blk->StmtCount = Count;
                    return Blk;
                }
            }
        }
        ParserCopy(P, &Save);
        /* fall through — not a scalar/pointer typedef local we handle */
    }

    /* Integer, pointer, and small fixed-array locals in Body IR.
     * Optional leading `static` → DeclIsStatic (persist across calls). */
    {
    int LeadingStatic = 0;
    if (Tok == TokenStaticType && IrCompilingFunctionBody)
    {
        struct ParseState Look;
        enum LexToken T2;
        ParserCopy(&Look, P);
        LexGetToken(&Look, NULL, TRUE); /* static */
        T2 = LexGetToken(&Look, NULL, FALSE);
        while (T2 == TokenConstType || T2 == TokenVolatileType)
        {
            LexGetToken(&Look, NULL, TRUE);
            T2 = LexGetToken(&Look, NULL, FALSE);
        }
        if (T2 == TokenIntType || T2 == TokenShortType || T2 == TokenCharType
            || T2 == TokenLongType || T2 == TokenUnsignedType
            || T2 == TokenStructType || T2 == TokenUnionType
            || T2 == TokenIdentifier) /* typedef-name or struct Tag */
        {
            LexGetToken(P, NULL, TRUE); /* consume static */
            Tok = LexGetToken(P, NULL, FALSE);
            LeadingStatic = 1;
            while (Tok == TokenConstType || Tok == TokenVolatileType)
            {
                if (Tok == TokenConstType)
                    IrCompileLeadingConst = 1;
                LexGetToken(P, NULL, TRUE);
                Tok = LexGetToken(P, NULL, FALSE);
            }
        }
    }
    /* long double → treat as double (BabyC FP is host double) */
#ifndef NO_FP
    if (Tok == TokenLongType && IrCompilingFunctionBody)
    {
        struct ParseState LookLD;
        enum LexToken Tld;
        ParserCopy(&LookLD, P);
        LexGetToken(&LookLD, NULL, TRUE); /* long */
        Tld = LexGetToken(&LookLD, NULL, FALSE);
        if (Tld == TokenDoubleType)
        {
            LexGetToken(P, NULL, TRUE); /* consume long */
            Tok = TokenDoubleType; /* fall into float/double decl below */
        }
    }
#endif
    if ((Tok == TokenIntType || Tok == TokenShortType || Tok == TokenCharType
        || Tok == TokenLongType || Tok == TokenUnsignedType
        || Tok == TokenSignedType) && IrCompilingFunctionBody)
    {
        IrStmt **List = NULL;
        int Cap = 0, Count = 0;
        int BaseElem = 4; /* sizeof int default */
        enum LexToken BaseTok = Tok;
        int IsUnsigned = 0;
        int IsLongLong = 0;
        struct ValueType *ScalarFromType = NULL;
        LexGetToken(P, NULL, TRUE); /* type */
        if (BaseTok == TokenSignedType)
        {
            /* signed [char|short|int|long [long]] — all as signed ints */
            if (LexGetToken(P, NULL, FALSE) == TokenCharType)
            { LexGetToken(P, NULL, TRUE); BaseElem = 1; ScalarFromType = &P->pc->CharType; }
            else if (LexGetToken(P, NULL, FALSE) == TokenShortType)
            { LexGetToken(P, NULL, TRUE); BaseElem = 2; ScalarFromType = &P->pc->ShortType; }
            else if (LexGetToken(P, NULL, FALSE) == TokenLongType)
            {
                LexGetToken(P, NULL, TRUE);
                if (LexGetToken(P, NULL, FALSE) == TokenLongType)
                { LexGetToken(P, NULL, TRUE); IsLongLong = 1; BaseElem = (int)sizeof(long long);
                  ScalarFromType = &P->pc->LongLongType; }
                else { BaseElem = (int)sizeof(long); ScalarFromType = &P->pc->LongType; }
            }
            else if (LexGetToken(P, NULL, FALSE) == TokenIntType)
            { LexGetToken(P, NULL, TRUE); BaseElem = (int)sizeof(int); ScalarFromType = &P->pc->IntType; }
            else
            { BaseElem = (int)sizeof(int); ScalarFromType = &P->pc->IntType; }
        }
        else if (BaseTok == TokenUnsignedType)
        {
            IsUnsigned = 1;
            /* unsigned [long [long]] | unsigned int | unsigned short | unsigned char */
            if (LexGetToken(P, NULL, FALSE) == TokenLongType)
            {
                LexGetToken(P, NULL, TRUE); /* long */
                if (LexGetToken(P, NULL, FALSE) == TokenLongType)
                {
                    LexGetToken(P, NULL, TRUE); /* long */
                    IsLongLong = 1;
                    BaseElem = (int)sizeof(long long);
                    ScalarFromType = &P->pc->UnsignedLongLongType;
                }
                else
                {
                    BaseElem = (int)sizeof(long);
                    ScalarFromType = &P->pc->UnsignedLongType;
                }
            }
            else if (LexGetToken(P, NULL, FALSE) == TokenIntType)
            {
                LexGetToken(P, NULL, TRUE);
                BaseElem = (int)sizeof(int);
                ScalarFromType = &P->pc->UnsignedIntType;
            }
            else if (LexGetToken(P, NULL, FALSE) == TokenShortType)
            {
                LexGetToken(P, NULL, TRUE);
                BaseElem = 2;
                ScalarFromType = &P->pc->UnsignedShortType;
            }
            else if (LexGetToken(P, NULL, FALSE) == TokenCharType)
            {
                LexGetToken(P, NULL, TRUE);
                BaseElem = 1;
                ScalarFromType = &P->pc->UnsignedCharType;
            }
            else
            {
                /* bare "unsigned" == unsigned int */
                BaseElem = (int)sizeof(int);
                ScalarFromType = &P->pc->UnsignedIntType;
            }
        }
        else if (BaseTok == TokenLongType && LexGetToken(P, NULL, FALSE) == TokenLongType)
        {
            LexGetToken(P, NULL, TRUE); /* second long */
            IsLongLong = 1;
            BaseElem = (int)sizeof(long long);
            ScalarFromType = &P->pc->LongLongType;
        }
        else if (BaseTok == TokenCharType)
        { BaseElem = 1; ScalarFromType = &P->pc->CharType; }
        else if (BaseTok == TokenShortType)
        { BaseElem = 2; ScalarFromType = &P->pc->ShortType; }
        else if (BaseTok == TokenLongType) { BaseElem = (int)sizeof(long); ScalarFromType = &P->pc->LongType; }
        else { BaseElem = (int)sizeof(int); ScalarFromType = &P->pc->IntType; }

        for (;;)
        {
            struct Value *LexVal;
            char *DName;
            int Slot;
            IrStmt *One;
            int IsPtr = 0;
            int StarPtr = 0;
            int ArrSize = 0;
            int NDims = 0;
            int Dims[IR_MAX_ARRAY_DIMS];
            int VLADimLine[IR_MAX_ARRAY_DIMS], VLADimChar[IR_MAX_ARRAY_DIMS];
            IrExpr *VLADimExprs[IR_MAX_ARRAY_DIMS];
            int di;
            IrExpr *VLASizeExpr = NULL;
            int VLASourceLine = 0, VLASourceChar = 0;

            for (di = 0; di < IR_MAX_ARRAY_DIMS; di++)
            {
                Dims[di] = 0;
                VLADimLine[di] = 0;
                VLADimChar[di] = 0;
                VLADimExprs[di] = NULL;
            }

            /* optional * / ** / *** for pointer (multi-level → still pointer bits) */
            int ConstPtr = 0;
            int StarCount = 0;
            while (LexGetToken(P, NULL, FALSE) == TokenAsterisk)
            {
                LexGetToken(P, NULL, TRUE);
                IsPtr = 1;
                StarPtr = 1;
                StarCount++;
                /* int * const p; / int * volatile p; */
                while (LexGetToken(P, NULL, FALSE) == TokenConstType
                    || LexGetToken(P, NULL, FALSE) == TokenVolatileType)
                {
                    if (LexGetToken(P, NULL, FALSE) == TokenConstType)
                        ConstPtr = 1;
                    LexGetToken(P, NULL, TRUE);
                }
            }
            if (LexGetToken(P, &LexVal, TRUE) != TokenIdentifier)
                return NULL;
            DName = LexVal->Val->Identifier;

            /* optional [N][M][P]... fixed array, or [] sized by brace-init later */
            while (LexGetToken(P, NULL, FALSE) == TokenLeftSquareBracket)
            {
                struct Value *Lv = NULL;
                int dim;
                if (VLASourceLine == 0)
                {
                    VLASourceLine = P->Line;
                    VLASourceChar = P->CharacterPos;
                }
                if (NDims >= IR_MAX_ARRAY_DIMS)
                    return NULL;
                LexGetToken(P, NULL, TRUE); /* [ */
                if (LexGetToken(P, NULL, FALSE) == TokenRightSquareBracket)
                {
                    /* only allow [] on first dim for brace-size inference;
                     * continue for trailing fixed dims: char a[][100] */
                    if (NDims != 0)
                        return NULL;
                    LexGetToken(P, NULL, TRUE);
                    ArrSize = -1;
                    Dims[0] = -1;
                    NDims = 1;
                    continue;
                }
                if (IrCompilePeekIntConst(P, &dim))
                {
                    if (dim <= 0 || dim > 16 * 1024 * 1024)
                        return NULL;
                    if (LexGetToken(P, NULL, TRUE) != TokenRightSquareBracket)
                        return NULL;
                    Dims[NDims++] = dim;
                }
                else
                {
                    /* Every dimension may be variably modified. Preserve each
                     * expression independently so it is evaluated exactly once
                     * and the runtime frame retains the full row-major shape. */
                    IrExpr *DimExpr;
                    if (ArrSize == -1 || VLADimExprs[NDims] != NULL)
                        return NULL;
                    VLADimLine[NDims] = P->Line;
                    VLADimChar[NDims] = P->CharacterPos;
                    if (VLASizeExpr == NULL)
                    {
                        VLASourceLine = P->Line;
                        VLASourceChar = P->CharacterPos;
                    }
                    DimExpr = IrCompileExpr(P);
                    if (DimExpr == NULL)
                        return NULL;
                    if (LexGetToken(P, NULL, TRUE) != TokenRightSquareBracket)
                        return NULL;
                    Dims[NDims] = -2;
                    VLADimExprs[NDims] = DimExpr;
                    if (VLASizeExpr == NULL)
                        VLASizeExpr = DimExpr;
                    NDims++;
                    ArrSize = -2;
                }
            }
            if (NDims > 0 && ArrSize != -1 && ArrSize != -2)
            {
                long long tot = 1;
                for (di = 0; di < NDims; di++)
                {
                    if (Dims[di] <= 0)
                        return NULL;
                    tot *= Dims[di];
                    if (tot > 16 * 1024 * 1024)
                        return NULL;
                }
                ArrSize = (int)tot;
                IsPtr = 1;
            }
            else if (ArrSize == -1 || ArrSize == -2)
                IsPtr = 1;

            /* T* scalar, T[N]... value array, or T *a[N] array-of-pointers. */
            Slot = IrInternLocal(DName);
            if (Slot < 0)
                return NULL;
            One = IrAllocStmt(P->pc, IrStDecl);
            if (One == NULL) return NULL;
            One->DeclIdent = DName;
            One->LocalSlot = Slot;
            One->DeclSlot = NULL;
            One->DeclIsStatic = LeadingStatic;
            /* const T x → const object; const T *p → not const ptr; T * const p → ConstPtr */
            One->DeclIsConst = ConstPtr || (IrCompileLeadingConst && !IsPtr);
            One->DeclNDims = 0;
            for (di = 0; di < IR_MAX_ARRAY_DIMS; di++)
                One->DeclDims[di] = 0;
            if (StarPtr && ArrSize > 0)
            {
                /* int *a[N] / int ***a[N][M]: array of pointer words (flat count) */
                One->DeclIsPtr = 1;
                One->DeclElemIsPtr = 1;
                One->DeclElemSize = (int)sizeof(void *);
                One->DeclPointeeSize = BaseElem > 0 ? BaseElem : (int)sizeof(int);
                One->DeclArraySize = ArrSize;
                One->DeclFromType = NULL;
                /* Typed pointee of each element (T, or T* layers for
                 * T **a[N]) so a[i][j] lowers by declared type. */
                if (ScalarFromType != NULL && P != NULL && NDims <= 1)
                {
                    struct ValueType *PT = ScalarFromType;
                    int sc;
                    for (sc = 0; PT != NULL && sc < StarCount - 1; sc++)
                        PT = TypeGetMatching(P->pc, P, PT, TypePointer, 0,
                                             P->pc->StrEmpty, TRUE);
                    IrCompileSetElemPointee(One, Slot, PT);
                }
                One->DeclNDims = (NDims > 0) ? NDims : 1;
                for (di = 0; di < NDims && di < IR_MAX_ARRAY_DIMS; di++)
                    One->DeclDims[di] = Dims[di];
                if (Slot < IR_COMPILE_MAX_LOCALS)
                    IrCompileLocalElemSize[Slot] = (int)sizeof(void *) * ArrSize;
            }
            else
            {
                One->DeclIsPtr = IsPtr;
                /* Multi-level T **p: first-level pointee is a pointer word.
                 * Single-level T *p: pointee is T (BaseElem). */
                if (IsPtr && ArrSize == 0 && StarCount >= 2)
                {
                    One->DeclElemSize = (int)sizeof(void *);
                    One->DeclPointeeSize = (int)sizeof(void *);
                }
                else if (IsPtr && ArrSize == 0)
                {
                    One->DeclElemSize = BaseElem > 0 ? BaseElem : (int)sizeof(void *);
                    One->DeclPointeeSize = BaseElem > 0 ? BaseElem : (int)sizeof(int);
                }
                else
                    One->DeclElemSize = (ArrSize > 0 || ArrSize == -2) ? BaseElem : 0;
                One->DeclArraySize = ArrSize;
                /* Value arrays need DeclFromType for unsigned element loads.
                 * StarPtr marks T *a[N]; value arrays keep ScalarFromType even
                 * though IsPtr is forced for owned-blob storage.
                 *
                 * Multi-level T **p / T ***p: DeclFromType is the *first-level
                 * pointee* (T* / T**), not the scalar base. Annotation walks
                 * pointer layers from DeclFromType to decide whether *p yields
                 * another pointer (es=sizeof(void*)) or a scalar. Storing only
                 * ScalarFromType made *pp look like int (es=4) and truncated
                 * intermediate pointer bits → SEGV on **pp = v under EXEC=vm. */
                if (ArrSize > 0 && StarPtr)
                    One->DeclFromType = NULL;
                else if (IsPtr && ArrSize == 0 && StarCount >= 2
                         && ScalarFromType != NULL && P != NULL)
                {
                    struct ValueType *PT = ScalarFromType;
                    int sc;
                    /* Build (StarCount-1) pointer wrappers around the scalar
                     * base so *pp sees pointee TypePointer. */
                    for (sc = 0; sc < StarCount - 1; sc++)
                    {
                        PT = TypeGetMatching(P->pc, P, PT, TypePointer, 0,
                                             P->pc->StrEmpty, TRUE);
                        if (PT == NULL)
                            break;
                    }
                    One->DeclFromType = (PT != NULL) ? PT : ScalarFromType;
                }
                else
                    One->DeclFromType = ScalarFromType;
                /* Full variable type on LocalFromType (StarCount TypePointer
                 * wrappers). Independent of DeclFromType which may hold only
                 * the first-level pointee for Member resolution. Must be set
                 * HERE so later expressions in the same body see it before
                 * IrAnnotateStmt runs after the whole body is compiled. */
                if (IsPtr && ArrSize == 0 && Slot >= 0 && Slot < IR_COMPILE_MAX_LOCALS
                    && ScalarFromType != NULL && P != NULL && StarCount >= 1)
                {
                    struct ValueType *Full = ScalarFromType;
                    int sc;
                    for (sc = 0; sc < StarCount; sc++)
                    {
                        Full = TypeGetMatching(P->pc, P, Full, TypePointer, 0,
                                               P->pc->StrEmpty, TRUE);
                        if (Full == NULL)
                            break;
                    }
                    if (Full != NULL)
                        IrCompileLocalFromType[Slot] = Full;
                    else if (One->DeclFromType != NULL)
                        IrCompileLocalFromType[Slot] = One->DeclFromType;
                    IrCompileLocalKind[Slot] = 2;
                }
                else if (One->DeclFromType != NULL && Slot >= 0
                         && Slot < IR_COMPILE_MAX_LOCALS)
                {
                    if (IrCompileLocalFromType[Slot] == NULL)
                        IrCompileLocalFromType[Slot] = One->DeclFromType;
                }
                if (ArrSize == -2)
                {
                    /* Keep the complete VLA shape and each bound expression.
                     * Expr remains the first bound for legacy consumers; new
                     * lowering/evaluation uses DeclDimExprs by dimension. */
                    if (VLASourceLine > 0) One->SourceLine = VLASourceLine;
                    if (VLASourceChar > 0) One->CharacterPos = VLASourceChar;
                    One->Expr = VLASizeExpr;
                    One->DeclElemSize = BaseElem > 0 ? BaseElem : (int)sizeof(int);
                    One->DeclNDims = NDims;
                    for (di = 0; di < NDims; di++)
                    {
                        One->DeclDims[di] = Dims[di];
                        One->DeclDimExprs[di] = VLADimExprs[di];
                        One->DeclDimSourceLine[di] = VLADimLine[di];
                        One->DeclDimSourceChar[di] = VLADimChar[di];
                    }
                }
                else if (NDims >= 1 && ArrSize > 0)
                {
                    One->DeclNDims = NDims;
                    for (di = 0; di < NDims; di++)
                        One->DeclDims[di] = Dims[di];
                }
                else if (NDims == 1 && ArrSize == -1)
                {
                    One->DeclNDims = 1;
                    One->DeclDims[0] = -1;
                }
                if (Slot >= 0 && Slot < IR_COMPILE_MAX_LOCALS)
                {
                    if (IsPtr && ArrSize == 0)
                    {
                        /* sizeof(p) = word; p+n scale via LocalScale/DeclPointeeSize */
                        int ps = One->DeclPointeeSize > 0 ? One->DeclPointeeSize
                            : (BaseElem > 0 ? BaseElem : (int)sizeof(int));
                        IrCompileLocalElemSize[Slot] = (int)sizeof(void *);
                        IrCompileLocalScale[Slot] = ps;
                    }
                    else if (ArrSize > 0)
                    {
                        IrCompileLocalElemSize[Slot] = BaseElem * ArrSize;
                        IrCompileLocalArraySize[Slot] = ArrSize;
                        if (BaseElem > 0)
                            IrCompileLocalScale[Slot] = BaseElem;
                    }
                    else if (ArrSize == -2)
                    {
                        IrCompileLocalElemSize[Slot] = BaseElem > 0 ? BaseElem : (int)sizeof(int);
                        IrCompileLocalArraySize[Slot] = -2; /* runtime-sized array */
                    }
                    else
                        IrCompileLocalElemSize[Slot] = BaseElem > 0 ? BaseElem : (int)sizeof(int);
                }
            }
            /* large locals: keep on IR up to 16K elements (e.g. char buf[8192]
             * in db_backup_old). Stack-overflow diagnostics still apply at runtime. */
            if (ArrSize > 16 * 1024 * 1024)
                return NULL;
            One->DeclInitVals = NULL;
            One->DeclInitCount = 0;
            One->DeclInitExprs = NULL;
            One->DeclInitNames = NULL;
            if (LexGetToken(P, NULL, FALSE) == TokenAssign)
            {
                LexGetToken(P, NULL, TRUE); /* = */
                /* char a[] = "str"; / char a[N] = "str"; */
                if ((ArrSize > 0 || ArrSize == -1) && BaseElem == 1
                    && LexGetToken(P, NULL, FALSE) == TokenStringConstant)
                {
                    struct Value *Lv = NULL;
                    const char *str;
                    int len, i;
                    long long *Vals;
                    LexGetToken(P, &Lv, TRUE);
                    if (Lv == NULL || Lv->Val == NULL || Lv->Val->Pointer == NULL)
                        return NULL;
                    str = (const char *)Lv->Val->Pointer;
                    len = (int)strlen(str);
                    if (ArrSize == -1)
                        ArrSize = len + 1; /* include NUL */
                    if (ArrSize < 1 || ArrSize > 16 * 1024 * 1024)
                        return NULL;
                    /* C: char c[3]="xyz" is valid (no room for NUL). */
                    if (len > ArrSize)
                        return NULL; /* string too long for array */
                    Vals = (long long *)calloc((size_t)ArrSize, sizeof(long long));
                    if (Vals == NULL) return NULL;
                    for (i = 0; i < len && i < ArrSize; i++)
                        Vals[i] = (unsigned char)str[i];
                    /* remaining bytes already 0 from calloc (NUL pad when fits) */
                    One->DeclArraySize = ArrSize;
                    One->DeclIsPtr = 1;
                    One->DeclElemSize = 1;
                    One->DeclInitVals = Vals;
                    One->DeclInitCount = ArrSize; /* full zero-pad already in Vals */
                    /* sizeof(a) for char a[]=".." must be ArrSize (incl NUL) */
                    if (Slot < IR_COMPILE_MAX_LOCALS)
                    {
                        IrCompileLocalElemSize[Slot] = ArrSize;
                        IrCompileLocalArraySize[Slot] = ArrSize;
                        IrCompileLocalScale[Slot] = 1;
                    }
                }
                else if ((ArrSize > 0 || ArrSize == -1)
                    && LexGetToken(P, NULL, FALSE) == TokenLeftBrace)
                {
                    /* T a[N] = { e0, e1, ... }; non-const exprs via DeclInitExprs.
                     * char a[R][C] = {"ab","cd"} — each string fills one row. */
                    long long *Vals;
                    IrExpr **Exprs = NULL;
                    int ni = 0, cap;
                    int rowLen = 0;
                    if (BaseElem == 1 && NDims >= 2 && Dims[NDims - 1] > 0)
                        rowLen = Dims[NDims - 1];
                    LexGetToken(P, NULL, TRUE); /* { */
                    cap = (ArrSize > 0) ? ArrSize : 16;
                    Vals = (long long *)calloc((size_t)cap, sizeof(long long));
                    if (Vals == NULL) return NULL;
                    while (LexGetToken(P, NULL, FALSE) != TokenRightBrace)
                    {
                        if (rowLen > 0 && LexGetToken(P, NULL, FALSE) == TokenStringConstant)
                        {
                            struct Value *Lv = NULL;
                            const char *str;
                            int len, k;
                            LexGetToken(P, &Lv, TRUE);
                            if (Lv == NULL || Lv->Val == NULL || Lv->Val->Pointer == NULL)
                            {
                                free(Vals); free(Exprs); return NULL;
                            }
                            str = (const char *)Lv->Val->Pointer;
                            len = (int)strlen(str);
                            if (len + 1 > rowLen)
                            {
                                free(Vals); free(Exprs); return NULL;
                            }
                            if (ni + rowLen > cap)
                            {
                                int ncap = cap;
                                long long *nv;
                                while (ncap < ni + rowLen) ncap *= 2;
                                if (ncap > 65536) { free(Vals); free(Exprs); return NULL; }
                                nv = (long long *)realloc(Vals, (size_t)ncap * sizeof(long long));
                                if (nv == NULL) { free(Vals); free(Exprs); return NULL; }
                                { int z; for (z = cap; z < ncap; z++) nv[z] = 0; }
                                Vals = nv;
                                if (Exprs != NULL)
                                {
                                    IrExpr **ne = (IrExpr **)realloc(Exprs, (size_t)ncap * sizeof(IrExpr *));
                                    if (ne == NULL) { free(Vals); free(Exprs); return NULL; }
                                    { int z; for (z = cap; z < ncap; z++) ne[z] = NULL; }
                                    Exprs = ne;
                                }
                                cap = ncap;
                            }
                            for (k = 0; k < len; k++)
                                Vals[ni + k] = (unsigned char)str[k];
                            for (; k < rowLen; k++)
                                Vals[ni + k] = 0;
                            ni += rowLen;
                        }
                        else if (LexGetToken(P, NULL, FALSE) == TokenLeftBrace)
                        {
                            /* nested brace: flatten into row-major values */
                            int depth = 0;
                            LexGetToken(P, NULL, TRUE); /* { */
                            depth = 1;
                            while (depth > 0 && LexGetToken(P, NULL, FALSE) != TokenEOF)
                            {
                                enum LexToken Ct = LexGetToken(P, NULL, FALSE);
                                if (Ct == TokenLeftBrace)
                                { LexGetToken(P, NULL, TRUE); depth++; continue; }
                                if (Ct == TokenRightBrace)
                                {
                                    LexGetToken(P, NULL, TRUE);
                                    depth--;
                                    if (depth == 0) break;
                                    continue;
                                }
                                if (Ct == TokenComma)
                                { LexGetToken(P, NULL, TRUE); continue; }
                                {
                                    IrExpr *Ex = IrCompileAssignExpr(P);
                                    if (Ex == NULL) { free(Vals); free(Exprs); return NULL; }
                                    if (ni >= cap)
                                    {
                                        int ncap = cap * 2;
                                        long long *nv;
                                        if (ncap > 65536) { free(Vals); free(Exprs); return NULL; }
                                        nv = (long long *)realloc(Vals, (size_t)ncap * sizeof(long long));
                                        if (nv == NULL) { free(Vals); free(Exprs); return NULL; }
                                        Vals = nv;
                                        if (Exprs != NULL)
                                        {
                                            IrExpr **ne = (IrExpr **)realloc(Exprs, (size_t)ncap * sizeof(IrExpr *));
                                            if (ne == NULL) { free(Vals); free(Exprs); return NULL; }
                                            { int z; for (z = ni; z < ncap; z++) ne[z] = NULL; }
                                            Exprs = ne;
                                        }
                                        cap = ncap;
                                    }
                                    long long folded;
                                    if (IrTryFoldInitConst(Ex, &folded))
                                    {
                                        Vals[ni] = folded;
                                        IrFreeExpr(Ex);
                                    }
                                    else
                                    {
                                        if (Exprs == NULL)
                                        {
                                            Exprs = (IrExpr **)calloc((size_t)cap, sizeof(IrExpr *));
                                            if (Exprs == NULL) { free(Vals); return NULL; }
                                        }
                                        Exprs[ni] = Ex;
                                        Vals[ni] = 0;
                                    }
                                    ni++;
                                }
                            }
                        }
                        else
                        {
                            IrExpr *Ex = IrCompileAssignExpr(P);
                            if (Ex == NULL)
                            {
                                free(Vals); free(Exprs); return NULL;
                            }
                            if (ni >= cap)
                            {
                                int ncap = cap * 2;
                                long long *nv;
                                if (ncap > 65536) { free(Vals); free(Exprs); return NULL; }
                                nv = (long long *)realloc(Vals, (size_t)ncap * sizeof(long long));
                                if (nv == NULL) { free(Vals); free(Exprs); return NULL; }
                                Vals = nv;
                                if (Exprs != NULL)
                                {
                                    IrExpr **ne = (IrExpr **)realloc(Exprs, (size_t)ncap * sizeof(IrExpr *));
                                    if (ne == NULL) { free(Vals); free(Exprs); return NULL; }
                                    { int z; for (z = ni; z < ncap; z++) ne[z] = NULL; }
                                    Exprs = ne;
                                }
                                cap = ncap;
                            }
                            long long folded;
                            if (IrTryFoldInitConst(Ex, &folded))
                            {
                                Vals[ni] = folded;
                                IrFreeExpr(Ex);
                            }
                            else
                            {
                                if (Exprs == NULL)
                                {
                                    Exprs = (IrExpr **)calloc((size_t)cap, sizeof(IrExpr *));
                                    if (Exprs == NULL) { free(Vals); return NULL; }
                                }
                                Exprs[ni] = Ex;
                                Vals[ni] = 0;
                                Ex = NULL;
                            }
                            ni++;
                        }
                        if (LexGetToken(P, NULL, FALSE) == TokenComma)
                            LexGetToken(P, NULL, TRUE);
                    }
                    if (LexGetToken(P, NULL, TRUE) != TokenRightBrace)
                    {
                        free(Vals); free(Exprs); return NULL;
                    }
                    if (ArrSize == -1)
                    {
                        if (ni <= 0 || ni > 4096) { free(Vals); free(Exprs); return NULL; }
                        ArrSize = ni;
                        One->DeclArraySize = ArrSize;
                        /* sizeof(a) for T a[] = {..}: was stuck at BaseElem (1)
                         * because IrCompileLocalElemSize was set before brace count. */
                        One->DeclIsPtr = 1;
                        One->DeclElemSize = BaseElem > 0 ? BaseElem : 1;
                        /* char a[][C] = {"..",..}: fix first dim from row count */
                        if (NDims >= 2 && Dims[NDims - 1] > 0 && rowLen > 0
                            && (ni % rowLen) == 0)
                        {
                            Dims[0] = ni / rowLen;
                            One->DeclNDims = NDims;
                            { int di; for (di = 0; di < NDims; di++)
                                One->DeclDims[di] = Dims[di]; }
                        }
                        if (Slot < IR_COMPILE_MAX_LOCALS)
                        {
                            IrCompileLocalElemSize[Slot] =
                                One->DeclElemSize * ArrSize;
                            IrCompileLocalArraySize[Slot] = ArrSize;
                            /* Scale = element size for sizeof(a[i]) */
                            IrCompileLocalScale[Slot] = One->DeclElemSize > 0
                                ? One->DeclElemSize : 1;
                        }
                    }
                    else if (ni > ArrSize)
                    {
                        free(Vals); free(Exprs); return NULL;
                    }
                    /* fixed T a[N]={..} — ensure sizeof uses full N*elem */
                    if (ArrSize > 0 && Slot < IR_COMPILE_MAX_LOCALS)
                    {
                        int es = One->DeclElemSize > 0 ? One->DeclElemSize
                            : (BaseElem > 0 ? BaseElem : 1);
                        One->DeclElemSize = es;
                        One->DeclArraySize = ArrSize;
                        IrCompileLocalElemSize[Slot] = es * ArrSize;
                        IrCompileLocalArraySize[Slot] = ArrSize;
                        IrCompileLocalScale[Slot] = es;
                    }
                    One->DeclInitVals = Vals;
                    One->DeclInitCount = ni;
                    One->DeclInitExprs = Exprs;
                }

                else if (ArrSize == 0)
                {
                    One->Expr = IrCompileAssignExpr(P);
                    if (One->Expr == NULL) return NULL;
                }
                else
                {
                    /* int dst[] = src; / int dst[N] = src; — array-to-array
                     * copy when RHS is Ident of array type (097). */
                    IrExpr *SrcE = IrCompileAssignExpr(P);
                    if (SrcE == NULL) return NULL;
                    if (SrcE->Kind != IrExIdent)
                        return NULL;
                    /* Infer [] size from source array local if known */
                    if (ArrSize == -1 && SrcE->LocalSlot >= 0
                        && SrcE->LocalSlot < IR_COMPILE_MAX_LOCALS
                        && IrCompileLocalArraySize[SrcE->LocalSlot] > 0)
                    {
                        ArrSize = IrCompileLocalArraySize[SrcE->LocalSlot];
                        One->DeclArraySize = ArrSize;
                        if (Slot < IR_COMPILE_MAX_LOCALS)
                        {
                            int es = One->DeclElemSize > 0 ? One->DeclElemSize
                                : (BaseElem > 0 ? BaseElem : (int)sizeof(int));
                            One->DeclElemSize = es;
                            IrCompileLocalArraySize[Slot] = ArrSize;
                            IrCompileLocalElemSize[Slot] = es * ArrSize;
                            IrCompileLocalScale[Slot] = es;
                        }
                    }
                    if (ArrSize <= 0)
                        return NULL;
                    One->Expr = SrcE;
                    /* Runtime: copy ArrSize*elem bytes from source array */
                    One->DeclInitCount = -1; /* flag: array-ident init */
                }
            }
            if (Count >= Cap)
            {
                int NCap = Cap ? Cap * 2 : 4;
                IrStmt **Bigger = (IrStmt **)realloc(List, (size_t)NCap * sizeof(IrStmt *));
                if (Bigger == NULL) return NULL;
                List = Bigger;
                Cap = NCap;
            }
            List[Count++] = One;
            if (LexGetToken(P, NULL, FALSE) == TokenComma)
            {
                LexGetToken(P, NULL, TRUE);
                continue;
            }
            break;
        }
        if (LexGetToken(P, NULL, TRUE) != TokenSemicolon)
        {
            free(List);
            return NULL;
        }
        if (Count == 1)
        {
            /* List is only a construction buffer; the returned declaration
             * owns One, not the temporary pointer vector. */
            IrStmt *Only = List[0];
            free(List);
            return Only;
        }
        S = IrAllocStmt(P->pc, IrStBlock);
        if (S == NULL)
        {
            free(List);
            return NULL;
        }
        S->Stmts = List;
        S->StmtCount = Count;
        return S;
    }
    if (Tok == TokenIntType || Tok == TokenShortType || Tok == TokenCharType
        || Tok == TokenLongType)
        return NULL;

    /* struct/union Tag *p;  or  struct/union Tag p;  or  union U { ... } u; */
    if ((Tok == TokenStructType || Tok == TokenUnionType) && IrCompilingFunctionBody)
    {
        struct ParseState Look;
        enum LexToken T;
        ParserCopy(&Look, P);
        LexGetToken(&Look, NULL, TRUE); /* struct/union */
        T = LexGetToken(&Look, NULL, TRUE); /* tag or { */
        /* Inline definition: struct/union [Tag] { ... } name; */
        if (T == TokenLeftBrace
            || (T == TokenIdentifier
                && LexGetToken(&Look, NULL, FALSE) == TokenLeftBrace))
        {
            struct ValueType *Typ = NULL;
            char *DName = NULL;
            int IsStatic = 0;
            int Slot;
            IrStmt *One;
            TypeParse(P, &Typ, &DName, &IsStatic);
            if (Typ == NULL || DName == NULL)
                return NULL;
            if (Typ->Base != TypeStruct && Typ->Base != TypeUnion)
                return NULL;
            if (Typ->Sizeof < 1 || Typ->Members == NULL)
                return NULL;
            /* scalar/pointer members only (same policy as value local) */
            if (!IrStructLayoutOk(Typ, 4))
                return NULL;
            Slot = IrInternLocal(DName);
            if (Slot < 0) return NULL;
            One = IrAllocStmt(P->pc, IrStDecl);
            if (One == NULL) return NULL;
            One->DeclIdent = DName;
            One->LocalSlot = Slot;
            One->DeclIsStatic = LeadingStatic;
            One->DeclIsConst = IrCompileLeadingConst; /* value local */
            One->DeclFromType = Typ;
            One->DeclIsPtr = 0;
            One->DeclElemSize = Typ->Sizeof;
            One->DeclArraySize = 1;
            if (Slot < IR_COMPILE_MAX_LOCALS)
            {
                IrCompileLocalElemSize[Slot] = Typ->Sizeof;
                IrCompileLocalFromType[Slot] = Typ;
            }
            if (LexGetToken(P, NULL, TRUE) != TokenSemicolon)
                return NULL;
            return One;
        }
        if (T == TokenIdentifier)
        {
            T = LexGetToken(&Look, NULL, FALSE);
            if (T == TokenAsterisk || T == TokenIdentifier)
            {
                struct Value *LexVal;
                char *DName;
                char *TagName;
                int Slot;
                int IsPtr = 0;
                int StarCount = 0;
                IrStmt *One;
                struct ValueType *FromTy = NULL;
                /* Multi-declarator (struct T a = {..}, *p, b[2] = {..};): each
                 * declarator goes through the same path below; finished ones
                 * are collected here and returned as one block. */
                IrStmt *DeclList[64];
                int DeclListN = 0;
                LexGetToken(P, NULL, TRUE); /* struct/union */
                LexGetToken(P, &LexVal, TRUE); /* tag */
                TagName = LexVal->Val->Identifier;
                if (TagName != NULL)
                {
                    enum BaseType Want = (Tok == TokenStructType) ? TypeStruct : TypeUnion;
                    FromTy = TypeGetMatching(P->pc, P, &P->pc->UberType, Want,
                                             0, TagName, TRUE);
                    if (FromTy != NULL && FromTy->Members == NULL)
                        FromTy = NULL; /* incomplete */
                }
            struct_next_declarator:
                while (LexGetToken(P, NULL, FALSE) == TokenAsterisk)
                {
                    LexGetToken(P, NULL, TRUE);
                    StarCount++;
                    IsPtr = 1;
                }
                if (LexGetToken(P, &LexVal, TRUE) != TokenIdentifier)
                    return NULL;
                DName = LexVal->Val->Identifier;
                /* optional [N]: struct Tag a[N]; or struct Tag *a[N] (ptr array) */
                {
                    int StructArrN = 0;
                    int StructArrInfer = 0;
                    int StructArrNDims = 0;
                    int StructArrDims[IR_MAX_ARRAY_DIMS] = {0};
                    long long StructArrProduct = 1;
                    while (LexGetToken(P, NULL, FALSE) == TokenLeftSquareBracket)
                    {
                        struct Value *Lv = NULL;
                        int dim;
                        if (StructArrNDims >= IR_MAX_ARRAY_DIMS) return NULL;
                        LexGetToken(P, NULL, TRUE); /* [ */
                        if (LexGetToken(P, NULL, FALSE) == TokenRightSquareBracket)
                        {
                            if (IsPtr || StructArrNDims != 0)
                                return NULL; /* only a single leading [] may infer size */
                            LexGetToken(P, NULL, TRUE);
                            StructArrInfer = 1;
                            StructArrN = -1;
                            StructArrDims[0] = -1;
                            StructArrNDims = 1;
                            break;
                        }
                        if (LexGetToken(P, &Lv, TRUE) != TokenIntegerConstant)
                            return NULL;
                        dim = (int)Lv->Val->Integer;
                        if (dim <= 0 || dim > 4096
                            || LexGetToken(P, NULL, TRUE) != TokenRightSquareBracket)
                            return NULL;
                        StructArrDims[StructArrNDims++] = dim;
                        StructArrProduct *= dim;
                        if (StructArrProduct > 4096) return NULL;
                    }
                    if (StructArrNDims > 0 && !StructArrInfer)
                    {
                        StructArrN = (int)StructArrProduct;
                        /* This Body-IR extension currently supports value arrays,
                         * not multidimensional arrays of struct pointers. */
                        if (IsPtr && StructArrNDims > 1) return NULL;
                    }
                Slot = IrInternLocal(DName);
                if (Slot < 0)
                    return NULL;
                /* value local: complete type; members may be scalar/BF,
                 * fixed scalar arrays (struct only), or nested struct/union. */
                if (!IsPtr)
                {
                    if (FromTy == NULL || FromTy->Sizeof < 1)
                        return NULL;
                    if (FromTy->Members != NULL)
                    {
                        int hi;
                        int isUnion = (FromTy->Base == TypeUnion);
                        for (hi = 0; hi < FromTy->Members->Size; hi++)
                        {
                            struct TableEntry *Ent;
                            for (Ent = FromTy->Members->HashTable[hi]; Ent != NULL; Ent = Ent->Next)
                            {
                                struct Value *MV = Ent->p.v.Val;
                                enum BaseType B;
                                struct ValueType *MT;
                                if (MV == NULL || MV->Typ == NULL)
                                    continue;
                                MT = MV->Typ;
                                B = MT->Base;
                                /* allow pointer/fptr members as opaque bits */
                                if (B == TypeFunction)
                                    return NULL;
                                if (B == TypeArray)
                                {
                                    /* array members (incl. union punning via char[]):
                                     * allow scalar/struct element arrays */
                                    {
                                        struct ValueType *ET = MT->FromType;
                                        enum BaseType EB;
                                        if (ET == NULL)
                                            return NULL;
                                        if (MT->ArraySize <= 0 && MT->Sizeof <= 0)
                                            return NULL;
                                        EB = ET->Base;
                                        /* TypeFP arrays are opaque blobs (101). */
                                        if (EB == TypeArray
                                            || EB == TypeFunction)
                                            return NULL;
                                        if ((EB == TypeStruct || EB == TypeUnion)
                                            && !IrStructLayoutOk(ET, 3))
                                            return NULL;
                                    }
                                }
                                else if (B == TypeStruct || B == TypeUnion)
                                {
                                    if (!IrStructLayoutOk(MT, 4))
                                        return NULL;
                                }
                            }
                        }
                    }
                }
                One = IrAllocStmt(P->pc, IrStDecl);
                if (One == NULL) return NULL;
                One->DeclIdent = DName;
                One->LocalSlot = Slot;
                One->DeclIsStatic = LeadingStatic;
                One->DeclIsConst = IrCompileLeadingConst && !IsPtr;
                One->DeclSlot = NULL;
                One->DeclFromType = FromTy;
                One->DeclInitVals = NULL;
                One->DeclInitCount = 0;
                One->DeclInitExprs = NULL;
                One->DeclInitNames = NULL;
                if (!IsPtr && StructArrN > 0 && StructArrNDims > 0)
                {
                    int di;
                    One->DeclNDims = StructArrNDims;
                    for (di = 0; di < StructArrNDims; ++di)
                        One->DeclDims[di] = StructArrDims[di];
                }
                if (!IsPtr && Slot >= 0 && Slot < IR_COMPILE_MAX_LOCALS && FromTy != NULL)
                {
                    IrCompileLocalFromType[Slot] = FromTy;
                    if (FromTy->Sizeof > 0)
                        IrCompileLocalElemSize[Slot] = FromTy->Sizeof;
                }
                if (IsPtr && StructArrN > 0)
                {
                    /* struct Tag *a[N]; — array of pointers */
                    One->DeclIsPtr = 1;
                    One->DeclElemIsPtr = 1;
                    One->DeclElemSize = (int)sizeof(void *);
                    One->DeclArraySize = StructArrN;
                    One->DeclFromType = FromTy; /* pointee type for a[i]->m */
                    /* B.2: pointee size on decl so frame LocalPointeeSize is set */
                    One->DeclPointeeSize = (FromTy != NULL && FromTy->Sizeof > 0)
                        ? FromTy->Sizeof : (int)sizeof(int);
                }
                else if (IsPtr)
                {
                    if (StarCount >= 2 && FromTy != NULL && Slot >= 0
                        && Slot < IR_COMPILE_MAX_LOCALS)
                    {
                        struct ValueType *Full = FromTy;
                        int sc;
                        for (sc = 0; sc < StarCount && Full != NULL; sc++)
                            Full = TypeGetMatching(P->pc, P, Full, TypePointer, 0,
                                                   P->pc->StrEmpty, TRUE);
                        IrCompileLocalCanonType[Slot] = Full;
                    }
                    One->DeclIsPtr = 1;
                    /* Word-sized slot; pointee size separate (struct T *p). */
                    One->DeclElemSize = (int)sizeof(void *);
                    One->DeclArraySize = 0;
                    /* Keep DeclFromType as the struct/union pointee (FromTy).
                     * Multi-level T **p still uses DeclPointeeSize=sizeof(void*).
                     * Do not wrap FromTy in TypePointer here: Member resolution
                     * (SafeMemberAddr / tree) expects the struct type for T **p
                     * so (*pp)->field can TableGet on struct members. Pointer
                     * depth for *pp vs **pp is handled by StarCount + Kind=2
                     * depth accounting in IrAnnotateExpr. */
                    One->DeclFromType = FromTy;
                    /* B.2: T *p — DeclPointeeSize for *(p+k) without TableGet.
                     * Multi-level (T **p): first-level pointee is pointer-sized. */
                    if (StarCount >= 2)
                        One->DeclPointeeSize = (int)sizeof(void *);
                    else
                        One->DeclPointeeSize = (FromTy != NULL && FromTy->Sizeof > 0)
                            ? FromTy->Sizeof : (int)sizeof(int);
                    if (Slot < IR_COMPILE_MAX_LOCALS)
                    {
                        /* sizeof(p) = word; p+n via Scale = pointee */
                        IrCompileLocalElemSize[Slot] = (int)sizeof(void *);
                        IrCompileLocalScale[Slot] = One->DeclPointeeSize > 0
                            ? One->DeclPointeeSize : (int)sizeof(void *);
                    }
                }
                else if (StructArrN > 0)
                {
                    /* struct Tag a[N]; — N-element blob, element type = FromTy */
                    One->DeclIsPtr = 1;
                    One->DeclElemSize = FromTy->Sizeof;
                    One->DeclArraySize = StructArrN;
                }
                else if (StructArrInfer)
                {
                    /* size filled after brace-init */
                    One->DeclIsPtr = 1;
                    One->DeclElemSize = FromTy->Sizeof;
                    One->DeclArraySize = 1; /* temporary */
                }
                else
                {
                    /* single struct value: 1-element blob */
                    One->DeclIsPtr = 0;
                    One->DeclElemSize = FromTy->Sizeof;
                    One->DeclArraySize = 1;
                }
                if (LexGetToken(P, NULL, FALSE) == TokenAssign)
                {
                    /* pointer: p = expr; */
                    if (IsPtr && StructArrN == 0 && !StructArrInfer)
                    {
                        LexGetToken(P, NULL, TRUE); /* = */
                        One->Expr = IrCompileAssignExpr(P);
                        if (One->Expr == NULL) return NULL;
                        goto struct_decl_done;
                    }
                    /* struct value: S t = make(...); or S t = other; (by-value copy)
                     * Brace-init still handled below. */
                    if (!IsPtr && StructArrN == 0 && !StructArrInfer
                        && LexGetToken(P, NULL, FALSE) == TokenAssign)
                    {
                        struct ParseState Look;
                        ParserCopy(&Look, P);
                        LexGetToken(&Look, NULL, TRUE); /* = */
                        if (LexGetToken(&Look, NULL, FALSE) != TokenLeftBrace)
                        {
                            LexGetToken(P, NULL, TRUE); /* = */
                            One->Expr = IrCompileAssignExpr(P);
                            if (One->Expr == NULL) return NULL;
                            /* single value blob (copy from expr result) */
                            One->DeclIsPtr = 1; /* owned blob */
                            One->DeclArraySize = 1;
                            goto struct_decl_done;
                        }
                    }
                    /* brace-init: sequential, designated (.m=), nested braces,
                     * integer/FP constants, and non-const expressions. */
                    long long *Vals = NULL;
                    IrExpr **Exprs = NULL;
                    char **Names = NULL;
                    int ni = 0, cap = 0, depth;
                    int anyExpr = 0;
                    LexGetToken(P, NULL, TRUE); /* = */
                    if (LexGetToken(P, NULL, FALSE) != TokenLeftBrace)
                        return NULL;
                    depth = 0;
                    do {
                        enum LexToken Ct;
                        struct Value *Lv = NULL;
                        int neg = 0;
                        char *desig = NULL;
                        long long cval = 0;
                        IrExpr *ex = NULL;
                        Ct = LexGetToken(P, NULL, FALSE);
                        if (Ct == TokenLeftBrace)
                        {
                            LexGetToken(P, NULL, TRUE);
                            depth++;
                            continue;
                        }
                        if (Ct == TokenRightBrace)
                        {
                            LexGetToken(P, NULL, TRUE);
                            if (depth > 0)
                                depth--;
                            if (depth == 0)
                                break;
                            if (LexGetToken(P, NULL, FALSE) == TokenComma)
                                LexGetToken(P, NULL, TRUE);
                            continue;
                        }
                        /* designated: .member = value (struct/union fields).
                         * Name stored in DeclInitNames; runtime packs by member. */
                        if (Ct == TokenDot)
                        {
                            LexGetToken(P, NULL, TRUE);
                            if (LexGetToken(P, &Lv, TRUE) != TokenIdentifier || Lv == NULL)
                            {
                                free(Vals); free(Exprs); free(Names);
                                return NULL;
                            }
                            desig = strdup(Lv->Val->Identifier);
                            if (desig == NULL)
                            {
                                free(Vals); free(Exprs); free(Names);
                                return NULL;
                            }
                            if (LexGetToken(P, NULL, TRUE) != TokenAssign)
                            {
                                free(desig);
                                free(Vals); free(Exprs); free(Names);
                                return NULL;
                            }
                            Ct = LexGetToken(P, NULL, FALSE);
                        }
                        if (Ct == TokenMinus)
                        {
                            LexGetToken(P, NULL, TRUE);
                            neg = 1;
                            Ct = LexGetToken(P, NULL, FALSE);
                        }
                        if (Ct == TokenIntegerConstant)
                        {
                            LexGetToken(P, &Lv, TRUE);
                            if (Lv == NULL) { free(Vals); free(Exprs); free(Names); return NULL; }
                            cval = (long long)Lv->Val->Integer;
                            if (neg) cval = -cval;
                        }
#ifndef NO_FP
                        else if (Ct == TokenFPConstant)
                        {
                            double d;
                            LexGetToken(P, &Lv, TRUE);
                            if (Lv == NULL) { free(Vals); free(Exprs); free(Names); return NULL; }
                            d = Lv->Val->FP;
                            if (neg) d = -d;
                            /* store IEEE bits in long long for runtime double load */
                            {
                                union { double d; long long l; } u;
                                u.d = d;
                                cval = u.l;
                            }
                        }
#endif
                        else
                        {
                            /* non-const expression */
                            if (neg)
                            {
                                /* unary minus already consumed — rebuild via expr */
                                free(Vals); free(Exprs); free(Names);
                                return NULL; /* require (-expr) with parens for now if complex */
                            }
                            ex = IrCompileAssignExpr(P);
                            if (ex == NULL)
                            {
                                free(Vals); free(Exprs); free(Names);
                                return NULL;
                            }
                            /* Root fold already turned enum rvalues into Const.
                             * Peel Const into DeclInitVals for DECL_STRUCT. */
                            if (ex->Kind == IrExConst)
                            {
                                cval = ex->ConstVal;
                                IrFreeExpr(ex);
                                ex = NULL;
                            }
                            else
                                anyExpr = 1;
                        }
                        if (ni >= cap)
                        {
                            int ncap = cap ? cap * 2 : 8;
                            long long *nv;
                            IrExpr **ne;
                            char **nn;
                            if (ncap > 4096) { free(Vals); free(Exprs); free(Names); return NULL; }
                            nv = (long long *)realloc(Vals, (size_t)ncap * sizeof(long long));
                            if (nv == NULL) { free(Vals); free(Exprs); free(Names); return NULL; }
                            Vals = nv;
                            ne = (IrExpr **)realloc(Exprs, (size_t)ncap * sizeof(IrExpr *));
                            if (ne == NULL) { free(Vals); free(Exprs); free(Names); return NULL; }
                            Exprs = ne;
                            nn = (char **)realloc(Names, (size_t)ncap * sizeof(char *));
                            if (nn == NULL) { free(Vals); free(Exprs); free(Names); return NULL; }
                            Names = nn;
                            /* zero new name/expr slots */
                            {
                                int z;
                                for (z = cap; z < ncap; z++)
                                { ne[z] = NULL; nn[z] = NULL; nv[z] = 0; }
                            }
                            cap = ncap;
                        }
                        Vals[ni] = cval;
                        Exprs[ni] = ex;
                        Names[ni] = desig;
                        ni++;
                        if (LexGetToken(P, NULL, FALSE) == TokenComma)
                            LexGetToken(P, NULL, TRUE);
                    } while (1);
                    if (ni <= 0)
                    {
                        free(Vals); free(Exprs); free(Names);
                        return NULL;
                    }
                    if (!anyExpr)
                    {
                        free(Exprs); Exprs = NULL;
                    }
                    if (StructArrInfer)
                    {
                        int mc = 0, hi;
                        struct TableEntry *Ent;
                        if (FromTy->Members != NULL)
                        {
                            for (hi = 0; hi < FromTy->Members->Size; hi++)
                                for (Ent = FromTy->Members->HashTable[hi]; Ent; Ent = Ent->Next)
                                    mc++;
                        }
                        if (mc < 1) mc = 1;
                        StructArrN = (ni + mc - 1) / mc;
                        if (StructArrN < 1 || StructArrN > 4096)
                        {
                            free(Vals); free(Exprs); free(Names);
                            return NULL;
                        }
                        One->DeclIsPtr = 1;
                        One->DeclElemSize = FromTy->Sizeof;
                        One->DeclArraySize = StructArrN;
                    }
                    else if (!(StructArrN == 0 && One->DeclArraySize == 1 && !IsPtr)
                             && !(StructArrN > 0 || One->DeclArraySize > 1))
                    {
                        free(Vals); free(Exprs); free(Names);
                        return NULL;
                    }
                    One->DeclInitVals = Vals;
                    One->DeclInitCount = ni;
                    One->DeclInitExprs = Exprs;
                    One->DeclInitNames = Names;
                }
            struct_decl_done:
                /* Next declarator of the same declaration (any of them may
                 * carry an initializer): `struct P a = {1, 2}, b = {3, 4};`
                 * previously failed the whole body (token path), where a
                 * double call result was then read as int (0.00). */
                if (LexGetToken(P, NULL, FALSE) == TokenComma)
                {
                    if (DeclListN >= 63)
                        return NULL;
                    DeclList[DeclListN++] = One;
                    LexGetToken(P, NULL, TRUE); /* , */
                    IsPtr = 0;
                    StarCount = 0;
                    goto struct_next_declarator;
                }
                if (LexGetToken(P, NULL, TRUE) != TokenSemicolon)
                {
                    if (One->DeclInitVals != NULL)
                    {
                        free(One->DeclInitVals);
                        One->DeclInitVals = NULL;
                        One->DeclInitCount = 0;
                    }
                    return NULL;
                }
                if (DeclListN > 0)
                {
                    IrStmt *Blk = IrAllocStmt(P->pc, IrStBlock);
                    IrStmt **List;
                    if (Blk == NULL) return NULL;
                    DeclList[DeclListN++] = One;
                    List = (IrStmt **)malloc((size_t)DeclListN * sizeof(IrStmt *));
                    if (List == NULL) return NULL;
                    memcpy(List, DeclList, (size_t)DeclListN * sizeof(IrStmt *));
                    Blk->Stmts = List;
                    Blk->StmtCount = DeclListN;
                    return Blk;
                }
                return One;
                }
            }
        }
    }

    } /* LeadingStatic scope for integer decls */

    /* float/double local scalars and fixed arrays with brace-init */
#ifndef NO_FP
    if ((Tok == TokenFloatType || Tok == TokenDoubleType) && IrCompilingFunctionBody)
    {
        IrStmt **List = NULL;
        int Cap = 0, Count = 0;
        int BaseElem = (int)sizeof(double);
        LexGetToken(P, NULL, TRUE); /* float/double */
        for (;;)
        {
            struct Value *LexVal;
            char *DName;
            int Slot, ArrSize = 0, IsPtr = 0, StarCount = 0;
            IrStmt *One;
            /* float *p / float **pp / float x */
            while (LexGetToken(P, NULL, FALSE) == TokenAsterisk)
            {
                LexGetToken(P, NULL, TRUE);
                IsPtr = 1;
                StarCount++;
            }
            if (LexGetToken(P, &LexVal, TRUE) != TokenIdentifier)
                return NULL;
            DName = LexVal->Val->Identifier;
            /* float a[N][M]...: flat product of dims (like int path) */
            {
                int NDims = 0;
                int Dims[IR_MAX_ARRAY_DIMS];
                int di;
                for (di = 0; di < IR_MAX_ARRAY_DIMS; di++) Dims[di] = 0;
                while (LexGetToken(P, NULL, FALSE) == TokenLeftSquareBracket)
                {
                    int dim;
                    if (NDims >= IR_MAX_ARRAY_DIMS)
                        return NULL;
                    LexGetToken(P, NULL, TRUE); /* [ */
                    if (LexGetToken(P, NULL, FALSE) == TokenRightSquareBracket)
                    {
                        if (NDims != 0) return NULL; /* only first dim may be [] */
                        LexGetToken(P, NULL, TRUE);
                        ArrSize = -1;
                        NDims = 1;
                        Dims[0] = -1;
                        break;
                    }
                    if (!IrCompilePeekIntConst(P, &dim))
                        return NULL;
                    if (dim <= 0 || dim > 16 * 1024 * 1024)
                        return NULL;
                    if (LexGetToken(P, NULL, TRUE) != TokenRightSquareBracket)
                        return NULL;
                    Dims[NDims++] = dim;
                }
                if (NDims > 0 && ArrSize != -1)
                {
                    long long tot = 1;
                    for (di = 0; di < NDims; di++)
                    {
                        tot *= Dims[di];
                        if (tot > 16 * 1024 * 1024) return NULL;
                    }
                    ArrSize = (int)tot;
                }
            }
            Slot = IrInternLocal(DName);
            if (Slot < 0) return NULL;
            One = IrAllocStmt(P->pc, IrStDecl);
            if (One == NULL) return NULL;
            One->DeclIdent = DName;
            One->LocalSlot = Slot;
            One->DeclSlot = NULL;
            One->DeclFromType = &P->pc->FPType;
            /* const double d / const float a[N]: object const (not const T *p). */
            One->DeclIsConst = IrCompileLeadingConst && StarCount == 0;
            /* float *p: pointer word; float x: scalar bits; float a[N]: blob */
            if (IsPtr && ArrSize == 0)
            {
                One->DeclIsPtr = 1;
                /* double **pp: DeclFromType is the first-level pointee
                 * (double *), same convention as the integer path. */
                if (StarCount >= 2)
                {
                    struct ValueType *PT = &P->pc->FPType;
                    int sc;
                    for (sc = 0; PT != NULL && sc < StarCount - 1; sc++)
                        PT = TypeGetMatching(P->pc, P, PT, TypePointer, 0,
                                             P->pc->StrEmpty, TRUE);
                    if (PT != NULL)
                        One->DeclFromType = PT;
                }
                One->DeclElemSize = (StarCount >= 2) ? (int)sizeof(void *) : BaseElem;
                One->DeclPointeeSize = (StarCount >= 2) ? (int)sizeof(void *) : BaseElem;
                One->DeclArraySize = 0;
                if (Slot >= 0 && Slot < IR_COMPILE_MAX_LOCALS)
                    IrCompileLocalElemSize[Slot] = (int)sizeof(void *);
                /* double *p: full type (float keeps the legacy path, as for
                 * the T *a[N] pointee below). */
                if (Tok == TokenDoubleType)
                    IrCompileStampLocalPointerType(P, Slot, &P->pc->FPType, StarCount);
            }
            else if (ArrSize == 0)
            {
                One->DeclIsPtr = 0;
                One->DeclElemSize = BaseElem;
                One->DeclArraySize = 0;
                if (Slot >= 0 && Slot < IR_COMPILE_MAX_LOCALS)
                    IrCompileLocalElemSize[Slot] = BaseElem;
            }
            else if (IsPtr)
            {
                /* float *a[N]: array of pointers */
                One->DeclIsPtr = 1;
                One->DeclElemIsPtr = 1;
                /* double *a[N]: typed pointee (float keeps the legacy path:
                 * the interpreter's FP working type is double-sized). */
                if (StarCount == 1 && BaseElem == (int)sizeof(double))
                    IrCompileSetElemPointee(One, Slot, &P->pc->FPType);
                One->DeclElemSize = (int)sizeof(void *);
                One->DeclPointeeSize = BaseElem;
                One->DeclArraySize = (ArrSize > 0) ? ArrSize : 1;
            }
            else
            {
                One->DeclIsPtr = 1;
                One->DeclElemSize = BaseElem;
                One->DeclArraySize = (ArrSize > 0) ? ArrSize : 1; /* -1 fixed later */
                if (Slot >= 0 && Slot < IR_COMPILE_MAX_LOCALS)
                    IrCompileLocalElemSize[Slot] = BaseElem;
            }
            One->DeclInitVals = NULL;
            One->DeclInitCount = 0;
            One->DeclInitExprs = NULL;
            One->DeclInitNames = NULL;
            if (LexGetToken(P, NULL, FALSE) == TokenAssign)
            {
                long long *Vals = NULL;
                int ni = 0, cap = 0;
                LexGetToken(P, NULL, TRUE);
                if (LexGetToken(P, NULL, FALSE) == TokenLeftBrace)
                {
                    LexGetToken(P, NULL, TRUE);
                    while (LexGetToken(P, NULL, FALSE) != TokenRightBrace)
                    {
                        enum LexToken Ct;
                        struct Value *Lv = NULL;
                        int neg = 0;
                        double d = 0;
                        Ct = LexGetToken(P, NULL, FALSE);
                        if (Ct == TokenMinus)
                        { LexGetToken(P, NULL, TRUE); neg = 1; Ct = LexGetToken(P, NULL, FALSE); }
                        if (Ct == TokenFPConstant)
                        {
                            LexGetToken(P, &Lv, TRUE);
                            d = Lv->Val->FP;
                        }
                        else if (Ct == TokenIntegerConstant)
                        {
                            LexGetToken(P, &Lv, TRUE);
                            d = (double)Lv->Val->Integer;
                        }
                        else
                        { free(Vals); return NULL; }
                        if (neg) d = -d;
                        if (ni >= cap)
                        {
                            int ncap = cap ? cap * 2 : 8;
                            long long *nv;
                            if (ncap > 4096) { free(Vals); return NULL; }
                            nv = (long long *)realloc(Vals, (size_t)ncap * sizeof(long long));
                            if (nv == NULL) { free(Vals); return NULL; }
                            Vals = nv; cap = ncap;
                        }
                        {
                            union { double d; long long l; } u;
                            u.d = d;
                            Vals[ni++] = u.l;
                        }
                        if (LexGetToken(P, NULL, FALSE) == TokenComma)
                            LexGetToken(P, NULL, TRUE);
                    }
                    LexGetToken(P, NULL, TRUE); /* } */
                    if (ArrSize < 0)
                    {
                        if (ni < 1) { free(Vals); return NULL; }
                        ArrSize = ni;
                        One->DeclArraySize = ArrSize;
                        One->DeclIsPtr = 1;
                    }
                    One->DeclInitVals = Vals;
                    One->DeclInitCount = ni;
                }
                else if (ArrSize == 0)
                {
                    /* scalar: float x = <expr>;  (const folded at runtime via IrEvalExpr) */
                    One->Expr = IrCompileAssignExpr(P);
                    if (One->Expr == NULL) return NULL;
                }
                else
                    return NULL;
            }
            else if (ArrSize < 0)
                return NULL; /* unsized needs brace */
            if (Count >= Cap)
            {
                int NCap = Cap ? Cap * 2 : 4;
                IrStmt **Bigger = (IrStmt **)realloc(List, (size_t)NCap * sizeof(IrStmt *));
                if (Bigger == NULL) return NULL;
                List = Bigger; Cap = NCap;
            }
            List[Count++] = One;
            if (LexGetToken(P, NULL, FALSE) == TokenComma)
            {
                LexGetToken(P, NULL, TRUE);
                continue;
            }
            if (LexGetToken(P, NULL, TRUE) != TokenSemicolon)
                return NULL;
            break;
        }
        if (Count == 1)
        {
            IrStmt *Only = List[0];
            free(List);
            return Only;
        }
        {
            IrStmt *Blk = IrAllocStmt(P->pc, IrStBlock);
            if (Blk == NULL) return NULL;
            Blk->Stmts = List;
            Blk->StmtCount = Count;
            return Blk;
        }
    }
#endif
    /* void *p; / void **pp; — pointer local (bits only) */
    if (Tok == TokenVoidType && IrCompilingFunctionBody)
    {
        struct ParseState Look;
        enum LexToken T;
        ParserCopy(&Look, P);
        LexGetToken(&Look, NULL, TRUE); /* void */
        T = LexGetToken(&Look, NULL, FALSE);
        if (T == TokenAsterisk)
        {
            IrStmt *One;
            struct Value *LexVal = NULL;
            char *DName;
            int Slot;
            LexGetToken(P, NULL, TRUE); /* void */
            while (LexGetToken(P, NULL, FALSE) == TokenAsterisk)
                LexGetToken(P, NULL, TRUE);
            if (LexGetToken(P, &LexVal, TRUE) != TokenIdentifier)
                return NULL;
            DName = LexVal->Val->Identifier;
            if (LexGetToken(P, NULL, FALSE) == TokenLeftSquareBracket)
                return NULL; /* no void *a[N] */
            Slot = IrInternLocal(DName);
            if (Slot < 0) return NULL;
            One = IrAllocStmt(P->pc, IrStDecl);
            if (One == NULL) return NULL;
            One->DeclIdent = DName;
            One->LocalSlot = Slot;
            One->DeclIsPtr = 1;
            One->DeclElemSize = (int)sizeof(void *);
            One->DeclArraySize = 0;
            One->DeclFromType = NULL;
            One->DeclPointeeSize = 1; /* void* — opaque, byte step for *(p+k) rare */
            if (Slot < IR_COMPILE_MAX_LOCALS)
                IrCompileLocalElemSize[Slot] = (int)sizeof(void *);
            if (LexGetToken(P, NULL, FALSE) == TokenAssign)
            {
                LexGetToken(P, NULL, TRUE);
                One->Expr = IrCompileAssignExpr(P);
                if (One->Expr == NULL) return NULL;
            }
            if (LexGetToken(P, NULL, TRUE) != TokenSemicolon)
                return NULL;
            return One;
        }
    }

    /* enum Color { RED=1, GREEN=2 };  →  int RED=1; int GREEN=2;  (+ optional enum Color c) */
    if (Tok == TokenEnumType && IrCompilingFunctionBody)
    {
        IrStmt **List = NULL;
        int Cap = 0, Count = 0;
        long long NextVal = 0;
        LexGetToken(P, NULL, TRUE); /* enum */
        /* optional tag */
        if (LexGetToken(P, NULL, FALSE) == TokenIdentifier)
            LexGetToken(P, NULL, TRUE);
        if (LexGetToken(P, NULL, FALSE) == TokenLeftBrace)
        {
            LexGetToken(P, NULL, TRUE); /* { */
            while (LexGetToken(P, NULL, FALSE) != TokenRightBrace
                   && LexGetToken(P, NULL, FALSE) != TokenEOF)
            {
                struct Value *Lv = NULL;
                char *EName;
                int Slot;
                IrStmt *One;
                long long V = NextVal;
                if (LexGetToken(P, &Lv, TRUE) != TokenIdentifier || Lv == NULL)
                    return NULL;
                EName = Lv->Val->Identifier;
                if (LexGetToken(P, NULL, FALSE) == TokenAssign)
                {
                    struct Value *Cv = NULL;
                    enum LexToken Ct;
                    int neg = 0;
                    LexGetToken(P, NULL, TRUE); /* = */
                    Ct = LexGetToken(P, &Cv, TRUE);
                    if (Ct == TokenMinus)
                    {
                        neg = 1;
                        Ct = LexGetToken(P, &Cv, TRUE);
                    }
                    if (Ct != TokenIntegerConstant || Cv == NULL)
                        return NULL;
                    V = (long long)Cv->Val->Integer;
                    if (neg) V = -V;
                }
                NextVal = V + 1;
                Slot = IrInternLocal(EName);
                if (Slot < 0) return NULL;
                One = IrAllocStmt(P->pc, IrStDecl);
                if (One == NULL) return NULL;
                One->DeclIdent = EName;
                One->LocalSlot = Slot;
                One->DeclIsPtr = 0;
                One->DeclElemSize = (int)sizeof(int);
                One->DeclArraySize = 0;
                One->DeclFromType = &P->pc->IntType;
                One->DeclInitVals = (long long *)calloc(1, sizeof(long long));
                if (One->DeclInitVals == NULL) return NULL;
                One->DeclInitVals[0] = V;
                One->DeclInitCount = 1;
                if (Slot < IR_COMPILE_MAX_LOCALS)
                    IrCompileLocalElemSize[Slot] = (int)sizeof(int);
                if (Count >= Cap)
                {
                    int NCap = Cap ? Cap * 2 : 8;
                    IrStmt **Bigger = (IrStmt **)realloc(List, (size_t)NCap * sizeof(IrStmt *));
                    if (Bigger == NULL) return NULL;
                    List = Bigger;
                    Cap = NCap;
                }
                List[Count++] = One;
                if (LexGetToken(P, NULL, FALSE) == TokenComma)
                    LexGetToken(P, NULL, TRUE);
            }
            if (LexGetToken(P, NULL, TRUE) != TokenRightBrace)
                return NULL;
        }
        /* optional: enum Color c;  /  enum Color c = GREEN; */
        if (LexGetToken(P, NULL, FALSE) == TokenIdentifier)
        {
            struct Value *Lv = NULL;
            char *DName;
            int Slot;
            IrStmt *One;
            LexGetToken(P, &Lv, TRUE);
            DName = Lv->Val->Identifier;
            Slot = IrInternLocal(DName);
            if (Slot < 0) return NULL;
            One = IrAllocStmt(P->pc, IrStDecl);
            if (One == NULL) return NULL;
            One->DeclIdent = DName;
            One->LocalSlot = Slot;
            One->DeclIsPtr = 0;
            One->DeclElemSize = (int)sizeof(int);
            One->DeclArraySize = 0;
            One->DeclFromType = &P->pc->IntType;
            One->DeclIsConst = IrCompileLeadingConst; /* const enum E c */
            if (Slot < IR_COMPILE_MAX_LOCALS)
                IrCompileLocalElemSize[Slot] = (int)sizeof(int);
            if (LexGetToken(P, NULL, FALSE) == TokenAssign)
            {
                LexGetToken(P, NULL, TRUE);
                One->Expr = IrCompileAssignExpr(P);
                if (One->Expr == NULL) return NULL;
            }
            if (Count >= Cap)
            {
                int NCap = Cap ? Cap * 2 : 4;
                IrStmt **Bigger = (IrStmt **)realloc(List, (size_t)NCap * sizeof(IrStmt *));
                if (Bigger == NULL) return NULL;
                List = Bigger;
                Cap = NCap;
            }
            List[Count++] = One;
        }
        if (LexGetToken(P, NULL, TRUE) != TokenSemicolon)
            return NULL;
        if (Count == 0)
            return IrAllocStmt(P->pc, IrStBlock); /* empty enum ok */
        if (Count == 1)
        {
            IrStmt *Only = List[0];
            free(List);
            return Only;
        }
        {
            IrStmt *Blk = IrAllocStmt(P->pc, IrStBlock);
            if (Blk == NULL) return NULL;
            Blk->Stmts = List;
            Blk->StmtCount = Count;
            return Blk;
        }
    }

    if (Tok == TokenFloatType || Tok == TokenDoubleType
        || Tok == TokenVoidType || Tok == TokenStructType || Tok == TokenUnionType
        || Tok == TokenEnumType)
        return NULL;

    if (Tok == TokenIf)
    {
        LexGetToken(P, NULL, TRUE);
        if (LexGetToken(P, NULL, TRUE) != TokenOpenBracket) return NULL;
        S = IrAllocStmt(P->pc, IrStIf);
        if (S == NULL) return NULL;
        S->Expr = IrCompileExpr(P);
        if (S->Expr == NULL) return NULL;
        if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket) return NULL;
        S->ThenStmt = IrCompileStmt(P);
        if (S->ThenStmt == NULL) return NULL;
        if (LexGetToken(P, NULL, FALSE) == TokenElse)
        {
            LexGetToken(P, NULL, TRUE);
            S->ElseStmt = IrCompileStmt(P);
            if (S->ElseStmt == NULL) return NULL;
        }
        return S;
    }

    if (Tok == TokenWhile)
    {
        struct ParseState CondPos;
        LexGetToken(P, NULL, TRUE);
        if (LexGetToken(P, NULL, TRUE) != TokenOpenBracket) return NULL;
        ParserCopy(&CondPos, P);
        S = IrAllocStmt(P->pc, IrStWhile);
        if (S == NULL) return NULL;
        S->Expr = IrCompileExpr(P);
        if (S->Expr == NULL) return NULL;
        if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket) return NULL;
        S->BodyStmt = IrCompileStmt(P);
        if (S->BodyStmt == NULL) return NULL;
        FastForCaptureCond(&CondPos, (FastForCond *)&S->FastCond);
        IrBindFastCondSlots(S);
        return S;
    }

    if (Tok == TokenDo)
    {
        LexGetToken(P, NULL, TRUE);
        S = IrAllocStmt(P->pc, IrStDo);
        if (S == NULL) return NULL;
        S->BodyStmt = IrCompileStmt(P);
        if (S->BodyStmt == NULL) return NULL;
        if (LexGetToken(P, NULL, TRUE) != TokenWhile) return NULL;
        if (LexGetToken(P, NULL, TRUE) != TokenOpenBracket) return NULL;
        {
            struct ParseState CondPos;
            ParserCopy(&CondPos, P);
            S->Expr = IrCompileExpr(P);
            if (S->Expr == NULL) return NULL;
            FastForCaptureCond(&CondPos, (FastForCond *)&S->FastCond);
        IrBindFastCondSlots(S);
        }
        if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket) return NULL;
        if (LexGetToken(P, NULL, TRUE) != TokenSemicolon) return NULL;
        return S;
    }

    if (Tok == TokenFor)
    {
        struct ParseState CondPos, IncPos; (void)IncPos;
        LexGetToken(P, NULL, TRUE);
        if (LexGetToken(P, NULL, TRUE) != TokenOpenBracket) return NULL;
        S = IrAllocStmt(P->pc, IrStFor);
        if (S == NULL) return NULL;
        /* init: empty, assign, or int i = 0; */
        if (LexGetToken(P, NULL, FALSE) != TokenSemicolon)
        {
            /* unsigned/signed/int/float for-init: route through IrCompileStmt
             * (int/unsigned paths exist; float scalar too). */
            S->InitStmt = IrCompileStmt(P);
            if (S->InitStmt == NULL)
                IR_FAIL("for-init: decl/expr form not supported in Body-IR");
        }
        else
            LexGetToken(P, NULL, TRUE); /* ; */
        /* condition */
        if (LexGetToken(P, NULL, FALSE) != TokenSemicolon)
        {
            ParserCopy(&CondPos, P);
            S->Expr = IrCompileExpr(P);
            if (S->Expr == NULL) return NULL;
            FastForCaptureCond(&CondPos, (FastForCond *)&S->FastCond);
        IrBindFastCondSlots(S);
        }
        if (LexGetToken(P, NULL, TRUE) != TokenSemicolon) return NULL;
        /* increment: expression without semicolon (i++ / i=i+1 / a++,b+=2) */
        if (LexGetToken(P, NULL, FALSE) != TokenCloseBracket)
        {
            struct ParseState IncSave;
            IrExpr *IncE;
            IrStmt *IncSt;
            ParserCopy(&IncSave, P);
            IncE = IrCompileExpr(P);
            if (IncE == NULL)
            {
                ParserCopy(P, &IncSave);
                return NULL;
            }
            /* must end at ) or we over-consumed */
            if (LexGetToken(P, NULL, FALSE) != TokenCloseBracket)
            {
                ParserCopy(P, &IncSave);
                return NULL;
            }
            IncSt = IrAllocStmt(P->pc, IrStExpr);
            if (IncSt == NULL) return NULL;
            IncSt->Expr = IncE;
            S->IncStmt = IncSt;
            if (IncE->Kind == IrExUnary
                && IncE->Op == TokenIncrement
                && IncE->Left != NULL && IncE->Left->Kind == IrExIdent)
            {
                /* ++ only; FastForDoInc is always +1. i-- / i+=2 → IncStmt. */
                S->FastInc.Active = 1;
                S->FastInc.VarName = IncE->Left->Ident;
                S->FastInc.VarSlot = IncE->Left->LocalSlot;
            }
            else if (IncE->Kind == IrExAssign && IncE->Left != NULL
                && IncE->Left->Kind == IrExIdent && IncE->Right != NULL
                && IncE->Right->Kind == IrExBinOp
                && (IncE->Right->Op == TokenPlus || IncE->Right->Op == TokenMinus)
                && IncE->Right->Left != NULL && IncE->Right->Left->Kind == IrExIdent
                && IncE->Left->Ident != NULL && IncE->Right->Left->Ident != NULL
                && strcmp(IncE->Left->Ident, IncE->Right->Left->Ident) == 0
                && IncE->Right->Right != NULL && IncE->Right->Right->Kind == IrExConst
                && IncE->Right->Right->ConstVal == 1)
            {
                /* i = i + 1 or i = i - 1 only. i+=2 must use IncStmt
                 * (FastForDoInc always steps by 1 — fuzz hard-fail class). */
                if (IncE->Right->Op == TokenPlus)
                {
                    S->FastInc.Active = 1;
                    S->FastInc.VarName = IncE->Left->Ident;
                    S->FastInc.VarSlot = IncE->Left->LocalSlot;
                }
                /* i = i - 1: leave FastInc off, run IncStmt */
            }
        }
        if (LexGetToken(P, NULL, TRUE) != TokenCloseBracket) return NULL;
        S->BodyStmt = IrCompileStmt(P);
        if (S->BodyStmt == NULL) return NULL;
        IrClassifyForFpCompile(S);
        return S;
    }

    /* assignment or expression statement: ident = expr ;  OR  expr ; (limited) */
    ParserCopy(&Save, P);
    if (Tok == TokenIdentifier)
    {
        struct Value *LexVal;
        struct Value *TypeVal = NULL;
        LexGetToken(P, &LexVal, TRUE);
        /* Typedef name (e.g. FILE, size_t, or user typedef struct Foo):
         * this is a declaration, not an expression. Body IR only handles a
         * subset of integer decls — return NULL so the function falls back
         * to the token path. Without this, `FILE *f;` is parsed as the
         * expression `FILE * f` and VariableGet("f") fails with 'f' is undefined. */
        if (VariableDefined(P->pc, LexVal->Val->Identifier))
        {
            VariableGet(P->pc, P, LexVal->Val->Identifier, &TypeVal);
            if (TypeVal != NULL && TypeVal->Typ != NULL
                && TypeVal->Typ->Base == Type_Type)
                IR_FAIL("typedef-name declaration (not in Body-IR subset)");
        }
        /* Ident may start assign or full expr (a=1, b=2). Reset and use
         * IrCompileExpr so comma is statement-level, not swallowed into RHS. */
        ParserCopy(P, &Save);
    }

    {
        IrExpr *Ex = IrCompileExpr(P);
        if (Ex == NULL) IR_FAIL("expression form not supported in Body-IR");
        if (LexGetToken(P, NULL, FALSE) != TokenSemicolon)
        {
            /* Match token-path diagnostic (132): expression not terminated and
             * next token is an identifier (start of next statement). */
            enum LexToken PeekSemi = LexGetToken(P, NULL, FALSE);
            IrFreeExpr(Ex);
            if (PeekSemi == TokenIdentifier)
                ProgramFail(P, "identifier not expected here");
            IR_FAIL("expected ';' after expression statement");
        }
        LexGetToken(P, NULL, TRUE); /* consume ';' */
        S = IrAllocStmt(P->pc, IrStExpr);
        if (S == NULL) IR_FAIL("OOM IrStExpr");
        S->Expr = Ex;
        return S;
    }
}

IrStmt *IrCompileBody(struct ParseState *BodyStart, struct ParseState *BodyEnd)
{
    struct ParseState P;
    IrStmt *Body;
    if (BodyEnd == NULL || BodyEnd->Pos <= BodyStart->Pos)
    {
        /* function body: BodyStart at '{', no End — compile until matched (already closed in tokens) */
        ParserCopy(&P, BodyStart);
        Body = IrCompileStmt(&P);
    }
    else
    {
        ParserCopy(&P, BodyStart);
        Body = IrCompileStmt(&P);
    }
    if (Body != NULL)
        IrAnnotateStmt(Body);
    return Body;
}


/* ---- Public compile API (parse.c / function definition) ---- */

void IrCompileSetParamContext(char **ParamNames, struct ValueType **ParamTypes, int NumParams)
{
    int li;
    IrCompileParamNames = ParamNames;
    IrCompileParamTypes = ParamTypes;
    IrCompileNumParams = NumParams;
    IrCompileNumLocals = 0;
    IrCompileScopeDepth = 0;
    IrCompileScopeStack[0] = 0;
    IrCompilingFunctionBody = 1;
    for (li = 0; li < IR_COMPILE_MAX_LOCALS; li++) {
        IrCompileLocalNames[li] = NULL;
        IrCompileLocalScale[li] = 0;
        IrCompileLocalElemPointee[li] = NULL;
        IrCompileLocalIsConst[li] = 0;
        IrCompileLocalKind[li] = 0;
        IrCompileLocalUnsigned[li] = 0;
        IrCompileLocalFromType[li] = NULL;
        IrCompileLocalCanonType[li] = NULL;
    }
}

void IrCompileSetReturnType(struct ValueType *RT) { IrCompileReturnType = RT; }

void IrCompileClearParamContext(void)
{
    IrCompileParamNames = NULL;
    IrCompileParamTypes = NULL;
    IrCompileNumParams = 0;
    IrCompileReturnType = NULL;
    IrCompileNumLocals = 0;
    IrCompilingFunctionBody = 0;
}

static void IrAnnotateExpr(IrExpr *E);
static void IrAnnotateStmt(IrStmt *S);
/* Register value width for an object of es bytes (1/2/8, else 32-bit). One
 * mapping for locals, members and parameters (was repeated inline). */
/* Id is T *a[N] (local, stamped at Decl), T **p / T *p[] parameter, or a
 * global array/pointer of T*: return T, the pointee of an element a[i]. */
static struct ValueType *IrCompileIdentElemPointee(IrExpr *Id)
{
    struct ValueType *T = NULL;
    if (Id == NULL || Id->Kind != IrExIdent)
        return NULL;
    if (Id->LocalSlot >= 0)
        return (Id->LocalSlot < IR_COMPILE_MAX_LOCALS)
            ? IrCompileLocalElemPointee[Id->LocalSlot] : NULL;
    if (Id->ParamSlot >= 0)
    {
        if (IrCompileParamTypes != NULL && Id->ParamSlot < IrCompileNumParams)
            T = IrCompileParamTypes[Id->ParamSlot];
    }
    else if (Id->CachedVal != NULL)
        T = Id->CachedVal->Typ;
    else if (Id->Ident != NULL && IrCompilePc != NULL)
    {
        /* Global T *g[N] before CachedVal is bound (annotate runs first). */
        struct Value *GV = NULL;
        if (TableGet(&IrCompilePc->GlobalTable, (char *)Id->Ident, &GV,
                     NULL, NULL, NULL) && GV != NULL)
            T = GV->Typ;
    }
    if (T == NULL || (T->Base != TypeArray && T->Base != TypePointer)
        || T->FromType == NULL || T->FromType->Base != TypePointer
        || T->FromType->FromType == NULL || T->FromType->FromType->Sizeof <= 0)
        return NULL;
    return T->FromType->FromType;
}

static inline unsigned char IrVwFromSize(int es)
{
    return (es == 1) ? IR_VW_I8 : (es == 2) ? IR_VW_I16
         : (es == 8) ? IR_VW_I64 : IR_VW_I32;
}
static void IrDaFunction(IrStmt *Body);


/* Resolve function return type by Ident name via GlobalTable (no CachedFunc). */
static struct ValueType *IrLookupFuncReturnType(const char *Name)
{
    struct Value *GV = NULL;
    if (Name == NULL || IrCompilePc == NULL)
        return NULL;
    if (!VariableDefined(IrCompilePc, (char *)Name))
        return NULL;
    if (!TableGet(&IrCompilePc->GlobalTable, (char *)Name, &GV, NULL, NULL, NULL)
        || GV == NULL || GV->Typ == NULL || GV->Typ->Base != TypeFunction
        || GV->Val == NULL)
        return NULL;
    return GV->Val->FuncDef.ReturnType;
}

static struct ValueType *IrCallReturnStruct(IrExpr *Call)
{
    struct ValueType *RT = NULL;
    struct Value *CF;
    if (Call == NULL)
        return NULL;
    CF = Call->CachedFunc;
    if (CF == NULL && Call->Left != NULL)
        CF = Call->Left->CachedFunc;
    if (CF != NULL && CF->Val != NULL)
        RT = CF->Val->FuncDef.ReturnType;
    if (RT == NULL && Call->Left != NULL && Call->Left->Kind == IrExIdent)
        RT = IrLookupFuncReturnType(Call->Left->Ident);
    if (RT == NULL && Call->Ident != NULL)
        RT = IrLookupFuncReturnType(Call->Ident);
    if (RT == NULL)
        return NULL;
    if (RT->Base == TypePointer && RT->FromType != NULL)
        return RT->FromType;
    return RT;
}

static void IrAnnotateExprBody(IrExpr *E);

static struct ValueType *IrCanonicalArrayIdentType(IrExpr *E)
{
    if (E == NULL || E->Kind != IrExIdent || E->LocalSlot < 0
        || E->LocalSlot >= IR_COMPILE_MAX_LOCALS)
        return NULL;
    return IrCompileLocalArrayType[E->LocalSlot];
}

static int IrCanonicalIsLoweredArrayIdent(IrExpr *E)
{
    return E != NULL && E->Kind == IrExIdent && E->LocalSlot >= 0
        && E->LocalSlot < IR_COMPILE_MAX_LOCALS
        && IrCompileLocalArraySize[E->LocalSlot] > 0
        && IrCompileLocalArrayType[E->LocalSlot] == NULL;
}

static struct ValueType *IrCompileDeclArrayType(IrStmt *S)
{
    struct ValueType *T;
    int d;
    if (S == NULL || IrCompilePc == NULL || S->DeclNDims <= 0
        || S->DeclNDims > IR_MAX_ARRAY_DIMS || S->DeclFromType == NULL)
        return NULL;
    T = S->DeclFromType;
    if (S->DeclElemIsPtr)
    {
        if (S->DeclElemPointee == NULL) return NULL;
        T = TypeGetMatching(IrCompilePc, NULL, S->DeclElemPointee,
                            TypePointer, 0, IrCompilePc->StrEmpty, TRUE);
    }
    for (d = S->DeclNDims - 1; d >= 0; --d)
    {
        int n = S->DeclDims[d];
        if (n <= 0) return NULL; /* VLA/incomplete dimension: don't guess */
        T = TypeGetMatching(IrCompilePc, NULL, T, TypeArray, n,
                            IrCompilePc->StrEmpty, TRUE);
        if (T == NULL) return NULL;
    }
    return T;
}

static struct ValueType *IrCanonicalExprValueType(IrExpr *E, int Depth)
{
    struct ValueType *T = NULL;
    struct Value *V = NULL;
    if (E == NULL || Depth > 48) return NULL;
    switch (E->Kind)
    {
    case IrExIdent:
        if (E->LocalSlot >= 0 && E->LocalSlot < IR_COMPILE_MAX_LOCALS)
            T = IrCompileLocalCanonType[E->LocalSlot] != NULL
                ? IrCompileLocalCanonType[E->LocalSlot]
                : IrCompileLocalFromType[E->LocalSlot];
        else if (E->ParamSlot >= 0 && IrCompileParamTypes != NULL
                 && E->ParamSlot < IrCompileNumParams)
            T = IrCompileParamTypes[E->ParamSlot];
        else if (E->CachedVal != NULL)
            T = E->CachedVal->Typ;
        else if (E->Ident != NULL && IrCompilePc != NULL
                 && TableGet(&IrCompilePc->GlobalTable, E->Ident, &V, NULL, NULL, NULL)
                 && V != NULL)
            T = V->Typ;
        return T;
    case IrExMember:
        T = IrCanonicalExprValueType(E->Left, Depth + 1);
        if (T != NULL && E->Op == TokenArrow && T->Base == TypePointer)
            T = T->FromType;
        if (T != NULL && (T->Base == TypeArray || T->Base == TypePointer))
            T = T->FromType;
        if (T != NULL && (T->Base == TypeStruct || T->Base == TypeUnion)
            && T->Members != NULL && E->Ident != NULL
            && TableGet(T->Members, E->Ident, &V, NULL, NULL, NULL) && V != NULL)
            return V->Typ;
        return NULL;
    case IrExIndex:
        T = IrCanonicalArrayIdentType(E->Left);
        if (T == NULL && E->Left != NULL && !IrCanonicalIsLoweredArrayIdent(E->Left))
            T = IrCanonicalExprValueType(E->Left, Depth + 1);
        if (T != NULL && (T->Base == TypeArray || T->Base == TypePointer))
            return T->FromType;
        /* PointeeType describes the pointed-to base for pointer-element
         * indexing; it is not the value type of a[i] itself. */
        if (E->PointeeType != NULL) return NULL;
        if (T != NULL && E->Left != NULL && (E->Left->TypeFlags & IR_TF_PTR)) return T;
        return NULL;
    case IrExUnary:
        if (E->Op == TokenCast) return NULL; /* Base is carried in I32. */
        if (E->Op == TokenAsterisk)
        {
            /* &object dereference retains the exact declared member type. */
            if (E->Left != NULL && E->Left->Kind == IrExUnary
                && E->Left->Op == TokenAmpersand)
                return IrCanonicalExprValueType(E->Left->Left, Depth + 1);
            if (E->Left != NULL && E->Left->Kind == IrExBinOp
                && (E->Left->Op == TokenPlus || E->Left->Op == TokenMinus))
            {
                IrExpr *P = (E->Left->Left != NULL && (E->Left->Left->TypeFlags & IR_TF_PTR))
                    ? E->Left->Left : E->Left->Right;
                T = IrCanonicalExprValueType(P, Depth + 1);
                if (T != NULL && (T->Base == TypePointer || T->Base == TypeArray)) return T->FromType;
                if (T != NULL && P != NULL && (P->TypeFlags & IR_TF_PTR)) return T;
            }
            T = IrCanonicalArrayIdentType(E->Left);
            if (T != NULL && (T->Base == TypePointer || T->Base == TypeArray)) return T->FromType;
            T = IrCanonicalExprValueType(E->Left, Depth + 1);
            if (T != NULL && (T->Base == TypePointer || T->Base == TypeArray)) return T->FromType;
            if (T != NULL && E->Left != NULL && (E->Left->TypeFlags & IR_TF_PTR)) return T;
        }
        if (E->Op == TokenAmpersand)
            return IrCanonicalExprValueType(E->Left, Depth + 1);
        return IrCanonicalExprValueType(E->Left, Depth + 1);
    case IrExVaArg:
        return E->VaType;
    case IrExCall:
        if (E->CachedFunc != NULL && E->CachedFunc->Val != NULL
            && E->CachedFunc->Val->FuncDef.ReturnType != NULL)
            return E->CachedFunc->Val->FuncDef.ReturnType;
        T = IrCanonicalExprValueType(E->Left, Depth + 1);
        if (T != NULL && T->Base == TypePointer) T = T->FromType;
        if (T != NULL && T->Base == TypeFunction) return T->FromType;
        if (E->Ident != NULL) return IrLookupFuncReturnType(E->Ident);
        if (E->Left != NULL && E->Left->Kind == IrExIdent)
            return IrLookupFuncReturnType(E->Left->Ident);
        return NULL;
    case IrExAssign:
        return IrCanonicalExprValueType(E->Left, Depth + 1);
    case IrExTernary:
        if (E->Right != NULL)
        {
            T = IrCanonicalExprValueType(E->Right, Depth + 1);
            if (T != NULL && (T->Base == TypePointer || T->Base == TypeArray)) return T;
        }
        if (E->Args != NULL && E->ArgCount > 0)
        {
            T = IrCanonicalExprValueType(E->Args[0], Depth + 1);
            if (T != NULL && (T->Base == TypePointer || T->Base == TypeArray)) return T;
        }
        return NULL;
    case IrExBinOp:
        if (E->Op == TokenComma) return IrCanonicalExprValueType(E->Right, Depth + 1);
        T = IrCanonicalExprValueType(E->Left, Depth + 1);
        if (T != NULL && (T->Base == TypePointer || T->Base == TypeArray)) return T;
        T = IrCanonicalExprValueType(E->Right, Depth + 1);
        if (T != NULL && (T->Base == TypePointer || T->Base == TypeArray)) return T;
        return NULL;
    default:
        return NULL;
    }
}

static enum BaseType IrCanonicalLeafBase(IrExpr *E)
{
    struct ValueType *T;
    struct Value *V = NULL;
    if (E == NULL) return TypeVoid;
    if (E->CanonicalFlags & IR_CF_VALID) return (enum BaseType)E->CanonicalBase;
    if (E->Kind == IrExUnary && E->Op == TokenCast)
        return (enum BaseType)E->I32;
    if (E->Kind == IrExConst)
    {
        if (E->Op == TokenFPConstant) return TypeFP;
        if (E->Op == TokenStringConstant) return TypePointer;
        if (E->I32 == TypeEnum) return TypeInt;
        /* Integer literals keep the C type chosen by the lexer from their
         * value and suffix (C11 6.4.4.1): 100000L is long, not long long. */
        if (E->I32 == TypeInt || E->I32 == TypeUnsignedInt
            || E->I32 == TypeLong || E->I32 == TypeUnsignedLong
            || E->I32 == TypeLongLong || E->I32 == TypeUnsignedLongLong)
            return (enum BaseType)E->I32;
        if (E->TypeFlags & IR_TF_UNSIGNED)
            return E->ValWidth >= IR_VW_I64 ? TypeUnsignedLongLong : TypeUnsignedInt;
        return E->ValWidth >= IR_VW_I64 ? TypeLongLong : TypeInt;
    }
    T = IrCanonicalExprValueType(E, 0);
    if (T == NULL && E->Kind == IrExIndex && E->PointeeType != NULL)
        return TypePointer;
    /* T (*a[N])(...): a pointer-kind local array slot holds function
     * pointers, so each element is a pointer even without a ValueType. */
    if (T == NULL && E->Kind == IrExIndex && E->Left != NULL
        && E->Left->Kind == IrExIdent && E->Left->LocalSlot >= 0
        && E->Left->LocalSlot < IR_COMPILE_MAX_LOCALS
        && IrCompileLocalKind[E->Left->LocalSlot] == 2
        && IrCompileLocalArraySize[E->Left->LocalSlot] > 0)
        return TypePointer;
    if (T != NULL)
    {
        /* Struct/union objects are lowered to an address-carrying slot
         * (IR_TF_PTR), but the C expression still has the aggregate type.
         * Only a pointer-kind local slot records its pointee aggregate as
         * FromType; every other source of T is the exact declared type. */
        if ((T->Base == TypeStruct || T->Base == TypeUnion)
            && !(E->Kind == IrExIdent && E->LocalSlot >= 0
                 && E->LocalSlot < IR_COMPILE_MAX_LOCALS
                 && IrCompileLocalKind[E->LocalSlot] == 2))
            return T->Base;
        /* In the lowered local-slot model, arrays and some pointer locals
         * retain the element/pointee ValueType while TypeFlags records decay. */
        if ((E->TypeFlags & IR_TF_PTR) && T->Base != TypePointer
            && T->Base != TypeArray && T->Base != TypeFunction)
            return TypePointer;
        return T->Base;
    }
    if (E->Kind == IrExIdent && E->Ident != NULL && IrCompilePc != NULL
        && TableGet(&IrCompilePc->GlobalTable, E->Ident, &V, NULL, NULL, NULL)
        && V != NULL && V->Typ != NULL) return V->Typ->Base;
    if (E->Kind == IrExMember || E->Kind == IrExIndex)
    {
        if (E->I32 >= TypeVoid && E->I32 <= TypeFunction)
            return (enum BaseType)E->I32;
    }
    if (E->TypeFlags & IR_TF_FP) return TypeFP;
    if (E->TypeFlags & IR_TF_PTR) return TypePointer;
    if (E->TypeFlags & IR_TF_INT)
        return (E->TypeFlags & IR_TF_UNSIGNED)
            ? (E->ValWidth >= IR_VW_I64 ? TypeUnsignedLongLong : TypeUnsignedInt)
            : (E->ValWidth >= IR_VW_I64 ? TypeLongLong : TypeInt);
    return TypeVoid;
}

static enum BaseType IrCanonicalPromotedBase(IrExpr *E)
{
    struct ValueType *ST = NULL;
    struct Value *MV = NULL;
    enum BaseType B = IrCanonicalLeafBase(E);
    int Width = 0, Unsigned = 0;
    if (!(B == TypeChar || B == TypeUnsignedChar || B == TypeShort
          || B == TypeUnsignedShort || B == TypeInt || B == TypeUnsignedInt
          || B == TypeLong || B == TypeUnsignedLong || B == TypeLongLong
          || B == TypeUnsignedLongLong || B == TypeEnum)) return B;
    if (E != NULL && E->Kind == IrExMember && E->Ident != NULL)
    {
        ST = IrCanonicalExprValueType(E->Left, 0);
        if (ST != NULL && E->Op == TokenArrow && ST->Base == TypePointer)
            ST = ST->FromType;
        if (ST != NULL && (ST->Base == TypeStruct || ST->Base == TypeUnion)
            && ST->Members != NULL
            && TableGet(ST->Members, E->Ident, &MV, NULL, NULL, NULL)
            && MV != NULL && MV->BitWidth > 0 && MV->Typ != NULL)
        {
            Width = MV->BitWidth;
            Unsigned = IrTypeIsUnsignedBase(MV->Typ->Base);
            if (!Unsigned || Width < BABYC_VALUE_BITS_INT) return TypeInt;
            return TypeUnsignedInt;
        }
    }
    return IrIntegerPromotionBase(B);
}

/* Exact aggregate type of an expression whose canonical base is struct/union.
 * Children are annotated first, so value-forwarding operators reuse the
 * operand's resolved identity; leaves use the declaration-derived type. */
static struct ValueType *IrCanonicalAggregateType(IrExpr *E, enum BaseType B)
{
    struct ValueType *T = NULL;
    if (E == NULL || (B != TypeStruct && B != TypeUnion)) return NULL;
    if (E->Kind == IrExAssign)
        T = E->Left != NULL ? E->Left->CanonicalType : NULL;
    else if (E->Kind == IrExBinOp && E->Op == TokenComma)
        T = E->Right != NULL ? E->Right->CanonicalType : NULL;
    else if (E->Kind == IrExTernary)
    {
        struct ValueType *A = E->Right != NULL ? E->Right->CanonicalType : NULL;
        struct ValueType *C = (E->Args != NULL && E->ArgCount > 0 && E->Args[0] != NULL)
            ? E->Args[0]->CanonicalType : NULL;
        T = (A == C) ? A : NULL; /* C11 6.5.15p3: both arms have the same type */
    }
    else
        T = IrCanonicalExprValueType(E, 0);
    return (T != NULL && T->Base == B) ? T : NULL;
}

static void IrAnnotateExpr(IrExpr *E)
{
    int i;
    enum BaseType B = TypeVoid, L, R;
    if (E == NULL) return;
    if ((E->CanonicalFlags & IR_CF_VALID)
        && (E->CanonicalOpFlags & IR_CF_VALID)) return;
    IrAnnotateExpr(E->Left);
    IrAnnotateExpr(E->Right);
    for (i = 0; E->Args != NULL && i < E->ArgCount; ++i)
        IrAnnotateExpr(E->Args[i]);
    IrAnnotateExprBody(E);

    /* Single canonical type assignment after source declarations, bitfield
     * metadata, casts, call signatures and child types have been annotated. */
    L = IrCanonicalLeafBase(E->Left);
    R = IrCanonicalLeafBase(E->Right);
    switch (E->Kind)
    {
    case IrExConst:
    case IrExIdent:
    case IrExMember:
    case IrExIndex:
        B = IrCanonicalLeafBase(E);
        break;
    case IrExVaArg:
        B = (E->VaType != NULL) ? E->VaType->Base : TypeInt;
        break;
    case IrExCall:
        if (E->CachedFunc != NULL && E->CachedFunc->Typ != NULL
            && E->CachedFunc->Typ->Base == TypeFunction
            && E->CachedFunc->Val != NULL
            && E->CachedFunc->Val->FuncDef.ReturnType != NULL)
            B = E->CachedFunc->Val->FuncDef.ReturnType->Base;
        else if (E->Left != NULL && E->Left->CachedFunc != NULL
            && E->Left->CachedFunc->Val != NULL
            && E->Left->CachedFunc->Val->FuncDef.ReturnType != NULL)
            B = E->Left->CachedFunc->Val->FuncDef.ReturnType->Base;
        else if (E->Ident != NULL)
        {
            struct ValueType *RT = IrLookupFuncReturnType(E->Ident);
            if (RT != NULL) B = RT->Base;
        }
        else if (E->Left != NULL && E->Left->Kind == IrExIdent && E->Left->Ident != NULL)
        {
            struct ValueType *RT = IrLookupFuncReturnType(E->Left->Ident);
            if (RT != NULL) B = RT->Base;
        }
        break;
    case IrExAssign:
        B = L;
        break;
    case IrExTernary:
        /* The condition does not participate in the conditional operator's
         * result type: only the true and false arms undergo common conversion. */
        L = IrCanonicalLeafBase(E->Right);
        R = IrCanonicalLeafBase(E->Args != NULL && E->ArgCount > 0 ? E->Args[0] : NULL);
        if (L == TypeFP || R == TypeFP) B = TypeFP;
        else if (L == TypePointer || R == TypePointer || L == TypeArray || R == TypeArray)
            B = TypePointer;
        else if (L == R && (L == TypeStruct || L == TypeUnion)) B = L;
        else B = IrUsualArithmeticCommonIntegerBase(IrCanonicalPromotedBase(E->Right),
                    IrCanonicalPromotedBase(E->Args != NULL && E->ArgCount > 0 ? E->Args[0] : NULL));
        break;
    case IrExBinOp:
        if (E->Op == TokenComma) B = R;
        else if (E->Op == TokenLogicalAnd || E->Op == TokenLogicalOr
            || E->Op == TokenEqual || E->Op == TokenNotEqual
            || E->Op == TokenLessThan || E->Op == TokenGreaterThan
            || E->Op == TokenLessEqual || E->Op == TokenGreaterEqual)
            B = TypeInt;
        else if (E->Op == TokenShiftLeft || E->Op == TokenShiftRight)
            B = IrIntegerPromotionBase(L);
        else if (L == TypeFP || R == TypeFP) B = TypeFP;
        else if (L == TypePointer && (E->Op == TokenPlus || E->Op == TokenMinus)) B = TypePointer;
        else B = IrUsualArithmeticCommonIntegerBase(L, R);
        break;
    case IrExUnary:
        if (E->Op == TokenCast) B = (enum BaseType)E->I32;
        else if (E->Op == TokenUnaryNot) B = TypeInt;
        else if (E->Op == TokenAmpersand) B = TypePointer;
        else if (E->Op == TokenAsterisk) B = IrCanonicalLeafBase(E);
        else B = L;
        break;
    default:
        B = TypeVoid;
        break;
    }
    E->CanonicalBase = (int)B;
    E->CanonicalType = IrCanonicalAggregateType(E, B);
    E->CanonicalFlags = IR_CF_VALID;
    if (B == TypeFP) E->CanonicalFlags |= IR_CF_FP;
    if (B == TypePointer || B == TypeArray || B == TypeFunction) E->CanonicalFlags |= IR_CF_PTR;
    if (IrTypeIsUnsignedBase(B)) E->CanonicalFlags |= IR_CF_UNSIGNED;
    if (((B == TypeLong || B == TypeUnsignedLong)
            && BABYC_VALUE_BITS_LONG > BABYC_VALUE_BITS_INT)
        || ((B == TypeLongLong || B == TypeUnsignedLongLong)
            && BABYC_VALUE_BITS_LLONG > BABYC_VALUE_BITS_INT))
        E->CanonicalFlags |= IR_CF_WIDE;

    /* Canonical operation type is resolved at the same lowering point. It
     * records the promoted/common operand type separately from the expression
     * result type (notably for comparisons, assignments and shifts). */
    {
        enum BaseType OB = B;
        enum BaseType PL = IrCanonicalPromotedBase(E->Left);
        enum BaseType PR = IrCanonicalPromotedBase(E->Right);
        if (E->Kind == IrExBinOp)
        {
            if (E->Op == TokenComma) OB = R;
            else if (E->Op == TokenShiftLeft || E->Op == TokenShiftRight)
                OB = PL;
            else if (E->Op == TokenLogicalAnd || E->Op == TokenLogicalOr
                || E->Op == TokenEqual || E->Op == TokenNotEqual
                || E->Op == TokenLessThan || E->Op == TokenGreaterThan
                || E->Op == TokenLessEqual || E->Op == TokenGreaterEqual)
                OB = (L == TypeFP || R == TypeFP) ? TypeFP : IrUsualArithmeticCommonIntegerBase(PL, PR);
            else if (L == TypeFP || R == TypeFP) OB = TypeFP;
            else if (L == TypePointer || R == TypePointer) OB = TypePointer;
            else OB = IrUsualArithmeticCommonIntegerBase(PL, PR);
        }
        else if (E->Kind == IrExTernary && E->ArgCount > 0 && E->Args != NULL)
        {
            enum BaseType TL = IrCanonicalLeafBase(E->Right);
            enum BaseType TR = IrCanonicalLeafBase(E->Args[0]);
            enum BaseType TPL = IrCanonicalPromotedBase(E->Right);
            enum BaseType TPR = IrCanonicalPromotedBase(E->Args[0]);
            OB = (TL == TypeFP || TR == TypeFP) ? TypeFP
               : (TL == TypePointer || TR == TypePointer || TL == TypeArray || TR == TypeArray) ? TypePointer
               : IrUsualArithmeticCommonIntegerBase(TPL, TPR);
        }
        else if (E->Kind == IrExAssign && E->Op != TokenAssign)
        {
            if (E->Op == TokenShiftLeftAssign || E->Op == TokenShiftRightAssign)
                OB = PL;
            else
                OB = (L == TypeFP || R == TypeFP) ? TypeFP
                   : (L == TypePointer || R == TypePointer) ? TypePointer
                   : IrUsualArithmeticCommonIntegerBase(PL, PR);
        }
        else if (E->Kind == IrExUnary && E->Op != TokenCast && E->Op != TokenAmpersand
                 && E->Op != TokenAsterisk)
            OB = PL;
        E->CanonicalOpBase = (int)OB;
        E->CanonicalOpFlags = (unsigned char)(IR_CF_VALID
            | (OB == TypeFP ? IR_CF_FP : 0)
            | ((OB == TypePointer || OB == TypeArray || OB == TypeFunction) ? IR_CF_PTR : 0)
            | ((IrTypeIsUnsignedBase(OB) || OB == TypePointer) ? IR_CF_UNSIGNED : 0)
            | ((OB == TypePointer && BABYC_VALUE_BITS_POINTER > BABYC_VALUE_BITS_INT) ? IR_CF_WIDE : 0)
            | ((OB == TypeLong || OB == TypeUnsignedLong || OB == TypeLongLong
                || OB == TypeUnsignedLongLong) && BABYC_VALUE_BITS_LONG > BABYC_VALUE_BITS_INT
                ? IR_CF_WIDE : 0)
            | ((OB == TypeLongLong || OB == TypeUnsignedLongLong)
                && BABYC_VALUE_BITS_LLONG > BABYC_VALUE_BITS_INT ? IR_CF_WIDE : 0));
    }
}

static void IrAnnotateExprBody(IrExpr *E)
{
    int i;
    int child_all = 1;
    int child_fp = 0;
    if (E == NULL)
        return;
    /* Children have already been annotated by the canonical wrapper. */

    /* Narrow local ++/-- need the declared base type in the emitted AddLocal
     * instruction. Keep this metadata work scoped to increment/decrement nodes
     * rather than adding checks to every local identifier in hot programs. */
    if (E->Kind == IrExUnary
        && (E->Op == TokenIncrement || E->Op == TokenDecrement)
        && E->Left != NULL && E->Left->Kind == IrExIdent
        && E->Left->LocalSlot >= 0 && E->Left->LocalSlot < IR_COMPILE_MAX_LOCALS)
    {
        int slot = E->Left->LocalSlot;
        struct ValueType *T = IrCompileLocalFromType[slot];
        unsigned char k = IrCompileLocalKind[slot];
        if ((k == 1 || k == 6) && T != NULL
            && (T->Base == TypeChar || T->Base == TypeUnsignedChar
                || T->Base == TypeShort || T->Base == TypeUnsignedShort))
            E->Left->I32 = (int)T->Base;
    }

    /* Conservative: only stamp facts we can prove. Never mark a node
     * resolved-not-FP unless it cannot yield a double (integer/char const). */
    if (E->Kind == IrExConst)
    {
        if (E->Op == TokenFPConstant)
        {
            E->TypeFlags = IR_TF_RESOLVED | IR_TF_FP;
            E->ValWidth = IR_VW_I64; /* double bits in long long */
        }
        else if (E->Op == TokenStringConstant)
        {
            /* string literal decays to char* */
            E->TypeFlags = IR_TF_PTR | IR_TF_RESOLVED;
            E->ElemSize = 1;
            E->ValWidth = (unsigned char)sizeof(void *);
        }
        else
        {
            /* IntegerConstant, CharacterConstant, sizeof/offsetof synthetic consts */
            E->TypeFlags = IR_TF_RESOLVED | IR_TF_INT;
            if (E->Op == TokenCharacterConstant)
                E->ValWidth = IR_VW_I8;
            else if (E->I32 == TypeUnsignedChar)
            {
                E->TypeFlags |= IR_TF_UNSIGNED;
                E->ValWidth = IR_VW_I8;
            }
            else if (E->I32 == TypeUnsignedShort)
            {
                E->TypeFlags |= IR_TF_UNSIGNED;
                E->ValWidth = IR_VW_I16;
            }
            else if (E->I32 == TypeUnsignedInt)
            {
                E->TypeFlags |= IR_TF_UNSIGNED;
                E->ValWidth = IR_VW_I32;
            }
            else if (E->I32 == TypeUnsignedLong || E->I32 == TypeUnsignedLongLong)
            {
                E->TypeFlags |= IR_TF_UNSIGNED;
                E->ValWidth = IR_VW_I64;
            }
            else if (E->I32 == TypeLong || E->I32 == TypeLongLong)
                E->ValWidth = IR_VW_I64;
            else
                E->ValWidth = IR_VW_I32;
        }
        return;
    }
    if (E->Kind == IrExIdent)
    {
        if (E->I32 == TypeFP)
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
        if (E->LocalSlot >= 0 && E->LocalSlot < IR_COMPILE_MAX_LOCALS)
        {
            unsigned char k = IrCompileLocalKind[E->LocalSlot];
            if (IrCompileLocalScale[E->LocalSlot] > 0)
            {
                E->ElemSize = IrCompileLocalScale[E->LocalSlot];
                E->TypeFlags |= IR_TF_PTR;
            }
            if (IrCompileLocalIsConst[E->LocalSlot])
                E->TypeFlags |= IR_TF_CONST;
            if (k == 1 || k == 6)
            {
                E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                if (k == 6 || IrCompileLocalUnsigned[E->LocalSlot])
                    E->TypeFlags |= IR_TF_UNSIGNED;
                {
                    int es = IrCompileLocalScale[E->LocalSlot];
                    E->ValWidth = IrVwFromSize((int)es);
                }
            }
            else if (k == 4)
            {
                /* int/uchar array base — resolved; scale = element size */
                E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                if (IrCompileLocalUnsigned[E->LocalSlot])
                    E->TypeFlags |= IR_TF_UNSIGNED;
                {
                    int es = IrCompileLocalScale[E->LocalSlot];
                    if (es <= 0)
                        es = IrCompileLocalElemSize[E->LocalSlot];
                    E->ValWidth = IrVwFromSize((int)es);
                    if (E->ElemSize <= 0 && es > 0)
                        E->ElemSize = es;
                }
            }
            else if (k == 3)
            {
                E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
                E->ValWidth = IR_VW_I64;
            }
            else if (k == 2)
            {
                /* int *p — always PTR; ElemSize = pointee scale (default int) */
                E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
                E->ValWidth = (unsigned char)sizeof(void *);
                if (E->ElemSize <= 0)
                {
                    int es = IrCompileLocalScale[E->LocalSlot];
                    if (es <= 0)
                        es = IrCompileLocalElemSize[E->LocalSlot];
                    if (es <= 0 && IrCompileLocalFromType[E->LocalSlot] != NULL
                        && IrCompileLocalFromType[E->LocalSlot]->Base == TypePointer
                        && IrCompileLocalFromType[E->LocalSlot]->FromType != NULL
                        && IrCompileLocalFromType[E->LocalSlot]->FromType->Sizeof > 0)
                        es = (int)IrCompileLocalFromType[E->LocalSlot]->FromType->Sizeof;
                    if (es <= 0)
                        es = (int)sizeof(int);
                    E->ElemSize = es;
                }
            }
            else if (k == 4 && IrCompileLocalElemSize[E->LocalSlot] > 0)
            {
                /* array base decay: element size for a[i] */
                if (E->ElemSize <= 0)
                    E->ElemSize = IrCompileLocalElemSize[E->LocalSlot];
            }
            /* local struct/union aggregate */
            if (!(E->TypeFlags & (IR_TF_RESOLVED | IR_TF_FP | IR_TF_PTR | IR_TF_INT))
                && IrCompileLocalFromType[E->LocalSlot] != NULL)
            {
                struct ValueType *FT = IrCompileLocalFromType[E->LocalSlot];
                if (FT->Base == TypeStruct || FT->Base == TypeUnion)
                    E->TypeFlags |= IR_TF_RESOLVED;
                else if (FT->Base == TypePointer)
                {
                    E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
                    if (FT->FromType != NULL && FT->FromType->Sizeof > 0)
                        E->ElemSize = (int)FT->FromType->Sizeof;
                }
                else if (FT->Base == TypeArray && FT->FromType != NULL)
                {
                    E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
                    if (FT->FromType->Sizeof > 0)
                        E->ElemSize = (int)FT->FromType->Sizeof;
                }
            }
        }
        else if (E->ParamSlot >= 0)
        {
            /* F2: prefer ParamType from compile context; fall back to I32.
             * Never invent INT when type is FP (printf %f). */
            struct ValueType *PT = NULL;
            if (IrCompileParamTypes != NULL
                && E->ParamSlot < IrCompileNumParams)
                PT = IrCompileParamTypes[E->ParamSlot];
            if (PT != NULL)
            {
                if (PT->Base == TypeFP)
                    E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
                else if (PT->Base == TypePointer || PT->Base == TypeArray)
                {
                    E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
                    if (PT->FromType != NULL && PT->FromType->Sizeof > 0)
                        E->ElemSize = (int)PT->FromType->Sizeof;
                }
                else if (PT->Base == TypeInt || PT->Base == TypeChar
                         || PT->Base == TypeShort || PT->Base == TypeLong
                         || PT->Base == TypeLongLong
                         || PT->Base == TypeUnsignedInt || PT->Base == TypeUnsignedChar
                         || PT->Base == TypeUnsignedShort || PT->Base == TypeUnsignedLong
                         || PT->Base == TypeUnsignedLongLong)
                {
                    E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                    if (PT->Base == TypeUnsignedInt || PT->Base == TypeUnsignedChar
                        || PT->Base == TypeUnsignedShort || PT->Base == TypeUnsignedLong
                        || PT->Base == TypeUnsignedLongLong)
                        E->TypeFlags |= IR_TF_UNSIGNED;
                    /* Width like a local of the same type: a long long
                     * parameter made `big % 1000` a 32-bit op in linear. */
                    E->ValWidth = IrVwFromSize((int)PT->Sizeof);
                }
                else if (PT->Base == TypeStruct || PT->Base == TypeUnion)
                    E->TypeFlags |= IR_TF_RESOLVED;
            }
            else if (E->I32 == TypeFP)
                E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
            else if (E->I32 == TypePointer)
                E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
            else if (E->I32 == TypeInt || E->I32 == TypeChar || E->I32 == TypeShort
                     || E->I32 == TypeLong || E->I32 == TypeLongLong
                     || E->I32 == TypeUnsignedInt || E->I32 == TypeUnsignedShort
                     || E->I32 == TypeUnsignedLong || E->I32 == TypeUnsignedLongLong
                     || E->I32 == TypeUnsignedChar)
            {
                E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                if (E->I32 == TypeUnsignedInt || E->I32 == TypeUnsignedShort
                    || E->I32 == TypeUnsignedLong || E->I32 == TypeUnsignedLongLong
                    || E->I32 == TypeUnsignedChar)
                    E->TypeFlags |= IR_TF_UNSIGNED;
            }
        }
        else
        {
            /* Global / unbound Ident: use I32 when present; else conservative INT
             * for plain C globals (float globals set I32=TypeFP at compile). */
            if (E->I32 == TypeFP)
                E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
            else if (E->I32 == TypePointer)
                E->TypeFlags |= IR_TF_PTR;
            else if (E->I32 == TypeInt || E->I32 == TypeChar || E->I32 == TypeShort
                     || E->I32 == TypeLong || E->I32 == TypeLongLong
                     || E->I32 == TypeUnsignedInt || E->I32 == TypeUnsignedShort
                     || E->I32 == TypeUnsignedLong || E->I32 == TypeUnsignedLongLong
                     || E->I32 == TypeUnsignedChar)
            {
                E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                if (E->I32 == TypeUnsignedInt || E->I32 == TypeUnsignedShort
                    || E->I32 == TypeUnsignedLong || E->I32 == TypeUnsignedLongLong
                    || E->I32 == TypeUnsignedChar)
                    E->TypeFlags |= IR_TF_UNSIGNED;
            }
            /* I32==0 / global: F2 bind from CachedVal when present */
            if (!(E->TypeFlags & (IR_TF_RESOLVED | IR_TF_FP | IR_TF_PTR | IR_TF_INT))
                && E->CachedVal != NULL && E->CachedVal->Typ != NULL)
            {
                struct ValueType *T = E->CachedVal->Typ;
                if (T->Base == TypeFP)
                    E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
                else if (T->Base == TypePointer || T->Base == TypeArray)
                {
                    E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
                    if (T->FromType != NULL)
                    {
                        int es = (T->FromType->Sizeof > 0 ? (int)T->FromType->Sizeof : (int)sizeof(int));
                        if (es > 0) E->ElemSize = es;
                        if (T->FromType->Base == TypeFP)
                            E->TypeFlags |= IR_TF_FP;
                        else if (T->FromType->Base == TypeInt
                                 || T->FromType->Base == TypeChar
                                 || T->FromType->Base == TypeShort
                                 || T->FromType->Base == TypeLong
                                 || T->FromType->Base == TypeLongLong
                                 || T->FromType->Base == TypeUnsignedInt
                                 || T->FromType->Base == TypeUnsignedChar
                                 || T->FromType->Base == TypeUnsignedShort
                                 || T->FromType->Base == TypeUnsignedLong
                                 || T->FromType->Base == TypeUnsignedLongLong)
                        {
                            /* array/pointer base stays PTR; element kind recorded via ElemSize */
                            if (T->FromType->Base == TypeUnsignedInt
                                || T->FromType->Base == TypeUnsignedChar
                                || T->FromType->Base == TypeUnsignedShort
                                || T->FromType->Base == TypeUnsignedLong
                                || T->FromType->Base == TypeUnsignedLongLong)
                                E->TypeFlags |= IR_TF_UNSIGNED;
                        }
                    }
                }
                else if (T->Base == TypeInt || T->Base == TypeChar || T->Base == TypeShort
                         || T->Base == TypeLong || T->Base == TypeLongLong
                         || T->Base == TypeUnsignedInt || T->Base == TypeUnsignedChar
                         || T->Base == TypeUnsignedShort || T->Base == TypeUnsignedLong
                         || T->Base == TypeUnsignedLongLong)
                {
                    E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                    if (T->Base == TypeUnsignedInt || T->Base == TypeUnsignedChar
                        || T->Base == TypeUnsignedShort || T->Base == TypeUnsignedLong
                        || T->Base == TypeUnsignedLongLong)
                        E->TypeFlags |= IR_TF_UNSIGNED;
                }
                else if (T->Base == TypeStruct || T->Base == TypeUnion)
                {
                    /* aggregate global — RESOLVED so Member can use CachedVal->Typ */
                    E->TypeFlags |= IR_TF_RESOLVED;
                }
                else if (T->Base == TypeFunction)
                {
                    E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
                }
            }
            /* Global without CachedVal: lookup via IrCompilePc */
            if (!(E->TypeFlags & (IR_TF_RESOLVED | IR_TF_FP | IR_TF_PTR | IR_TF_INT))
                && E->Ident != NULL && IrCompilePc != NULL)
            {
                struct Value *GV = NULL;
                if (VariableDefined(IrCompilePc, E->Ident)
                    && TableGet(&IrCompilePc->GlobalTable, E->Ident, &GV, NULL, NULL, NULL)
                    && GV != NULL && GV->Typ != NULL)
                {
                    struct ValueType *T = GV->Typ;
                    if (T->Base == TypeFunction)
                        E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
                    else if (T->Base == TypePointer || T->Base == TypeArray)
                    {
                        E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
                        if (T->FromType != NULL && T->FromType->Sizeof > 0)
                            E->ElemSize = (int)T->FromType->Sizeof;
                    }
                    else if (T->Base == TypeFP)
                        E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
                    else if (T->Base == TypeStruct || T->Base == TypeUnion)
                        E->TypeFlags |= IR_TF_RESOLVED;
                    else if (T->Base == TypeInt || T->Base == TypeChar
                             || T->Base == TypeShort || T->Base == TypeLong
                             || T->Base == TypeLongLong
                             || T->Base == TypeUnsignedInt || T->Base == TypeUnsignedChar
                             || T->Base == TypeUnsignedShort || T->Base == TypeUnsignedLong
                             || T->Base == TypeUnsignedLongLong)
                        E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                    else
                        E->TypeFlags |= IR_TF_RESOLVED;
                }
                else if (E->LocalSlot < 0 && E->ParamSlot < 0)
                {
                    /* Soft: unbound global Ident (e.g. array defined in same
                     * translation unit but bind miss) — RESOLVED only. */
                    E->TypeFlags |= IR_TF_RESOLVED;
                }
            }
            else if (E->LocalSlot < 0 && E->ParamSlot < 0 && E->Ident != NULL)
                E->TypeFlags |= IR_TF_RESOLVED;
        }
        return;
    }
    if (E->Kind == IrExMember)
    {
        struct ValueType *ST = NULL;
        struct Value *MemVal = NULL;
        struct ValueType *MT = NULL;
        IrExpr *Base = E->Left;

        if (E->I32 == TypeFP)
        {
            E->TypeFlags = IR_TF_RESOLVED | IR_TF_FP;
            return;
        }
        if (E->I32 == TypePointer)
        {
            E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
            return;
        }
        if (E->I32 == TypeInt || E->I32 == TypeChar || E->I32 == TypeShort
            || E->I32 == TypeLong || E->I32 == TypeLongLong
            || E->I32 == TypeUnsignedInt || E->I32 == TypeUnsignedShort
            || E->I32 == TypeUnsignedLong || E->I32 == TypeUnsignedLongLong
            || E->I32 == TypeUnsignedChar)
        {
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
            if (E->I32 == TypeUnsignedInt || E->I32 == TypeUnsignedShort
                || E->I32 == TypeUnsignedLong || E->I32 == TypeUnsignedLongLong
                || E->I32 == TypeUnsignedChar)
                E->TypeFlags |= IR_TF_UNSIGNED;
            return;
        }

        /* F2: walk Left chain from Ident root, tracking type through
         * Index/Member so nested Gpath.pts[i].x resolves on Pt not Path. */
        {
            IrExpr *Chain[16];
            int nch = 0;
            IrExpr *R = Base;
            while (R != NULL && nch < 16)
            {
                Chain[nch++] = R;
                if (R->Kind == IrExIndex || R->Kind == IrExMember)
                    R = R->Left;
                else if (R->Kind == IrExUnary
                         && (R->Op == TokenAsterisk || R->Op == TokenAmpersand))
                    R = R->Left;
                else
                    break;
            }
            /* R is Ident/Call root (or NULL) */
            if (R != NULL && R->Kind == IrExIdent)
            {
                if (R->LocalSlot >= 0 && R->LocalSlot < IR_COMPILE_MAX_LOCALS
                    && IrCompileLocalFromType[R->LocalSlot] != NULL)
                    ST = IrCompileLocalFromType[R->LocalSlot];
                else if (R->ParamSlot >= 0 && IrCompileParamTypes != NULL
                         && R->ParamSlot < IrCompileNumParams)
                    ST = IrCompileParamTypes[R->ParamSlot];
                else if (R->CachedVal != NULL && R->CachedVal->Typ != NULL)
                    ST = R->CachedVal->Typ;
            }
            else if (R != NULL && R->Kind == IrExCall)
                ST = IrCallReturnStruct(R);
            /* Call in chain: inject return pointee as struct base */
            {
                int ci;
                for (ci = nch - 1; ci >= 0 && ST == NULL; ci--)
                {
                    if (Chain[ci]->Kind == IrExCall)
                        ST = IrCallReturnStruct(Chain[ci]);
                }
            }
            /* apply chain from root toward Base (Chain is Base..near-root) */
            {
                int ci;
                for (ci = nch - 1; ci >= 0 && ST != NULL; ci--)
                {
                    IrExpr *N = Chain[ci];
                    if (N->Kind == IrExMember && N->Ident != NULL
                        && (ST->Base == TypeStruct || ST->Base == TypeUnion)
                        && ST->Members != NULL)
                    {
                        struct Value *MV = NULL;
                        if (TableGet(ST->Members, N->Ident, &MV, NULL, NULL, NULL)
                            && MV != NULL && MV->Typ != NULL)
                            ST = MV->Typ;
                        else
                            ST = NULL;
                    }
                    else if (N->Kind == IrExIndex)
                    {
                        if (ST->Base == TypeArray && ST->FromType != NULL)
                            ST = ST->FromType;
                        else if (ST->Base == TypePointer && ST->FromType != NULL)
                            ST = ST->FromType;
                    }
                    else if (N->Kind == IrExUnary && N->Op == TokenAsterisk)
                    {
                        if (ST->Base == TypePointer && ST->FromType != NULL)
                            ST = ST->FromType;
                    }
                    /* peel ptr/array wrappers between steps */
                    {
                        int peel;
                        for (peel = 0; ST != NULL && peel < 2; peel++)
                        {
                            if (ST->Base == TypePointer && ST->FromType != NULL
                                && (ST->FromType->Base == TypeStruct
                                    || ST->FromType->Base == TypeUnion
                                    || ST->FromType->Base == TypeArray))
                                ST = ST->FromType;
                            else
                                break;
                        }
                    }
                }
            }
        }
        /* ST is now type of Base; lookup E->Ident as member */
        if (ST != NULL && ST->Base == TypePointer && ST->FromType != NULL)
            ST = ST->FromType;
        if (ST != NULL && (ST->Base == TypeStruct || ST->Base == TypeUnion)
            && ST->Members != NULL && E->Ident != NULL
            && TableGet(ST->Members, E->Ident, &MemVal, NULL, NULL, NULL)
            && MemVal != NULL && MemVal->Typ != NULL)
        {
            MT = MemVal->Typ;
            /* array member: decay to element for Index chain; flag as PTR-ish */
            if (MT->Base == TypeArray && MT->FromType != NULL)
            {
                if (MT->FromType->Sizeof > 0)
                    E->ElemSize = (int)MT->FromType->Sizeof;
                if (MT->FromType->Base == TypeFP)
                    E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
                else
                {
                    E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                    if (MT->FromType->Base == TypeUnsignedInt
                        || MT->FromType->Base == TypeUnsignedChar
                        || MT->FromType->Base == TypeUnsignedShort
                        || MT->FromType->Base == TypeUnsignedLong
                        || MT->FromType->Base == TypeUnsignedLongLong)
                        E->TypeFlags |= IR_TF_UNSIGNED;
                }
                /* Keep array member addressable as aggregate for Index */
            }
            else if (MT->Base == TypeFP)
                E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
            else if (MT->Base == TypePointer)
            {
                E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
                if (MT->FromType != NULL && MT->FromType->Sizeof > 0)
                    E->ElemSize = (int)MT->FromType->Sizeof;
            }
            else if (MT->Base == TypeStruct || MT->Base == TypeUnion)
            {
                /* nested struct value — not a scalar; leave for runtime unless used as ptr */
                E->TypeFlags |= IR_TF_RESOLVED;
            }
            else
            {
                E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                if (MT->Base == TypeUnsignedInt || MT->Base == TypeUnsignedChar
                    || MT->Base == TypeUnsignedShort || MT->Base == TypeUnsignedLong
                    || MT->Base == TypeUnsignedLongLong)
                    E->TypeFlags |= IR_TF_UNSIGNED;
                E->ValWidth = IrVwFromSize((int)MT->Sizeof);
            }
        }
        return;
    }
    if (E->Kind == IrExIndex)
    {
        E->ElemSize = 0;
        /* T *a[N]: a[i] is a pointer with declared pointee T; a[i][j] has
         * type T. Stamped from the declaration so tree and linear address
         * a[i][j] by type (the tree used to assume char, 1-byte stride). */
        if (IrCompileIdentElemPointee(E->Left) != NULL)
        {
            /* Only the type; the row's other flags keep their historical
             * meaning (ElemSize == sizeof(void *) marks a pointer element). */
            E->PointeeType = IrCompileIdentElemPointee(E->Left);
        }
        else if (E->Left != NULL && E->Left->Kind == IrExIndex
                 && E->Left->PointeeType != NULL)
        {
            struct ValueType *T = E->Left->PointeeType;
            E->TypeFlags = IR_TF_RESOLVED;
            if (T->Base == TypeFP)
                E->TypeFlags |= IR_TF_FP;
            else if (T->Base == TypePointer)
                E->TypeFlags |= IR_TF_PTR;
            else if (IS_INTEGER_NUMERIC_TYPE(T))
            {
                E->TypeFlags |= IR_TF_INT;
                if (IrTypeIsUnsignedBase(T->Base))
                    E->TypeFlags |= IR_TF_UNSIGNED;
            }
            E->ValWidth = (T->Base == TypeFP || T->Base == TypePointer)
                ? IR_VW_I64 : IrVwFromSize((int)T->Sizeof);
            E->ElemSize = (T->Base == TypePointer && T->FromType != NULL
                           && T->FromType->Sizeof > 0) ? (int)T->FromType->Sizeof
                        : (T->Sizeof > 0 ? (int)T->Sizeof : 0);
            return;
        }
        if (E->I32 == TypeFP)
            E->TypeFlags = IR_TF_RESOLVED | IR_TF_FP;
        else
        {
            /* Element width from base local array or pointer scale. */
            IrExpr *Base = E->Left;
            struct ValueType *BT = NULL;
            while (Base != NULL && Base->Kind == IrExIndex)
                Base = Base->Left;
            /* Resolve FP element type independently of ElemSize: that size can
             * already be populated while the expression still defaults to INT. */
            if (Base != NULL && Base->Kind == IrExIdent)
            {
                if (Base->LocalSlot >= 0 && Base->LocalSlot < IR_COMPILE_MAX_LOCALS)
                    BT = IrCompileLocalFromType[Base->LocalSlot];
                else if (Base->ParamSlot >= 0 && IrCompileParamTypes != NULL
                         && Base->ParamSlot < IrCompileNumParams)
                    BT = IrCompileParamTypes[Base->ParamSlot];
                else if (Base->CachedVal != NULL)
                    BT = Base->CachedVal->Typ;
                if (BT != NULL && (BT->Base == TypePointer || BT->Base == TypeArray)
                    && BT->FromType != NULL && BT->FromType->Base == TypeFP)
                {
                    E->TypeFlags = IR_TF_RESOLVED | IR_TF_FP;
                    if (BT->FromType->Sizeof > 0)
                        E->ElemSize = (int)BT->FromType->Sizeof;
                    else
                        E->ElemSize = (int)sizeof(double);
                    E->ValWidth = IR_VW_I64;
                }
                /* double *q (local pointer, DeclFromType = pointee FP):
                 * q[i] is a double. Missing before: q[1] printed the IEEE
                 * bits as an integer. Multi-level double **pp carries a
                 * pointer DeclFromType and does not match. */
                else if (BT != NULL && BT->Base == TypeFP && E->Left == Base
                         && Base->LocalSlot >= 0
                         && Base->LocalSlot < IR_COMPILE_MAX_LOCALS
                         && IrCompileLocalKind[Base->LocalSlot] == 2
                         && IrCompileLocalScale[Base->LocalSlot] == (int)sizeof(double))
                {
                    E->TypeFlags = IR_TF_RESOLVED | IR_TF_FP;
                    E->ElemSize = (int)sizeof(double);
                    E->ValWidth = IR_VW_I64;
                }
            }
            if (!(E->TypeFlags & IR_TF_FP) && Base != NULL && Base->Kind == IrExIdent
                && Base->LocalSlot >= 0
                && Base->LocalSlot < IR_COMPILE_MAX_LOCALS)
            {
                int k = IrCompileLocalKind[Base->LocalSlot];
                /* Scale holds element/pointee size; ElemSize[] may be total bytes. */
                int es = IrCompileLocalScale[Base->LocalSlot];
                if (es <= 0)
                    es = (k == 1 || k == 4) ? (int)sizeof(int) : 0;
                if (k == 5)
                {
                    E->ElemSize = es > 0 ? es : (int)sizeof(double);
                    E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
                }
                else if (k == 4 && es > 0)
                {
                    E->ElemSize = es;
                    E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                    if (IrCompileLocalUnsigned[Base->LocalSlot])
                        E->TypeFlags |= IR_TF_UNSIGNED;
                    if (es == 1)
                        E->ValWidth = IR_VW_I8;
                    else if (es == 2)
                        E->ValWidth = IR_VW_I16;
                    else if (es == 8)
                        E->ValWidth = IR_VW_I64;
                    else
                        E->ValWidth = IR_VW_I32;
                }
                else if (k == 2 && es > 0)
                {
                    /* int *p; p[i] → INT (load). int **pp; pp[i] → PTR.
                     * Pointee pointer ⇒ keep PTR; scalar pointee ⇒ INT. */
                    struct ValueType *FT = IrCompileLocalFromType[Base->LocalSlot];
                    int pointee_ptr = (FT != NULL && FT->Base == TypePointer
                        && FT->FromType != NULL && FT->FromType->Base == TypePointer);
                    if (!pointee_ptr && FT != NULL && FT->Base == TypePointer
                        && FT->FromType != NULL && FT->FromType->Base == TypeArray
                        && FT->FromType->FromType != NULL
                        && FT->FromType->FromType->Base == TypePointer)
                        pointee_ptr = 1;
                    E->ElemSize = es;
                    if (pointee_ptr)
                    {
                        E->TypeFlags |= IR_TF_PTR;
                        E->ValWidth = (unsigned char)sizeof(void *);
                    }
                    else
                    {
                        E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                        E->ValWidth = IrVwFromSize((int)es);
                    }
                }
            }
            /* F2: pointer Ident with ElemSize / FromType when scale missing */
            if (E->ElemSize <= 0 && Base != NULL && Base->Kind == IrExIdent)
            {
                int es = 0;
                if (Base->ElemSize > 0)
                    es = Base->ElemSize;
                else if (Base->LocalSlot >= 0 && Base->LocalSlot < IR_COMPILE_MAX_LOCALS)
                {
                    es = IrCompileLocalScale[Base->LocalSlot];
                    if (es <= 0 && IrCompileLocalFromType[Base->LocalSlot] != NULL)
                    {
                        struct ValueType *FT = IrCompileLocalFromType[Base->LocalSlot];
                        if (FT->Base == TypeArray && FT->FromType != NULL)
                            es = (int)FT->FromType->Sizeof;
                        else if (FT->Base == TypePointer && FT->FromType != NULL)
                            es = (int)FT->FromType->Sizeof;
                        else if (FT->Base != TypeStruct && FT->Base != TypeUnion)
                            es = (int)FT->Sizeof;
                    }
                }
                else if (Base->ParamSlot >= 0 && IrCompileParamTypes != NULL
                         && Base->ParamSlot < IrCompileNumParams
                         && IrCompileParamTypes[Base->ParamSlot] != NULL)
                {
                    struct ValueType *PT = IrCompileParamTypes[Base->ParamSlot];
                    if (PT->Base == TypePointer && PT->FromType != NULL)
                        es = (int)PT->FromType->Sizeof;
                    else if (PT->Base == TypeArray && PT->FromType != NULL)
                        es = (int)PT->FromType->Sizeof;
                }
                else if (Base->CachedVal != NULL && Base->CachedVal->Typ != NULL)
                {
                    struct ValueType *T = Base->CachedVal->Typ;
                    if (T->Base == TypeArray && T->FromType != NULL)
                        es = (int)T->FromType->Sizeof;
                    else if (T->Base == TypePointer && T->FromType != NULL)
                        es = (int)T->FromType->Sizeof;
                }
                if (es > 0)
                {
                    E->ElemSize = es;
                    if (!(E->TypeFlags & IR_TF_FP))
                    {
                        E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                        E->ValWidth = IrVwFromSize((int)es);
                    }
                }
            }
            if (E->Left && (E->Left->TypeFlags & IR_TF_INT)
                && !(E->TypeFlags & IR_TF_FP))
                E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
            if (E->Left && (E->Left->TypeFlags & IR_TF_UNSIGNED))
                E->TypeFlags |= IR_TF_UNSIGNED;
            /* F2: Index of Member array — ElemSize from Member annotate */
            if (E->ElemSize <= 0 && E->Left != NULL && E->Left->Kind == IrExMember
                && E->Left->ElemSize > 0)
            {
                E->ElemSize = E->Left->ElemSize;
                if (E->Left->TypeFlags & IR_TF_FP)
                    E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
                else if (E->Left->TypeFlags & IR_TF_INT)
                {
                    E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                    if (E->Left->TypeFlags & IR_TF_UNSIGNED)
                        E->TypeFlags |= IR_TF_UNSIGNED;
                }
            }
            /* F2: global array/pointer Index via Base->CachedVal */
            if (E->ElemSize <= 0 && Base != NULL && Base->Kind == IrExIdent
                && Base->CachedVal != NULL && Base->CachedVal->Typ != NULL)
            {
                struct ValueType *T = Base->CachedVal->Typ;
                struct ValueType *ElemT = NULL;
                int es = 0;
                if (T->Base == TypeArray && T->FromType != NULL)
                    ElemT = T->FromType;
                else if (T->Base == TypePointer && T->FromType != NULL)
                    ElemT = T->FromType;
                if (ElemT != NULL)
                    es = (ElemT->Sizeof > 0 ? (int)ElemT->Sizeof : (int)sizeof(int));
                if (es > 0)
                {
                    E->ElemSize = es;
                    if (ElemT->Base == TypeFP)
                        E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
                    else
                    {
                        E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                        if (ElemT->Base == TypeUnsignedInt
                            || ElemT->Base == TypeUnsignedChar
                            || ElemT->Base == TypeUnsignedShort
                            || ElemT->Base == TypeUnsignedLong
                            || ElemT->Base == TypeUnsignedLongLong)
                            E->TypeFlags |= IR_TF_UNSIGNED;
                        E->ValWidth = IrVwFromSize((int)es);
                    }
                }
            }
        }
        return;
    }
    if (E->Kind == IrExUnary && E->Op == TokenCast)
    {
        /* Pointer cast (T *)x: ConstVal = pointee size, I32 = pointee base.
         * The result is a pointer whatever T is; (double *)malloc(..) used
         * to be typed FP (linear converted the address to a double) and
         * (char *)p INT8 (truncated). */
        if (E->ConstVal > 0)
        {
            E->TypeFlags = IR_TF_RESOLVED | IR_TF_PTR;
            E->ElemSize = (int)E->ConstVal;
            E->ValWidth = IR_VW_I64;
            return;
        }
        if (E->I32 == TypeFP)
        {
            E->TypeFlags = IR_TF_RESOLVED | IR_TF_FP;
            E->ValWidth = IR_VW_I64;
        }
        else if (E->I32 == TypePointer)
            E->TypeFlags = IR_TF_PTR | IR_TF_RESOLVED;
        else if (E->I32 == TypeChar)
        {
            E->TypeFlags = IR_TF_RESOLVED | IR_TF_INT;
            E->ValWidth = IR_VW_I8;
        }
        else if (E->I32 == TypeShort)
        {
            E->TypeFlags = IR_TF_RESOLVED | IR_TF_INT;
            E->ValWidth = IR_VW_I16;
        }
        else if (E->I32 == TypeLong || E->I32 == TypeLongLong)
        {
            E->TypeFlags = IR_TF_RESOLVED | IR_TF_INT;
            E->ValWidth = (E->I32 == TypeLongLong) ? IR_VW_I64 : (unsigned char)sizeof(long);
        }
        else if (E->I32 == TypeInt || E->I32 == TypeUnsignedInt)
        {
            E->TypeFlags = IR_TF_RESOLVED | IR_TF_INT;
            if (E->I32 == TypeUnsignedInt)
                E->TypeFlags |= IR_TF_UNSIGNED;
            E->ValWidth = IR_VW_I32;
        }
        else if (E->I32 == TypeUnsignedChar || E->I32 == TypeUnsignedShort
                 || E->I32 == TypeUnsignedLong || E->I32 == TypeUnsignedLongLong)
        {
            E->TypeFlags = IR_TF_RESOLVED | IR_TF_INT | IR_TF_UNSIGNED;
            if (E->I32 == TypeUnsignedChar) E->ValWidth = IR_VW_I8;
            else if (E->I32 == TypeUnsignedShort) E->ValWidth = IR_VW_I16;
            else if (E->I32 == TypeUnsignedLongLong) E->ValWidth = IR_VW_I64;
            else E->ValWidth = (unsigned char)sizeof(long);
        }
        else
        {
            /* I32==0 / unknown target: still mark resolved (default int cast) */
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
            E->ValWidth = IR_VW_I32;
        }
        return;
    }
    if (E->Kind == IrExUnary && E->Op == TokenAsterisk)
    {
        E->ElemSize = 0;
        /* *(p+i) / *(i+p): stamp UNSIGNED from array/pointer Ident (792). */
        if (E->Left != NULL && E->Left->Kind == IrExBinOp
            && (E->Left->Op == TokenPlus || E->Left->Op == TokenMinus))
        {
            IrExpr *Cands[2];
            int ci;
            Cands[0] = E->Left->Left;
            Cands[1] = E->Left->Right;
            for (ci = 0; ci < 2; ci++)
            {
                IrExpr *P = Cands[ci];
                struct ValueType *PT = NULL;
                if (P == NULL || P->Kind != IrExIdent) continue;
                if (P->LocalSlot >= 0 && P->LocalSlot < IR_COMPILE_MAX_LOCALS)
                    PT = IrCompileLocalFromType[P->LocalSlot];
                else if (P->ParamSlot >= 0 && IrCompileParamTypes != NULL
                         && P->ParamSlot < IrCompileNumParams)
                    PT = IrCompileParamTypes[P->ParamSlot];
                else if (P->CachedVal != NULL)
                    PT = P->CachedVal->Typ;
                if (PT == NULL) continue;
                if (PT->Base != TypePointer && PT->Base != TypeArray) continue;
                if (PT->FromType == NULL) continue;
                {
                    struct ValueType *ElemT = PT->FromType;
                    E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                    E->TypeFlags &= (unsigned short)~IR_TF_PTR;
                    if (ElemT->Base == TypeUnsignedInt || ElemT->Base == TypeUnsignedChar
                        || ElemT->Base == TypeUnsignedShort || ElemT->Base == TypeUnsignedLong
                        || ElemT->Base == TypeUnsignedLongLong)
                        E->TypeFlags |= IR_TF_UNSIGNED;
                    if (ElemT->Base == TypeFP)
                    {
                        E->TypeFlags &= (unsigned short)~IR_TF_INT;
                        E->TypeFlags |= IR_TF_FP;
                    }
                    if (ElemT->Sizeof > 0)
                        E->ElemSize = (int)ElemT->Sizeof;
                    break;
                }
            }
        }
        {
            /* *p, *p++, *++p, *p--, *--p, *(a+k) on T *a[N]: the operand's
             * pointer value has the type of the root identifier. */
            IrExpr *B = E->Left;
            while (B != NULL && B->Kind == IrExUnary
                   && (B->Op == TokenIncrement || B->Op == TokenDecrement))
                B = B->Left;
            if (B != NULL && B->Kind == IrExBinOp
                && (B->Op == TokenPlus || B->Op == TokenMinus))
            {
                if (B->Left != NULL && B->Left->Kind == IrExIdent
                    && IrCompileIdentElemPointee(B->Left) != NULL)
                    B = B->Left;
                else if (B->Op == TokenPlus && B->Right != NULL
                         && B->Right->Kind == IrExIdent
                         && IrCompileIdentElemPointee(B->Right) != NULL)
                    B = B->Right;
            }
            /* Element of T *a[N] (local, parameter or global): a pointer with
             * declared pointee T, exactly as the a[i] Index annotation. */
            if (B != NULL && B->Kind == IrExIdent
                && IrCompileIdentElemPointee(B) != NULL)
            {
                E->TypeFlags = IR_TF_RESOLVED | IR_TF_PTR;
                E->ElemSize = (int)sizeof(void *);
                E->ValWidth = (unsigned char)sizeof(void *);
                E->PointeeType = IrCompileIdentElemPointee(B);
                return;
            }
        }
        if (E->Left != NULL
            && (E->Left->Kind == IrExIdent
                || (E->Left->Kind == IrExUnary
                    && (E->Left->Op == TokenIncrement || E->Left->Op == TokenDecrement)
                    && E->Left->Left != NULL && E->Left->Left->Kind == IrExIdent)))
        {
            IrExpr *B = (E->Left->Kind == IrExIdent) ? E->Left : E->Left->Left;
            struct ValueType *PT = NULL;
            if (B->LocalSlot >= 0 && B->LocalSlot < IR_COMPILE_MAX_LOCALS)
                PT = IrCompileLocalFromType[B->LocalSlot];
            else if (B->ParamSlot >= 0 && IrCompileParamTypes != NULL
                     && B->ParamSlot < IrCompileNumParams)
                PT = IrCompileParamTypes[B->ParamSlot];
            else if (B->CachedVal != NULL)
                PT = B->CachedVal->Typ;
            if (PT != NULL && (PT->Base == TypePointer || PT->Base == TypeArray)
                && PT->FromType != NULL)
            {
                struct ValueType *ElemT = PT->FromType;
                if (ElemT->Base == TypeFP)
                {
                    E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
                    E->ValWidth = IR_VW_I64;
                    E->ElemSize = (int)sizeof(double);
                    return;
                }
                /* *out when out is T**: pointee is pointer → full word store (596) */
                if (ElemT->Base == TypePointer || ElemT->Base == TypeArray)
                {
                    E->TypeFlags |= IR_TF_RESOLVED | IR_TF_PTR;
                    E->ElemSize = (int)sizeof(void *);
                    return;
                }
                if (ElemT->Sizeof > 0)
                {
                    E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                    E->ElemSize = (int)ElemT->Sizeof;
                    if (ElemT->Base == TypeUnsignedInt || ElemT->Base == TypeUnsignedChar
                        || ElemT->Base == TypeUnsignedShort || ElemT->Base == TypeUnsignedLong
                        || ElemT->Base == TypeUnsignedLongLong)
                        E->TypeFlags |= IR_TF_UNSIGNED;
                    return;
                }
            }
        }
        /* F2: mark RESOLVED only (no INT/PTR invent) — verify needs a flag,
         * runtime paths must not see IR_TF_INT on multi-level *p. */
        if (E->Left != NULL
            && (E->Left->TypeFlags & (IR_TF_PTR | IR_TF_RESOLVED)))
            E->TypeFlags |= IR_TF_RESOLVED;
        /* *a[i] on int *a[N]: Index has TF_PTR+ElemSize==sizeof(void*) →
         * pointee is scalar (int). Multi-level *p on Ident keeps PTR. */
        if (E->Left != NULL && E->Left->Kind == IrExIndex
            && E->Left->ElemSize == (int)sizeof(void *))
        {
            /* *a[i] on array-of-pointer: pointee is scalar (default int).
             * Detect via ElemSize==sizeof(void*) on Index of local array. */
            IrExpr *Root = E->Left->Left;
            int is_aop = 0;
            while (Root != NULL && Root->Kind == IrExIndex)
                Root = Root->Left;
            if (Root != NULL && Root->Kind == IrExIdent
                && Root->LocalSlot >= 0
                && Root->LocalSlot < IR_COMPILE_MAX_LOCALS
                && IrCompileLocalElemIsPtr[Root->LocalSlot])
                is_aop = 1;
            if (is_aop)
            {
                E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                E->TypeFlags &= (unsigned short)~IR_TF_PTR;
                E->ElemSize = (int)sizeof(int);
                E->ValWidth = IR_VW_I32;
            }
        }
        else if (E->Left != NULL && (E->Left->TypeFlags & IR_TF_PTR)
            && (E->Left->ElemSize == (int)sizeof(void *) || E->Left->ElemSize == 0))
        {
            /* Infer the result type from the declared pointer depth, not from
             * the previous dereference's PTR flag. The latter was stamped with
             * sizeof(void*) for every level, so **pp (int **pp) was incorrectly
             * marked as a pointer too; then `**pp + 1` scaled by sizeof(void*).
             * Count stars from this node to the root identifier and consume that
             * many pointer layers from its declared type. */
            IrExpr *Root = E;
            struct ValueType *RT = NULL;
            int stars = 0, ptrdepth = 0;
            while (Root != NULL && Root->Kind == IrExUnary
                   && Root->Op == TokenAsterisk)
            {
                ++stars;
                Root = Root->Left;
            }
            /* ****&p: & cancels one star against the variable address */
            if (Root != NULL && Root->Kind == IrExUnary
                && Root->Op == TokenAmpersand && Root->Left != NULL)
            {
                if (stars > 0)
                    --stars;
                Root = Root->Left;
            }
            if (Root != NULL && Root->Kind == IrExIdent)
            {
                if (Root->LocalSlot >= 0 && Root->LocalSlot < IR_COMPILE_MAX_LOCALS)
                    RT = IrCompileLocalFromType[Root->LocalSlot];
                else if (Root->ParamSlot >= 0 && IrCompileParamTypes != NULL
                         && Root->ParamSlot < IrCompileNumParams)
                    RT = IrCompileParamTypes[Root->ParamSlot];
                else if (Root->CachedVal != NULL)
                    RT = Root->CachedVal->Typ;
            }
            else if (Root != NULL && Root->Kind == IrExCall)
            {
                /* **f(): return type is the full pointer chain (int**), same
                 * model as LocalFromType for variables — no +1. */
                if (Root->CachedFunc != NULL && Root->CachedFunc->Val != NULL)
                    RT = Root->CachedFunc->Val->FuncDef.ReturnType;
                if (RT == NULL && Root->Left != NULL && Root->Left->Kind == IrExIdent)
                    RT = IrLookupFuncReturnType(Root->Left->Ident);
                if (RT == NULL && Root->Ident != NULL)
                    RT = IrLookupFuncReturnType(Root->Ident);
            }
            if (RT != NULL)
            {
                while (RT != NULL && (RT->Base == TypePointer || RT->Base == TypeArray))
                {
                    ++ptrdepth;
                    RT = RT->FromType;
                }
                /* Local DeclFromType is the *pointee* (DeclIsPtr / Kind 2). The slot
                 * itself is one more pointer level not present in that chain.
                 * Params/globals store the full type — do not add. Without this,
                 * int **pp with DeclFromType=int* has ptrdepth=1 and *pp is
                 * wrongly classified as scalar (es=4 → truncated store).
                 *
                 * Multi-level T **p where DeclFromType is a non-pointer (struct
                 * Tag kept for Member TableGet): Scale/pointee is sizeof(void*)
                 * → one extra pointer layer before the scalar/struct. */
                if (Root != NULL && Root->Kind == IrExIdent
                    && Root->LocalSlot >= 0 && Root->LocalSlot < IR_COMPILE_MAX_LOCALS
                    && IrCompileLocalKind[Root->LocalSlot] == 2)
                {
                    int ls = Root->LocalSlot;
                    /* Full variable type (TypePointer chain) already counts
                     * every * on the slot. Legacy pointee-only metadata
                     * (scalar/struct DeclFromType) needs +1 for the slot. */
                    if (IrCompileLocalFromType[ls] == NULL
                        || (IrCompileLocalFromType[ls]->Base != TypePointer
                            && IrCompileLocalFromType[ls]->Base != TypeArray))
                    {
                        ptrdepth += 1;
                        if (IrCompileLocalFromType[ls] != NULL
                            && IrCompileLocalScale[ls] == (int)sizeof(void *))
                            ptrdepth += 1;
                    }
                }
                if (ptrdepth > stars)
                {
                    E->TypeFlags &= (unsigned short)~IR_TF_INT;
                    E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
                    E->ElemSize = (int)sizeof(void *);
                }
                else
                {
                    E->TypeFlags &= (unsigned short)~IR_TF_PTR;
                    E->TypeFlags |= IR_TF_RESOLVED;
                    E->ElemSize = 0;
                    if (RT != NULL && RT->Base == TypeFP)
                    {
                        E->TypeFlags &= (unsigned short)~IR_TF_INT;
                        E->TypeFlags |= IR_TF_FP;
                    }
                    else
                    {
                        E->TypeFlags |= IR_TF_INT;
                        if (RT != NULL && (RT->Base == TypeUnsignedInt
                            || RT->Base == TypeUnsignedChar
                            || RT->Base == TypeUnsignedShort
                            || RT->Base == TypeUnsignedLong
                            || RT->Base == TypeUnsignedLongLong))
                            E->TypeFlags |= IR_TF_UNSIGNED;
                        if (RT != NULL && RT->Sizeof > 0)
                            E->ElemSize = (int)RT->Sizeof;
                    }
                }
            }
            else
            {
                /* Unknown type metadata: don't invent another pointer level. */
                E->TypeFlags &= (unsigned short)~IR_TF_PTR;
                E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                E->ElemSize = 0;
            }
        }
    }
    /* F2: &x → PTR resolved; do NOT invent ElemSize (broke multi-level ptr). */
    if (E->Kind == IrExUnary && E->Op == TokenAmpersand)
        E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
    if (E->Kind == IrExUnary && E->Op == TokenSizeof)
        E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
    /* Do not force INT on FP/pointer casts — earlier Unary Cast arm already
     * stamped IR_TF_FP / IR_TF_PTR from I32. Overwriting with INT made
     * int→FP return casts look ambiguous to lower (954 STRICT). */
    if (E->Kind == IrExUnary && E->Op == TokenCast
        && !(E->TypeFlags & (IR_TF_FP | IR_TF_PTR))
        && E->I32 != TypeFP && E->I32 != TypePointer)
        E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;

    if (E->Kind == IrExUnary
        && (E->Op == TokenIncrement || E->Op == TokenDecrement)
        && E->Left != NULL)
    {
        if (E->Left->TypeFlags & IR_TF_FP)
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
        else if (E->Left->TypeFlags & IR_TF_PTR)
        {
            E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
            if (E->Left->ElemSize > 0)
                E->ElemSize = E->Left->ElemSize;
        }
        else
        {
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
            if (E->Left->TypeFlags & IR_TF_UNSIGNED)
                E->TypeFlags |= IR_TF_UNSIGNED;
            E->ValWidth = E->Left->ValWidth ? E->Left->ValWidth : IR_VW_I32;
        }
    }
    if (E->Kind == IrExUnary
        && (E->Op == TokenUnaryNot || E->Op == TokenUnaryExor)
        && E->Left != NULL
        && (E->Left->TypeFlags & (IR_TF_INT | IR_TF_PTR | IR_TF_RESOLVED)))
    {
        E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
        E->ValWidth = IR_VW_I32;
    }
    if (E->Kind == IrExCall)
    {
        /* Default INT. FP only when return type known FP.
         * Do NOT stamp PTR on Call TypeFlags (breaks some arithmetic paths);
         * Member uses IrCallReturnStruct for f()->m without flags. */
        struct ValueType *RT = NULL;
        if (E->CachedFunc != NULL && E->CachedFunc->Val != NULL)
            RT = E->CachedFunc->Val->FuncDef.ReturnType;
        else if (E->Left != NULL && E->Left->CachedFunc != NULL
                 && E->Left->CachedFunc->Val != NULL)
            RT = E->Left->CachedFunc->Val->FuncDef.ReturnType;
        /* User functions may not carry CachedFunc (forward/recursive calls). */
        if (RT == NULL && E->Left != NULL && E->Left->Kind == IrExIdent)
            RT = IrLookupFuncReturnType(E->Left->Ident);
        if (RT == NULL && E->Ident != NULL)
            RT = IrLookupFuncReturnType(E->Ident);
        if (RT != NULL && RT->Base == TypeFP)
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
        else if (RT != NULL && RT->Base == TypePointer && RT->FromType != NULL)
        {
            struct ValueType *Ft = RT->FromType;
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_PTR;
            if (Ft->Base == TypeUnsignedChar || Ft->Base == TypeUnsignedShort
                || Ft->Base == TypeUnsignedInt || Ft->Base == TypeUnsignedLong
                || Ft->Base == TypeUnsignedLongLong)
                E->TypeFlags |= IR_TF_UNSIGNED;
            if (Ft->Base == TypePointer || Ft->Base == TypeArray)
                E->ElemSize = (unsigned char)sizeof(void *);
            else if (Ft->Sizeof > 0 && Ft->Sizeof < 256)
                E->ElemSize = (unsigned char)Ft->Sizeof;
            else
                E->ElemSize = (unsigned char)sizeof(int);
        }
        else if (!(E->TypeFlags & IR_TF_FP))
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
    }

    if (E->Left != NULL && (E->Left->TypeFlags & IR_TF_FP))
        child_fp = 1;
    if (E->Right != NULL && (E->Right->TypeFlags & IR_TF_FP))
        child_fp = 1;
    if (E->Kind == IrExTernary && E->Args != NULL && E->ArgCount > 0
        && (E->Args[0]->TypeFlags & IR_TF_FP))
        child_fp = 1;
    if (child_fp && (E->Kind == IrExBinOp || E->Kind == IrExTernary || E->Kind == IrExUnary))
        E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
    else if (E->Kind == IrExTernary && !child_fp
             && E->Right != NULL && E->Args != NULL && E->ArgCount >= 1
             && E->Args[0] != NULL
             && E->Right->Kind == IrExConst && E->Args[0]->Kind == IrExConst
             && E->Right->Op != TokenStringConstant
             && E->Args[0]->Op != TokenStringConstant)
    {
        /* Integer const arms only: arr ? 1 : 0.
         * String const arms ("OK"/"ERR") stay PTR. Calls/BinOp fall back. */
        E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
    }
    else if (E->Kind == IrExTernary && !child_fp
             && E->Right != NULL && E->Args != NULL && E->ArgCount >= 1
             && E->Args[0] != NULL)
    {
        /* Pointer ternary: either arm is string const or carries TF_PTR
         * (z ? z : "?"). Stamp PTR so CallArgIsPtr packs it for %s. */
        int rp = (E->Right->Kind == IrExConst && E->Right->Op == TokenStringConstant)
                 || ((E->Right->TypeFlags & IR_TF_PTR) && !(E->Right->TypeFlags & IR_TF_FP));
        int ap = (E->Args[0]->Kind == IrExConst && E->Args[0]->Op == TokenStringConstant)
                 || ((E->Args[0]->TypeFlags & IR_TF_PTR) && !(E->Args[0]->TypeFlags & IR_TF_FP));
        if (rp || ap)
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_PTR;
    }
    else if (E->Kind == IrExBinOp && !child_fp)
    {
        int li = E->Left && (E->Left->TypeFlags & IR_TF_INT);
        int ri = E->Right && (E->Right->TypeFlags & IR_TF_INT);
        int lp = E->Left && (E->Left->TypeFlags & IR_TF_PTR);
        int rp = E->Right && (E->Right->TypeFlags & IR_TF_PTR);
        int op = (int)E->Op;
        int is_rel = (op == TokenEqual || op == TokenNotEqual
                      || op == TokenLessThan || op == TokenGreaterThan
                      || op == TokenLessEqual || op == TokenGreaterEqual
                      || op == TokenLogicalAnd || op == TokenLogicalOr);
        /* Pure integer op — both sides INT, neither PTR */
        if (li && ri && !lp && !rp)
        {
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
            if ((E->Left->TypeFlags & IR_TF_UNSIGNED)
                || (E->Right->TypeFlags & IR_TF_UNSIGNED))
                E->TypeFlags |= IR_TF_UNSIGNED;
            {
                unsigned char w = IR_VW_I32;
                if (E->Left->ValWidth == IR_VW_I64 || E->Right->ValWidth == IR_VW_I64)
                    w = IR_VW_I64;
                else if (E->Left->ValWidth == IR_VW_I16 && E->Right->ValWidth == IR_VW_I16)
                    w = IR_VW_I16;
                E->ValWidth = w;
            }
        }
        /* RESOLVED-only arith → INT */
        else if (!lp && !rp
                 && ((li || (E->Left && (E->Left->TypeFlags & IR_TF_RESOLVED)))
                     && (ri || (E->Right && (E->Right->TypeFlags & IR_TF_RESOLVED))))
                 && (op == TokenPlus || op == TokenMinus || op == TokenAsterisk
                     || op == TokenSlash || op == TokenModulus
                     || op == TokenAmpersand || op == TokenArithmeticOr
                     || op == TokenArithmeticExor
                     || op == TokenShiftLeft || op == TokenShiftRight))
        {
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
            E->ValWidth = IR_VW_I32;
        }
        /* n * call(...) : INT × (INT|PTR|RESOLVED) → INT for multiplicative ops
         * (fact recursion: Call may carry PTR from function Ident). */
        else if ((li || ri)
                 && (op == TokenAsterisk || op == TokenSlash || op == TokenModulus)
                 && !(lp && rp))
        {
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
            E->ValWidth = IR_VW_I32;
        }
        /* Relational / equality / logical: result is INT when any side typed */
        else if (is_rel && (li || ri || lp || rp
                 || (E->Left && (E->Left->TypeFlags & IR_TF_RESOLVED))
                 || (E->Right && (E->Right->TypeFlags & IR_TF_RESOLVED))))
        {
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
            E->ValWidth = IR_VW_I32;
        }
        /* Comma: type of right operand */
        else if (op == TokenComma && E->Right != NULL)
        {
            E->TypeFlags |= (E->Right->TypeFlags & (IR_TF_RESOLVED|IR_TF_INT|IR_TF_PTR|IR_TF_FP|IR_TF_UNSIGNED));
            if (!(E->TypeFlags & (IR_TF_RESOLVED|IR_TF_INT|IR_TF_PTR|IR_TF_FP)))
                E->TypeFlags |= IR_TF_RESOLVED;
            E->ElemSize = E->Right->ElemSize;
            E->ValWidth = E->Right->ValWidth;
        }
        /* ptr ± int → PTR; ptr - ptr → INT (ptrdiff). Index scalar loads → INT. */
        else if ((lp || rp) && (op == TokenPlus || op == TokenMinus))
        {
            int left_ptrval = lp && E->Left && E->Left->Kind != IrExIndex
                && !(E->Left->Kind == IrExBinOp && !(E->Left->TypeFlags & IR_TF_PTR));
            int right_ptrval = rp && E->Right && E->Right->Kind != IrExIndex
                && !(E->Right->Kind == IrExBinOp && !(E->Right->TypeFlags & IR_TF_PTR));
            /* Index marked PTR only for pointer pointee; treat non-Index PTR as value */
            if (lp && E->Left && E->Left->Kind == IrExIndex)
                left_ptrval = (E->Left->TypeFlags & IR_TF_PTR) && !(E->Left->TypeFlags & IR_TF_INT);
            if (rp && E->Right && E->Right->Kind == IrExIndex)
                right_ptrval = (E->Right->TypeFlags & IR_TF_PTR) && !(E->Right->TypeFlags & IR_TF_INT);
            if (left_ptrval && right_ptrval && op == TokenMinus)
            {
                /* ptrdiff_t: not a pointer — host printf args and scale gates. */
                E->TypeFlags &= (unsigned short)~IR_TF_PTR;
                E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                E->ElemSize = 0;
            }
            else if (left_ptrval || right_ptrval)
            {
                E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
                if (left_ptrval && E->Left->ElemSize > 0)
                    E->ElemSize = E->Left->ElemSize;
                else if (right_ptrval && E->Right->ElemSize > 0)
                    E->ElemSize = E->Right->ElemSize;
            }
            else if (li || ri)
            {
                /* scalar ± scalar (including p[i] loads) */
                E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
                E->ElemSize = 0;
            }
        }
        /* Integer const folded with INT */
        if (li && E->Right && E->Right->Kind == IrExConst
            && (E->Right->TypeFlags & IR_TF_RESOLVED)
            && !(E->Right->TypeFlags & IR_TF_FP) && !lp)
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
        if (ri && E->Left && E->Left->Kind == IrExConst
            && (E->Left->TypeFlags & IR_TF_RESOLVED)
            && !(E->Left->TypeFlags & IR_TF_FP) && !rp)
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
    }
    else if (E->Kind == IrExTernary && !child_fp)
    {
        /* Args[0]=cond, Args[1]=then, Args[2]=else (or Right) */
        int tf = 0, pf = 0, rf = 0;
        if (E->Args != NULL && E->ArgCount >= 2)
        {
            int i;
            for (i = 1; i < E->ArgCount; i++)
            {
                if (E->Args[i] == NULL) continue;
                if (E->Args[i]->TypeFlags & IR_TF_FP) { child_fp = 1; break; }
                if (E->Args[i]->TypeFlags & IR_TF_INT) tf = 1;
                if (E->Args[i]->TypeFlags & IR_TF_PTR) pf = 1;
                if (E->Args[i]->TypeFlags & IR_TF_RESOLVED) rf = 1;
            }
        }
        if (E->Right != NULL)
        {
            if (E->Right->TypeFlags & IR_TF_FP) child_fp = 1;
            if (E->Right->TypeFlags & IR_TF_INT) tf = 1;
            if (E->Right->TypeFlags & IR_TF_PTR) pf = 1;
            if (E->Right->TypeFlags & IR_TF_RESOLVED) rf = 1;
        }
        /* Condition (Left) must not pollute result: p ? *p : 99 is INT
         * even though the condition is a pointer. Only arms matter. */
        if (child_fp)
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
        else if (pf && !tf)
            E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
        else if (tf || rf)
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
        else if (pf)
            E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
    }
    else if (E->Kind == IrExAssign && E->Right)
    {
        if ((E->Right->TypeFlags & IR_TF_FP) || (E->Left && (E->Left->TypeFlags & IR_TF_FP)))
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_FP;
        else if ((E->Right->TypeFlags & IR_TF_INT)
                 && !(E->Right->TypeFlags & IR_TF_PTR))
        {
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
            if (E->Right->TypeFlags & IR_TF_UNSIGNED)
                E->TypeFlags |= IR_TF_UNSIGNED;
            if (E->Right->ValWidth)
                E->ValWidth = E->Right->ValWidth;
        }
        else if (E->Left && (E->Left->TypeFlags & IR_TF_INT)
                 && !(E->Left->TypeFlags & IR_TF_PTR))
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
        else if (E->Left && (E->Left->TypeFlags & IR_TF_PTR))
            E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
        else if (E->Left && (E->Left->TypeFlags & IR_TF_RESOLVED)
                 && (E->Right->TypeFlags & IR_TF_RESOLVED))
            E->TypeFlags |= IR_TF_RESOLVED;
        else if (E->Right->TypeFlags & IR_TF_RESOLVED)
            E->TypeFlags |= IR_TF_RESOLVED;
        else if (E->Left && (E->Left->TypeFlags & IR_TF_RESOLVED))
            E->TypeFlags |= IR_TF_RESOLVED;
    }
    else if (E->Kind == IrExTernary && !child_fp && E->Args != NULL && E->ArgCount >= 2)
    {
        /* a ? b : c — result INT if both arms INT */
        IrExpr *A = E->Args[0], *B = E->ArgCount > 1 ? E->Args[1] : NULL;
        int ai = A && (A->TypeFlags & IR_TF_INT) && !(A->TypeFlags & IR_TF_PTR);
        int bi = B && (B->TypeFlags & IR_TF_INT) && !(B->TypeFlags & IR_TF_PTR);
        if (ai && bi)
        {
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
            if ((A->TypeFlags & IR_TF_UNSIGNED) || (B->TypeFlags & IR_TF_UNSIGNED))
                E->TypeFlags |= IR_TF_UNSIGNED;
        }
        else if (ai || bi)
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
    }
    else if (E->Kind == IrExUnary && !child_fp && E->Left
             && (E->Left->TypeFlags & IR_TF_INT)
             && E->Op != TokenAsterisk && E->Op != TokenAmpersand)
        E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
    if (E->Kind == IrExBinOp && (E->Op == TokenPlus || E->Op == TokenMinus)) {
        /* ptr ± int / int + ptr → PTR. Require one side PTR and the other
         * non-PTR. Both-PTR is pointer difference (INT). */
        int lp = E->Left && (E->Left->TypeFlags & IR_TF_PTR);
        int rp = E->Right && (E->Right->TypeFlags & IR_TF_PTR);
        if (lp && !rp)
        {
            if (E->ElemSize <= 0 && E->Left->ElemSize > 0)
                E->ElemSize = E->Left->ElemSize;
            if (E->ElemSize > 0)
                E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
        }
        else if (rp && !lp)
        {
            if (E->ElemSize <= 0 && E->Right->ElemSize > 0)
                E->ElemSize = E->Right->ElemSize;
            if (E->ElemSize > 0)
                E->TypeFlags |= IR_TF_PTR | IR_TF_RESOLVED;
        }
    }
    if (E->Kind == IrExIndex && E->Left && !(E->TypeFlags & IR_TF_RESOLVED)) {
        if ((E->Left->TypeFlags & IR_TF_INT) || (E->Left->LocalSlot >= 0 && E->Left->LocalSlot < IR_COMPILE_MAX_LOCALS
            && IrCompileLocalKind[E->Left->LocalSlot] == 4 && !(E->Left->TypeFlags & IR_TF_FP))) {
            E->TypeFlags |= IR_TF_RESOLVED | IR_TF_INT;
            if (E->ValWidth == 0) E->ValWidth = IR_VW_I32;
        }
    }
    { int fus=0;
      if (!(E->TypeFlags & IR_TF_FP)) {
        if (E->Kind==IrExUnary && (E->Op==TokenIncrement||E->Op==TokenDecrement) && E->Left && E->Left->Kind==IrExIdent
            && !(E->Left->TypeFlags & (IR_TF_FP|IR_TF_PTR|IR_TF_CONST))) fus=1;
        else if (E->Kind==IrExAssign && E->Left && E->Right) {
          int l=(E->Left->Kind==IrExIdent||E->Left->Kind==IrExIndex||(E->Left->Kind==IrExUnary&&E->Left->Op==TokenAsterisk));
          int r=(E->Right->Kind==IrExIdent||E->Right->Kind==IrExConst||E->Right->Kind==IrExIndex
            ||(E->Right->Kind==IrExUnary&&E->Right->Op==TokenAsterisk)
            ||(E->Right->Kind==IrExBinOp&&(E->Right->Op==TokenPlus||E->Right->Op==TokenMinus)&&!(E->Right->TypeFlags&(IR_TF_FP|IR_TF_PTR))));
          if (l&&r) fus=1;
        } else if (E->Kind==IrExBinOp && E->Left && E->Right) {
          int lop=E->Op; int is_rel=(lop==TokenEqual||lop==TokenNotEqual||lop==TokenLessThan||lop==TokenGreaterThan||lop==TokenLessEqual||lop==TokenGreaterEqual);
          int is_land=(lop==TokenLogicalAnd);
          if (is_rel||is_land) {
            int l=(E->Left->Kind==IrExIdent||E->Left->Kind==IrExIndex||E->Left->Kind==IrExConst||(E->Left->Kind==IrExUnary&&E->Left->Op==TokenAsterisk)||(E->Left->Kind==IrExBinOp&&is_land));
            int r=(E->Right->Kind==IrExIdent||E->Right->Kind==IrExIndex||E->Right->Kind==IrExConst||(E->Right->Kind==IrExUnary&&E->Right->Op==TokenAsterisk)||(E->Right->Kind==IrExBinOp&&is_land));
            if (l&&r) fus=1;
          }
        }
      }
      if (fus) E->TypeFlags |= IR_TF_FUSABLE;
    }
    (void)child_all;
}

static void IrAnnotateStmt(IrStmt *S)
{
    int i;
    if (S == NULL)
        return;
    if (S->Kind == IrStDecl && S->LocalSlot >= 0
        && S->LocalSlot < IR_COMPILE_MAX_LOCALS)
    {
        if (S->DeclIsConst)
            IrCompileLocalIsConst[S->LocalSlot] = 1;
        if (S->DeclFromType != NULL)
        {
            /* Prefer a fuller TypePointer chain already stamped at Decl time. */
            struct ValueType *Cur = IrCompileLocalFromType[S->LocalSlot];
            int cur_d = 0, decl_d = 0;
            struct ValueType *T;
            for (T = Cur; T && (T->Base == TypePointer || T->Base == TypeArray);
                 T = T->FromType)
                cur_d++;
            for (T = S->DeclFromType;
                 T && (T->Base == TypePointer || T->Base == TypeArray);
                 T = T->FromType)
                decl_d++;
            if (Cur == NULL || decl_d >= cur_d)
                IrCompileLocalFromType[S->LocalSlot] = S->DeclFromType;
        }
        if ((S->DeclIsPtr || S->DeclArraySize > 0) && S->DeclElemSize > 0)
            IrCompileLocalScale[S->LocalSlot] = S->DeclElemSize;
        if (S->DeclArraySize > 0)
        {
            IrCompileLocalArraySize[S->LocalSlot] = S->DeclArraySize;
            if (IrCompilePc != NULL && !S->DeclIsFptr)
                IrCompileLocalArrayType[S->LocalSlot] = IrCompileDeclArrayType(S);
        }
        /* Classify slot for TypeFlags / future typed bytecode.
         * 1=int scalar 2=ptr 3=fp scalar 4=int array 5=fp array */
        if (S->DeclIsFptr)
            IrCompileLocalKind[S->LocalSlot] = 2;
        else if (S->DeclArraySize > 0)
        {
            if (S->DeclFromType != NULL && S->DeclFromType->Base == TypeFP)
                IrCompileLocalKind[S->LocalSlot] = 5;
            else
                IrCompileLocalKind[S->LocalSlot] = 4;
            if (S->DeclElemIsPtr)
                IrCompileLocalElemIsPtr[S->LocalSlot] = 1;
        }
        else if (S->DeclIsPtr || S->DeclElemIsPtr)
            IrCompileLocalKind[S->LocalSlot] = 2;
        else if (S->DeclFromType != NULL && S->DeclFromType->Base == TypeFP)
            IrCompileLocalKind[S->LocalSlot] = 3;
        else
            IrCompileLocalKind[S->LocalSlot] = 1;
        /* Unsigned element/scalar — parallel flag (do not clobber array kind 4/5). */
        if (S->DeclFromType != NULL
            && (S->DeclFromType->Base == TypeUnsignedInt
                || S->DeclFromType->Base == TypeUnsignedLong
                || S->DeclFromType->Base == TypeUnsignedLongLong
                || S->DeclFromType->Base == TypeUnsignedShort
                || S->DeclFromType->Base == TypeUnsignedChar))
        {
            IrCompileLocalUnsigned[S->LocalSlot] = 1;
            if (S->DeclArraySize == 0 && !S->DeclIsPtr)
                IrCompileLocalKind[S->LocalSlot] = 6; /* unsigned scalar */
        }
    }
    IrAnnotateExpr(S->Expr);
    IrAnnotateStmt(S->ThenStmt);
    IrAnnotateStmt(S->ElseStmt);
    IrAnnotateStmt(S->BodyStmt);
    IrAnnotateStmt(S->InitStmt);
    IrAnnotateStmt(S->IncStmt);
    if (S->Stmts != NULL)
    {
        for (i = 0; i < S->StmtCount; i++)
            IrAnnotateStmt(S->Stmts[i]);
    }
    if (S->DeclInitExprs != NULL)
    {
        for (i = 0; i < S->DeclInitCount; i++)
            IrAnnotateExpr(S->DeclInitExprs[i]);
    }
}

/* G4 — conservative intra-procedural definite-assignment.
 * Loop bodies do not publish defs. If-without-else does not define.
 * Only scalar Ident locals/params get IR_TF_DEFINITELY_INIT. */
#define IR_DA_WORDS ((IR_COMPILE_MAX_LOCALS + 31) / 32)

static void IrDaSet(unsigned *D, int Slot)
{
    if (Slot >= 0 && Slot < IR_COMPILE_MAX_LOCALS)
        D[Slot / 32] |= 1u << (Slot % 32);
}

static int IrDaGet(const unsigned *D, int Slot)
{
    if (Slot < 0 || Slot >= IR_COMPILE_MAX_LOCALS)
        return 0;
    return (D[Slot / 32] >> (Slot % 32)) & 1u;
}

static void IrDaAnd(unsigned *Dst, const unsigned *Src)
{
    int i;
    for (i = 0; i < IR_DA_WORDS; i++)
        Dst[i] &= Src[i];
}

static void IrDaCopy(unsigned *Dst, const unsigned *Src)
{
    int i;
    for (i = 0; i < IR_DA_WORDS; i++)
        Dst[i] = Src[i];
}

static void IrDaKillLhs(IrExpr *Lhs, unsigned *Def)
{
    if (Lhs != NULL && Lhs->Kind == IrExIdent && Lhs->LocalSlot >= 0)
        IrDaSet(Def, Lhs->LocalSlot);
}

static void IrDaExpr(IrExpr *E, unsigned *Def)
{
    int i;
    if (E == NULL)
        return;
    if (E->Kind == IrExAssign)
    {
        IrDaExpr(E->Right, Def);
        IrDaExpr(E->Left, Def);
        IrDaKillLhs(E->Left, Def);
        return;
    }
    if (E->Kind == IrExUnary
        && (E->Op == TokenIncrement || E->Op == TokenDecrement))
    {
        IrDaExpr(E->Left, Def);
        IrDaKillLhs(E->Left, Def);
        return;
    }
    if (E->Kind == IrExIdent)
    {
        if (E->ParamSlot >= 0)
            E->TypeFlags |= IR_TF_DEFINITELY_INIT;
        else if (E->LocalSlot >= 0 && IrDaGet(Def, E->LocalSlot))
            E->TypeFlags |= IR_TF_DEFINITELY_INIT;
        return;
    }
    IrDaExpr(E->Left, Def);
    IrDaExpr(E->Right, Def);
    if (E->Args != NULL)
    {
        for (i = 0; i < E->ArgCount; i++)
            IrDaExpr(E->Args[i], Def);
    }
}

static void IrDaStmt(IrStmt *S, unsigned *Def)
{
    int i;
    unsigned ThenDef[IR_DA_WORDS], ElseDef[IR_DA_WORDS];
    if (S == NULL)
        return;
    switch (S->Kind)
    {
        case IrStDecl:
            if (S->LocalSlot >= 0
                && (S->DeclIsStatic || S->DeclInitCount > 0 || S->DeclInitVals != NULL))
                IrDaSet(Def, S->LocalSlot);
            if (S->DeclInitExprs != NULL)
            {
                for (i = 0; i < S->DeclInitCount; i++)
                    IrDaExpr(S->DeclInitExprs[i], Def);
            }
            return;
        case IrStExpr:
        case IrStReturn:
            IrDaExpr(S->Expr, Def);
            return;
        case IrStIf:
            IrDaExpr(S->Expr, Def);
            IrDaCopy(ThenDef, Def);
            IrDaStmt(S->ThenStmt, ThenDef);
            if (S->ElseStmt != NULL)
            {
                IrDaCopy(ElseDef, Def);
                IrDaStmt(S->ElseStmt, ElseDef);
                IrDaCopy(Def, ThenDef);
                IrDaAnd(Def, ElseDef);
            }
            /* no else: defs from then do not survive */
            return;
        case IrStWhile:
        case IrStDo:
            IrDaExpr(S->Expr, Def);
            IrDaCopy(ThenDef, Def);
            IrDaStmt(S->BodyStmt, ThenDef); /* discard body defs */
            return;
        case IrStFor:
            IrDaStmt(S->InitStmt, Def);
            IrDaExpr(S->Expr, Def);
            IrDaCopy(ThenDef, Def);
            IrDaStmt(S->BodyStmt, ThenDef);
            IrDaStmt(S->IncStmt, ThenDef); /* discard */
            return;
        case IrStBlock:
            if (S->Stmts != NULL)
            {
                for (i = 0; i < S->StmtCount; i++)
                    IrDaStmt(S->Stmts[i], Def);
            }
            return;
        default:
            IrDaExpr(S->Expr, Def);
            IrDaStmt(S->ThenStmt, Def);
            IrDaStmt(S->ElseStmt, Def);
            IrDaStmt(S->BodyStmt, Def);
            IrDaStmt(S->InitStmt, Def);
            IrDaStmt(S->IncStmt, Def);
            if (S->Stmts != NULL)
            {
                for (i = 0; i < S->StmtCount; i++)
                    IrDaStmt(S->Stmts[i], Def);
            }
            return;
    }
}

static void IrDaFunction(IrStmt *Body)
{
    unsigned Def[IR_DA_WORDS];
    int i;
    for (i = 0; i < IR_DA_WORDS; i++)
        Def[i] = 0;
    /* params are defined at entry — stamped on each Ident use via ParamSlot */
    IrDaStmt(Body, Def);
}


/* Point3b: BodyIr → frame shape (filled at compile, consumed by ir_exec Meta). */
#define IR_BODY_FRAME_MAX 1024
typedef struct IrBodyFrameEntry {
    void *Body;
    short NumLocals;
    short NumParams;
    unsigned char SlotKind[IR_COMPILE_MAX_LOCALS];
    unsigned char SlotWidth[IR_COMPILE_MAX_LOCALS];
} IrBodyFrameEntry;
static IrBodyFrameEntry IrBodyFrames[IR_BODY_FRAME_MAX];
static int IrBodyFrameCount = 0;

void IrBodyFrameRegister(void *Body, int NumLocals, int NumParams)
{
    int i, n;
    IrBodyFrameEntry *E;
    if (Body == NULL)
        return;
    n = NumLocals;
    if (n < 0) n = 0;
    if (n > IR_COMPILE_MAX_LOCALS) n = IR_COMPILE_MAX_LOCALS;
    if (NumParams < 0) NumParams = 0;
    /* update if same Body recompiled */
    for (i = 0; i < IrBodyFrameCount; i++)
    {
        if (IrBodyFrames[i].Body == Body)
        {
            E = &IrBodyFrames[i];
            goto fill;
        }
    }
    if (IrBodyFrameCount >= IR_BODY_FRAME_MAX)
        return;
    E = &IrBodyFrames[IrBodyFrameCount++];
    E->Body = Body;
fill:
    E->NumLocals = (short)n;
    E->NumParams = (short)NumParams;
    for (i = 0; i < n; i++)
    {
        /* IrCompileLocalKind: 1=int 2=ptr 3=fp 4=int[] 5=fp[] */
        unsigned char ck = IrCompileLocalKind[i];
        unsigned char k = IR_FRAME_SLOT_UNK;
        unsigned char w = 4;
        if (ck == 1) { k = IR_FRAME_SLOT_INT; w = 4; }
        else if (ck == 2) { k = IR_FRAME_SLOT_PTR; w = (unsigned char)sizeof(void *); }
        else if (ck == 3) { k = IR_FRAME_SLOT_FP; w = (unsigned char)sizeof(double); }
        else if (ck == 4 || ck == 5)
        {
            /* arrays need arena — mark UNK so TplReady stays off */
            k = IR_FRAME_SLOT_UNK;
            w = (ck == 5) ? (unsigned char)sizeof(double) : 4;
        }
        else if (IrCompileLocalScale[i] > 0 || IrCompileLocalElemSize[i] == (int)sizeof(void *))
        { k = IR_FRAME_SLOT_PTR; w = (unsigned char)sizeof(void *); }
        else
        { k = IR_FRAME_SLOT_INT; w = 4; }
        E->SlotKind[i] = k;
        E->SlotWidth[i] = w;
    }
}

int IrBodyFrameLookup(void *Body, short *NumLocalsOut, short *NumParamsOut,
                      unsigned char *SlotKind, unsigned char *SlotWidth)
{
    int i, j;
    if (Body == NULL)
        return 0;
    for (i = 0; i < IrBodyFrameCount; i++)
    {
        if (IrBodyFrames[i].Body == Body)
        {
            if (NumLocalsOut) *NumLocalsOut = IrBodyFrames[i].NumLocals;
            if (NumParamsOut) *NumParamsOut = IrBodyFrames[i].NumParams;
            if (SlotKind != NULL || SlotWidth != NULL)
            {
                int n = IrBodyFrames[i].NumLocals;
                if (n > IR_COMPILE_MAX_LOCALS) n = IR_COMPILE_MAX_LOCALS;
                for (j = 0; j < n; j++)
                {
                    if (SlotKind) SlotKind[j] = IrBodyFrames[i].SlotKind[j];
                    if (SlotWidth) SlotWidth[j] = IrBodyFrames[i].SlotWidth[j];
                }
            }
            return 1;
        }
    }
    return 0;
}


/* Point4: walk tree and bind globals/intrinsics into CachedVal after annotate. */
static void IrBindGlobalsExpr(struct ParseState *P, IrExpr *E)
{
    int i;
    if (E == NULL) return;
    IrBindGlobalsExpr(P, E->Left);
    IrBindGlobalsExpr(P, E->Right);
    if (E->Args)
        for (i = 0; i < E->ArgCount; i++)
            IrBindGlobalsExpr(P, E->Args[i]);
    if (E->Kind == IrExIdent && E->Ident != NULL
        && E->LocalSlot < 0 && E->ParamSlot < 0)
        IrBindIdentSlots(P, E);
}
/* F3: file-scope names already in GlobalTable — fill FastCond/Inc caches
 * so the first iteration does not IrVarTableGet. Locals (slot >= 0) skipped. */
static void IrBindNameCache(struct ParseState *P, const char *Name, int Slot,
                            struct Value **Cache)
{
    struct Value *GV = NULL;
    if (Name == NULL || Cache == NULL || *Cache != NULL || Slot >= 0)
        return;
    if (P == NULL || P->pc == NULL || !VariableDefined(P->pc, Name))
        return;
    if (TableGet(&P->pc->GlobalTable, Name, &GV, NULL, NULL, NULL)
        && GV != NULL && !GV->OutOfScope && GV->Typ != NULL
        && GV->Typ->Base != TypeFunction)
        *Cache = GV;
}
static void IrBindGlobalsStmt(struct ParseState *P, IrStmt *S)
{
    int i;
    if (S == NULL) return;
    IrBindGlobalsExpr(P, S->Expr);
    IrBindGlobalsStmt(P, S->ThenStmt);
    IrBindGlobalsStmt(P, S->ElseStmt);
    IrBindGlobalsStmt(P, S->BodyStmt);
    IrBindGlobalsStmt(P, S->InitStmt);
    IrBindGlobalsStmt(P, S->IncStmt);
    if (S->Stmts)
        for (i = 0; i < S->StmtCount; i++)
            IrBindGlobalsStmt(P, S->Stmts[i]);
    if (S->FastCond.Active)
    {
        IrBindNameCache(P, S->FastCond.VarName, S->FastCond.VarSlot, &S->FastCond.CachedVar);
        IrBindNameCache(P, S->FastCond.BoundVarName, S->FastCond.BoundSlot, &S->FastCond.CachedBound);
    }
    if (S->FastInc.Active)
        IrBindNameCache(P, S->FastInc.VarName, S->FastInc.VarSlot, &S->FastInc.CachedVar);
}

void *IrCompileFunctionBody(struct ParseState *BodyStart)
{
    IrCompileAllocRecord *TrackMark = IrCompileAllocTop;
    int WasTracking = IrCompileTrackActive;
    IrCompileTrackActive = 1;
    IrCompileWhy = NULL;
    IrCompilePc = (BodyStart != NULL) ? BodyStart->pc : NULL;
    IrStmt *Body = IrCompileBody(BodyStart, NULL);
    if (Body != NULL)
    {
        IrDaFunction(Body);
        IrVerifyBody(Body, NULL); /* F2: BABYC_IR_VERIFY */
        /* Point4: bind globals/intrinsics still unbound after slot assign. */
        if (BodyStart != NULL)
            IrBindGlobalsStmt(BodyStart, Body);
        /* Point3b: publish frame shape for executor Meta (TplNLocals at compile). */
        IrBodyFrameRegister(Body, IrCompileNumLocals, IrCompileNumParams);
        PicocIrBodyCompiled++;
    }
    else {
        PicocIrBodyTokenFallback++;
        if (IrCompileWhy == NULL)
            IrCompileWhy = "stmt/expr not supported in Body-IR";
    }
    IrCompilePc = NULL;
    /* If compilation failed, free every allocation made by this attempt,
     * including partial nodes and unattached growable vectors.  On success the
     * IR owns those allocations, so only discard their tracking records. */
    IrCompileTrackEnd(TrackMark, Body != NULL);
    IrCompileTrackActive = WasTracking;
    return Body;
}

