---
paths:
  - src/moonlight-server/rtsp/**
  - src/moonlight-protocol/rtsp/**
  - src/moonlight-server/rest/endpoints.hpp
  - src/moonlight-server/api/endpoints.cpp
  - tests/testRTSP.cpp
---

# Encrypted RTSP (Sunshine's rtspenc://)

- A client that sends `corever>=1` on /launch or /resume gets `rtspenc://` in `sessionUrl0`; its
  `StreamSession::rtsp_encrypted` is set. Over the API, the caller (fenrir) sets `rtsp_encrypted`.
  - `sessions/add` echoes `rtsp_encrypted` in its reply; fenrir hands out `rtspenc://` only on that
    answer, since an older Wolf ignores the field and speaks plaintext. Keep the echo truthful.
  - Wire format and IVs: `src/moonlight-protocol/rtsp/encryption.hpp` (mirrors moonlight-common-c
    `RtspConnection.c`). Key = the launch's rikey (`aes_key`, hex), as for the control stream.
- An encrypted message has no plaintext session hint, so the session is the one whose key
  authenticates it (`get_encrypted_session` tries each encrypted session's key).
- Plaintext RTSP never matches an encrypted session (`get_session` skips them). That's the point:
  same-NAT or on-path peers can't SETUP/ANNOUNCE it or read its ping payloads and ENet secret.
  - Never add a plaintext fallback for an encrypted session; a client can't downgrade itself either,
    the mode is fixed at launch over HTTPS.
- Replies take `tcp_connection::encryption_seq_`, one process-wide counter: a repeated seq reuses
  the GCM nonce. Never make it per session: two sessions can share a key (an API caller, a /resume
  reusing a rikey); QA recovered a reply's plaintext from two per-session counters.
- A message no key opens gets no reply (there's nothing to seal it with): Moonlight times out.
- `WOLF_RTSP_REQUIRE_ENCRYPTION` is read once (`state::parse_rtsp_require_encryption`) into
  `Host::rtsp_require_encryption`: it refuses plaintext-only clients at /launch, /resume (XML
  status 403) and API `sessions/add` (HTTP 403). Default off keeps old clients working.
  - A security switch: an unrecognised value turns it on (with a warning), never silently off.
  - Tests set the Host field; never `setenv` it in a test (`tests.md`).
- Tests: `tests/testRTSP.cpp` "Encrypted RTSP wire format" (an independently generated vector),
  "Commands (Encrypted RTSP)"; `tests/testWolfAPI.cpp` "Sessions API: a session added with encrypted
  RTSP says so" (also the API refusal); `tests/testMoonlight.cpp` "HTTPS /launch and /resume:
  encrypted RTSP", "launch", "Multiple users" (corever parsing, same rules as fenrir's).
