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
  - Wire format and IVs: `src/moonlight-protocol/rtsp/encryption.hpp` (mirrors moonlight-common-c
    `RtspConnection.c`). Key = the launch's rikey (`aes_key`, hex), the same key the control stream uses.
- An encrypted message has no plaintext session hint, so the session is the one whose key
  authenticates it (`get_encrypted_session` tries each encrypted session's key).
- Plaintext RTSP never matches an encrypted session (`get_session` skips them). That's the point:
  same-NAT or on-path peers can't SETUP/ANNOUNCE it or read its ping payloads and ENet secret.
  - Never add a plaintext fallback for an encrypted session; a client can't downgrade itself either,
    the mode is fixed at launch over HTTPS.
- Replies take `rtsp_encryption_seq` (shared by copies of the session): a fixed seq reuses the GCM
  nonce under one key. A /resume is a new session with a new key and counter.
- A message no key opens gets no reply at all (there's nothing to seal it with): Moonlight times out.
- `WOLF_RTSP_REQUIRE_ENCRYPTION=TRUE` refuses plaintext-only clients at /launch, /resume (XML
  status 403) and API `sessions/add`; default FALSE keeps old clients working.
- Tests: `tests/testRTSP.cpp` "Encrypted RTSP wire format" (an independently generated vector),
  "Commands (Encrypted RTSP)"; `tests/testMoonlight.cpp` "launch", "Multiple users" (corever parsing).
