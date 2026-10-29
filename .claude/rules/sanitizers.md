---
paths:
  - .github/workflows/linux-build-test.yml
  - tests/tsan-suppressions.txt
  - tests/storm.hpp
---

# Sanitizer CI legs (ASan, TSan)

- Both are `sanitize:` entries in the `test` matrix: g++, static, RelWithDebInfo, `-DIMMER_NO_FREE_LIST=1`.
- Reports vary run to run (interleavings): one clean run proves little; compare several.
- TSan doesn't stop at a report: the suite runs to the end and exits 66, so "All tests passed" with a
  red step means races were reported. Grep the log for `WARNING: ThreadSanitizer`.
  - Detached server threads outlive their test, so a report may name a later test. Read the stacks.
- `tests/tsan-suppressions.txt` (`race:` = any frame of either racing stack, by function/file/library):
  - Catch2's per-test std::clog capture (JUnit reporter, CI only) races detached servers' logging.
  - Boost.Thread: only `~thread_data_base` (detach of a `future::then` thread); never the whole .so.
  - Uninstrumented libs (GLib, GStreamer, gst-wayland-display, libcrypto) sync through futexes or
    atomics TSan can't see: hundreds of reports otherwise, all crossing their threads.
  - So Wolf code running on a GStreamer thread (pipeline callbacks, the RTP payloaders) is not covered.
  - Never suppress a race in Wolf's own code without a ticket, and name the ticket on the line.
- What it catches: API `state_->sockets` touched off the io_context thread (removing `sse_broadcast`'s
  `dispatch` gives 9 reports in `[API]`) and concurrent process-group terminates (`[PROCESS]`).
- It also catches an ENet call on a control host without `enet_mutex` (dozens of reports in `[ENET]`).
  - Not an off-thread `write_queue` write: asio's internal mutexes give TSan a happens-before.
  - Not the env leak: glibc's environ is uninstrumented; only the env test's assertion catches it.
- A flake that shows only on CI's 4 vCPUs reproduces with `taskset -c 0-3` and the JUnit reporter.
- Running it by hand: Ubuntu 26.04 + CI's apt list + `gstreamer1.0-plugins-{base,good}` (without
  `videotestsrc` "Stopping a stuck pipeline doesn't block the event bus" fails and hangs the run).
  - The suite takes ~2 min under TSan (CI and 4 pinned cores).
  - `TSAN_OPTIONS=log_path=<prefix>` keeps reports out of the console.
- The TSan row runs with `test_virtual_input: ON`; inputtino's thread races are patched at fetch time
  (see `inputtino-patch.md`).
