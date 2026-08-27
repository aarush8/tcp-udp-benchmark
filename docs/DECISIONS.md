# Engineering decisions and interview discussion

| Decision | Reason | Cost / limitation |
|---|---|---|
| C++20 + Linux/POSIX directly | RAII descriptor ownership, atomics, threads and a start barrier; socket behavior remains visible | Linux-specific, requires a compiler/library with `std::barrier` |
| Length + sequence + fragment header | Explicit, portable framing; reject oversized lengths before allocation | 24 extra application bytes per frame/datagram |
| Echo RTT and byte comparison | One local clock; correctness is checked for every successful result | Validation and application work are inside RTT; not pure wire delay |
| One outstanding logical request per worker | Simple attribution, bounded memory, interpretable request/response latency | Does not fill the bandwidth-delay product like a pipelined bulk stream |
| UDP application fragmentation | Supports logical payloads through 1 MiB without invalid native datagrams | Fragment stop-and-wait/pacing changes workload semantics; cannot infer link capacity |
| No UDP retries | Loss and excessive delay remain visible | Cannot distinguish forward loss from reverse loss or a missed deadline |
| Thread per TCP connection, capped at 32 | Correct blocking baseline, easy ownership and concurrent-client testing | Stacks/scheduling overhead; no claim of thousands-of-clients scalability |
| Nonblocking `poll()` within the same workers | Isolates the readiness/waiting path while retaining architecture | Not an epoll reactor; more readiness syscalls can cost CPU |
| Independent coalescing/buffer options | Factorial trials can separate network-policy effects from allocation/copy effects | More variants to validate; full buffer reuse retains memory per connection |
| Explicit TCP_NODELAY option | Tests interaction of small writes and Nagle behavior | Disabling Nagle may increase small segments; coalescing can avoid the issue without doing so |
| Requested and effective socket sizes | Linux can double/clamp sizes; requested values alone mislead | Initial observations do not capture later TCP autotuning |
| Process CPU accounting | Portable Linux user/system CPU, includes all process threads | Not host-wide CPU or NIC/IRQ/softirq cost; startup costs matter for short runs |
| Raw trial records + source snapshots | Rebuild an old implementation even before a first Git commit | More artifacts; exact timings still depend on machine state |
| Fail visibly | Nonzero client exit on errors/timeouts; no samples become null | Analysis must handle incomplete trials rather than hiding them |

## What the measurements taught

1. Stream correctness comes first: a frame can be fragmented over many `recv()` calls or coalesced with its successor. A single-call implementation can pass a quick localhost test and still be wrong.
2. Application write boundaries influence TCP policy even though they are not message boundaries. A 24-byte header followed by a small body produced millisecond stalls with Nagle enabled; the full-frame write removed the observed stall.
3. Reusing a buffer can remove initialization, allocation, and copying work together. The controlled factor here is a buffering strategy, not an isolated allocator microbenchmark. Reuse changed 1 MiB RTT substantially while paced throughput stayed similar.
4. A faster request path need not increase goodput when pacing constrains starts. Small-message throughput also reflects scheduler sleep overhead. A capped echo workload is not a link-speed benchmark.
5. Concurrency and nonblocking I/O are different decisions. More workers increased achieved aggregate goodput under the same budget; O_NONBLOCK plus `poll()` did not show a consistent throughput advantage and used more client CPU here.
6. Larger buffers helped latency when a 16 KiB TCP frame exceeded very small buffers, but increasing already sufficient buffers showed little benefit. Bigger is not automatically faster.
7. Measurement code can introduce artifacts: the first harness ended a duration-limited test before its last pacing gap finished. Those initial raw runs are retained; later comparisons use a corrected common window. A finite run can still exceed its nominal scheduling cap by one message per client because the first message starts immediately.
8. Negative results belong in the report. Coalescing alone did not consistently improve large-payload latency; nonblocking I/O did not earn a general speedup claim; no observed UDP loss at these local rates proves behavior under congestion.

## Implemented vs. future work

Implemented: framed TCP, fragment-echo UDP, blocking/nonblocking data sockets, bounded concurrent clients, RTT/goodput/loss/CPU accounting, JSON and CSV, reproducible matrices, trial-aware plots, two independent optimization switches, unit/integration tests, ASan/UBSan validation, and syscall diagnostics.

Future work, not claimed as implemented or measured: an epoll reactor, pipelined or unidirectional bulk transfer, `sendmmsg`/`recvmmsg`, `writev`/`sendmsg` scatter-gather, timestamping, separate receiver-side UDP reports, `TCP_INFO` retransmission telemetry, IPv6, CPU affinity/frequency controls, controlled `tc netem` impairment, real NIC/remote-machine tests, and embedded ARM measurements. These would be separate experiments with their own correctness checks, not automatic performance upgrades.
