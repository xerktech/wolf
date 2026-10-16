---
paths:
  - src/moonlight-server/control/**
  - src/moonlight-server/streaming/**
  - src/moonlight-server/sessions/**
  - src/moonlight-server/rest/endpoints.hpp
  - src/moonlight-server/events/events.hpp
  - tests/testControl.cpp
  - tests/testWolfAPI.cpp
---

# Stream sessions and stream instances

- A `session_id` is the paired client's cert hash: it identifies the client, not one stream.
  - `/resume` replaces the running `StreamSession` with a new one under the same `session_id`
    (new `enet_secret_payload`, `rtp_secret_payload`, `aes_key`).
- `PauseStreamEvent` / `StopStreamEvent` are keyed by `session_id` only, so they end every
  pipeline of that client, including a resumed stream. Never fire them on behalf of a stream
  instance that a resume has already replaced.
  - A dead client's ENet peer lingers until ENet times it out (~5 s) and can disconnect after the
    resume. `control.cpp` treats it as superseded: it fires `EndSupersededStreamEvent` (keyed by
    `rtp_secret_payload`) instead of a pause.
  - On ENet connect the newest peer owns the session; older peers of the same session are dropped.
  - A stop's runner exits milliseconds after the stop, often after a /resume re-added the
    session_id. The runner-exit path fires `StopStreamEvent` only while its own instance is the
    running one (`state::is_running_instance`), else `EndSupersededStreamEvent` (XERK-1964).
- `PauseStreamEvent` is also broadcast on the API event stream; external tools (fenrir) read it as
  "this client's stream paused". Don't overload it with per-instance semantics.
- Tests: `tests/testControl.cpp` — "control: a dead client's late pause doesn't end its resumed
  stream" (real ENet), "streaming: a superseded stream instance ends only its own pipelines".
  - "sessions: a stopped or replaced stream instance is no longer the running one".
  - The ENet test client must keep servicing its host after connect, or the server never sees the
    CONNECT (the handshake ack is sent on the next service call).
- `StreamSession::stream_state` backs the `state` field of `GET /api/v1/sessions`, so a client that
  missed events (fenrir's wolf-agent after an SSE reconnect) can rebuild the stream from it.
  - It counts running pipelines, never flags: after a pause, a restart on the same session can start
    new pipelines before the old ones finish EOS, and a flag cleared on the old exit would read wrong.
  - A pipeline decrements the atom it captured at start, not the session currently under its id.
  - The pause/start/pipeline bookkeeping lives in `setup_stream_pipeline_handlers`, separate from
    the compositor/runner handlers, so a test can drive it with fake pipelines (`StreamPipelines`).
  - Tests: `tests/testWolfAPI.cpp` "Sessions API: the listed state follows the stream's events"
    (pause → API start mid-teardown, resume + `EndSupersededStreamEvent`); "Sessions APIs" covers
    the listing only.
