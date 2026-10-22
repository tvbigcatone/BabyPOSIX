/*  */
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>

#include "../interpreter.h"

/* stdarg cursor bridge used by v* formatted I/O. */
struct Value *StdargVaArgValue(struct ParseState *, struct Value *, struct ValueType *);
void IrFrameMarkAddressInitialized(struct ParseState *, void *, int);

#define MAX_FORMAT (80)
#define MAX_SCANF_ARGS (10)

static int Stdio_ZeroValue = 0;
static int EOFValue = EOF;
static int BUFSIZValue = BUFSIZ;
static int FILENAME_MAXValue = FILENAME_MAX;
static int _IOFBFValue = _IOFBF;
static int _IOLBFValue = _IOLBF;
static int _IONBFValue = _IONBF;
static int L_tmpnamValue = L_tmpnam;
static int GETS_MAXValue = 255;  /* arbitrary maximum size of a gets() file */

static FILE *stdinValue;
static FILE *stdoutValue;
static FILE *stderrValue;


/* our own internal output stream which can output to FILE * or strings */
typedef struct StdOutStreamStruct
{
    FILE *FilePtr;
    char *StrOutPtr;
    int StrOutLen;
    int CharCount;

} StdOutStream;

/* our representation of varargs within picoc */
struct StdVararg
{
    struct Value **Param;
    int NumArgs;
    void *Cursor; /* non-NULL for v* calls receiving BabyC va_list */
    int CursorIndex;
};

/* initializes the I/O system so error reporting works */
void BasicIOInit(Picoc *pc)
{
    pc->CStdOut = stdout;
    stdinValue = stdin;
    stdoutValue = stdout;
    stderrValue = stderr;
}

/* output a single character to either a FILE * or a string */
void StdioOutPutc(int OutCh, StdOutStream *Stream)
{
    if (Stream->FilePtr != NULL) {
        /* output to stdio stream */
        putc(OutCh, Stream->FilePtr);
        Stream->CharCount++;
    } else {
        /* string target: write while room for data (keep 1 byte for NUL when bounded) */
        if (Stream->StrOutLen < 0 || Stream->StrOutLen > 1) {
            *Stream->StrOutPtr = (char)OutCh;
            Stream->StrOutPtr++;
            if (Stream->StrOutLen > 1)
                Stream->StrOutLen--;
        }
        /* C99 snprintf: count every char that would have been written */
        Stream->CharCount++;
    }
}

/* output a string to either a FILE * or a string */
void StdioOutPuts(const char *Str, StdOutStream *Stream)
{
    if (Str == NULL)
        Str = "(null)";
    if (Stream->FilePtr != NULL) {
        fputs(Str, Stream->FilePtr);
        while (*Str != '\0') {
            Stream->CharCount++;
            Str++;
        }
        return;
    }
    /* string target */
    while (*Str != '\0') {
        if (Stream->StrOutLen < 0 || Stream->StrOutLen > 1) {
            *Stream->StrOutPtr = *Str;
            Stream->StrOutPtr++;
            if (Stream->StrOutLen > 1)
                Stream->StrOutLen--;
        }
        Stream->CharCount++; /* C99 would-be length */
        Str++;
    }
}

/* printf-style format of an int or other word-sized object */
void StdioFprintfWord(StdOutStream *Stream, const char *Format, unsigned int Value)
{
    char tmp[128];
    int n = sprintf(tmp, Format, Value);
    if (n < 0)
        n = 0;
    if (Stream->FilePtr != NULL) {
        fputs(tmp, Stream->FilePtr);
        Stream->CharCount += n;
    } else
        StdioOutPuts(tmp, Stream);
}

/* printf-style format of a long */
void StdioFprintfLong(StdOutStream *Stream, const char *Format, uint64_t Value) {
    char PlatformFormat[MAX_FORMAT+1], *FPos = PlatformFormat;

    while (*Format) {
        char *UseFormat = NULL;

        switch (*Format) {
        case 'd':
            UseFormat = PRId64;
            break;
        case 'i':
            UseFormat = PRIi64;
            break;
        case 'o':
            UseFormat = PRIo64;
            break;
        case 'u':
            UseFormat = PRIu64;
            break;
        case 'x':
            UseFormat = PRIx64;
            break;
        case 'X':
            UseFormat = PRIX64;
            break;
        /* Ignore the %l (long) specifier, because of course we're doing longs in this function */
        case 'l':
            break;
        default:
            *FPos++ = *Format;
            break;
        }
        ++Format;
        if (UseFormat) {
            strcpy(FPos, UseFormat);
            FPos += strlen(UseFormat);
        }
    }

    {
        char tmp[160];
        int n = sprintf(tmp, PlatformFormat, Value);
        if (n < 0)
            n = 0;
        if (Stream->FilePtr != NULL) {
            fputs(tmp, Stream->FilePtr);
            Stream->CharCount += n;
        } else
            StdioOutPuts(tmp, Stream);
    }
}

/* printf-style format of a floating point number */
void StdioFprintfFP(StdOutStream *Stream, const char *Format, double Value)
{
    char tmp[160];
    int n = sprintf(tmp, Format, Value);
    if (n < 0)
        n = 0;
    if (Stream->FilePtr != NULL) {
        fputs(tmp, Stream->FilePtr);
        Stream->CharCount += n;
    } else
        StdioOutPuts(tmp, Stream);
}

/* printf-style format of a pointer */
void StdioFprintfPointer(StdOutStream *Stream, const char *Format, void *Value)
{
    char tmp[160];
    int n;
    /* %s vs %p: host sprintf handles both when Format is the OneFormatBuf */
    if (Format != NULL && Format[0] == '%' && (Format[1] == 's' || strstr(Format, "s") != NULL)
        && (strchr(Format, 's') != NULL) && (strchr(Format, 'p') == NULL))
    {
        /* string: copy via OutPuts so truncation + would-be length are C99 */
        const char *s = (const char *)Value;
        if (s == NULL)
            s = "(null)";
        /* If Format is not plain "%s", fall back to sprintf into tmp */
        if (strcmp(Format, "%s") == 0) {
            StdioOutPuts(s, Stream);
            return;
        }
    }
    n = sprintf(tmp, Format, Value);
    if (n < 0)
        n = 0;
    if (Stream->FilePtr != NULL) {
        fputs(tmp, Stream->FilePtr);
        Stream->CharCount += n;
    } else
        StdioOutPuts(tmp, Stream);
}

/* internal do-anything v[s][n]printf() formatting system with output
    to strings or FILE * */
static struct Value *StdioVarargNext(struct ParseState *Parser,
    struct StdVararg *Args, int Index, struct ValueType *RequestedType)
{
    if (Args == NULL) return NULL;
    if (Args->Cursor != NULL)
    {
        union AnyValue ListStorage;
        struct Value List;
        (void)Index;
        memset(&ListStorage, 0, sizeof(ListStorage));
        memset(&List, 0, sizeof(List));
        List.Val = &ListStorage;
        List.Typ = Parser->pc->VoidPtrType;
        ListStorage.Pointer = Args->Cursor;
        return StdargVaArgValue(Parser, &List, RequestedType);
    }
    if (Args->Param == NULL || Index < 1 || Index > Args->NumArgs) return NULL;
    return Args->Param[Index];
}

int StdioBasePrintf(struct ParseState *Parser, FILE *Stream, char *StrOut,
    int StrOutLen, char *Format, struct StdVararg *Args)
{
    /* Args->Param[0] = format string Value*; data args at Param[1..NumArgs].
     * Always index the Param[] pointer array — do NOT assume Values are
     * contiguous in memory (IR packs them sequentially, but sizes/alignment
     * differ across MSVC vs GCC and LP64 vs LLP64). */
    struct Value *ThisArg = NULL;
    /* Next data-arg index into Args->Param[]. Param[0] is format Value*;
     * data args are Param[1], Param[2], ... up to Param[NumArgs].
     * NumArgs is the count of data args (not including format). */
    int ArgCount = 1;
    char *FPos;
    char OneFormatBuf[MAX_FORMAT+1];
    int OneFormatCount;
	int ShowLong = 0; /* 0=int, 1=long (%l), 2=long long (%ll) */
	int ShowShort = 0; /* 0=int, 1=short (%h), 2=char (%hh) */
    struct ValueType *ShowType;
    StdOutStream SOStream;
    Picoc *pc = Parser->pc;

    if (Format == NULL)
        ProgramFail(Parser, "printf: format string is NULL (undefined behavior in C)");
    if (Args == NULL || (Args->NumArgs > 0 && Args->Param == NULL && Args->Cursor == NULL))
        ProgramFail(Parser, "printf: invalid argument list");

    FPos = Format;
    SOStream.FilePtr = Stream;
    SOStream.StrOutPtr = StrOut;
    SOStream.StrOutLen = StrOutLen;
    SOStream.CharCount = 0;

    while (*FPos != '\0') {
        if (*FPos == '%') {
            /* work out what type we're printing */
            FPos++;
            ShowType = NULL;
            ShowLong = 0;
            ShowShort = 0;
            OneFormatBuf[0] = '%';
            OneFormatCount = 1;

            do {
                switch (*FPos) {
                case 'd':
                case 'i':
                    /* %d / %ld / %lld  (ShowLong: 0=int, 1=long, 2=long long) */
                    if (ShowLong >= 2)
                        ShowType = &pc->LongLongType;
                    else if (ShowLong == 1)
                        ShowType = &pc->LongType;
                    else
                        ShowType = &pc->IntType; /* %hd/%hhd: host printf sees length in fmt */
                    ShowLong = 0;
                    ShowShort = 0;
                    break;
                case 'u':
                    if (ShowLong >= 2)
                        ShowType = &pc->UnsignedLongLongType;
                    else if (ShowLong == 1)
                        ShowType = &pc->UnsignedLongType;
                    else
                        ShowType = &pc->UnsignedIntType;
                    ShowLong = 0;
                    break;
                case 'o':
                case 'x':
                case 'X':
                    /* same length hierarchy as %u */
                    if (ShowLong >= 2)
                        ShowType = &pc->UnsignedLongLongType;
                    else if (ShowLong == 1)
                        ShowType = &pc->UnsignedLongType;
                    else
                        ShowType = &pc->IntType;
                    ShowLong = 0;
                    break; /* integer base conversions */
                case 'l':
                    /* first l → long; second l → long long (%lld / %llu) */
                    if (ShowLong < 2)
                        ShowLong++;
                    break; /* long / long long integer */
                case 'h':
                    /* first h → short; second h → char (%hd / %hhd) */
                    if (ShowShort < 2)
                        ShowShort++;
                    break;
                case '*':
                    /* Dynamic width or precision: consume next data arg and
                     * splice decimal digits into OneFormatBuf (replacing '*').
                     * Args->Param[0]=format; data at Param[1..NumArgs];
                     * ArgCount is the next index to consume. */
                    if (Args != NULL && (Args->Param != NULL || Args->Cursor != NULL)
                        && ArgCount <= Args->NumArgs) {
                        int StarVal;
                        int IsPrecision = (FPos > Format && FPos[-1] == '.');
                        char Digs[16];
                        int di = 0, dv;
                        ThisArg = StdioVarargNext(Parser, Args, ArgCount++, &pc->IntType);
                        StarVal = (ThisArg != NULL && ThisArg->Typ != NULL
                                   && IS_NUMERIC_COERCIBLE(ThisArg))
                            ? (int)ExpressionCoerceInteger(ThisArg) : 0;
                        /* C: negative width means left adjustment; negative
                         * precision is treated as if precision were omitted. */
                        if (IsPrecision && StarVal < 0) {
                            if (OneFormatCount > 1 && OneFormatBuf[OneFormatCount - 1] == '.')
                                --OneFormatCount;
                            break;
                        }
                        if (!IsPrecision && StarVal < 0) {
                            int k, HasMinus = 0;
                            for (k = 1; k < OneFormatCount; ++k)
                                if (OneFormatBuf[k] == '-') HasMinus = 1;
                            if (!HasMinus && OneFormatCount + 1 < MAX_FORMAT) {
                                OneFormatBuf[OneFormatCount++] = '-';
                            }
                            StarVal = (StarVal == (-2147483647 - 1)) ? 512 : -StarVal;
                        }
                        if (StarVal > 512) StarVal = 512;
                        dv = StarVal;
                        if (dv == 0) Digs[di++] = '0';
                        else {
                            char tmp[16]; int ti = 0;
                            while (dv > 0 && ti < 15) { tmp[ti++] = '0' + (dv % 10); dv /= 10; }
                            while (ti > 0) Digs[di++] = tmp[--ti];
                        }
                        if (OneFormatCount + di < MAX_FORMAT) {
                            int k;
                            for (k = 0; k < di; k++)
                                OneFormatBuf[OneFormatCount++] = Digs[k];
                        }
                    }
                    break;
                case 'e':
                case 'E':
                    ShowType = &pc->FPType;
                    break;      /* double, exponent form */
                case 'f':
                case 'F':
                    ShowType = &pc->FPType;
                    break;      /* double, fixed-point */
                case 'g':
                case 'G':
                    ShowType = &pc->FPType;
                    break;      /* double, flexible format */
                case 'a':
                case 'A':
                    ShowType = &pc->IntType;
                    break;     /* hexadecimal, 0x- format */
                case 'c':
                    ShowType = &pc->IntType;
                break;     /* character */
                case 's':
                    ShowType = pc->CharPtrType;
                    break;  /* string */
                case 'p':
                    ShowType = pc->VoidPtrType;
                    break;  /* pointer */
                case 'n':
                    ShowType = &pc->VoidType;
                    break;    /* number of characters written */
                case 'm':
                    ShowType = &pc->VoidType;
                    break;    /* strerror(errno) */
                case '%':
                    ShowType = &pc->VoidType;
                    break;    /* just a '%' character */
                case '\0':
                    ShowType = &pc->VoidType;
                    break;    /* end of format string */
                }

                /* copy one character of format across to the OneFormatBuf
                 * (skip 'l' length and '*' which already spliced digits) */
                if (*FPos != 'l' && *FPos != '*') {
                    /* keep 'h' so host printf sees %hd / %hhd */
                    OneFormatBuf[OneFormatCount] = *FPos;
                    OneFormatCount++;
                }
                /* do special actions depending on the conversion type */
                if (ShowType == &pc->VoidType) {
                    switch (*FPos) {
                    case 'm':
                        StdioOutPuts(strerror(errno), &SOStream);
                        break;
                    case '%':
                        StdioOutPutc(*FPos, &SOStream);
                        break;
                    case '\0':
                        OneFormatBuf[OneFormatCount] = '\0';
                        StdioOutPutc(*FPos, &SOStream);
                        break;
                    case 'n':
                        /* store SOStream.CharCount; size from %n / %hn / %ln / %lln */
                        ThisArg = (Args != NULL
                                   && (Args->Param != NULL || Args->Cursor != NULL)
                                   && ArgCount <= Args->NumArgs)
                            ? StdioVarargNext(Parser, Args, ArgCount++, pc->VoidPtrType) : NULL;
                        if (ThisArg == NULL || ThisArg->Val == NULL)
                            ProgramFail(Parser, "printf: missing pointer argument for %%n (undefined behavior in C)");
                        {
                            void *dst = NULL;
                            if (ThisArg->Typ != NULL && ThisArg->Typ->Base == TypePointer)
                                dst = ThisArg->Val->Pointer;
                            else if (ThisArg->Typ != NULL && ThisArg->Typ->Base == TypeArray)
                                dst = (void *)ThisArg->Val;
                            else if (ThisArg->Typ != NULL
                                     && (ThisArg->Typ->Base == TypePointer
                                         || ThisArg->Typ == pc->VoidPtrType
                                         || ThisArg->Typ == pc->CharPtrType))
                                dst = ThisArg->Val->Pointer;
                            /* CharPtrType packs from IR: still a pointer value */
                            if (dst == NULL && ThisArg->Val != NULL)
                                dst = ThisArg->Val->Pointer;
                            if (dst == NULL)
                                ProgramFail(Parser, "printf: NULL pointer argument for %%n (undefined behavior in C)");
                            {
                                if (ShowLong >= 2)
                                    *(long long *)dst = (long long)SOStream.CharCount;
                                else if (ShowLong == 1)
                                    *(long *)dst = (long)SOStream.CharCount;
                                else if (ShowShort >= 2)
                                    *(signed char *)dst = (signed char)SOStream.CharCount;
                                else if (ShowShort == 1)
                                    *(short *)dst = (short)SOStream.CharCount;
                                else
                                    *(int *)dst = (int)SOStream.CharCount;
                            }
                        }
                        ShowLong = 0;
                        ShowShort = 0;
                        break;
                    }
                }

                FPos++;

            } while (ShowType == NULL && OneFormatCount < MAX_FORMAT);

            if (ShowType == NULL)
                ProgramFail(Parser, "printf: invalid or incomplete format conversion (undefined behavior in C)");
            if (ShowType != &pc->VoidType) {
                if (Args == NULL || (Args->Param == NULL && Args->Cursor == NULL)
                    || ArgCount > Args->NumArgs)
                    ProgramFail(Parser, "printf: missing argument for format conversion (undefined behavior in C)");
                else {
                    /* null-terminate the buffer */
                    OneFormatBuf[OneFormatCount] = '\0';

                    /* next data arg: Param[0]=format, Param[1..NumArgs]=values;
                     * ArgCount is next index (starts at 1) */
                    ThisArg = StdioVarargNext(Parser, Args, ArgCount++, ShowType);
                    if (ThisArg == NULL || ThisArg->Typ == NULL || ThisArg->Val == NULL) {
                        ProgramFail(Parser, "printf: invalid argument for format conversion (undefined behavior in C)");
                    }

                    /* Integer output: always coerce through ExpressionCoerce* so
                     * the full host width is used. This is compile-time portable:
                     *   LP64  (Linux x86_64): long == long long == 64
                     *   LLP64 (Windows x64):  long == 32, long long == 64
                     *   ILP32 (Win32/Linux32): long == 32, long long == 64
                     * %lld/%llu always select LongLong* types (see format parse).
                     * Never read Val->LongInteger for a long-long arg on LLP64 —
                     * that only sees the low 32 bits of the union. */
                    if (ShowType == &pc->LongLongType
                        || ShowType == &pc->UnsignedLongLongType
                        || ShowType == &pc->LongType
                        || ShowType == &pc->UnsignedLongType) {
                        if (IS_NUMERIC_COERCIBLE(ThisArg))
                            StdioFprintfLong(&SOStream, OneFormatBuf,
                                (uint64_t)ExpressionCoerceUnsignedInteger(ThisArg));
                        else
                            ProgramFail(Parser, "printf: argument type does not match format conversion (undefined behavior in C)");
                    } else if (ShowType == &pc->UnsignedIntType
                               || ShowType == &pc->IntType) {
                        if (IS_NUMERIC_COERCIBLE(ThisArg))
                            StdioFprintfWord(&SOStream, OneFormatBuf,
                                (unsigned int)ExpressionCoerceUnsignedInteger(ThisArg));
                        else
                            ProgramFail(Parser, "printf: argument type does not match format conversion (undefined behavior in C)");
                    } else if ((ShowType != NULL && ShowType->Base == TypeFP)) {
                        /* show a floating point number */
                        if (IS_NUMERIC_COERCIBLE(ThisArg))
                            StdioFprintfFP(&SOStream, OneFormatBuf,
                                ExpressionCoerceFP(ThisArg));
                        else
                            ProgramFail(Parser, "printf: argument type does not match format conversion (undefined behavior in C)");
                    } else if (ShowType == pc->CharPtrType) {
                        if (ThisArg->Typ->Base == TypePointer) {
                            if (ThisArg->Val->Pointer == NULL)
                                ProgramFail(Parser, "printf: NULL string argument (undefined behavior in C)");
                            StdioFprintfPointer(&SOStream, OneFormatBuf,
                                ThisArg->Val->Pointer);
                        }
                        else if (ThisArg->Typ->Base == TypeArray &&
                                    ThisArg->Typ->FromType->Base == TypeChar)
                            StdioFprintfPointer(&SOStream, OneFormatBuf,
                                (char *)ThisArg->Val);
                        else
                            ProgramFail(Parser, "printf: argument type does not match format conversion (undefined behavior in C)");
                    } else if (ShowType == pc->VoidPtrType) {
                        if (ThisArg->Typ->Base == TypePointer)
                            StdioFprintfPointer(&SOStream, OneFormatBuf,
                                ThisArg->Val->Pointer);
                        else if (ThisArg->Typ->Base == TypeArray)
                            StdioFprintfPointer(&SOStream, OneFormatBuf,
                                (char *)ThisArg->Val);
                        else
                            ProgramFail(Parser, "printf: argument type does not match format conversion (undefined behavior in C)");
                    }

                }
            }
        } else {
            /* just output a normal character */
            StdioOutPutc(*FPos, &SOStream);
            FPos++;
        }
    }

    /* null-terminate */
    /* sprintf uses StrOutLen=-1 (unbounded); still need NUL terminator */
    if (SOStream.StrOutPtr != NULL && (SOStream.StrOutLen > 0 || SOStream.StrOutLen < 0))
        *SOStream.StrOutPtr = '\0';

    return SOStream.CharCount;
}


/* PicoC maps float and double to the same TypeFP (sizeof double). Host
 * scanf("%f") writes a 4-byte float into that 8-byte slot → value reads as 0.
 * Rewrite short float conversions to the long (double) form before calling libc. */
static char *StdioScanfPromoteFloatFmt(struct ParseState *Parser, const char *Fmt)
{
    size_t n, i, o;
    char *Out;
    if (Fmt == NULL)
        return NULL;
    n = strlen(Fmt);
    /* Each floating conversion may gain one byte when mapped to the
     * interpreter's double-sized FP execution representation. */
    Out = HeapAllocStack(Parser->pc, (int)n * 2 + 4);
    if (Out == NULL)
        ProgramFail(Parser, DiagnosticMsgOutOfMemory);
    for (i = 0, o = 0; i < n; )
    {
        char length = '\0';
        char conversion;
        if (Fmt[i] != '%')
        {
            Out[o++] = Fmt[i++];
            continue;
        }
        Out[o++] = Fmt[i++];
        if (i < n && Fmt[i] == '%')
        {
            Out[o++] = Fmt[i++];
            continue;
        }
        /* scanf has assignment suppression and field width, not printf flags
         * or precision. The validator rejects unsupported grammar first. */
        if (i < n && Fmt[i] == '*') Out[o++] = Fmt[i++];
        while (i < n && Fmt[i] >= '0' && Fmt[i] <= '9') Out[o++] = Fmt[i++];
        if (i < n && (Fmt[i] == 'h' || Fmt[i] == 'l' || Fmt[i] == 'L' ||
                      Fmt[i] == 'j' || Fmt[i] == 'z' || Fmt[i] == 't'))
        {
            length = Fmt[i];
            Out[o++] = Fmt[i++];
            if ((length == 'h' || length == 'l') && i < n && Fmt[i] == length)
                Out[o++] = Fmt[i++];
        }
        if (i >= n) break; /* The validator reports the incomplete conversion. */
        conversion = Fmt[i++];
        if (strchr("aAeEfFgG", conversion) != NULL)
        {
            /* BabyC currently represents long double with the same FP object
             * storage as double. Passing %Lf to host scanf would write a host
             * long double (often 16 bytes) into that smaller guest object. */
            if (length == '\0' || length == 'L')
            {
                if (length == 'L') Out[o - 1] = 'l';
                else Out[o++] = 'l';
            }
        }
        Out[o++] = conversion;
    }
    Out[o] = '\0';
    return Out;
}

/* internal do-anything v[s][n]scanf() formatting system with input
    from strings or FILE * */
/* Validate the scanf grammar before passing a guest format to the host CRT.
 * This catches malformed conversions and missing destination arguments. Type
 * and object-size validation needs provenance metadata and is handled separately. */
static int StdioScanfRequiredArgs(struct ParseState *Parser, const char *Fmt)
{
    const char *p = Fmt;
    int required = 0;
    if (Fmt == NULL)
        ProgramFail(Parser, "scanf: format string is NULL (undefined behavior in C)");
    while (*p != '\0') {
        int suppressed = 0, had_width = 0;
        char length = '\0', conversion;
        if (*p++ != '%')
            continue;
        if (*p == '%') { ++p; continue; }
        if (*p == '\0')
            ProgramFail(Parser, "scanf: incomplete format conversion (undefined behavior in C)");
        if (*p == '*') { suppressed = 1; ++p; }
        while (*p >= '0' && *p <= '9') { had_width = 1; ++p; }
        if (*p == 'h' || *p == 'l' || *p == 'L' || *p == 'j' ||
            *p == 'z' || *p == 't') {
            length = *p++;
            if ((length == 'h' || length == 'l') && *p == length) {
                ++p;
                length = (length == 'h') ? 'H' : 'q'; /* hh / ll */
            }
        }
        if (*p == '\0')
            ProgramFail(Parser, "scanf: incomplete format conversion (undefined behavior in C)");
        if (*p == '[') {
            conversion = '[';
            ++p;
            if (*p == '^') ++p;
            if (*p == ']') ++p;
            while (*p != '\0' && *p != ']') ++p;
            if (*p != ']')
                ProgramFail(Parser, "scanf: unterminated scanset (undefined behavior in C)");
        } else {
            conversion = *p;
            if (!strchr("diouxXfFeEgGaAcspn", conversion))
                ProgramFail(Parser, "scanf: invalid format conversion '%%%c' (undefined behavior in C)", conversion);
        }
        ++p;

        /* Reject length/conversion combinations that make the host variadic
         * scanf interpret a destination pointer with the wrong type/size. */
        if (strchr("diouxXn", conversion) != NULL) {
            if (length != '\0' && length != 'h' && length != 'H' &&
                length != 'l' && length != 'q' && length != 'j' &&
                length != 'z' && length != 't')
                ProgramFail(Parser, "scanf: invalid length modifier for %%%c (undefined behavior in C)", conversion);
        } else if (strchr("aAeEfFgG", conversion) != NULL) {
            if (length != '\0' && length != 'l' && length != 'L')
                ProgramFail(Parser, "scanf: invalid length modifier for %%%c (undefined behavior in C)", conversion);
        } else if (conversion == 'c' || conversion == 's' || conversion == '[') {
            if (length != '\0' && length != 'l')
                ProgramFail(Parser, "scanf: invalid length modifier for %%%c (undefined behavior in C)", conversion);
        } else if (conversion == 'p') {
            if (length != '\0')
                ProgramFail(Parser, "scanf: invalid length modifier for %%p (undefined behavior in C)");
        }
        if (conversion == 'n' && (suppressed || had_width))
            ProgramFail(Parser, "scanf: assignment suppression or field width with %%n is undefined behavior in C");
        if (!suppressed) ++required;
    }
    return required;
}

int StdioBaseScanf(struct ParseState *Parser, FILE *Stream, char *StrIn,
    char *Format, struct StdVararg *Args)
{
    struct Value *ThisArg;
    struct Value *ScanfValue[MAX_SCANF_ARGS] = { NULL };
    int ArgCount = 0;
    void *ScanfArg[MAX_SCANF_ARGS] = { NULL };
    int NeededArgs;

    if (Format == NULL)
        ProgramFail(Parser, "scanf: format string is NULL (undefined behavior in C)");
    if (Stream == NULL && StrIn == NULL)
        ProgramFail(Parser, "scanf: input stream/string is NULL");
    if (Args == NULL)
        ProgramFail(Parser, "scanf: invalid argument list");
    if (Args->NumArgs > MAX_SCANF_ARGS)
        ProgramFail(Parser, "too many arguments to scanf() - %d max",
         MAX_SCANF_ARGS);
    {
        NeededArgs = StdioScanfRequiredArgs(Parser, Format);
        if (NeededArgs > Args->NumArgs)
            ProgramFail(Parser,
                "scanf: format requires %d destination arguments but only %d supplied (undefined behavior in C)",
                NeededArgs, Args->NumArgs);
    }

    /* Index Param[] like printf — do not assume Values are contiguous.
     * Args->Param[0] is format; data args at Param[1..NumArgs]. */
    for (ArgCount = 0; ArgCount < NeededArgs; ArgCount++) {
        ThisArg = (Args != NULL && Args->Cursor != NULL)
            ? StdioVarargNext(Parser, Args, ArgCount + 1, Parser->pc->VoidPtrType)
            : ((Args != NULL && Args->Param != NULL)
                ? Args->Param[ArgCount + 1] : NULL);
        if (ThisArg == NULL || ThisArg->Typ == NULL)
            ProgramFail(Parser,
                "missing argument to scanf() - argument %d after format",
                ArgCount+1);

        if (ThisArg->Typ->Base == TypePointer) {
            if (ThisArg->Val->Pointer == NULL)
                ProgramFail(Parser, "scanf: NULL destination pointer (undefined behavior in C)");
            ScanfArg[ArgCount] = ThisArg->Val->Pointer;
            /* The host CRT writes through this variadic pointer. Remember the
             * source Value so successful assignments can update init tracking. */
            ScanfValue[ArgCount] = ThisArg;
        }
        else if (ThisArg->Typ->Base == TypeArray) {
            ScanfArg[ArgCount] = (char *)ThisArg->Val;
            ScanfValue[ArgCount] = ThisArg;
        }
        else
            ProgramFail(Parser,
                "non-pointer argument to scanf() - argument %d after format",
                ArgCount+1);
    }

    {
        char *Promoted = StdioScanfPromoteFloatFmt(Parser, Format);
        int n;
        if (Stream != NULL)
            n = fscanf(Stream, Promoted, ScanfArg[0], ScanfArg[1], ScanfArg[2],
                ScanfArg[3], ScanfArg[4], ScanfArg[5], ScanfArg[6], ScanfArg[7],
                ScanfArg[8], ScanfArg[9]);
        else
            n = sscanf(StrIn, Promoted, ScanfArg[0], ScanfArg[1], ScanfArg[2],
                ScanfArg[3], ScanfArg[4], ScanfArg[5], ScanfArg[6], ScanfArg[7],
                ScanfArg[8], ScanfArg[9]);
        /* Only successful conversions write destinations. Propagate that fact
         * to Tree Value metadata and to IR frame-local init bits. */
        if (n > 0) {
            int written = n < NeededArgs ? n : NeededArgs;
            int wi;
            for (wi = 0; wi < written; ++wi) {
                if (ScanfValue[wi] != NULL)
                    ExpressionMarkPtrArgEscaped(ScanfValue[wi]);
                IrFrameMarkAddressInitialized(Parser, ScanfArg[wi], 1);
            }
        }
        return n;
    }
}

/* Shared argument guards are declared before the first wrapper so every
 * libc entry point validates before handing arguments to the host CRT. */
static void StdioRequireFile(struct ParseState *Parser, void *Fp, const char *What);
static void StdioRequireBuf(struct ParseState *Parser, void *Buf, const char *What);

/* stdio calls */
void StdioFopen(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "fopen path");
    StdioRequireBuf(Parser, Param[1]->Val->Pointer, "fopen mode");
    ReturnValue->Val->Pointer = fopen(Param[0]->Val->Pointer,
        Param[1]->Val->Pointer);
}

void StdioFreopen(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "freopen path");
    StdioRequireBuf(Parser, Param[1]->Val->Pointer, "freopen mode");
    StdioRequireFile(Parser, Param[2]->Val->Pointer, "freopen");
    ReturnValue->Val->Pointer = freopen(Param[0]->Val->Pointer,
        Param[1]->Val->Pointer, Param[2]->Val->Pointer);
}


/* Reject NULL FILE* / buffers before libc — always report like ProgramFail
 * (avoids silent CRT exit on Windows MinGW and unclean SEGV). */
static void StdioRequireFile(struct ParseState *Parser, void *Fp, const char *What)
{
    if (Fp == NULL)
        ProgramFail(Parser, "%s: FILE* is NULL", What ? What : "stdio");
}

static void StdioRequireBuf(struct ParseState *Parser, void *Buf, const char *What)
{
    if (Buf == NULL)
        ProgramFail(Parser, "%s: buffer is NULL", What ? What : "stdio");
}

void StdioFclose(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "fclose");
    ReturnValue->Val->Integer = fclose(Param[0]->Val->Pointer);
}

void StdioFread(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    if (Param[1]->Val->Integer < 0 || Param[2]->Val->Integer < 0)
        ProgramFail(Parser, "fread: negative size or count is invalid");
    if (Param[1]->Val->Integer > 0 && Param[2]->Val->Integer > 0)
        StdioRequireBuf(Parser, Param[0]->Val->Pointer, "fread");
    StdioRequireFile(Parser, Param[3]->Val->Pointer, "fread");
    ReturnValue->Val->Integer = (int)fread(Param[0]->Val->Pointer,
        (size_t)Param[1]->Val->Integer, (size_t)Param[2]->Val->Integer, Param[3]->Val->Pointer);
}

void StdioFwrite(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    if (Param[1]->Val->Integer < 0 || Param[2]->Val->Integer < 0)
        ProgramFail(Parser, "fwrite: negative size or count is invalid");
    if (Param[1]->Val->Integer > 0 && Param[2]->Val->Integer > 0)
        StdioRequireBuf(Parser, Param[0]->Val->Pointer, "fwrite");
    StdioRequireFile(Parser, Param[3]->Val->Pointer, "fwrite");
    ReturnValue->Val->Integer = (int)fwrite(Param[0]->Val->Pointer,
        (size_t)Param[1]->Val->Integer, (size_t)Param[2]->Val->Integer, Param[3]->Val->Pointer);
}

void StdioFgetc(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "fgetc");
    ReturnValue->Val->Integer = fgetc(Param[0]->Val->Pointer);
}

void StdioFgets(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    if (Param[1]->Val->Integer <= 0) ProgramFail(Parser, "fgets: buffer size must be positive");
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "fgets");
    StdioRequireFile(Parser, Param[2]->Val->Pointer, "fgets");
    ReturnValue->Val->Pointer = fgets(Param[0]->Val->Pointer,
        Param[1]->Val->Integer, Param[2]->Val->Pointer);
}

void StdioRemove(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "remove path");
    ReturnValue->Val->Integer = remove(Param[0]->Val->Pointer);
}

void StdioRename(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "rename old path");
    StdioRequireBuf(Parser, Param[1]->Val->Pointer, "rename new path");
    ReturnValue->Val->Integer = rename(Param[0]->Val->Pointer,
        Param[1]->Val->Pointer);
}

void StdioRewind(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "rewind");
    rewind(Param[0]->Val->Pointer);
}

void StdioTmpfile(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    ReturnValue->Val->Pointer = tmpfile();
}

void StdioClearerr(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "clearerr");
    clearerr((FILE *)Param[0]->Val->Pointer);
}

void StdioFeof(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "feof");
    ReturnValue->Val->Integer = feof((FILE *)Param[0]->Val->Pointer);
}

void StdioFerror(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "ferror");
    ReturnValue->Val->Integer = ferror((FILE *)Param[0]->Val->Pointer);
}

void StdioFileno(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "fileno");
#ifndef WIN32
    ReturnValue->Val->Integer = fileno(Param[0]->Val->Pointer);
#else
    ReturnValue->Val->Integer = _fileno(Param[0]->Val->Pointer);
#endif
}

void StdioFflush(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    /* fflush(NULL) flushes all streams — allowed by C */
    ReturnValue->Val->Integer = fflush(Param[0]->Val->Pointer);
}

void StdioFgetpos(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "fgetpos");
    StdioRequireBuf(Parser, Param[1]->Val->Pointer, "fgetpos position output");
    ReturnValue->Val->Integer = fgetpos(Param[0]->Val->Pointer,
        Param[1]->Val->Pointer);
}

void StdioFsetpos(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "fsetpos");
    StdioRequireBuf(Parser, Param[1]->Val->Pointer, "fsetpos position input");
    ReturnValue->Val->Integer = fsetpos(Param[0]->Val->Pointer,
        Param[1]->Val->Pointer);
}

void StdioFputc(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[1]->Val->Pointer, "fputc");
    ReturnValue->Val->Integer = fputc(Param[0]->Val->Integer,
        Param[1]->Val->Pointer);
}

void StdioFputs(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "fputs");
    StdioRequireFile(Parser, Param[1]->Val->Pointer, "fputs");
    ReturnValue->Val->Integer = fputs(Param[0]->Val->Pointer,
        Param[1]->Val->Pointer);
}

void StdioFtell(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "ftell");
    ReturnValue->Val->Integer = ftell(Param[0]->Val->Pointer);
}

void StdioFseek(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "fseek");
    if (Param[2]->Val->Integer != SEEK_SET && Param[2]->Val->Integer != SEEK_CUR && Param[2]->Val->Integer != SEEK_END)
        ProgramFail(Parser, "fseek: origin must be SEEK_SET, SEEK_CUR, or SEEK_END");
    ReturnValue->Val->Integer = fseek(Param[0]->Val->Pointer,
        Param[1]->Val->Integer, Param[2]->Val->Integer);
}

void StdioPerror(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    perror(Param[0]->Val->Pointer);
}

void StdioPutc(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[1]->Val->Pointer, "putc");
    ReturnValue->Val->Integer = putc(Param[0]->Val->Integer,
        Param[1]->Val->Pointer);
}

void StdioPutchar(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    ReturnValue->Val->Integer = putchar(Param[0]->Val->Integer);
}

void StdioSetbuf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "setbuf");
    setbuf(Param[0]->Val->Pointer, Param[1]->Val->Pointer);
}

void StdioSetvbuf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "setvbuf");
    if (Param[2]->Val->Integer != _IOFBF && Param[2]->Val->Integer != _IOLBF && Param[2]->Val->Integer != _IONBF)
        ProgramFail(Parser, "setvbuf: invalid buffering mode");
    if (Param[3]->Val->Integer < 0) ProgramFail(Parser, "setvbuf: negative buffer size is invalid");
    setvbuf(Param[0]->Val->Pointer, Param[1]->Val->Pointer,
        Param[2]->Val->Integer, Param[3]->Val->Integer);
}

void StdioUngetc(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[1]->Val->Pointer, "ungetc");
    ReturnValue->Val->Integer = ungetc(Param[0]->Val->Integer,
        Param[1]->Val->Pointer);
}

void StdioPuts(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "puts string");
    ReturnValue->Val->Integer = puts(Param[0]->Val->Pointer);
}

void StdioGets(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    (void)ReturnValue; (void)Param; (void)NumArgs;
    ProgramFail(Parser,
        "gets: disabled because the interface cannot bound destination writes; use fgets instead");
}

void StdioGetchar(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    ReturnValue->Val->Integer = getchar();
}

void StdioPrintf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "printf format");

    struct StdVararg PrintfArgs = { NULL, 0, NULL, 0 };

    PrintfArgs.Param = Param;
    PrintfArgs.NumArgs = NumArgs - 1;
    ReturnValue->Val->Integer = StdioBasePrintf(Parser, stdout, NULL, 0,
        Param[0]->Val->Pointer, &PrintfArgs);
}

void StdioVprintf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "vprintf format");

    { struct StdVararg Args; memset(&Args, 0, sizeof(Args));
      Args.Cursor = Param[1]->Val->Pointer;
      Args.NumArgs = 1024;
      ReturnValue->Val->Integer = StdioBasePrintf(Parser, stdout, NULL, 0,
          Param[0]->Val->Pointer, &Args); }
}

void StdioFprintf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[1]->Val->Pointer, "fprintf format");

    struct StdVararg PrintfArgs = { NULL, 0, NULL, 0 };

    StdioRequireFile(Parser, Param[0]->Val->Pointer, "fprintf");
    PrintfArgs.Param = Param + 1;
    PrintfArgs.NumArgs = NumArgs - 2;
    ReturnValue->Val->Integer = StdioBasePrintf(Parser, Param[0]->Val->Pointer,
        NULL, 0, Param[1]->Val->Pointer, &PrintfArgs);
}

void StdioVfprintf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "vfprintf");
    StdioRequireBuf(Parser, Param[1]->Val->Pointer, "vfprintf format");

    { struct StdVararg Args; memset(&Args, 0, sizeof(Args));
      Args.Cursor = Param[2]->Val->Pointer; Args.NumArgs = 1024;
      ReturnValue->Val->Integer = StdioBasePrintf(Parser, Param[0]->Val->Pointer,
          NULL, 0, Param[1]->Val->Pointer, &Args); }
}

void StdioSprintf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "sprintf destination");
    StdioRequireBuf(Parser, Param[1]->Val->Pointer, "sprintf format");

    struct StdVararg PrintfArgs = { NULL, 0, NULL, 0 };

    PrintfArgs.Param = Param + 1;
    PrintfArgs.NumArgs = NumArgs - 2;
    ReturnValue->Val->Integer = StdioBasePrintf(Parser, NULL,
        Param[0]->Val->Pointer, -1, Param[1]->Val->Pointer, &PrintfArgs);
}

void StdioSnprintf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    if (Param[1]->Val->Integer < 0) ProgramFail(Parser, "snprintf: negative size is invalid");
    if (Param[1]->Val->Integer > 0) StdioRequireBuf(Parser, Param[0]->Val->Pointer, "snprintf destination");
    StdioRequireBuf(Parser, Param[2]->Val->Pointer, "snprintf format");

    struct StdVararg PrintfArgs = { NULL, 0, NULL, 0 };

    PrintfArgs.Param = Param + 2;
    PrintfArgs.NumArgs = NumArgs - 3;
    ReturnValue->Val->Integer = StdioBasePrintf(Parser, NULL,
        Param[0]->Val->Pointer, Param[1]->Val->Integer, Param[2]->Val->Pointer,
        &PrintfArgs);
}

void StdioScanf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "scanf format");

    struct StdVararg ScanfArgs = { NULL, 0, NULL, 0 };

    ScanfArgs.Param = Param;
    ScanfArgs.NumArgs = NumArgs - 1;
    ReturnValue->Val->Integer = StdioBaseScanf(Parser, stdin, NULL,
        Param[0]->Val->Pointer, &ScanfArgs);
}

void StdioFscanf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "fscanf");
    StdioRequireBuf(Parser, Param[1]->Val->Pointer, "fscanf format");

    struct StdVararg ScanfArgs = { NULL, 0, NULL, 0 };

    ScanfArgs.Param = Param+1;
    ScanfArgs.NumArgs = NumArgs - 2;
    ReturnValue->Val->Integer = StdioBaseScanf(Parser, Param[0]->Val->Pointer,
        NULL, Param[1]->Val->Pointer, &ScanfArgs);
}

void StdioSscanf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "sscanf input");
    StdioRequireBuf(Parser, Param[1]->Val->Pointer, "sscanf format");

    struct StdVararg ScanfArgs = { NULL, 0, NULL, 0 };

    ScanfArgs.Param = Param+1;
    ScanfArgs.NumArgs = NumArgs - 2;
    ReturnValue->Val->Integer = StdioBaseScanf(Parser, NULL,
        Param[0]->Val->Pointer, Param[1]->Val->Pointer, &ScanfArgs);
}

void StdioVsprintf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "vsprintf destination");
    StdioRequireBuf(Parser, Param[1]->Val->Pointer, "vsprintf format");

    { struct StdVararg Args; memset(&Args, 0, sizeof(Args));
      Args.Cursor = Param[2]->Val->Pointer; Args.NumArgs = 1024;
      ReturnValue->Val->Integer = StdioBasePrintf(Parser, NULL,
          Param[0]->Val->Pointer, -1, Param[1]->Val->Pointer, &Args); }
}

void StdioVsnprintf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    if (Param[1]->Val->Integer < 0) ProgramFail(Parser, "vsnprintf: negative size is invalid");
    if (Param[1]->Val->Integer > 0) StdioRequireBuf(Parser, Param[0]->Val->Pointer, "vsnprintf destination");
    StdioRequireBuf(Parser, Param[2]->Val->Pointer, "vsnprintf format");

    { struct StdVararg Args; memset(&Args, 0, sizeof(Args));
      Args.Cursor = Param[3]->Val->Pointer; Args.NumArgs = 1024;
      ReturnValue->Val->Integer = StdioBasePrintf(Parser, NULL,
          Param[0]->Val->Pointer, Param[1]->Val->Integer, Param[2]->Val->Pointer, &Args); }
}

void StdioVscanf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "vscanf format");

    { struct StdVararg Args; memset(&Args, 0, sizeof(Args));
      Args.Cursor = Param[1]->Val->Pointer; Args.NumArgs = MAX_SCANF_ARGS;
      ReturnValue->Val->Integer = StdioBaseScanf(Parser, stdin, NULL,
          Param[0]->Val->Pointer, &Args); }
}

void StdioVfscanf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireFile(Parser, Param[0]->Val->Pointer, "vfscanf");
    StdioRequireBuf(Parser, Param[1]->Val->Pointer, "vfscanf format");

    { struct StdVararg Args; memset(&Args, 0, sizeof(Args));
      Args.Cursor = Param[2]->Val->Pointer; Args.NumArgs = MAX_SCANF_ARGS;
      ReturnValue->Val->Integer = StdioBaseScanf(Parser, Param[0]->Val->Pointer,
          NULL, Param[1]->Val->Pointer, &Args); }
}

void StdioVsscanf(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    StdioRequireBuf(Parser, Param[0]->Val->Pointer, "vsscanf input");
    StdioRequireBuf(Parser, Param[1]->Val->Pointer, "vsscanf format");

    { struct StdVararg Args; memset(&Args, 0, sizeof(Args));
      Args.Cursor = Param[2]->Val->Pointer; Args.NumArgs = MAX_SCANF_ARGS;
      ReturnValue->Val->Integer = StdioBaseScanf(Parser, NULL,
          Param[0]->Val->Pointer, Param[1]->Val->Pointer, &Args); }
}

static void StdioGetdelim(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    char **lineptr;
    unsigned int *n;
    int delim;
    FILE *fp;
    char *buf;
    unsigned int cap;
    unsigned int len;
    int ch;
    (void)NumArgs;
    lineptr = (char **)Param[0]->Val->Pointer;
    n = (unsigned int *)Param[1]->Val->Pointer;
    delim = (int)Param[2]->Val->Integer;
    fp = (FILE *)Param[3]->Val->Pointer;
    if (lineptr == NULL || n == NULL) {
        errno = EINVAL;
        ReturnValue->Val->Integer = -1;
        return;
    }
    StdioRequireFile(Parser, fp, "getdelim");
    buf = *lineptr;
    cap = *n;
    len = 0;
    for (;;) {
        ch = fgetc(fp);
        if (ch == EOF) {
            if (len == 0) {
                ReturnValue->Val->Integer = -1;
                return;
            }
            break;
        }
        if (len + 2 > cap) {
            unsigned int ncap = cap ? cap * 2u : 128u;
            char *nbuf;
            if (ncap < len + 2u)
                ncap = len + 2u;
            nbuf = (char *)realloc(buf, ncap);
            if (nbuf == NULL) {
                errno = ENOMEM;
                ReturnValue->Val->Integer = -1;
                return;
            }
            BabycHeapTrackAllocation(nbuf, ncap);
            buf = nbuf;
            cap = ncap;
        }
        buf[len++] = (char)ch;
        if (ch == delim)
            break;
    }
    buf[len] = '\0';
    *lineptr = buf;
    *n = cap;
    ReturnValue->Val->Integer = (int)len;
}

static void StdioGetline(struct ParseState *Parser, struct Value *ReturnValue,
    struct Value **Param, int NumArgs)
{
    struct Value *fwd[4];
    union AnyValue delim_any;
    struct Value delim_val;
    (void)NumArgs;
    memset(&delim_any, 0, sizeof(delim_any));
    delim_any.Integer = '\n';
    memset(&delim_val, 0, sizeof(delim_val));
    delim_val.Typ = &Parser->pc->IntType;
    delim_val.Val = &delim_any;
    fwd[0] = Param[0];
    fwd[1] = Param[1];
    fwd[2] = &delim_val;
    fwd[3] = Param[2];
    StdioGetdelim(Parser, ReturnValue, fwd, 4);
}

/* handy structure definitions */
const char StdioDefs[] =
    "typedef struct __va_listStruct va_list;\n"
    "typedef struct __FILEStruct FILE;\n"
    "#ifndef SEEK_SET\n"
    "#define SEEK_SET 0\n"
    "#endif\n"
    "#ifndef SEEK_CUR\n"
    "#define SEEK_CUR 1\n"
    "#endif\n"
    "#ifndef SEEK_END\n"
    "#define SEEK_END 2\n"
    "#endif\n";

/* all stdio functions */
struct LibraryFunction StdioFunctions[] =
{
    {StdioFopen, "FILE *fopen(char *, char *);"},
    {StdioFreopen, "FILE *freopen(char *, char *, FILE *);"},
    {StdioFclose, "int fclose(FILE *);"},
    {StdioFread, "int fread(void *, int, int, FILE *);"},
    {StdioFwrite, "int fwrite(void *, int, int, FILE *);"},
    {StdioFgetc, "int fgetc(FILE *);"},
    {StdioFgetc, "int getc(FILE *);"},
    {StdioFgets, "char *fgets(char *, int, FILE *);"},
    {StdioFputc, "int fputc(int, FILE *);"},
    {StdioFputs, "int fputs(char *, FILE *);"},
    {StdioRemove, "int remove(char *);"},
    {StdioRename, "int rename(char *, char *);"},
    {StdioRewind, "void rewind(FILE *);"},
    {StdioTmpfile, "FILE *tmpfile();"},
    {StdioClearerr,"void clearerr(FILE *);"},
    {StdioFeof, "int feof(FILE *);"},
    {StdioFerror, "int ferror(FILE *);"},
    {StdioFileno, "int fileno(FILE *);"},
    {StdioFflush, "int fflush(FILE *);"},
    {StdioFgetpos, "int fgetpos(FILE *, int *);"},
    {StdioFsetpos, "int fsetpos(FILE *, int *);"},
    {StdioFtell, "int ftell(FILE *);"},
    {StdioFseek, "int fseek(FILE *, int, int);"},
    {StdioPerror, "void perror(char *);"},
    {StdioPutc, "int putc(char *, FILE *);"},
    {StdioPutchar, "int putchar(int);"},
    {StdioPutchar, "int fputchar(int);"},
    {StdioSetbuf, "void setbuf(FILE *, char *);"},
    {StdioSetvbuf, "void setvbuf(FILE *, char *, int, int);"},
    {StdioUngetc, "int ungetc(int, FILE *);"},
    {StdioPuts, "int puts(char *);"},
    {StdioGets, "char *gets(char *);"},
    {StdioGetchar, "int getchar();"},
    {StdioPrintf, "int printf(char *, ...);"},
    {StdioFprintf, "int fprintf(FILE *, char *, ...);"},
    {StdioSprintf, "int sprintf(char *, char *, ...);"},
    {StdioSnprintf,"int snprintf(char *, int, char *, ...);"},
    {StdioScanf, "int scanf(char *, ...);"},
    {StdioFscanf, "int fscanf(FILE *, char *, ...);"},
    {StdioSscanf, "int sscanf(char *, char *, ...);"},
    {StdioVprintf, "int vprintf(char *, va_list);"},
    {StdioVfprintf,"int vfprintf(FILE *, char *, va_list);"},
    {StdioVsprintf,"int vsprintf(char *, char *, va_list);"},
    {StdioVsnprintf,"int vsnprintf(char *, int, char *, va_list);"},
    {StdioVscanf, "int vscanf(char *, va_list);"},
    {StdioVfscanf, "int vfscanf(FILE *, char *, va_list);"},
    {StdioVsscanf, "int vsscanf(char *, char *, va_list);"},
    {StdioGetdelim, "ssize_t getdelim(char **, size_t *, int, FILE *);"},
    {StdioGetline, "ssize_t getline(char **, size_t *, FILE *);"},
    {NULL, NULL}
};

/* creates various system-dependent definitions */
void StdioSetupFunc(Picoc *pc)
{
    struct ValueType *StructFileType;
    struct ValueType *FilePtrType;

    /* make a "struct __FILEStruct" which is the same size as a
        native FILE structure */
    StructFileType = TypeCreateOpaqueStruct(pc, NULL,
        TableStrRegister(pc, "__FILEStruct"), sizeof(FILE));

    /* get a FILE * type */
    FilePtrType = TypeGetMatching(pc, NULL, StructFileType, TypePointer, 0,
        pc->StrEmpty, TRUE);

    /* make a "struct __va_listStruct" which is the same size as
        our struct StdVararg */
    TypeCreateOpaqueStruct(pc, NULL, TableStrRegister(pc, "__va_listStruct"),
        sizeof(FILE));

    /* define EOF equal to the system EOF */
    VariableDefinePlatformVar(pc, NULL, "EOF", &pc->IntType,
        (union AnyValue*)&EOFValue, FALSE);
    VariableDefinePlatformVar(pc, NULL, "BUFSIZ", &pc->IntType,
        (union AnyValue*)&BUFSIZValue, FALSE);
    VariableDefinePlatformVar(pc, NULL, "FILENAME_MAX", &pc->IntType,
        (union AnyValue*)&FILENAME_MAXValue, FALSE);
    VariableDefinePlatformVar(pc, NULL, "_IOFBF", &pc->IntType,
        (union AnyValue*)&_IOFBFValue, FALSE);
    VariableDefinePlatformVar(pc, NULL, "_IOLBF", &pc->IntType,
        (union AnyValue*)&_IOLBFValue, FALSE);
    VariableDefinePlatformVar(pc, NULL, "_IONBF", &pc->IntType,
        (union AnyValue*)&_IONBFValue, FALSE);
    VariableDefinePlatformVar(pc, NULL, "L_tmpnam", &pc->IntType,
        (union AnyValue*)&L_tmpnamValue, FALSE);
    VariableDefinePlatformVar(pc, NULL, "GETS_MAX", &pc->IntType,
        (union AnyValue*)&GETS_MAXValue, FALSE);

    /* define stdin, stdout and stderr */
    VariableDefinePlatformVar(pc, NULL, "stdin", FilePtrType,
        (union AnyValue*)&stdinValue, FALSE);
    VariableDefinePlatformVar(pc, NULL, "stdout", FilePtrType,
        (union AnyValue*)&stdoutValue, FALSE);
    VariableDefinePlatformVar(pc, NULL, "stderr", FilePtrType,
        (union AnyValue*)&stderrValue, FALSE);

    /* define NULL, TRUE and FALSE */
    if (!VariableDefined(pc, TableStrRegister(pc, "NULL")))
        VariableDefinePlatformVar(pc, NULL, "NULL", &pc->IntType,
            (union AnyValue*)&Stdio_ZeroValue, FALSE);
}

/* portability-related I/O calls */
void PrintCh(char OutCh, FILE *Stream)
{
    putc(OutCh, Stream);
}

void PrintSimpleInt(long Num, FILE *Stream)
{
    fprintf(Stream, "%ld", Num);
}

void PrintStr(const char *Str, FILE *Stream)
{
    fputs(Str, Stream);
}

void PrintFP(double Num, FILE *Stream)
{
    fprintf(Stream, "%f", Num);
}

