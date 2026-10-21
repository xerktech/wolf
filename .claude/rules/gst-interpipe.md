---
paths:
  - docker/patches/gst-interpipe-*.patch
  - docker/wolf.Dockerfile
  - tests/testGSTPlugin.cpp
---

# gst-interpipe patches

- `docker/wolf.Dockerfile` and the CI `test` job apply `docker/patches/gst-interpipe-*.patch` with one
  glob, so the patches apply in name order: a later patch is a diff on top of the earlier ones.
  - Regenerate a patch from a checkout of `GST_INTERPIPE_REF` with the earlier patches applied, and
    check that the whole glob still applies to a fresh checkout.
- interpipesrc drops an `unlock_stop` that finds its src pad in mode NONE (XERK-2130). Don't remove it.
  - A live basesrc going to PLAYING defers an `unlock_stop` to its streaming thread. A stop right after
    the start can run its `unlock` first; the deferred call then re-arms appsrc's `create`, which waits
    forever under the stream lock, and `set_state(NULL)` hangs. Plain `appsrc is-live=true` has the
    same race (GStreamer's, unfixed up to main).
  - Don't key it on the pad's FLUSHING flag: basesrc calls `unlock_stop` for a flushing seek or
    FLUSH_STOP before the pad is unflushed, so that would wedge every flushing seek.
    No test guards this: element flush events from an app thread trip TSan lock-order reports
    inside GStreamer (and can hang), so a CI test can't send them.
  - Tests: `tests/testGSTPlugin.cpp` "Stopping an interpipe consumer right after it starts ends it".
- A CI hang's last log line doesn't name the hung test: the interpipe tests log nothing, so the hang
  can be several tests past the last test that logged.
