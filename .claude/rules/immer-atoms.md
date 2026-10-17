---
paths:
  - src/**
  - tests/**
---

# immer atoms: pointers into a snapshot

- `atom->load()` returns a box (a refcounted snapshot); `find()` on it returns a pointer into that box.
  - Never keep the pointer past the full expression of `atom->load()->find(...)`: once the temporary box
    is gone, only the atom keeps the map alive, and any concurrent `update` frees it.
  - Hold the box (`auto snap = atom->load(); auto *p = snap->find(k);`) or copy the value out first.
- XERK-2023: the StartRunner handler passed such a pointer to the runner thread; a sibling runner's exit
  (`StopStreamEvent` erases the session's devices queue) freed it — SIGSEGV or immer's
  `debug_size_heap` assert when ~32 runners on one session exit together.
- Tests: `tests/testWolfAPI.cpp` "Runners: many concurrent starts on one session that exit at once".
  It segfaulted on every CI leg before the fix; it can't prove absence, only catch a regression often.
