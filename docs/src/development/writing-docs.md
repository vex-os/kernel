# Writing docs

## Page shape

A page about a kernel-wide mechanism follows this order:

1. what it is and which problem it solves;
2. how it works, with one diagram if the flow is not obvious;
3. the invariants and the traps that a reader must know;
4. where the code is.

A page about one implementation (a driver, a filesystem, a bus) uses fixed
headings:

- `## Discovery`
- `## Attach`
- `## Data path`
- `## Why it is done this way`
- `## Limits`
- `## Source`

`## Why it is done this way` is the reason the page exists. Use one
sub-section per decision: the constraint, the choice, and what breaks
otherwise. If a page has nothing for that section and the implementation is
small, do not create the page. Add a row to the table of its section instead.

## Where a page lives

`architecture/` explains a mechanism that several parts use. `drivers/`,
`filesystems/` and `buses/` explain one concrete implementation. A mechanism
page must not grow into a tour of one driver, and a driver page must not
repeat the mechanism.

## Source links

Link to the source with an absolute GitHub URL:

`[ahci.c](https://github.com/vex-os/kernel/blob/main/kernel/drivers/storage/ahci.c)`

Use the line anchor for the function or the decision that the text is about.
Relative links work on GitHub but break in the built book, because the book
contains only `docs/src`.

## Synced tables

A table that must not drift from the code, such as the syscall numbers, can
include the source block at build time. mdBook reads the file when it builds
the page, so the table follows the header. Put `ANCHOR:` and `ANCHOR_END:`
comments around the block in the header, then point the include directive at
it. The path is relative to the page file; the mdBook guide has the exact
syntax.

If the path gets awkward or the block needs formatting, generate the page with
a script instead. The kconfig scripts are the local precedent.

## Prose

Write in ASD-STE100 Simplified Technical English, as the code comments do:
short sentences, one statement per sentence, active voice, present tense.
Explain the reason and the invariant, not the steps of the code. Do not copy a
`///` block from a header into the page.

## Adding a page

Add the file under `docs/src`, then add one line to `SUMMARY.md`. A file that
is not in `SUMMARY.md` is not part of the book. Run `mdbook serve docs` to
check the page while you write it, and `mdbook build docs` before you commit.
