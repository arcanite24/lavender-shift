# FH2 reverse-engineering records

One `[[finding]]` per fact about Forza Horizon 2 (TU v2), in Pinyon
Shift's format, so the two projects' records can be read side by side.
Each record has these fields:

- `group`: the section it belongs to.
- `name`: a short name.
- `v2`: the TU v2 address or addresses.
- `fh1_v4`: the matching FH1 v4 address, when the fact came across from
  Pinyon.
- `what`: what it does.
- `how`: how it was established (hook, trace, xref, string, experiment, or
  the FH1 v4 → TU v2 address mapper).
- `confidence`: confirmed, probable or guess.
- `source`: the code or doc that holds the detail.

Rules:

- Search these files for an address before adding a record. Extend the
  existing record instead of adding a second one.
- Record negative results too, so they are not retried.
- Write only our own analysis: no disassembly listings, decompiled code
  or game data.

Named addresses and offsets that code uses also go in
[`config/fh2-symbols.toml`](../../config/fh2-symbols.toml).

| File | Covers |
| --- | --- |
| [rendering.toml](rendering.toml) | Post effects, render scenarios, camera, HUD |
| [systems.toml](systems.toml) | Launch options, world, cars, events, time of day, pause menu and system UI |
| [threads.toml](threads.toml) | Simulation step, GPU waits, pacing |
