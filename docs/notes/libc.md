# libc surface (runtime/libc.c, net.c, wchar.c, printf.c, pthread.c)

Owner stream: "C library surface". Covers every non-GL import of `librainbowsix.so` (205 `.dynsym` undefined
symbols that are not `gl*`/`egl*`; 7 of them are data objects). All 198 functions have a handler; the 7 data imports
get real bytes in game memory. A host test (`runtime/test/libc_test.c`) checks that every one of the 205 names is
registered.

## Build facts

- `net.c` and `wchar.c` are **part of the `libc.c` translation unit** (`#include`d at its bottom; each is empty when
  compiled alone). Nothing has to be added to `RUNTIME_SRC` in `build_runner.py` / `build_apk.py`. This also lets
  them use libc.c's `A()/S()/RET()/IMP()` macros and `set_errno`.
- On Windows `ws2_32.dll` is loaded with `LoadLibrary` the first time a socket is created, so no `-lws2_32` is
  needed on the link line (the runner's link line has none).
- Compiles warning-free with `zig cc -target x86_64-windows-gnu -Wall` (the `build_runner.py` flags), with
  `zig cc -target aarch64-linux-gnu`, and with the NDK clang (`aarch64-linux-android24 -Wall -Wextra`).
- Test: `py -m ziglang cc -O1 -target x86_64-windows-gnu -Iruntime -Itools/recomp/runtime runtime/test/libc_test.c -o build/libc/libc_test.exe && build/libc/libc_test.exe`
  (Windows host: it uses `GetTickCount64`). 138 checks, 0 failed at the time of writing.

## Things other streams must do

1. **`tools/recomp/pack_image.py` `DATA_IMPORTS`** must also contain `_toupper_tab_`, `__dso_handle` and
   `_ZSt7nothrow`. Today it lists `__sF _ctype_ _tolower_tab_ __stack_chk_guard SL_IID_*`. Without the three names their
   GOT slots are not patched (the game would read an import stub's bytes as the `_toupper_tab_` pointer).
   `rt_data_imports_init` already fills all three, and gives any other unknown data import 256 zero bytes (logged,
   not fatal).
2. **Data folder.** Call `rt_libc_set_data_dir("<host folder holding the game data>")` before the game runs
   (`game.c`/`host_main.c`/`android_main.c`), or set the environment variable `R6SV_DATA`. The folder is what
   `/sdcard/gameloft/games/GloftR6HP` maps to. Other spellings of the same place (`/mnt/sdcard/...`,
   `/storage/sdcard0/...`, `/storage/emulated/0/...`, `/storage/emulated/legacy/...`) are registered by default;
   `rt_libc_add_data_prefix("/some/other/prefix")` adds more (docs/JAVA-SIDE.md did not exist when this was
   written). `rt_path_alias` (core.c) still applies first, so `/data/data/<pkg>` aliases keep working.
3. **setjmp in the lifter** (below).

## setjmp / longjmp contract

`setjmp` must return twice, which a host function reached through `rc_import` cannot do (its C frame is gone).
So the lifter emits setjmp **inline in the translated function** around a runtime helper:

```c
/* recomp.h (lifter side) */
#include <setjmp.h>
void *rt_setjmp_prepare(struct cpu *c, uint32_t guest_jmp_buf);   /* runtime/libc.c */

/* in place of `bl setjmp` (r0 = guest jmp_buf), after r14 has been set to the return address: */
RC_SAVE();                                    /* the register locals must be in c: r4-r11, sp, lr are saved from c */
if (setjmp(*(jmp_buf *)rt_setjmp_prepare(c, r0)) == 0) {
    r0 = 0;                                   /* first return */
} else {
    RC_LOAD();                                /* second return: longjmp already filled c (r0 = value, r4-r11, sp, lr, d8-d15, depth) */
}
/* then continue with the instruction after the call */
```

- `rt_setjmp_prepare` writes the game's `jmp_buf` (runtime-private layout, 30 words of bionic's 64: `[0]` magic
  `0x52365356`, `[4..19]` d8-d15, `[20..27]` r4-r11, `[28]` sp, `[29]` lr), records `c->depth` and
  `rt_current_hostfn`, and returns the host `jmp_buf` kept for that game address (32 records per thread, reused by
  address). The game never reads the buffer itself.
- `longjmp(buf, val)` (the import handler) finds the record for that address on the calling thread, loads r4-r11,
  sp, lr, d8-d15 from the game buffer into `c`, sets `r0 = val ? val : 1`, `r15 = lr`, restores `c->depth`, and does
  the host `longjmp`. `_longjmp`, `siglongjmp` are the same handler. A longjmp to a buffer with no record on the
  thread is a fatal error with the guest call chain.
- If a call reaches the `setjmp` import handler instead (through a pointer or tail call) it saves the buffer and
  returns 0 once, and logs that a later longjmp will fail. Fix it in the lifter if it ever shows up.
- Unit-tested in `test_setjmp` by writing the macro by hand.

## What is implemented

**Memory/strings/ctype/math**: the old set plus `calloc`, `div` (8-byte struct returned through the hidden pointer in
r0), `fgetpos`/`fsetpos` (32-bit `fpos_t`), `feof`/`ferror`/`clearerr`, `nanosleep`, `chdir` (a virtual cwd; the
process cwd is untouched), `access`, `getcwd`, `__android_log_write`, the float libm set (`sinf cosf tanf asinf acosf
atanf atan2f expf logf log10f powf sqrtf floorf ceilf fmodf ldexpf`, float args/results in r0/r1), `isdigit isprint
isgraph tolower`. NaN results of all libm and `__aeabi_d*` helpers are the ARM default NaN (positive) instead of x86's.
`qsort` is a stable merge sort (the old insertion sort was O(n^2)). `clock()` is in microseconds like bionic.
`sysconf` knows the bionic `_SC_*` numbers for page size, CPU count, clock ticks, open max, physical pages.
`setlocale` accepts NULL/""/"C"/"POSIX" (returns "C"), anything else returns NULL.

**EABI and C++ runtime**: `__aeabi_idiv/uidiv/idivmod/uidivmod/ldivmod/uldivmod` (division by zero gives quotient 0
and remainder = dividend, what libgcc leaves after `__aeabi_idiv0` returns; `INT_MIN/-1` gives `INT_MIN`),
`__aeabi_dadd/dsub/dmul/ddiv`, `i2d ui2d l2d ul2d`, `d2iz d2uiz d2lz d2ulz` (saturating, NaN gives 0),
`__aeabi_atexit`, `__cxa_atexit` (accepted, destructors never run), `__cxa_guard_acquire/release/abort` (ARM EABI
guard word, byte 0 = done, bit 8 = in progress, other threads spin), `__cxa_pure_virtual` (fatal with call chain),
`_Znwj _Znaj _ZnajRKSt9nothrow_t _ZdlPv _ZdaPv` (+ the other nothrow forms; no exceptions: a failed allocation returns
0), `__aeabi_unwind_cpp_pr0/1/2` (return `_URC_FAILURE`; the game imports no `__cxa_throw`, so nothing unwinds).

**Data imports** (`rt_data_imports_init`): `_ctype_`, `_tolower_tab_`, `_toupper_tab_` are pointer variables (the GOT
slot holds the variable's address) to 257-entry tables indexed from -1 with bionic's BSD flags (`_U 1, _L 2, _N 4,
_S 8, _P 0x10, _C 0x20, _X 0x40, _B 0x80`); `__sF` is three 84-byte `FILE`s with `_flags` and `_file` (+14) set so the
`fileno` macro gives 0/1/2; `__stack_chk_guard` a fixed word; `__dso_handle` and `_ZSt7nothrow` are zeroed 16-byte
objects.

**Files**: `fopen` (always binary), `fdopen`, `open` (O_CREAT/EXCL/TRUNC/APPEND mapped from bionic's octal values),
`stat/fstat` (bionic 104-byte struct: mode +16, size +48, blocks +64, atime/mtime/ctime +72/80/88), `mkdir unlink
remove rename opendir readdir closedir`. Every path goes through `rt_map_path`: `rt_host_path` aliases, then the
data-folder prefix mapping (prefix matched case-insensitively at a path separator), then relative paths against the
`chdir` folder, then on non-Windows hosts a **case-insensitive retry component by component** when the exact name does
not exist (Windows is case-insensitive already). Host errno values go back as Linux errno. Descriptors 0-2 cannot be
closed by the game. `read/write/close/fstat` also work on socket descriptors.

**Time**: bionic `struct tm` (9 ints, `tm_gmtoff`, `tm_zone`); `localtime` fills a real offset, `mktime` writes the
normalised fields back into the guest struct; `gettimeofday` zeroes the `timezone` argument.

**Signals** (recorded, never delivered): `sigaction` stores handler/mask/flags and hands them back through `oldact`,
`bsd_signal`/`signal` return the previous handler, `sigprocmask` returns 0. `raise(sig)` and `pthread_kill(t, sig)` call
`rt_signal_note`, which counts and logs each signal number once; `pthread_kill(t, 0)` returns 0 or `ESRCH`.

**Wide characters** (`wchar.c`, 4-byte `wchar_t`): `wcslen wcsnlen wcscmp wcsncmp wcscoll wcscpy wcsncpy wcscat
wcsncat wcschr wcsrchr wcsstr wcsdup wcsxfrm wmemcpy wmemmove wmemset wmemcmp wmemchr btowc wctob mbrtowc wcrtomb
wcstol wcstoul wcstod wcsftime swprintf vswprintf`, and the class/case functions `iswalpha iswdigit iswlower iswupper
iswspace iswprint iswpunct iswxdigit iswcntrl iswgraph iswalnum iswblank towlower towupper wctype iswctype`. Classes
are Unicode for Latin, Greek, Cyrillic, CJK, Hangul, kana and fullwidth forms, ASCII-only elsewhere; case maps cover
ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic, fullwidth. `mbrtowc`/`wcrtomb` are UTF-8 (stateless: an incomplete
sequence returns -2). `swprintf(buf, n, fmt, ...)` converts the wide format to UTF-8, formats with the narrow engine
(`%ls %lc %s %c` all work), converts back, and returns the character count or -1 when `n` was too small (the buffer
then holds `n-1` characters and a NUL). Width and precision count bytes, not characters, for non-ASCII text; a byte
that is not valid UTF-8 (Latin-1 in a narrow `%s`) becomes the code point of the same value.

**Threads** (`pthread.c`): mutex/cond/sem objects are released by their `destroy` call (the slot is reusable, which
matters when the game frees memory that held a mutex), `pthread_detach`, host thread stacks are 8 MB (translated code
recurses on the host stack), `pthread_kill` as above.

## Networking (`net.c`)

Game socket descriptors are **512..1023**, an index into a table of host sockets (so they fit the 1024-bit `fd_set`
and never collide with file descriptors). All host sockets are non-blocking; blocking behaviour is emulated with
`select` so a call cannot hang on a dead peer. Policy, chosen by `R6SV_NET`:

| value | behaviour |
|---|---|
| unset | local only: loopback, 10/8, 172.16/12, 192.168/16, 169.254/16, multicast and broadcast work with real host sockets; `connect`/`sendto` to any other address fails **at once** with `ENETUNREACH` (blocking or not); `getaddrinfo`/`gethostbyname` resolve numeric addresses and `localhost` only, anything else fails at once (`EAI_NONAME`, 8, or NULL) |
| `real` | everything goes to the host network; DNS uses the host resolver; a blocking `connect` gives up after 5 s (`ETIMEDOUT`) |
| `off` | `socket()` fails with `ENETDOWN` |

- `socket(AF_INET, SOCK_STREAM|SOCK_DGRAM [|SOCK_NONBLOCK], 0)`; AF_INET6/AF_UNIX/SOCK_RAW fail (`EAFNOSUPPORT`,
  `EPROTONOSUPPORT`). `bind listen accept connect shutdown getsockname getpeername send recv sendto recvfrom` with the
  Linux flag values (`MSG_PEEK 2, MSG_DONTWAIT 0x40, MSG_NOSIGNAL`), `read`/`write`/`close` on socket fds.
- Non-blocking `connect` to a reachable address returns -1/`EINPROGRESS`; `select`/`poll` then report it writable (on
  Windows a failed connect arrives as an exception and is folded into "writable") and `getsockopt(SO_ERROR)` gives
  the real Linux errno (`ECONNREFUSED` 111 for a closed local port). Tested.
- `select(nfds, r, w, e, timeval*)` with bionic's 128-byte `fd_set` (bit `fd%32` of little-endian word `fd/32`) and
  32-bit `timeval`; descriptors that are not sockets (files) count as always ready; `poll` is built on the same
  engine. The timeout is not written back.
- `getsockopt/setsockopt`: SOL_SOCKET (1) options REUSEADDR 2, TYPE 3, ERROR 4, BROADCAST 6, SNDBUF 7, RCVBUF 8,
  KEEPALIVE 9, LINGER 13, RCVTIMEO 20 and SNDTIMEO 21 (kept in the table and honoured by blocking calls), IPPROTO_TCP
  TCP_NODELAY, IPPROTO_IP TOS/TTL/MULTICAST_TTL/ADD/DROP_MEMBERSHIP; others are accepted and ignored (`setsockopt`) or
  `ENOPROTOOPT` (`getsockopt`). `ioctl` FIONBIO 0x5421 and FIONREAD 0x541B; other requests `EINVAL` (no interface
  list). `fcntl` F_GETFD/F_SETFD/F_GETFL/F_SETFL (O_NONBLOCK 04000).
- `getaddrinfo` writes bionic's 32-byte `addrinfo` (`flags family socktype protocol addrlen canonname addr next`,
  canonname *before* addr) in game memory, one block per entry (addrinfo + 16-byte sockaddr_in + canonical name), two
  entries (stream, datagram) when `hints->ai_socktype` is 0; honours AI_PASSIVE, AI_CANONNAME, AI_NUMERICHOST,
  AI_NUMERICSERV; service by number or a small name table. `freeaddrinfo` walks `ai_next` (+28). `gethostbyname`
  returns a `hostent` in a per-thread guest block. `inet_addr` (a.b, a.b.c, hex/octal parts), `inet_aton`, `inet_ntoa`
  (struct by value in r0), `inet_pton/ntop` (IPv4), `htons/htonl/ntohs/ntohl`, `gethostname` ("localhost").

## Left out or approximate

- `freopen` fails with `ENOSYS`; `tmpnam` returns NULL; `getenv` always returns NULL; `fscanf` reads one line (old
  behaviour); `getwc/putwc/ungetwc` are stubs (no wide FILE I/O); no `wprintf`/`swscanf`.
- `mmap` copies from a heap block and `munmap` is a no-op (blocks leak); `MAP_FIXED` and shared writable file maps
  are not supported.
- Host mutexes are always recursive: a relock or same-thread `trylock` of a normal mutex succeeds (deadlocks/EBUSY on
  the device).
- `strtol`/`strtoul` clamp but do not set `ERANGE`. `strcoll` and `wcscoll` compare by code unit (C locale).
- IPv6, AF_UNIX, raw sockets, `sendmsg/recvmsg`, `getifaddrs`, `SIOCGIF*` are not implemented.
- Signals are never delivered, so `SIGSEGV`/`SIGABRT` handlers the game installs never run; C++ exceptions are not
  supported.
- The `setjmp` import handler cannot return twice; the lifter must inline it (see above).
- Not exercised on a real device: the Android build only compiles (NDK clang); sockets were tested on Windows loopback.
