# SDL_GameControllerDB

`gamecontrollerdb.txt` and `LICENSE` are unmodified copies from
[mdqinc/SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB)
at commit `4e87e3c6c2574f49e1cdf8526b6f829b61e5ccb3` (2026-11-17), under the
zlib license in `LICENSE`. They are community gamepad mappings for SDL, for
controllers SDL does not know by itself.

The build joins this file and `config/gamecontrollerdb-extra.txt` (Lavender's
own mappings, which come later and so win) into `gamecontrollerdb.txt` beside
the game. To update, replace both files from a newer commit and change the
commit above.
