# Writing a test

A test is one C file in `ktest/`. The build makes one ELF for it, and `kst`
runs it as a child process on the test image.

## The file

Name the file `ktest/test.<name>.c`. The program returns 0 for success and
uses the checks from `ktest/ktest.h`:

```c
#include "ktest.h"

int main(int argc, char** argv)
{
    ktest_printf("hello from %s\n", argv[0]);
    ktest_assert(argc == 1);

    return 0;
}
```

- `ktest_expect(expression, expected)` compares the value and keeps the test
  running after a mismatch.
- `ktest_assert(condition)` stops the test with a message.
- `ktest_result()` returns 0 when every check held, and 1 otherwise. A test
  that uses `ktest_expect` returns it from `main`.

`ktest.h` also has file helpers, formatted output, thread helpers and raw
system call wrappers (`ktest_syscall1` to `ktest_syscall6`). The user ABI is
in `kernel/include/usr/vex/`; a test includes it as `<vex/...>`.

## Add it to the build

Add the name to `ktest_programs` in `ktest/meson.build`:

```meson
ktest_programs = [
    'hello',
    'uname',
    # ...
]
```

The order in that list is the order that `kst all` runs the tests. A test that
takes the machine over, such as the framebuffer test, goes last.

The build copies each ELF to `/kst/<name>.elf` on the DATA partition, and
`kst` finds it by name.

## Run it

Build and boot the image:

```sh
ninja -C build ktest-run
```

Then run one test or all of them at the `kst` prompt:

```
> run hello
> all
```

`kst` prints `[ OK ]` or `[FAIL]` with a timestamp for each program. A failure
names the exit status, the signal or the failed `waitpid`.

## Rules

- A test is a separate process, forked by `kst`. It cannot see the memory of
  another test.
- A test runs with the full capability set, because `kst` is init. A test can
  mount, format and corrupt the DATA partition.
- The tests share the DATA partition. A test that writes leaves the files for
  the tests that follow.
- There is no unit test suite. Check a kernel change on the test image.
- After a run, check the ext2 partition with `e2fsck`. See
  [the test image](../getting-started/ktest.md).

## Where the code is

| File | Contents |
|---|---|
| `ktest/ktest.h` | The checks and the helpers that a test uses |
| `ktest/ktest.c` | The implementation of the helpers |
| `ktest/kst.c` | The runner |
| `ktest/meson.build` | The test list and the build rules |
| `ktest/test.<name>.c` | One test |
