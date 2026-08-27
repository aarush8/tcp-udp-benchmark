# Validation

The CMake/CTest suite contains GoogleTest unit tests, Python real-socket integration tests, and Python automation/analysis regressions. The Python checks use real loopback sockets and subprocess exit/status validation. The current suite contains 12 C++ cases, 16 integration methods, and 11 automation/analysis methods, with additional parameter combinations inside several checks. No performance threshold is a correctness assertion.

## Coverage

- Protocol encoding/decoding, big-endian sequence round trip, maximum/minimum lengths, malformed magic/header, and protocol-specific fragment bounds.
- Exact TCP framing with byte-at-a-time headers and multiple adjacent frames.
- A 1 MiB transfer into a small nonblocking send buffer, with a concurrent draining peer, exercising partial writes and EAGAIN.
- Receive and send deadlines, mid-frame EOF (including zero body bytes after a valid header), closed-peer send without SIGPIPE termination.
- Interrupted blocking receive resuming after a signal with no SA_RESTART.
- Move ownership and descriptor closure verified with `fcntl(F_GETFD)`/EBADF.
- Nearest-rank percentiles, goodput unit conversion, CPU percentage arithmetic, null sample handling.
- UDP duplicate/gap/reordering tracking, oversized datagram rejection, missing echo accounting with a deterministic dropping fixture, duplicate/stale echo injection.
- TCP/UDP at 64 B, 4 KiB and 1 MiB, blocking/nonblocking, four concurrent clients.
- Every write/buffering/I/O/transport combination at 16 KiB and four clients.
- Invalid configuration, connection refusal, stalled peer, TCP idle intervals exceeding the per-frame timeout, active-connection shutdown, connection caps, 30 sequential reconnects at a one-worker limit, repeated-connection descriptor cleanup, deadline-bounded low-rate UDP fragment pacing, CSV quoting and repeated iterations.

The echo-drop fixture is a test, not an experimental network-loss measurement. A sequence reordering unit test proves bookkeeping behavior; the default stop-and-wait network workload does not exercise high-volume reordering.

## Re-run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure

cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DNETBENCH_SANITIZERS=ON
cmake --build build-asan -j 4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure
```

Local execution logs can be kept under the ignored `results/validation/` directory. The compiler and build details for measurement executables are in each experiment's `environment.json`. Tests need an installed GoogleTest development package; the build does not silently fetch dependencies from the network. A runtime-only build can use `-DBUILD_TESTING=OFF`.

ASan/UBSan and leak detection were run on this Linux host. ThreadSanitizer, Valgrind, and hardware/network fault injection are not claimed. Process resource limits can still prevent starting a benchmark; the client releases its start barrier when thread creation fails, and the server cancels existing workers on an exception before joining them.

## Automation and style

Automation regressions check bounded readiness reads, child cleanup, failed-run classification, matrix sizes, mismatched comparison controls, and plotting with missing successful variants. Analysis tests need matplotlib and explicitly skip if it is absent; the configured CI jobs install it. Synthetic records in these tests are fixtures, never benchmark results.

```bash
python3 -m pip install -r requirements-dev.txt
ruff check scripts tests
ruff format --check scripts tests
clang-format --dry-run --Werror src/*.cpp include/netbench/*.hpp tests/unit.cpp
```

The GitHub Actions workflow runs release and sanitizer configurations with Python lint/format checks. Local execution and configured CI are distinct; CI success is established by a completed workflow run.
