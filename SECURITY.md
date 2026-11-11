# Security policy

Please report a vulnerability privately through GitHub's **Report a
vulnerability** feature. Do not open a public issue containing exploit
details, private paths, credentials, dumps or game content.

Only the latest tagged release is supported. Setup downloads pinned
archives and checks each against its SHA-256; the Visual Studio Build Tools
installer must carry a valid Microsoft Authenticode signature. A local
native build necessarily runs compilers and source obtained from the
projects in `THIRD_PARTY_NOTICES.md`. Review `config/release-toolchain.json`
and the pinned ShiftGlue revision before running setup if your threat
model requires it.
