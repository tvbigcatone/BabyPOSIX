CC=gcc
CFLAGS=-Wall -O2 -DENABLE_WTHREADS=1 -DENABLE_WSHMEM=1 -Iwinapi/include -DUNIX_HOST -DWITH_CSTDLIB_EXT -DWITH_SQLITE -DVER=\"babyc\" -Wno-incompatible-pointer-types -Wno-pointer-sign -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -I. -Ithird_party/sqlite
LIBS=-lm -pthread -lrt /usr/lib/x86_64-linux-gnu/libsqlite3.so.0
TARGET=babyc
SRCS=picoc.c diagnostic.c table.c lex.c parse.c ir_exec.c ir_frame.c ir_ops.c ir_mem.c ir_call.c ir_fast.c ir_eval.c ir_stmt.c ir_shadow.c ir_token_loop.c ir_compile.c ir_verify.c ir_fncode.c ir_lin_light.c ir_lin_exec.c ir_lin_ptr.c ir_lin_compile.c ir_lin_emit.c ir_lower.c ir_storage.c ir_vm.c expression.c heap.c type.c variable.c clibrary.c platform.c include.c debug.c types.c \
	platform/platform_unix.c platform/library_unix.c platform/guest_thread.c \
	cstdlib/stdio.c cstdlib/math.c cstdlib/string.c cstdlib/stdlib.c \
	cstdlib/time.c posix/posix_time.c posix/posix_unistd.c posix/posix_fd.c posix/posix_dirent.c posix/posix_pthread.c posix/posix_stat.c posix/posix_misc.c posix/posix_more.c posix/posix_cover.c posix/posix_types.c posix/posix_wait.c posix/posix_pwd.c posix/posix_signal.c cstdlib/errno.c cstdlib/ctype.c cstdlib/stdbool.c cstdlib/unistd.c cstdlib/stdint.c cstdlib/printf_format.c cstdlib/mini_printf.c \
	cstdlib_ext/stdarg.c cstdlib_ext/ctype_ext.c cstdlib_ext/limits.c cstdlib_ext/assert.c cstdlib_ext/win.c \
	winapi/src/werror.c winapi/src/wfile_handles.c winapi/src/wfiles.c winapi/src/wfiles_private.c winapi/src/wmsgbox.c winapi/src/wperf.c winapi/src/wprivate.c winapi/src/wprocess.c winapi/src/wshmem.c winapi/src/wstats.c winapi/src/wstring.c winapi/src/wthreads.c winapi/src/wtime.c winapi/src/wdisplay.c \
	third_party/sqlite/sqlite.c
OBJS=$(SRCS:.c=.o)
all: $(TARGET)
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LIBS)
clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: token_v2
token_v2: all  # TOKEN_V2 always compiled in (single token architecture)

# Portable IR expr dispatch table (default ON):
#   GCC/Clang → computed goto; MSVC → dense switch (jump table at /O2)
# Force switch path everywhere: -DBABYC_NO_DISPATCH_TABLE

# --- Safe-check profiles (uninit + OOB) ---
# micro:  checks compiled force-off (fase M / footprint)
# debug:  checks force-on (desktop debug)
# default product: ON, override with BABYC_SAFE=0 or BABYC_NO_UNINIT=1
.PHONY: safe_micro safe_debug
safe_micro:
	$(MAKE) clean all CFLAGS="$(CFLAGS) -DBABYC_SAFE_FORCE_OFF"
safe_debug:
	$(MAKE) clean all CFLAGS="$(CFLAGS) -DBABYC_SAFE_FORCE_ON -g"

# Non-blocking performance gate (piano recovery post babyc(8))
# Usage: make perf_gate
#        make perf_gate PERF_BASELINE=tools/perf_baseline_babyc8.json
.PHONY: perf_gate
perf_gate: $(TARGET)
	BABYC="$(CURDIR)/$(TARGET)" python3 tools/perf_gate.py \
	  $(if $(PERF_BASELINE),--baseline $(PERF_BASELINE),) \
	  $(if $(PERF_STRICT),--strict,)
