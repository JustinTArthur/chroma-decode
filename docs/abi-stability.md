# ABI stability

libchromadec follows [Semantic Versioning](https://semver.org).

## Pre-1.0 (0.x.y)

- ABI may break across **minor** versions: `0.1.x → 0.2.0` is an ABI break.
- ABI is stable within a minor version: `0.1.0 → 0.1.x` is binary-compatible.
- Each release builds with `SameMinorVersion` CMake compatibility, so
  `find_package(chromadec 0.1 CONFIG)` accepts `0.1.x` but not `0.2.0`.

## After 1.0

- ABI stable across **major** versions: `1.x.y → 1.(x+1).0` is
  binary-compatible.
- ABI may break only at major-version boundaries: `1.x.y → 2.0.0` is an ABI
  break.

## Symbol export discipline

Only symbols matching `chd_*` are exported.

- ELF (Linux, *BSD): enforced by the version script at `meson/chromadec.map`.
- macOS: enforced by `meson/chromadec.exports`.
- Windows: a `.def` file is added in a follow-up.

Any new public function must:
1. Have a name matching the `chd_*` pattern.
2. Be declared in a header under `include/chromadec/`.
3. Have its rationale and lifetime rules documented in the header.
4. Pass the CI ABI-checker against the previous release tag.

## Version probe

```c
#include <chromadec/version.h>

int major, minor, patch;
chd_version(&major, &minor, &patch);
// or:
const char *v = chd_version_string();   // e.g. "0.1.0"
```
