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
- The same goes for `for (x : *atom->load())` / `atom->load().get()`: in C++20 the box dies once the range
  is initialised, so nothing keeps the container alive while the loop runs; iterate a named snapshot.
- XERK-2023: the StartRunner handler passed such a pointer to the runner thread; a sibling runner's exit
  (`StopStreamEvent` erases the session's devices queue) freed it — SIGSEGV or immer's
  `debug_size_heap` assert when ~32 runners on one session exit together.
- Tests: `tests/testWolfAPI.cpp` "Runners: many concurrent starts on one session that exit at once".
  It segfaulted on every CI leg before the fix; it can't prove absence, only catch a regression often.
- The other sites (hot-plug handler, API pair endpoints, `pair_next_phase`, control's session lookups)
  have storm tests (`tests/storm.hpp` helpers): a reader races a thread that keeps replacing the atom.
  - A normal build rarely crashes with a fix reverted; the CI ASan leg is what catches the read of
    the freed snapshot. Its `-DIMMER_NO_FREE_LIST=1` sends freed nodes straight back to malloc, where
    ASan watches them; it's a safeguard (every reverted fix was also caught without it).
  - The storms are probabilistic: each test caught its reverted fix ~10/10 runs on a 4-core box.
    Don't cut their iteration counts; they end well inside their time budgets when the code is right.
  - Tests: "Pairing: a phase lookup racing other clients' pairing", "Pair API: completing pairings
    while other pairing requests come and go", "control: a connecting peer's session lookup racing
    session updates", "control: a stopped stream's peer lookup racing peers connecting",
    "Hot-plugging devices while other streams start and stop".
