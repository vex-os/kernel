# Plans

This directory holds work plans. The plans are not part of the book. The book
describes the code as it is. A plan describes work that is not done.

- [kernel-plan.md](kernel-plan.md) — the kernel work, grouped by subsystem.
  It has the item IDs, the cost, the waves and the decisions.
- [mlibc-sysdeps-plan.md](mlibc-sysdeps-plan.md) — the mlibc sysdeps session
  that uses the finished system call ABI.

The plans share one rule. An item is done only when a ktest passes and the
architecture chapter matches the code. The plan then loses the item. A plan
that describes the past is worse than no plan.

Base commit of this version: `6312e19`, 2026-11-03.
