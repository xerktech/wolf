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
  - Never suppress a race in Wolf's own code without a ticket, and name it on the line (XERK-2101).
- What it catches: API `state_->sockets` touched off the io_context thread (removing `sse_broadcast`'s
  `dispatch` gives 9 reports in `[API]`), the process runner's env/terminate races (`[PROCESS]`).
- Running it by hand: Ubuntu 26.04 + CI's apt list + `gstreamer1.0-plugins-{base,good}` (without
  `videotestsrc` "Stopping a stuck pipeline doesn't block the event bus" fails and hangs the run).
  - The whole suite takes ~6 min under TSan; `TSAN_OPTIONS=log_path=<prefix>` keeps reports out of
    the console.
