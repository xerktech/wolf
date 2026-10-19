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
- Keep `close_inherited_fds{}` last in the `bp::child` arguments, after any stdio redirect.
  - Placed before a stdout/stderr redirect, the redirect's dup2 would re-clear close-on-exec on
    fds 1-2, masking a range that wrongly starts below 3, and the test would no longer catch it.
- Tests: `tests/platforms/linux/process.cpp` "Process runner doesn't leak Wolf's sockets to the child"
  (fails without the initializer; its stderr assertion catches a range that starts below 3).
  - The same inheritance kept a sibling's stdout/stderr pipe open, so a runner waited for its siblings
    to exit (zombies meanwhile): "Process runner returns when its app exits while a sibling runner still
  runs".
- The stop handlers use `bp::group::terminate(std::error_code &)`: the throwing overload throws once the
  group is gone (a second stop gets `killpg(-1)` → EINVAL).
  - `fire_event` swallows a `std::system_error` (Boost.Process' errors are one) by abandoning the
    loop: every handler registered after the throwing one silently never runs.
  - Tests: "Process runner survives a repeated stop".
- A runner registers its stop handlers only once its app exists, so a stop fired before that is lost.
  Every runner (process and docker) calls `is_stopped` right after registering and stops the app if
  the session/lobby is already gone (`events::Runner::run`). Registering takes the bus's exclusive
  lock, so a stop being fired has finished, and removed the session, by then.
  - So a session must be in `running_sessions` before its `StreamSession` event fires, or its
    no-compositor runner can see it as stopped (HTTPS `/launch` and the API both add first).
  - `is_stopped` matches by session_id, not instance: a `/resume` keeps the runners.
  - Tests: "Process runner stops an app whose session was stopped while it started" (unit),
    `tests/testWolfAPI.cpp` "Runners: a session stopped while its runner starts doesn't leave the app
    running" (through `setup_moonlight_handlers`).
- The app's environment is a copy (`bp::environment env = this_process::environment()`), never `auto`:
  that is Wolf's live environment, and assigning to it `setenv()`s in Wolf, racing every `getenv` and
  leaking one app's variables (`XDG_RUNTIME_DIR`, `PULSE_SINK`) into Wolf and later apps.
  - Tests: "Process runner passes the app's environment without changing Wolf's".
- `terminate_group` runs under the runner's `terminate_lock`: handlers fire under the bus's shared lock,
  so two stops on two threads would race on the group's pgid. Tests: "Process runner handles stops
  fired on two threads at once" (fails only under TSan).
