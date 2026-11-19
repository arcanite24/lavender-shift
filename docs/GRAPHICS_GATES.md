# Graphics gates (issue #5, phases 1-3)

What each item on `gfx/issue5-phases` has been checked with, and what is
still to check with the game. Everything here was written while another
session had the game, so no item has been seen in play. One game session
can clear the list from the top; every row says what to run and what
passes.

States: **offline-tested** (a test without the game passed), **compiles**
(Windows and Android compile; never executed), **gate pending** (needs the
game).

## Before anything else

1. **Replay fidelity (SR-8).** Every shader A/B below rests on this. The
   existing replays of `.local/logs/{mb,lod,wall}.fh1dump` show solid
   colour bars in places, and nearly every word differs from the capture,
   so it is unproven that a replay reproduces FH2's frame.

   ```
   python tools/frame-replay.py replay .local/logs/mb.fh1dump --out .local/ab/fidelity --label base
   ```

   Pass: `front.png` matches a screenshot of the same frame (F8) apart from
   the HUD, with no bars. If not, the shader rows below need play instead.

## Items

| Item | State | Gate (with the game) |
| --- | --- | --- |
| SR-14 rows, auto mip bias, GSR | compiles; on `main` | Each row applies live; GSR on Android |
| SR-9 guest names, interface check | offline-tested (fixtures, all 291 modules of `fr-2x` fit themselves) | A guest-named file loads at 1x and 2x, on Windows and Android; a misfit logs and falls back. Live reload is not done: a restart reloads |
| SR-8 replay A/B, discovery | offline-tested (decode and compare on three dumps; discovery and `--check-mod` on `fr-2x`) | `frame-replay.py ab` gives a pair and a PSNR; fidelity above |
| SR-16 data patches | offline-tested (patches applied to the real TrackSettings.xml: priority, path case, CRLF, problems) | Start with `mods_log_game_files=.xml` and drive in free roam: are `TrackSettings.xml` and `Ribbon_00/TimeOfDay*.xml` opened? Then enable `docs/examples/data-patches` as a mod: shadows reach further. Frame time on the RTX 4080 and UHD 620. No preset row until then |
