# BabyPOSIX thin-layer session 11 — delta vs overlay baseline 2026-10-21

Baseline: `/tmp/opencode/new_baseline/babyc.zip` overlayed onto `/workspace`.
This zip is the differential vs that overlay (not vs session 10).

## Shared IR files (required report)

These files are shared with the other team. Status vs overlay baseline:

### `src/ir_fncode.c` — NOT MODIFIED
No edits. Overlay already contains `FnCodeVoidParamLocalUsesSafe*` used by
`void_ptr_typed_local` / STRICT `pthread_create_join`.

### `src/ir_lin_tabs.h` — NOT MODIFIED
Linear opcode tables untouched.

### `src/ir_lin_exec.c` — NOT MODIFIED
Linear interpreter untouched.

### `src/ir_lower.c` — NOT MODIFIED
Overlay already contains `IrLinThreadContextCapture` / `Restore`,
`LinFrameDepth` / `LinFrameRestoreDepth`. This session consumes those APIs
from `platform/guest_thread.c` and `posix/posix_pthread.c` without changing
the implementation.

### `src/ir_runtime.h` — NOT MODIFIED
`IrLinThreadContext` layout unchanged.

### `src/ir_compile.c` — MODIFIED (object-like macro expansion)

**Why:** leftover test `define_unary.c` (`#define NEG1 -100`, `#define SUM 1+2`)
failed because IR compile of an identifier that is an object-like macro only
accepted a single `TokenIntegerConstant` / `TokenStringConstant` body. Unary
minus (`-100`) and binary `1+2` were ignored, so `NEG1` became 0 and `SUM`
became 1.

**What changed:** in `IrCompilePrimary`, when an identifier is a 0-param macro:
1. Keep the string-literal fast path only if the body is a *single* string
   token followed by `TokenEndOfFunction` (adjacent-string concat still works).
2. Otherwise compile the **full** macro body with `IrCompileExpr` so unary
   minus, `1+2`, `~0`, `!0`, and parenthesized forms lower to Linear.

**Risk to other team:** object-like macros that previously silently took only
the first integer token now evaluate the whole expression (C-correct). Macros
whose body is not an expression still fail compile and residualize as before.

TREE / LINEAR_STRICT `define_unary`: PASS after this change.
NO_IR `define_unary` still FAIL (`SUM` → 1): Tree `ExpressionParse` of a
0-param macro body is a leftover preprocessor issue, not Linear. Not patched
this session (would touch `expression.c`, shared-adjacent).

## Trampoline / Linear guest (`src/platform/guest_thread.c`)

**Problem:** `BabycGuestCall` armed `PicocPlatformSetExitPoint` (overwriting
`PicocExitBuf`) then on longjmp restored Linear/heap and `return NULL`. That:
- swallowed nonzero `ProgramFail` (`exit(1)` / abort in a worker);
- left a dangling `jmp_buf` after a *successful* worker return (next
  `PlatformExit` would longjmp into a dead trampoline frame);
- `BabycGuestCallInt2` (qsort/bsearch) had no Linear/heap snapshot at all.

**Fix:**
- `BabycGuestSnap` captures heap/stack/`IrCallDepth`/`LinFrameDepth`/
  `IrLinThreadContext` **and** `PicocExitBuf` + `PicocExitPointSet`.
- On worker longjmp: unwind Linear/IR/heap, restore outer `PicocExitBuf`.
  `PicocExitValue == 0` (`exit(0)` / `guest_exit_zero`) → `return NULL` so
  join succeeds. Nonzero → `PlatformExit` on the restored (outer) buf, or
  `exit()` if the outer point is unset (worker thread; no cross-thread longjmp).
- Successful return restores the outer `PicocExitBuf` so `main`'s setjmp
  remains valid.
- Same snapshot applied to `BabycGuestCallInt2`.

`guest_exit_zero`, `pthread_create_join`, `pthread_exit_retval`: TREE and
LINEAR_STRICT PASS.

## pthread thin layer (`src/posix/posix_pthread.c`)

- `pthread_exit` restore now also copies `PicocExitBuf` / `PicocExitPointSet`
  (same nesting contract as the trampoline). Heap restore still needed because
  `pthread_exit` never returns through `BabycGuestCall`.
- Destroy poisons the guest handle to `(bp_*_t)-1` instead of `0`, so
  `BabyPosixMutexEnsure` / `CondEnsure` no longer reanimate a destroyed
  INITIALIZER-looking handle (`pthread_destroy_poison`).
- `pthread_attr_destroy(NULL)` returns `EINVAL` (`pthread_attr_null`).

## POSIX leftover APIs (thin layer, not libWINAPI)

- `cstdlib/stdio.c`: `getdelim` / `getline`. Guest `char**` / `size_t*`
  (`unsigned int` on the guest ABI). Growth via host `realloc` +
  `BabycHeapTrackAllocation` so later `free()` is tracked.
- `posix/posix_unistd.c`: `confstr(_CS_PATH)` (guest `_CS_PATH` is 0),
  `eaccess`, `AT_FDCWD` now `(-100)` (was unsigned wrap `4294967196`, which
  broke `define_unary`'s `#if AT_FDCWD != -100`).
- `posix/posix_fd.c`, `posix/posix_stat.c`: same `AT_FDCWD (-100)`.
- `posix/posix_cover.c`: `sendfile` maps guest fds through
  `BabyPosixFdNativeOf`, copies guest `off_t` (`int`) to host `off_t`, writes
  the updated offset back. GIL dropped around the syscall.
- Wired leftover modules already on disk but not in the overlay Makefile:
  `posix_types.c` (`sys/types.h`), `posix_wait.c` (`sys/wait.h`),
  `posix_pwd.c` (`pwd.h`/`grp.h`), `posix_signal.c` (`signal.h`).
  `include.c` + `Makefile` register them.

`posix_s11` TREE + LINEAR_STRICT PASS.

## Matrice POSIX 4 path (58 tests in `src/tests/posix`)

Last full run was *before* leftover header wiring + `define_unary` IR fix.
Spot-check after those fixes:

PASS TREE+STRICT: `pthread_create_join`, `pthread_exit_retval`,
`void_ptr_typed_local`, `guest_exit_zero`, `posix_s11`, `define_unary`,
`pthread_attr_null`, `pthread_destroy_poison`, `include_headers`,
`pwd_uname`, `wait_macros`.

Known residual LINEAR_STRICT `strict: tree fallback (body)` (pre-existing
Linear incompleteness, **not masked**; not fixed this session because a
root-cause Linear lower would require `ir_lin_compile.c` / `ir_fncode.c`):
- `dup_basic`, `fcntl_basic`, `fd_rollback`, `msync_utimens`, `select_poll`

Known residual NO_IR: `define_unary` (`SUM` → 1) — Tree macro body parse.

## Build

`make -C src -j2` (no `clean`). Touched `.c`/`.h` before rebuild.
No changes to `ir_fncode.c` / `ir_lin_tabs.h` / `ir_lin_exec.c` / `ir_lower.c`.
