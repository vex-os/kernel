# Contributing to Vex

Vex is a hobby operating system kernel written in C (gnu2x) and built with Meson. Limine boots it over UEFI. amd64 is the only implemented
architecture. The Meson build maps the riscv64 and aarch64 names, and their EDK2 and Limine binaries are vendored, but there is no
`kernel/arch/` code for them yet.

`ktest/` is the kernel's own test image: the Limine config, the image script, the test runner `kst` and the test programs that it runs.
Real userspace lives elsewhere.

Read this document before you send a change. [docs/kernel/architecture.md](docs/kernel/architecture.md) describes where things are in the
kernel.

## AI-assisted contributions

You can use AI tools, but a human drives the work:

- Review every line that a tool generates. Understand it well enough to explain it and to fix it.
- You are responsible for the change, as if you wrote all of it yourself.
- Unreviewed, bulk or fully automated changes are not accepted.
- Mark a commit that has material AI help with an `Assisted-by:` trailer (see [Trailers](#trailers)).

## Building and running

Builds must be cross builds. Meson stops with an error if you do not give a cross file.

```sh
meson setup build --cross-file cross/amd64-clang.ini   # or cross/amd64-gcc.ini (x86_64-elf-* toolchain)
ninja -C build                    # builds build/kernel/vexsys.elf
ninja -C build ktest-image        # builds build/ktest/vexsys.boot.img
ninja -C build ktest-run          # builds the image and boots it in QEMU with OVMF
```

- The defaults are `werror=true` and `warning_level=2`, so a new warning fails the build.
- The image is a GPT disk with a FAT16 ESP, an ext2 DATA partition with the tests, and a FAT16 FAT partition (`ada0n1p3`) for the FAT
  tests. Making it needs `sfdisk`, `mkfs.fat`, mtools (`mmd`, `mcopy`) and `mke2fs` from e2fsprogs.
- `ktest/qemu.amd64.sh` puts the shell port on stdio (Ctrl-A X quits QEMU) and the log port in `kernel.log`. It switches the terminal to raw
  mode, as the terminal would otherwise echo each line and the guest echoes it again. Under WSL it calls `qemu-system-x86_64.exe` with
  Windows paths. Extra arguments:
  - `slow`: icount. `slow=N` sets the icount shift: one instruction takes 2^N ns, so a bigger N is slower (`slow` is `slow=0`).
  - `verbose`: `-d cpu_reset,int -no-reboot`.
  - a number `N`: log to `qemu_N.log` instead of `qemu.log`.
- `ktest/limine.conf.in` is a template. `ktest/meson.build` sets the terminal (`tty1337`), the shell port (`serial1`, COM1) and the log port
  (`serial2`, COM2), and pins them to the q35 COM ports with `unit=`. Limine boots the kernel with `init=/kst.elf` from the DATA partition.
  The kernel log goes to the log port and to `fbd0`.

### Configuration

The kernel uses Kconfig. Kconfiglib is vendored in `contrib/kconfiglib`. The config lives in `build/.config`. At configure time,
`scripts/kconfig/sync_config.py` seeds it from `configs/<arch>_defconfig`, or from `-Ddefconfig=<file in configs/>`. Meson reconfigures when
any Kconfig file changes.

- Targets: `ninja -C build menuconfig | oldconfig | listnewconfig | defconfig | savedefconfig`.
- To update a defconfig, run `ninja -C build savedefconfig` and copy `build/defconfig` over `configs/<arch>_defconfig`. Defconfigs only
  store values that differ from the Kconfig defaults.
- The generated `generated.config.h` is force-included (`-include`) into every kernel translation unit. `CONFIG_*` macros are always
  visible, so do not add includes for them.
- Gate optional subsystems in `meson.build` with `kconfig.get('CONFIG_X', 'n') == 'y'`, not with `#ifdef` around whole files. A new
  optional driver or filesystem needs a Kconfig entry and a matching gate in the directory's `meson.build`.

## Testing

There is no unit test suite. Boot the test image and run the tests in `kst`:

- `kst` clears the screen on the shell terminal and takes commands: `help`, `list`, `run <test|path>...`, `all`, `clear`, `exit`. It writes
  to kplog only until the terminal is open.
- Each test is a separate ELF from `ktest/test.<name>.c`. `ktest/meson.build` lists them in the order that `all` runs them. A test that
  takes over the machine (framebuffer) goes last.
- A test returns 0 for success and checks with `ktest_expect` from `ktest/ktest.h`.
- `-Dktest=false` skips the whole directory and drops the `ktest-image` and `ktest-run` targets.
- After a run, check the DATA partition with `e2fsck -fn "build/ktest/vexsys.boot.img?offset=$((10240 * 512))"`.

## Code style

### Formatting

Format C code with `clang-format-20` and `.clang-format`: 4 spaces, 140 columns, `if(` with no space before the parenthesis, `T* p`. Other
versions can format differently. `contrib/**` is excluded. Check changed files with `clang-format-20 --dry-run --Werror <files>`.

### Language

- C23 idioms are used throughout: `nullptr`, `static_assert`, `alignas` (`_Alignas` is deprecated in C23), `unreachable()`, `restrict`
  pointer parameters, and `{}` for empty initialization (not `{ 0 }`).
- Do not initialize a struct with its fields on one line. Either list the designated fields one per line, with a comma after the last one
  so clang-format keeps them apart, or start from `{}` and assign the fields after it. A compound literal passed as an argument becomes a
  named local first. File-scope tables (ops, drivers, Limine requests, lookup tables) keep their initializers.
- Give every struct type a name, also for local lookup tables: `struct xattr_space { … };`, then `static const struct xattr_space spaces[]`.
  Anonymous enums for constants are fine.
- Give a non-trivial size expression a name with a file-level `#define`, for example `#define MIN_REC_LEN (size_t)(4 + sizeof(...))`,
  instead of writing it inline as an array bound.
- Use a ternary only for a short pick between constants (`valid ? VALID_VALUE : 0`). When either side has a call, a dereference or a
  non-trivial operand, write `if`/`else`.
- A `switch` case does not open a scope. Move a case body that needs locals into a static function: `case FIONREAD: return
  count_input(device, argument);`.
- Never use a standalone increment or decrement statement (`++x;`, `x++;`, `--x;`, `x--;`). Outside a `for` header, write `x += 1` or
  `x -= 1`. Inside a `for` header, a one-letter counter uses `++i` or `--i`, and a longer name uses `page += 1` or `level -= 1`.
- A `for` header has all three parts or none (`for(;;)`). A loop that does not fill all three is a `while`, with the step at the end of its
  body.
- Test a bitmask against zero explicitly: `if(0 == (flags & FLAG))`, not `if(!(flags & FLAG))`.
- Compare pointers to `nullptr` only for the null case (`if(ptr == nullptr)`). For non-null, use the pointer itself (`if(ptr)`). A call
  whose result only signals failure can be negated directly: `if(!pmentry_get(table, i, true))`.
- C23 still allows implicit conversions, and the code relies on them. Never write `!= 0`, `!= nullptr`, `0 !=` or `nullptr !=`: use a value
  or pointer that is tested for non-zero by itself (`if(count)`, `bool taken = *prev;`). `== 0` and `== nullptr` are fine.
- Cast only where an implicit conversion does not do the job: where the compiler would warn (a narrowing or a change of signedness that it
  flags), a pointer to or from an integer, a varargs argument. A narrowing that compiles without a warning, such as
  `uint32_t value = UINT64_C(42);`, needs no cast. Put the operand of a cast in parentheses: `(uint32_t)(address)`, `(void*)(fault)`.
- Do not dereference the result of a call in place (`this_hart()->caps`, `pframe_of(phys)->flags`). Put the result in a named local first
  (`hart_local_t* hart = this_hart();`), then use its fields. `(*prev)->next` in list walks is a plain pointer dereference and is fine.
- Do not put a call in the middle of the arguments of another call (`pmap_map(pmap, page_align(virt), pframe_phys(pframe), vprot)`), and do
  not nest calls more than one level deep. Put the result in a named local first, so that the name says what the value is. A call that is the
  only or the last argument is fine: `kfree(port_of(interface))`, `panic("%s", strerror(retval))`. So is a conversion of one value to another
  type: `virt_addr(value)`, `phys_addr(value)`, `user_addr(value)`, `user_addr_to_virt(address)`, `sizeof` and casts.
- Keep a condition short: three operands joined by `&&` or `||` at most, and on one line. Name the parts in `bool` locals
  (`bool inside = address_less(start, end);`), or split the test into several `if` blocks that each return or continue.
- Do not pass `int64_t`, `uint64_t`, `time_t` or other fixed-width integers to `%ld` or `%lu`. Cast to the type that the format names
  (`(long long)(x)` with `%lld`, `(unsigned long long)(x)` with `%llu`), or keep the value in that type from the start.
- `kmalloc` is relatively heavy. Keep it off hot paths (per allocation, per write, per create); mount and setup paths are fine. Slab only
  serves objects up to `SLAB_MAX_SIZE` (half a page), so do not allocate block-sized buffers. Walk the data one stack-sized piece at a time
  instead. For a small fixed object, use `slab_alloc` with `static_assert(sizeof(...) <= SLAB_MAX_SIZE)`.

### Types and names

- Every kernel type is used through a `_t` typedef. Tags end in `_s`, `_u` or `_e` (`struct thread_s`, `union fat_entry_u`,
  `enum cpu_feature_e`):
  - Forward declarations of the types of other files (`typedef struct vnode_s vnode_t;`) form one block right after the includes. C11
    allows identical typedefs to repeat across headers.
  - The typedefs of the file's own types form a second block right before its first struct definition (after the `#define`s), in
    definition order. Structs then refer to each other through pointers in any order.
  - The typedef of an enum goes right after its definition, as enums cannot be forward-declared.
  - Do not make pointer typedefs.
- Name a variable or field after the stem of its type (`thread_t* thread`, `file_t* file`), not an abbreviation such as `thrd` or `fp`. Use
  a role name such as `parent`, `curr` or `dir` when that says more.
- Use whole words in new names, also where POSIX or Linux abbreviate: `process_group_t`, `process_group_find`, `process->group`, not
  `pgrp_t`. POSIX ids that match syscall names (`pgid`, `sid`) and established terms (`vnode`, `pmap`, `pslab`, `pdma`, `hart`, `irq`) stay.
  Do not rename existing files: the short file names will be renamed together later.
- Public functions are named `<subsystem>_<verb>`: `block_interface_add_root`, `block_interface_find`, `block_interface_for_each`, not
  `add_block_interface_root`.
- A function named `_get`, `_find` or `_locate` that returns a reference-counted object (`process_locate`, `block_interface_find`,
  `process_get_cwd`) gives the caller a new reference to drop. Only a function that breaks this rule needs a comment that says so.

### Headers

- Kernel-private headers use only `#pragma once`.
- Headers under `include/usr/` are the userspace ABI. They use a classic path-derived guard, for example `usr/vex/errno.h` →
  `USR_VEX_ERRNO_H`, closed with `#endif /* USR_VEX_ERRNO_H */`. Never start these names with `_`. Keep the guard in sync when you rename
  the header.
- Headers under `include/usr/` must compile as ANSI C89/ISO C90: no `//` comments, `nullptr`, `bool`, `static_assert`, `restrict`, `inline`,
  designated initializers, or declarations mixed with code. Only `usr/vex/types.h` touches `<stdint.h>`, and only behind its C23/C++
  check.
- Structs and unions under `include/usr/` use the `__k_`-prefixed fixed-width types (`__k_int32_t`, `__k_uint64_t`, `__k_pid_t`) for every
  field, never `int` or `long`: C fixes only the minimum sizes of its plain types. A field with the address of user memory is a
  `__k_uintptr_t`, or a pointer. A field with a size in memory is a `__k_size_t`.

### Order in a file

Top-level order in a translation unit:

1. includes;
2. forward declarations (typedefs of other files, `extern` asm/linker symbols);
3. compile-time constants (object-like `#define`s, anonymous `enum`s);
4. the file's own typedef block;
5. type definitions;
6. read-only data that lands in `.rodata`, such as `static const char* const names[] = { … }`;
7. static and global variables, in any order among themselves;
8. static functions;
9. public functions;
10. closers: `init_*` functions with their `initcall_*`, and `sys_*` handlers with their `define_syscall`, each macro right after its
    function.

More details:

- Static functions go in dependency order, callees first, so a static prototype is only necessary for mutual recursion.
- An ops or driver table (`static const vnode_ops_t foo_ops = { .read = &foo_read, … }`) goes right after the last function it points to.
- Function-like macros and `static_assert`s stay next to what they belong to.
- Headers follow the same order, but all prototypes come before the `static __always_inline` bodies.

### Comments

Code documents itself through names. A comment is only worth having when it says something the code cannot: why it is done this way and
not the obvious way, an invariant (locking, ownership, units, range), or an outside requirement (a spec, the Linux ABI, a hardware quirk).
Never restate what the code next to it does.

- Before you write a comment, try to make it unnecessary: rename (`free_slots` instead of `slots // A set bit means free`), move the code
  into a function whose name says what it does, or turn the claim into a `static_assert` or `kassert`.
- Do not describe the algorithm of a function above it. Preconditions and ownership rules that every caller must respect stay at the
  declaration: in the header for public functions, at the definition for static ones. A guarantee that only some callers rely on goes at
  those call sites, as the reason that the caller is correct.
- Constants and fields get a comment only when it expands a spec name (FAT `BS_*`/`BPB_*`, AHCI registers, ELF), adds units or meaning
  that the name lacks, or states an invariant. Format headers such as `vex/elf.h` keep their full constant tables for future ports, but
  drop comments that only repeat the name.
- Write comments approximately in ASD-STE100 Simplified Technical English: short sentences, one statement per sentence, active voice,
  present tense, simple common words with one meaning each.
- Use `//` (`#` in assembly and shell), start with a capital letter, and leave off the trailing period. Headers under `include/usr/` use
  `/* */`.
- A short note about a single line goes after it, if the line still fits in 140 columns.
- Wrap longer comments at clause boundaries, at roughly the width of the code they describe. Prefer two short lines to one line that runs
  out to 140 columns.
- Tagged comments:
  - `TODO:` is work that nobody has planned yet.
  - `UNDONE:` is work whose approach is known but not done.
  - `FIXME:` is either of those where the current behavior is a bug.
  - `NOTE:` is a remark worth the attention of the reader. Repeat it to raise the weight: `NOTENOTE:` marks something that the surrounding
    code cannot be understood without.
- A function or scope left empty on purpose holds a single `// empty` (`/* empty */` in C89 code).
- Do not leave commented-out code. The jokes and rants already in comments are part of the history of the project; leave them alone.

#### Documentation comments

A public function whose contract the signature cannot express (a calling protocol, lock or interrupt state, reference ownership, return
codes with distinct meanings, an obligation of an `arch_*` implementation) gets a `///` block above its prototype. Most functions get no
such block. In the block:

- Prose is one short line that says what the function does, only where the name does not, or the steps of a calling protocol. No reasons.
- `@pre` states what must be true at the call (locks held or not, interrupt state, who calls, references the caller holds). `@post` states
  what is true afterwards when the prose does not say it.
- `@param` only for a parameter whose meaning the name does not carry. `@retval` lists return values that mean different things.
  `@return` describes a single kind of result.
- `@note` holds what an implementation or a caller must know that is not a precondition: a reason (`takes no lock, as a panic mid-write
  writes again`) or an obligation (`vcache_remove the target once its last name is gone`). Keep it to a sentence.
- Commands use `@`, not `\`. The text of a command wraps onto plain `/// ` lines, not indented under it.
- The clang cross file builds with `-Wdocumentation`, so a `@param` that drifts from the signature fails the build.
- An op in an ops table follows the same rules: a block only when the contract of the op is not obvious, commands included, as hover shows
  it where the op is implemented. Clang does not check commands on struct fields, so when you change the signature of an op, update its
  block by hand.
- Every declaration gets its own complete block, even when that repeats a neighbor: `///` attaches only to the next declaration, and hover
  tooltips show nothing else. A comment above a group or a "same as X" does not help.

### Vendored code

`contrib/` is vendored third-party code: Limine, uACPI, kconfiglib and EDK2 firmware. Each component has a `README.vex` and an `update.sh`.
Do not edit it in place; update it through the script.

## Commit messages

- Make one logical change per commit.
- The subject line is `<area>: <summary>`:
  - Take the area from the directories that the commit changes: `fs:` for a change only under `kernel/fs/`, `ext2:` for one only in the
    ext2 driver, `ktest:` for the test image. Use `general:` only when a commit spans several areas. Keep unrelated areas out of a commit
    instead of widening its area.
  - Write the summary in lowercase and in the imperative mood (`fs: reject getdents on non-directories`), with no trailing period.
- Keep all lines within 72 columns.
- Most commits need no body. Write one only when the reader needs something that the subject and the diff do not show: why the change is
  done this way, or an outside contract (a spec, the Linux ABI, a hardware quirk). Do not list the changed files or functions.
- Write the body in ASD-STE100 Simplified Technical English, as for comments: short sentences, one statement per sentence, active voice,
  present tense.

### Trailers

Trailers go in the last paragraph of the message, after a blank line.

- A commit that has material AI help gets an `Assisted-by:` trailer in the form `AGENT_NAME:MODEL_VERSION`, after the Linux kernel. Use the
  full lowercase model identifier, not only the model family:

  ```
  fs: leave a mount for the parent of its mountpoint on ".."

  Assisted-by: Claude:claude-opus-5-5
  ```

- Do not add `Co-authored-by:` for AI tools.
