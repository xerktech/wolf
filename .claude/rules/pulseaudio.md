---
paths:
  - src/core/src/platforms/linux/pulseaudio/**
  - src/core/src/core/audio.hpp
  - src/moonlight-server/sessions/**
  - src/moonlight-server/rest/endpoints.hpp
  - src/moonlight-server/wolf.cpp
  - tests/platforms/linux/pulse.cpp
---

# PulseAudio virtual sinks

- A request whose callback owns a heap ref (`new std::shared_ptr<...>`) must also get
  `release_if_cancelled`: libpulse cancels pending operations on connection loss without calling
  the callback, so the ref leaks and a promise it holds never breaks (`flush` waited out its timeout).
- `AudioDevice::sink_name` is a view of the caller's string: format anything from it before
  `queue_op`, never inside the queued op (it can run later, once the connection is READY).
- A session's sink belongs to the session, not to a runner: several runners can share a session
  (API `runners/start`), so the session's `StopStreamEvent` handler removes it and session runners
  get no `audio_sink`.
  - `/resume` carries `audio_sink` over to the new `StreamSession`, like the producers; dropping it
    leaks the sink. Tests: `tests/testMoonlight.cpp` "/resume keeps what still runs for the stream"
    (drives `endpoints::https::resume_session`, which the HTTPS handler calls).
  - A lobby has exactly one runner, so its runner's exit still removes the lobby's sink.
- At startup Wolf unloads `virtual_sink_*` null sinks left by a Wolf that crashed or was killed
  (`remove_stale_sinks` in `wolf.cpp`). Another Wolf's live sinks have the same names, so:
  - only on a server Wolf owns: embedded (`WOLF_EMBED_PULSE=true`) or the PA container it started;
    an external `PULSE_SERVER` may be shared, and is left alone.
  - only ~3 s after the servers started, and not if one failed to bind (`shutdown_requested`): a
    second Wolf started by mistake binds nothing and exits, and must not unload the running one's sinks.
  - never a sink of a session/lobby this Wolf is running: checked in the module-list callback.
- Runners mount the socket by its bare path: libpulse reports it as `unix:<path>` when it came from
  `PULSE_SERVER`, and Docker splits binds on ':' (`audio_server_socket_path`).
- Tests: `tests/platforms/linux/pulse.cpp` (`[PULSE]`, CMake `TEST_PULSE`) starts its own
  `pulseaudio` daemon per test, so it needs the binary on PATH but no running server; it SIGSTOPs
  and kills that daemon to test connection loss.
