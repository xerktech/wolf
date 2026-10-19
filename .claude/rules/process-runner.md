---
paths:
  - "src/moonlight-server/runners/process.*"
  - "tests/platforms/linux/process.cpp"
---

# Process runner: fd inheritance

- Wolf's sockets (HTTP/S, RTSP, ENet, API) are not opened close-on-exec, and Boost.Process doesn't
  close fds before exec.
  - Every `bp::child` must pass `close_inherited_fds{}`. Without it, an app that outlives Wolf keeps
    47984/47989/48010 bound, and the next Wolf dies on "bind: Address already in use" (XERK-2049).
- `close_inherited_fds` marks fds >= 3 close-on-exec; it never closes them.
  - Closing breaks Boost.Process's exec-error pipe, so a missing binary would no longer throw.
  - Never touch fds 0-2: they are the app's stdio.
- The app inherits Wolf's stdout/stderr, so its output reaches Wolf's log.
  - Don't capture them into `std::future`s: nothing reads them until exit, so the output is
    lost and buffered whole in memory.
- The fcntl fallback (kernel < 5.11) is capped at 65536 fds. One fcntl per possible fd up to an
  `infinity` RLIMIT_NOFILE (~1e9) would stall every launch for minutes.
- Keep `close_inherited_fds{}` last in the `bp::child` arguments, after the stdio redirects.
  - Placed earlier, a redirect's dup2 re-clears close-on-exec on fds 1-2, which masks a range that
    wrongly starts below 3, and the test no longer catches it.
- Tests: `tests/platforms/linux/process.cpp` "Process runner doesn't leak Wolf's sockets to the child"
  (fails without the initializer; its stderr assertion catches a range that starts below 3).
