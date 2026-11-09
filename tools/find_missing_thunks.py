#!/usr/bin/env python3
"""List function-pointer targets the generated tree does not register.

Runs Pinyon Shift's tools/find-fh1-missing-thunks.py (which needs Pinyon's
address mapper beside it) on a loaded FH2 image and one of Lavender Shift's
generated module trees, and prints [functions] entries for review. Import
thunks count as registered: a [functions] entry for one is "outranked by
import", and in a facade it breaks the link.

    python -I tools/find_missing_thunks.py <pinyon checkout> <image.bin> <generated dir> [--load 0x82000000]

The image is the module as loaded with title update v2 applied (base
0x82000000 for default.xex, 0x88000000 for XMediaFacade_default.xex,
0x89000000 for SpeechFacade_default.xex). Pinyon's build writes one from
.local/game/codegen, where each XEX sits beside its .xexp:

    pinyon_shift_fh1_archive_extract --xex-image <xex> <out.bin> --apply-patch
"""
from __future__ import annotations

import importlib.util
import re
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    pinyon = Path(sys.argv[1])
    spec = importlib.util.spec_from_file_location(
        "find_fh1_missing_thunks", pinyon / "tools" / "find-fh1-missing-thunks.py")
    finder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(finder)

    def registered(generated: Path) -> set[int]:
        init = next(generated.glob("*_init.cpp"))
        text = init.read_text(encoding="utf-8", errors="ignore")
        # Every entry, imports included (Pinyon's matches sub_ names only).
        return {int(x, 16) for x in re.findall(r"\{ 0x([0-9A-F]{8}), \w+ \}", text)}

    v4map = finder.V4MAP

    def extent(image, start: int, limit: int = 0x800) -> int | None:
        """Pinyon's extent, except that a branch to a .pdata function start is
        a tail call that ends the path. Pinyon's follows a forward one inside
        the bound and then rejects the thunk for entering that function, which
        dropped FH2's adjustor thunk at 831B1760 (this -= 4, then a branch
        0x68 bytes on to sub_831B17C8)."""
        visited, stack, last = set(), [start], start
        while stack:
            pc = stack.pop()
            while True:
                if pc in visited:
                    break
                if not start <= pc < start + limit or (
                        pc != start and (pc in image.functions or image.containing(pc))):
                    return None
                word = image.words[image.index(pc)]
                if word == 0:
                    return None
                visited.add(pc)
                last = max(last, pc)
                op, link = word >> 26, word & 1
                if op == 16:
                    displacement = word & 0xFFFC
                    displacement -= 0x10000 if displacement & 0x8000 else 0
                    target = (pc + displacement) & 0xFFFFFFFF
                    if not link and not word & 2 and target not in image.functions:
                        stack.append(target)
                    if not link and (word >> 21) & 0x14 == 0x14:  # branch always
                        break
                elif op == 18 and not link:
                    target = v4map.branch_target(word, pc)
                    if (target is not None and start <= target < start + limit
                            and target not in image.functions):
                        stack.append(target)
                    break  # tail branch or in-function jump
                elif (op == 19 and (word >> 1) & 0x3FF in (16, 528) and not link
                      and (word >> 21) & 0x14 == 0x14):
                    break  # unconditional blr/bctr
                pc += 4
        return last + 4 - start

    finder.registered = registered
    finder.extent = extent
    sys.argv = [sys.argv[0]] + sys.argv[2:]
    return finder.main()


if __name__ == "__main__":
    sys.exit(main())
