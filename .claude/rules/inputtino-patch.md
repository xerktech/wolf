---
paths:
  - src/core/CMakeLists.txt
  - src/core/patches/**
  - src/core/src/platforms/linux/uinput/**
  - src/moonlight-server/control/input_handler.cpp
---

# inputtino is patched at fetch time

- `src/core/patches/inputtino-thread-safety.patch` is applied to the FetchContent'd inputtino (`PATCH_COMMAND`).
  - It fixes data races TSan found (XERK-2412): keyboard repeat thread, uhid/joypad event threads, PS5 report state,
    and feedback callbacks (`set_on_rumble`/`set_on_led`/...) set on one thread and invoked on another.
  - Device threads are now joined in the destructors, so no inputtino thread outlives its device and calls Wolf
    callbacks (which log) during static teardown.
- Bumping `GIT_TAG`: regenerate the patch against the new tag (or drop the hunks upstream already has), or the
  configure step fails in `git apply`.
- `WOLF_CUSTOM_INPUTTINO_SRC` bypasses the patch: a custom source must carry these fixes itself.
- Inputtino callbacks run on inputtino's threads, not Wolf's: anything they touch must be thread-safe.
- Tests: the CI TSan leg runs with `TEST_VIRTUAL_INPUT=ON`; it is what catches a regression here.
