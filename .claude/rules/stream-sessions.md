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
  - So the IP fallback (a CONNECT whose secret matches nothing) never matches a stream instance with a
    connected peer (`has_connected_peer`): anyone behind the client's NAT could otherwise drop it.
    - Peers of an instance a /resume replaced don't count: a client without the secret that crashed and
      resumed must get in while its dead peer times out. Tests: "control: a peer matched only by IP
      can't take over a live stream".
  - Joypads outlive a /resume, so their feedback (rumble, LEDs) sends through `control_feedback`, which
    each ENet connect points at its peer/key. Never capture the peer or key in a device callback.
    Tests: "control: joypad feedback reaches the resumed stream's client".
  - A stop's runner exits milliseconds after the stop, often after a /resume re-added the
    session_id. The runner-exit path fires `StopStreamEvent` only while its own instance is the
    running one (`state::is_running_instance`), else `EndSupersededStreamEvent` (XERK-1964).
  - A dead peer that times out after its stream was stopped (none listed under its id) fires
    nothing: a /resume may be about to add a stream under the same id.
- Interpipe nodes are global, so producers are named by `StreamSession::producer_name`
  (`state::producer_name`), not the session_id: a stream added ~15 ms after the old one's stop
  would otherwise attach to, and collide with, its still-dying producers (no video, then segfaults).
  - It is random per `create_stream_session`; Moonlight's own /resume keeps the producers
    (wayland display) running, so it carries the name over.
  - Consumers get it from the running session; `listen_to_producer` rewrites the configured
    `listen-to=<session_id>_<kind>` before the pipeline plays. Lobby leave switches back to it.
  - A consumer starting after its session joined a lobby must start on the lobby's producers
    (`state::stream_producer_name`): `SwitchStreamProducerEvents` only reaches running pipelines.
    A /resume's new pipelines can start seconds after a join (XERK-2045).
  - `follow_producer` resolves the name after the switch handler is registered, under a lock shared
    with it; join/leave update `connected_sessions` before firing the switch. Keep both orders,
    or a join while the stream starts is lost.
- Interpipe also keys a producer's listeners by consumer element name, so `own_consumer` renames the
  configured `interpipesrc_<session_id>_<kind>` to one unique per pipeline start (random hex).
  - With a shared name, a /resume's pipeline started before the old one's EOS is refused ("Already
    listening") and gets nothing once the old one leaves: no video (XERK-2025).
  - Never derive it from the instance (`rtp_secret_payload`): an API start after a pause reuses the
    instance's secrets and can start ~ms after the pause, before its EOS (XERK-2046).
  - Look the consumer up by the returned name (switch handlers, `listen_to_producer`), never by
    re-deriving it from the session_id.
  - Tests: "streaming: a resumed stream keeps receiving after the superseded one ends",
    "streaming: each pipeline start names its own consumer",
    "streaming: a stream restarted right after a pause keeps receiving".
- `PauseStreamEvent` is also broadcast on the API event stream; external tools (fenrir) read it as
  "this client's stream paused". Don't overload it with per-instance semantics.
- Tests: `tests/testControl.cpp` — "control: a dead client's late pause doesn't end its resumed
  stream" (real ENet), "streaming: a superseded stream instance ends only its own pipelines".
  - "sessions: a stopped or replaced stream instance is no longer the running one".
  - "streaming: a stream listens to its own instance's producers".
  - "sessions: a stream whose session joined a lobby listens to the lobby's producers".
  - "streaming: a lobby join while a stream starts is not lost" (fails if the producer is resolved
    before the switch handler is registered; needs gst-interpipe).
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
- The control stream uses Sunshine's V2 encryption when the client enables it in the RTSP ANNOUNCE
  (`x-ss-general.encryptionEnabled`, offered in DESCRIBE); state is `StreamSession::control_stream`.
  - V1 (Nvidia's) only puts seq's low byte in the IV: seq bits 8..31 are not authenticated and
    anyone can rewrite them. Never trust a V1 seq for anything, including replay checks.
  - V2 packets pass a replay window (`accept_seq` in `moonlight/control.hpp`). Never make it a strict
    `seq > last` check: Moonlight numbers all packets with one counter but sends them over several
    ENet channels, some unreliable, so they arrive out of order.
  - The window belongs to the stream instance, not the ENet peer: a new peer of the same stream
    must not reset it. `/resume` gets a fresh `control_stream` (new key, seq restarts at 0).
  - Packets Wolf sends take `send_seq++`: a fixed seq reuses the GCM nonce under one key.
  - RTSP is unauthenticated: an ANNOUNCE's mode is only a hint (used for what Wolf sends meanwhile).
    The client's first control packet whose GCM tag verifies (`authenticated_mode`) fixes the mode for
    the stream instance (`encryption_negotiated`); later ANNOUNCEs are ignored.
    - Never let any ANNOUNCE fix it, first or last: a spoofed one sent before the client's own locks a
      V2 client into V1 (no control, no video), one sent after flips a V1 client. Tests: "control: the
      first packet that authenticates fixes the encryption mode".
    - Until then `encrypt_and_send` sends each host packet in both modes (the client drops the copy it
      can't decrypt): a spoofed hint must not hide e.g. the termination from the real client.
  - A stream instance accepts one ANNOUNCE (`pipelines_announced`); later ones get 455 and change nothing.
    - Each accepted ANNOUNCE fires Video/AudioSession, i.e. starts a new set of pipelines: a flood of
      them crashed Wolf in interpipe (XERK-2096). Moonlight sends one per /launch or /resume, and
      a /resume is a new instance, so it gets its own.
    - Don't move the limit into the Video/AudioSession handlers: the API's `sessions/start` restarts
      an instance after a pause through them (XERK-2046).
    - Accepted cost: a spoofed ANNOUNCE that beats the client's own claims the instance, so that
      stream fails to start (Moonlight aborts on a non-200 ANNOUNCE). A fake-ip match ignores the
      source IP (`get_session`), and fenrir's fake ip is the pod IP; RTSP auth is XERK-2115.
    - A 400 ANNOUNCE (bad display mode) doesn't use it up: keep the claim after the validation.
    - Tests: `tests/testRTSP.cpp` "RTSP: a stream instance starts its pipelines from one ANNOUNCE only";
      `tests/testMoonlight.cpp` "/resume keeps what still runs for the stream" (not carried over).
  - A seq past UINT32_MAX is dropped on purpose (it would reuse a nonce, and accept that packet's replay).
  - Tests: `tests/testControl.cpp` "Control replay window", "Control V2 encryption", "control: a
    replayed encrypted packet is dropped"; `tests/testRTSP.cpp` DESCRIBE / "ANNOUNCE control".
- ENet isn't thread-safe; feedback and the stop's termination are sent from other threads, so every ENet
  call on a control host holds `enet_mutex` (`control.cpp`) (XERK-2101).
  - `run_control` never blocks inside `enet_host_service` (timeout 0 under the lock); it waits on the socket
    outside the lock, capped at `max_unlocked_wait` so ENet's retransmit and ping timers still run.
  - `send_seq++` and the send share one lock, so the wire order is the seq order.
  - Only ThreadSanitizer catches a missing lock (dozens of races); the seq order fails without it.
    Tests: "control: joypad feedback sent while the client streams input arrives intact".
