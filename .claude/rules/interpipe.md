---
paths:
  - "src/moonlight-server/streaming/**"
  - "src/moonlight-server/sessions/**"
  - "docker/gstreamer.Dockerfile"
  - "docker/patches/gst-interpipe-*.patch"
---

# gst-interpipe producer switching

- A stream's consumer pipeline (`interpipesrc`) starts on its own producer, then a lobby join
  switches `listen-to` to the lobby's producer (`SwitchStreamProducerEvents`).
- Stock gst-interpipe does not hand the switched listener the new node's caps, so the consumer
  keeps the old caps: with zero copy on NVIDIA that is CUDAMemory caps over system-memory buffers
  ("Input buffer is not CUDA", no video). `docker/patches/gst-interpipe-renegotiate-on-switch.patch`
  fixes this (XERK-2496); keep it while the gst-interpipe pin in `gstreamer.Dockerfile` lacks it.
- The patch is pinned to a gst-interpipe commit; bumping that commit means re-checking the patch.
