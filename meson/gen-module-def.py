#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Emit a Windows module-definition file naming the public C ABI.

The ELF version script and the Mach-O exports list both glob `chd_*`. A .def
file has no such wildcard, so the export set is enumerated here from the public
headers at build time rather than hand-maintained as a second list that could
drift away from the ABI it is meant to describe.
"""

import re
import sys

DECL = re.compile(r"\b(chd_\w+)\s*\(")


def declared_symbols(headers):
    names = set()
    for path in headers:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        text = re.sub(r"//[^\n]*", " ", text)
        names.update(DECL.findall(text))
    return sorted(names)


def main(argv):
    if len(argv) < 2:
        sys.exit("usage: gen-module-def.py OUTPUT HEADER...")
    out_path, headers = argv[0], argv[1:]

    names = declared_symbols(headers)
    if not names:
        sys.exit("gen-module-def.py: no chd_* declarations found in the public headers")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("EXPORTS\n")
        for name in names:
            f.write("    {}\n".format(name))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))