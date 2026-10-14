# MiniKV

<p align="center">
  <strong>A lightweight, Redis-inspired in-memory key-value server written in C11.</strong><br />
  Built around non-blocking networking, incremental data-structure operations, TTL-based expiry, and append-only-file persistence.
</p>

<p align="center">
  <img alt="Language" src="https://img.shields.io/badge/language-C11-00599C?logo=c&logoColor=white" />
  <img alt="Platform" src="https://img.shields.io/badge/platform-Linux-333333?logo=linux&logoColor=white" />
  <img alt="Protocol" src="https://img.shields.io/badge/protocol-RESP2-blue" />
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green.svg" />
  <img alt="Version" src="https://img.shields.io/badge/version-0.1.0-orange" />
</p>

MiniKV is a compact systems-programming project exploring the core engineering behind a networked key-value store: an event-driven server, a RESP-compatible command parser, a custom hash table, expiration management, and append-only persistence.

It is designed for learning, experimentation, and further development. MiniKV implements a useful subset of Redis-style commands and can be tested with `redis-cli`; it is **not a drop-in replacement for Redis** and should not be exposed to untrusted networks without additional security controls.

## Contents

- [Highlights](#highlights)
- [Architecture](#architecture)
- [Supported commands](#supported-commands)
- [Requirements](#requirements)
- [Build and run](#build-and-run)
- [Connect with redis-cli](#connect-with-redis-cli)
- [Persistence and durability](#persistence-and-durability)
- [Testing](#testing)
- [Benchmark notes](#benchmark-notes)
- [Limitations and roadmap](#limitations-and-roadmap)
- [Project layout](#project-layout)
- [Developer](#developer)
- [License](#license)

## Highlights

- **C11 implementation** with strict compiler warnings enabled.
- **Non-blocking TCP server** using Linux `epoll` and per-client input/output buffers.
- **RESP2 and inline command parsing**, including pipelined and fragmented requests.
- **Key-value operations** for setting, reading, deleting, checking, and incrementing values.
- **TTL support** with lazy expiration and sampled active expiration.
- **Incremental hash-table rehashing** to avoid doing an entire table resize in one operation.
- **Append-only file (AOF)** persistence with configurable fsync policies and recovery from a torn trailing record.
- **Resource caps** for command arguments, request buffers, bulk values, and queued output.
- **Automated tests** using AddressSanitizer and UndefinedBehaviorSanitizer, plus an optional libFuzzer-compatible parser harness.

## Architecture

MiniKV uses a single-threaded, event-driven design. A single event loop processes socket readiness, parses client commands, executes operations against the in-memory dictionary, handles expirations, and flushes queued replies.

```text
TCP clients / redis-cli
          |
          v
   non-blocking sockets
          |
          v
      Linux epoll
          |
          v
 per-client input buffers
          |
          v
 RESP2 / inline parser
          |
          v
 command dispatcher
      /         \
     v           v
 hash table    TTL index
     |
     v
 AOF write / fsync
     |
     v
 queued client replies
```

### Core design choices

| Component | Approach | Rationale |
|---|---|---|
| Event loop | Single-threaded `epoll` | Simple command ordering without application-level locks |
| Protocol | RESP2 subset plus inline commands | Interoperability with common Redis clients for supported commands |
| Dictionary | Chained hash table with incremental rehashing | Spreads resize work across operations |
| Expiration | Lazy checks plus sampled active expiry | Removes expired keys on access and reclaims untouched expired entries |
| Persistence | Append-only file with configurable fsync | Allows state reconstruction after restart |
| Client I/O | Per-client buffers and output back-pressure | Handles partial reads/writes and slow consumers |

The single-threaded model also has trade-offs: expensive commands such as `KEYS`, and blocking disk synchronization under some durability policies, can delay other clients.

## Supported commands

MiniKV currently implements the following command set:

| Category | Commands |
|---|---|
| Connectivity | `PING`, `ECHO`, `QUIT` |
| Strings / values | `SET`, `GET`, `INCR`, `DECR` |
| `SET` options | `EX`, `PX`, `EXAT`, `PXAT` |
| Keys | `DEL`, `EXISTS`, `DBSIZE`, `KEYS`, `FLUSHALL`, `SELECT` |
| Expiration | `EXPIRE`, `PEXPIRE`, `EXPIREAT`, `PEXPIREAT`, `TTL`, `PTTL`, `PERSIST` |
| Server | `INFO` |

Only the implemented subset is supported. Redis features outside this list may be unavailable or behave differently from Redis. Compatibility should be validated against the exact client commands your application uses.

## Requirements

- Linux with `epoll` support
- A C compiler such as GCC or Clang
- GNU Make
- Python 3 for integration tests
- Clang for the optional libFuzzer target
- `redis-cli` for the client examples (optional)

The server itself is written in C and uses standard/Linux system interfaces. The test and fuzz workflows have the additional tools listed above.

## Build and run

Clone the repository, then enter the project directory:

```bash
git clone <YOUR-REPOSITORY-URL>
cd minikv
```

Build the release executable:

```bash
make
```

Start MiniKV on port `6399`, using `minikv.aof` for persistence and the `everysec` fsync policy:

```bash
./minikv -p 6399 -a minikv.aof -f everysec
```

Available options:

| Option | Description |
|---|---|
| `-p port` | TCP port to listen on; default is `6399` |
| `-b bind_addr` | Address to bind; default is loopback (`127.0.0.1`) |
| `-a aof_file\|none` | AOF path, or `none` to disable persistence |
| `-f always\|everysec\|no` | AOF fsync policy |

For local development, keep the server bound to loopback unless you have explicitly configured and reviewed network access controls.

## Connect with redis-cli

In another terminal:

```bash
redis-cli -p 6399 PING
redis-cli -p 6399 SET greeting "Hello, MiniKV!"
redis-cli -p 6399 GET greeting
redis-cli -p 6399 SET session:demo active EX 60
redis-cli -p 6399 TTL session:demo
redis-cli -p 6399 INCR page:views
```

Expected examples include `PONG` for `PING`, and the stored value for `GET`. Exact output formatting depends on the client and command mode.

## Persistence and durability

MiniKV can append write operations to an AOF and replay that log during startup. The fsync policy controls how aggressively the process asks the operating system to synchronize the log to storage:

- **`always`** — synchronizes writes according to the server's durability path before sending queued replies. This is the strongest configured mode, with a performance cost.
- **`everysec`** — aims to synchronize about once per second; a sudden failure can lose recent writes that were not synchronized.
- **`no`** — does not request periodic fsync through this policy; durability then depends more heavily on operating-system writeback behavior.

TTL operations are recorded using absolute deadlines so replay does not inadvertently restart a relative expiration interval. Recovery handles a torn trailing AOF record, but this should not be interpreted as protection against every possible storage failure or filesystem corruption scenario.

**Important:** Durability guarantees depend on the selected policy, filesystem, operating system, storage hardware, and failure mode. Back up important data and validate the behavior in your own environment before relying on MiniKV for critical workloads.

## Testing

Run the unit and integration test suites:

```bash
make test
```

The test target builds sanitizer-enabled binaries and exercises the dictionary, parser, and server over real sockets. Current test coverage includes:

- Parser handling of incomplete and malformed commands
- Dictionary inserts, lookups, deletes, overwrites, clearing, and rehashing
- Pipelined commands and byte-by-byte fragmented requests
- Binary-safe value round trips, including a 5 MB value
- High connection counts and slow-reader behavior
- TTL expiration and reclamation of untouched expired keys
- Protocol errors and server survival after client disconnects
- AOF recovery scenarios, including forced termination and a torn trailing record

Run the optional parser fuzz target with Clang/libFuzzer:

```bash
make fuzz
./fuzz/fuzz_resp -max_len=4096
```

A passing test suite is useful evidence, not a proof that the program is free of defects or vulnerabilities. Run tests on your target platform and add workload-specific tests before deployment.

## Benchmark notes

The following figures are from the project's recorded benchmark run using `redis-benchmark` with 300,000 operations and 50 clients, with the client and server sharing a laptop-class virtual machine. Both servers were configured with AOF `everysec`.

| Operation | MiniKV, pipeline 1 | Redis 7.0, pipeline 1 | MiniKV, pipeline 16 | Redis 7.0, pipeline 16 |
|---|---:|---:|---:|---:|
| `SET` | 106k ops/s | 94k ops/s | 1.02M ops/s | 0.57M ops/s |
| `GET` | 91k ops/s | 102k ops/s | 1.24M ops/s | 0.97M ops/s |
| `INCR` | 102k ops/s | 99k ops/s | 0.89M ops/s | 0.68M ops/s |

These are environment-specific, previously recorded measurements—not universal performance claims. Results can vary with hardware, operating system, compiler, persistence settings, and benchmark configuration. Pipeline depth changes the workload substantially, and the pipeline-1 results may be limited by the benchmark client. Re-run the benchmark on your own hardware before drawing comparisons.

## Limitations and roadmap

MiniKV is an actively developed, deliberately small implementation. Known limitations and potential next steps include:

- [ ] AOF rewrite/compaction to control log growth
- [ ] Background fsync to reduce event-loop stalls
- [ ] Incremental key scanning (`SCAN`) and additional data types
- [ ] Hash-table shrinking, stronger hash-flooding resistance, and memory limits/eviction
- [ ] More extensive authentication and access-control support before any non-local deployment
- [ ] Broader fault-injection, fuzzing, and long-duration stress testing
- [ ] Optional disk-oriented storage experiments, such as an LSM-tree

### Security and deployment notice

MiniKV should currently be treated as an experimental project, not as a hardened, multi-tenant production database. Do not expose it directly to the public internet. Keep it on loopback or a trusted private network, restrict host-level access, and avoid storing sensitive production data unless you have independently reviewed and tested the relevant security and durability properties.

## Project layout

```text
minikv/
├── src/
│   ├── server.c       # Event loop, connections, commands, and AOF handling
│   ├── dict.c         # Hash table and incremental rehashing
│   ├── dict.h         # Dictionary interface
│   ├── resp.c         # RESP/inline command parser
│   └── resp.h         # Parser interface and command structures
├── tests/
│   ├── test_unit.c    # C unit tests
│   └── integration.py # End-to-end tests using real sockets
├── fuzz/
│   └── fuzz_resp.c    # Parser fuzzing harness
├── Makefile
├── LICENSE
└── README.md
```

## Developer

**Demiyan Dissanayake**  
Software Developer · Creator of MiniKV  
**Organization:** [Dexel Software Solutions](https://github.com/Dexel-Software-Solutions)

- **GitHub:** [Dexel-Software-Solutions](https://github.com/Dexel-Software-Solutions)
- **Email:** [dexelsoftwaresolutions@gmail.com](mailto:dexelsoftwaresolutions@gmail.com)
- **Email:** [demiyandissanayake@gmail.com](mailto:demiyandissanayake@gmail.com)

MiniKV is developed as a systems-programming project focused on C, networking, data structures, protocol implementation, and persistence engineering.

## License

MiniKV is released under the [MIT License](LICENSE).

---

<p align="center">
  Built with C11, curiosity, and a focus on systems engineering.<br />
  <sub>MiniKV · v0.1.0 · © 2026 Demiyan Dissanayake</sub>
</p>
