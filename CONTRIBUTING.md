# Contributing

Start with the [README](README.md) and [docs/STAGE0.md](docs/STAGE0.md).
Reverse-engineering findings go in `docs/findings/*.toml`.

## Before opening a change

- Keep game files, extracted assets, generated translations, build output,
  logs, frame dumps and user data out of commits and issue attachments
  ([docs/LEGAL.md](docs/LEGAL.md)).
- Do not paste code copied or translated from the original game.
- Keep changes focused, use Conventional Commits, and say how they were
  tested.

From PowerShell on Windows:

```powershell
.\tools\check-repository-boundary.ps1
dotnet build .\launcher\LavenderShift.Launcher -c Release
.\tools\build.ps1
```

Changes to the runtime should also build and run against a supported,
personally dumped disc. Never upload that build.

## Reports

Include your Windows version, CPU, GPU, driver version, the launcher stage
that failed and `source\.local\logs\setup-error.json` (or the last lines of
the setup log). Do not attach an ISO, XEX, extracted game files, generated
`.cpp` files or minidumps.

By contributing, you agree that your contribution is licensed under this
repository's BSD 3-Clause License.
