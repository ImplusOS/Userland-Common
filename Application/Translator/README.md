# Translator — Linux programs for the other CPU

The translator runs a Linux program built for the CPU the kernel was not
built for: x86_64 programs on arm64 ImplusOS, AArch64 programs on x86_64
ImplusOS. The kernel starts it in the program's place (see
`Docs/Architecture/Compat_Layers.md` §7 for the kernel side and the limits);
it loads the program, runs its instructions -- x86_64 code on an arm64 host
through a JIT that compiles it to AArch64 machine code -- and turns its
Linux system calls into the host's.

| `ARCH` (host) | Output | Runs |
|---|---|---|
| `arm64`  | `Build/arm64/Userland/Translator/x86_64.ELF`   | Linux x86_64 programs |
| `x86_64` | `Build/x86_64/Userland/Translator/aarch64.ELF` | Linux AArch64 programs |

It is built with every other app (`make app_build`, picked up from
`Userland/Application/*`) and staged as `/Userland/Translator/`.

## How it is built

A static Linux-ABI program of the host CPU, no C library: the cross `gcc`
in freestanding mode, linked at `0x4000000000` (`USER_CODE_BASE`) with
libgcc (binary128 soft-float for the x87 on arm64, 128-bit division), and
`EI_OSABI` patched to `ELFOSABI_LINUX` so the kernel runs it under its Linux
ABI. Built without `-Wconversion`: an instruction interpreter truncates on
purpose everywhere.

## Source

```
Source/
  xl.h               shared types, the gthread_t, the guest interface
  main.c             command line, ELF loading, initial stack + auxv
  thread.c           run loop, fault recovery, clone/fork, thread exit
  signal.c           guest dispositions and masks, host handler, delivery
  syscall.c          canonical (x86_64-numbered) layer: what the translator
                     handles itself (signals, clone, exit, /proc/self/exe,
                     /proc/self/maps with the guest stack added, ...)
  hostsys_generic.c  canonical -> asm-generic host (arm64)
  hostsys_x86_64.c   canonical -> x86_64 host (identity)
  host.h, host_*.S   host syscall numbers, traps, clone, setjmp, ucontext
  util.c             string functions, printf, logging, mmap, spinlock
  jit.h, jit.c       the JIT's guest-independent runtime: per-thread code
                     and metadata arenas, guest PC -> translation map,
                     indirect-branch table, host PC -> translation (faults),
                     invalidation when trusted code changes
  a64emit.h          AArch64 instruction encoders (the arm64 JIT back end)
  guest/x86_64/      x86_cpu.c (decoder, integer), x86_sse.c, x87.c,
                     x86_sys.c (CPUID, syscalls, rt_sigframe),
                     x86_block.c (block engine), x86_jit.c (JIT, engine
                     selection), x86_ops.h
  guest/aarch64/     a64_cpu.c (integer, branches, loads/stores),
                     a64_fp.c (FP, Advanced SIMD), a64_sys.c (syscalls,
                     rt_sigframe), a64_block.c (translation cache), a64_ops.h
```

The command line is qemu-user's: `<translator> [-0 argv0] [-d] [-strace]
<program> [args...]`; the kernel passes `-0 <argv[0]> <path> <args>`.

## Execution

The x86_64 guest has three engines that compute identical results:

- the interpreter (`x86_cpu.c` + `x86_sse.c` + `x87.c`) -- one instruction
  at a time, the reference (`XL_NOBLOCKS=1`);
- the block engine (`x86_block.c`) -- traces of predecoded "uops" (handler +
  operands), direct-threaded (each handler tail-calls the next,
  `musttail`), the interpreter for forms without a handler of their own.
  The default on hosts without a JIT back end, and under `XL_NOJIT=1`;
- the JIT (`x86_jit.c`, on an arm64 host) -- the default there. A block (a
  run of instructions entered at the top; it continues past conditional
  branches, whose taken side leaves through a side exit) is decoded,
  analysed and compiled to AArch64 code:
  - **Registers.** The sixteen integer registers live in host registers
    (x9-x15, x19, x21-x28) while generated code runs; x20 holds the CPU
    structure, x29 the `gthread_t`. XMM, x87 and MMX state stay in memory.
  - **Flags.** The interpreter's lazy flags are the state between blocks.
    Inside one, a backward liveness pass drops flags nobody reads, and the
    instruction that set the live ones leaves its operands where they are
    (a register nothing overwrites, or a copy in one of four flag
    temporaries): a consumer rebuilds the host condition from them, so
    `cmp` + `jcc` becomes `cmp` + `b.cond` and `test` + `jz` a `tst` +
    `b.eq`. The lazy state is written to memory only at exits, before
    calls into C and for the rare conditions the interpreter's `x86_cond()`
    evaluates. COMISS/UCOMISD keep EFLAGS bits in a register.
  - **Memory.** Guest addresses are host addresses: a load is a load. Each
    access has a "fault site" (host offset -> guest instruction + where the
    pending flags are), so the guest RIP is not stored before it; a fault in
    generated code is mapped back by `guest_jit_fault()`, which rebuilds the
    registers and flags from the host signal context, and the guest gets
    its SIGSEGV on the right instruction with the right state. Every
    instruction does all its memory accesses before writing a register.
    Once there is a second thread, loads are followed by `DMB ISHLD` and
    stores preceded by `DMB ISHST` (x86's TSO); LOCKed read-modify-writes,
    XCHG, XADD and CMPXCHG are `LDAXR`/`STLXR` loops plus `DMB ISH`
    (misaligned ones go to the interpreter).
  - **SSE.** Moves, logic, integer SIMD, unpack/pack, shuffles, shifts,
    PMOVMSKB, scalar and packed FP arithmetic, MIN/MAX, compares and the
    common conversions are AdvSIMD; FP results are checked for NaN and the
    interpreter redoes the instruction when there is one (x86 NaN rules
    differ from AArch64's). Everything else (x87, MMX, string instructions,
    CPUID...) calls the interpreter for that one instruction from
    generated code.
  - **Linking.** A direct exit jumps to the dispatcher once, which then
    patches the exit's branch to the target block; indirect jumps, calls
    and returns probe a per-thread indirect-branch table inline. Each block
    checks for pending signals on entry, so chained loops still stop.
  - **Code that changes.** A block from a range mapped executable and not
    writable is trusted until that range is unmapped, remapped or made
    writable: `syscall.c` logs those changes and every thread's cache kills
    the affected blocks the next time it enters the dispatcher (a killed
    block's entry is patched to go back to the dispatcher, so branches
    already chained to it stay correct). Any other block compares its guest
    bytes on every entry. A full cache is simply emptied.

  The guest program and its dynamic linker are loaded into one anonymous
  reservation each: the whole pages of every segment are mapped privately
  from the file over it, so the host pages them in on demand (and the
  ImplusOS arm64 kernel shares them between processes); only the partial
  pages at segment ends are copied in with `pread`.

  The host must allow writable and executable anonymous memory and EL0
  cache maintenance (`DC CVAU`/`IC IVAU`, `CTR_EL0`): the arm64 kernel sets
  `SCTLR_EL1.UCI`/`UCT`, as Linux does.

The AArch64 guest has the interpreter (`a64_cpu.c` + `a64_fp.c`) and the
block engine (`a64_block.c`), the default.

Rough numbers on an Apple M3 (Linux VM under HVF), interpreter / block
engine / JIT: an x86_64 benchmark mix (`Tests/x86_bench.c`) 2.36 / 0.61 /
0.06 s; a dash `while` loop of 100,000 iterations 12.9 / 3.8 / 0.26 s;
xkbcomp compiling a full keymap 0.75 / 0.17 / 0.02 s. On arm64 ImplusOS
(same host, HVF), block engine / JIT: the benchmark 1.22 / 0.048 s, the
dash loop 7.6 / 0.26 s, xkbcomp 0.46 / 0.048 s.

## Debugging

At run time (environment of the translated program):

- `XL_DEBUG=1` — log the load and unusual events.
- `XL_STRACE=1` — log every guest system call (canonical names, path
  arguments as strings) with its result; `XL_STRACE=2` also logs the call
  before it is made, which shows a call that blocks.

A guest that dies of a signal it does not handle gets a line on stderr with
the guest PC, the fault address and the registers; an unsupported
instruction shows up as SIGILL with the instruction bytes.

When a program works with `XL_NOJIT=1` but not with the JIT (x86_64 guest),
these narrow down which translation is at fault:

- `XL_JIT_DEBUG=<letters>`: `s` SSE through the interpreter, `f` flags
  written out after every instruction, `a` LOCKed instructions through the
  interpreter, `c` no chaining and no indirect-branch cache, `1` one
  instruction per translation, `v` every instruction also run by the
  interpreter from the same state and the two results compared (registers,
  XMM, RIP, the flags the instruction defines); the first mismatch at each
  guest PC is logged with its bytes. `v` skips instructions that write
  memory, which cannot be run twice.
- `XL_JIT_FBK=<lo>-<hi>`: instruction kinds `lo..hi` (the `K_*` enum in
  `x86_jit.c`) go to the interpreter. Running the program once per kind
  finds the kind whose translation is broken.
- `XL_JIT_SP=<lo>-<hi>`: the same for the native SSE plans (`SP_*`); those
  go through the interpreter's SSE helper instead.
- `XL_JIT_FPC=<lo>-<hi>` (hex, offsets from the program's load address):
  `f` only for the translations that start in that range. Halving the
  range while the program still works finds the one block whose flags go
  wrong in a program too big to step through (it is how the
  `bn_mul_mont` carry bug in Chromium's BoringSSL was found).

At build time (`make ... XL_EXTRA_CFLAGS=...`), for a program the kernel
starts itself (AUTOSTART), whose environment cannot be set:

- `-DXL_DEFAULT_STRACE=1` (or `2`) — strace on by default.
- `-DXL_WATCHDOG=200000000ULL` — x86 guest under `XL_NOBLOCKS=1`: log the
  PC and registers every so many instructions without a system call (a
  guest spinning somewhere).
- `-DXL_PROFILE` — at `exit_group`: trace entries, translations, uops per
  trace, and which instructions still went to the interpreter (with the
  JIT, also the guest PCs that did so most often).
- `-DJX_CODE_SIZE=... -DJX_META_SIZE=...` — shrink the JIT's per-thread
  arenas (bytes) to exercise cache flushes.

## Testing

The x86_64 guest is checked differentially: a test program that runs
~159,500 instruction/operand combinations (integer ALU with flags,
shifts, multiply/divide, bit ops, SSE/SSE2/SSE3 with NaN, infinities and
denormals, conversions, x87; all sixteen conditions read straight after
each flag-setting form with SETcc and with Jcc, including (U)COMISS/SD;
the carry read by ADC/SBB/INC after a logic op or TEST cleared it, over a
stale CF = 1; plain and LOCKed read-modify-writes on memory, aligned and not; every
immediate of the SSE shuffles, every count of the SSE shifts, memory
operands of the SSE forms the JIT runs natively) prints its results; the same source built for macOS
x86_64 and run under Rosetta gives the reference output, and every engine
must match it line for line. The sources are in `Tests/`:

```sh
# reference (macOS, Apple silicon)
clang -arch x86_64 -O1 -mno-red-zone -msse3 -w Tests/x86_diff.c -o x86_diff-mac
arch -x86_64 ./x86_diff-mac > ref.txt
# under test: a Linux binary, run as <translator> ./x86_diff-linux; PIE,
# because the arm64 kernel cannot place a fixed-address program this low
x86_64-elf-gcc -O1 -mno-red-zone -msse3 -ffreestanding -fPIE -nostdlib -w \
    -c Tests/x86_diff.c -o x86_diff.o
x86_64-elf-ld -pie --dynamic-linker /lib64/ld-linux-x86-64.so.2 \
    -o x86_diff-linux x86_diff.o
# compare, leaving out the CMPXCHG lines: Rosetta sets its flags from
# destination - accumulator, Bochs and QEMU (and this translator) from
# accumulator - destination
diff <(grep -v ^cmpxchg ref.txt) <(grep -v ^cmpxchg out.txt)
```

`Tests/x86_bench.c` (built the same way, with -O2) is the benchmark behind
the numbers above. Two more programs cover what the differential test
cannot, linked against glibc from the Linux runtime stage (`x86_64-elf-gcc
-O2 -fPIE -ffreestanding -c`, then `x86_64-elf-ld -pie` with
`--dynamic-linker /lib64/ld-linux-x86-64.so.2` against
`Build/x86_64/LinuxRuntime/stage`'s `libc.so.6` and `ld-linux-x86-64.so.2`):

- `Tests/x86_jit_test.c` -- self-modifying code in an RWX page, code
  rewritten under `mprotect`, new code mapped at an old address, SIGALRM
  ending a loop that never leaves its block, and a SIGSEGV whose handler
  checks the registers and flags and fixes the address so the faulting
  store is retried;
- `Tests/x86_mt_test.c` -- message passing between threads with no fences
  (fails within a few runs if loads or stores are not kept in order) and
  LOCKed counters and a CMPXCHG spinlock from four threads.

The interpreters can be exercised on a plain Linux machine of the host CPU
(the translator is an ordinary static Linux program): run
`x86_64.ELF /path/to/x86_64/dash -c '...'` on an arm64 Linux box or VM.
For the AArch64 interpreter there is a shortcut: `make ARCH=arm64
GUEST=aarch64` builds an AArch64-on-AArch64 translator, so its results can be
compared with the same program run natively on an arm64 machine.
