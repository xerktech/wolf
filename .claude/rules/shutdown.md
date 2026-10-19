---
paths:
  - src/moonlight-server/wolf.cpp
  - src/moonlight-server/exceptions/exceptions.h
---

# Process shutdown

- `run()` ends in `std::_Exit` after the graceful drain; never let it return to `main` (XERK-2022).
  - Detached pipeline threads can still be dropping gst-wayland-display's GL renderer; a normal
    `exit()` tears down NVIDIA's EGL state under them → SIGSEGV in `libnvidia-eglcore`.
  - So nothing runs after it: no `run()` locals' destructors, no static destructors, no atexit.
    Any cleanup the shutdown needs (e.g. removing PulseAudio sinks) must be done synchronously
    in `run()` before `_Exit`, not left to runner threads or destructors.
- A crash writes `backtrace.dump` into `WOLF_CFG_FOLDER` (on fenrir: the session's PVC); the next
  start prints it as "Stack trace" and renames it. Count those files to tell whether a shutdown
  crashed — supervisord logging `exit status 0` doesn't rule it out.

# Startup failures

- Each server runs inside `run_server` (wolf.cpp): a throw (e.g. port already taken) is logged and
  shuts Wolf down through the graceful path with exit code 1 — no crash, no `backtrace.dump`.
  - So server entry points must throw when they can't bind, never log and return: RTSP used to
    keep Wolf running with no RTSP, and a null ENet host segfaulted (exit 11).
  - Tests: `tests/testControl.cpp` "control: a port that's already taken throws instead of
    crashing"; `tests/testRTSP.cpp` "RTSP server on a port that's already taken throws…".
- The Unix-socket API is the exception: its failure (e.g. a socket path over ~107 chars) is logged
  and Wolf keeps streaming without the API.
