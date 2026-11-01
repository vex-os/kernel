# Random numbers

Vex supplies random bytes to `getrandom(2)`, `/dev/random` and `/dev/urandom`. One generator serves all three. The design has three layers: sources, a pool and a generator.

## The three layers

```
 sources         pool                output
+------------+   +------------+      +------------+
| rdseed     |   | BLAKE2s    | seed | ChaCha20   |
| rdrand     |-->| hash state |----->| fast key   |--> getrandom(2)
| jitter     |   | + credit   |      | erasure    |--> /dev/random
| limine     |   +------------+      +------------+--> /dev/urandom
| irq events |
+------------+
```

The sources collect noise. The pool mixes the samples and counts the entropy that the kernel trusts. The generator turns the pool into output.

## Sources

A source declares itself with `define_random_source_polled` or `define_random_source_event`. The declaration lands in the `.random_sources` linker section. The kernel walks that section at boot.

| Source | Kind | Trusted | Sample | Credit |
| --- | --- | --- | --- | --- |
| `rdseed` | polled | yes | 32 bytes | 256 bits |
| `rdrand` | polled | no | 32 bytes | 256 bits, not credited |
| `jitter` | polled | yes | 64 bytes | 8 bits |
| `limine` | polled | yes | 64 bytes | 512 bits, capped at 256 |
| `irq` | event | yes | 32 values per buffer | 1 bit per 64 events |

A polled source has a `probe` function and a `poll` function. The probe tells whether the hardware exists. The poll fills a buffer of at most 64 bytes. A short poll earns a proportional share of the credit.

An event source has no poll function. Its code pushes an event into a buffer for the current hart. The IRQ source mixes the interrupt number into that buffer with a timestamp.

The `trusted` flag controls only the credit. Every sample enters the pool. An untrusted source can add bytes to the pool. It cannot make the generator ready.

The `random.trust_<name>` kernel option changes the default. The value `0` removes the credit. Any other value grants it.

The kernel polls a polled source at most once in 100 ms. A caller can poll the sources before it reads bytes. A worker thread also polls them when the pool needs noise. `random_source_poll_all()` takes a trylock, so one poller runs at a time.

The kernel stops asking a source that returns `-ENODEV`. It also stops after five failures in a row.

### Why the trust differs

`rdseed` is the architectural entropy instruction. The kernel counts its output as entropy.

`rdrand` returns the output of a random generator in the CPU. The kernel mixes the bytes and counts no entropy. A compromised CPU must not seed the kernel alone.

The kernel trusts `limine` as a whole. Limine collects entropy from RDSEED, RDRAND and the UEFI random generator. It falls back to the PCG32 generator. The reply does not say which source made a value. The kernel cannot separate the sources. It trusts the bootloader, unless the command line changes the trust.

`jitter` measures time deltas around a small workload. The timing of a virtual machine can be regular. The source claims one bit for eight measurements. A poll with more than 32 repeated deltas returns `-EAGAIN`.

`irq` events are weak because software can observe interrupt timing. One event earns 1/64 of a bit. The stream is continuous, so the events still contribute.

## Credit accounting

The pool counts trusted bits in `credit_bits`. The count stops at 256 bits. The generator needs 256 bits to take its first seed. A reseed sets the count back to 0.

This count is a policy, not a measurement. The kernel cannot test a sample. The estimate comes from the author of the source. The trust flag says whether the kernel accepts the estimate.

Event credit accumulates in fractions of a bit. The worker keeps the remainder for the next drain.

## The pool and reseeds

The pool is a BLAKE2s-256 hash state, not a byte buffer. `random_add()` feeds the sample into the state. It also adds the credit.

A reseed does four steps:

1. It finishes a copy of the pool. The result is a 32-byte seed.
2. It starts the pool again and feeds the seed back into it. The next seed depends on this one.
3. It mixes the seed into the key with a keyed BLAKE2s. The old key is an input, so a weak seed cannot reduce the strength of the key.
4. It sets the credit to 0. On the first reseed it also triggers the ready event.

Two conditions start a reseed. The credit must be at least 256 bits, and 100 ms must have passed since the last reseed. The first reseed ignores the interval.

The code checks the conditions in two places. `random_add()` checks them while the generator has no seed. The output path checks them for each 256-byte chunk. There is no periodic reseed timer.

Between reseeds, the generator produces bytes from its key.

## Output bytes

`fill()` produces output in 256-byte chunks. For each chunk it calls `generate()`:

1. It starts ChaCha20 with the current key, a zero nonce and counter 0.
2. It XORs the keystream into the output buffer.
3. It takes the next 64-byte block as the new key.
4. It zeroes the block and the stream state.

Each chunk uses a fresh key, so the nonce stays zero. The erasure means a later compromise of the key does not reveal earlier output.

Before the first seed, `generate_unseeded()` derives a temporary key from the pool. It feeds that key back into the pool, so the next call differs. The bytes do not repeat, but an attacker can guess them. `GRND_INSECURE` uses this path.

## Waiting for the first seed

`random_wait_ready()` polls the sources and checks the ready flag. A blocking caller waits on the ready event with a 100 ms timeout. The timeout matters because no other thread polls the sources while the caller sleeps. The caller wakes, polls again and repeats.

`getrandom(2)` has two flags:

- `GRND_NONBLOCK` fails with `EAGAIN` when the generator has no seed.
- `GRND_INSECURE` skips the wait and accepts weak bytes.

Without `GRND_INSECURE`, the call waits for the first seed. Then it copies the bytes out in 256-byte chunks. A pending signal or stop ends a long call early. The call returns the bytes that it produced.

`/dev/random` and `/dev/urandom` use the same generator and the same rules. Both wait for the first seed. `O_NONBLOCK` turns that wait into `EAGAIN`. A write to either device stirs the pool and earns no credit. `poll()` reports readable only after the first seed.

## Boot sequence

| Stage | Action |
| --- | --- |
| early | The kernel initializes the ready event and the worker event. |
| bootinfo, late | The kernel moves the Limine entropy out of bootloader memory and zeroes the original. |
| bootinfo, last | The kernel probes and polls the sources once. The generator usually takes its first seed here. |
| services | The kernel creates the per-hart event buffers and starts the worker thread. |
| dev | The kernel registers `/dev/random` and `/dev/urandom`. |

The IRQ hook drops events until the worker publishes its buffers. Event noise therefore starts after the services stage.

## Why it is done this way

- **The pool is a hash, not a buffer.** The sources produce different sizes and different claims. A hash accepts any size and keeps no copy of the samples.
- **The key chains.** The new key depends on the old key and the new seed. A weak reseed cannot lower the entropy that the key already has.
- **The key changes for each chunk.** This gives forward secrecy. It also holds the pool lock for less time.
- **The credit is explicit.** An untrusted source cannot seed the kernel alone. The rule is visible in the code and in the command line.
- **Both devices are the same.** This follows the FreeBSD model. Vex does not copy the Linux behavior where `/dev/random` blocks after the first seed.
- **The reseed is lazy.** A timer would add a thread and a wakeup for no gain. The output path already knows when it needs a new key.

## Limits

- The kernel cannot prove an entropy claim. A trusted source that lies can seed the generator with weak data.
- Limine can use the PCG32 fallback. The kernel cannot detect this case.
- The jitter source measures timing. Timing is regular on some virtual machines.
- The tests check basic properties: length, distinct calls, bit balance and error paths.
- The secure `random_bytes()` has no caller in the tree. The syscall and the devices wait for the seed, then they use the insecure path.

## Source

- [random.c](https://github.com/vex-os/kernel/blob/main/kernel/random/random.c) — pool, reseed, output and wait
- [random_source.c](https://github.com/vex-os/kernel/blob/main/kernel/random/random_source.c) — polling, event buffers and the worker
- [random_source.h](https://github.com/vex-os/kernel/blob/main/kernel/include/vex/random_source.h) — the source structure and the registration macros
- [random_dev.c](https://github.com/vex-os/kernel/blob/main/kernel/random/random_dev.c) — `/dev/random` and `/dev/urandom`
- [sys_random.c](https://github.com/vex-os/kernel/blob/main/kernel/proc/sys_random.c) — `getrandom(2)`
- [limine.c](https://github.com/vex-os/kernel/blob/main/kernel/random/limine.c) — the bootloader entropy source
- [jitter.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/random/jitter.c) — the timing source
- [rdseed.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/random/rdseed.c) and [rdrand.c](https://github.com/vex-os/kernel/blob/main/kernel/arch/amd64/random/rdrand.c) — the CPU instructions
- [irq.c](https://github.com/vex-os/kernel/blob/main/kernel/irq/irq.c) — the interrupt source
- [test.random.c](https://github.com/vex-os/kernel/blob/main/ktest/test.random.c) — the tests
