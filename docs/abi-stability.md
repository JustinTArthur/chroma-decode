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

## Extending option structs

Caller-populated option structs (`chd_nn_session_opts_t`, `chd_dropout_opts_t`)
are passed **by pointer** and carry no `size`/version field, so the library
cannot tell how large the caller's struct is. That constrains how they may grow
without breaking binary compatibility:

- **ABI-safe (allowed within a major version):** add a field by consuming a
  slot from the trailing `reserved[]` array, leaving `sizeof` and every existing
  field offset unchanged. Two rules make this sound:
  1. The new field's **zero value must mean "previous behavior"** — an old
     binary zero-fills `reserved`, so the library reads the new field as `0`.
  2. Callers **must** zero-initialise, via the struct's `*_default()` initializer
     (e.g. `chd_nn_session_opts_default`) or `= {0}`. This is already required.
- **ABI break (major version only):** growing the struct past `reserved`,
  reordering fields, or changing a field's type/size. An old caller binary then
  allocates a smaller struct than a newer library reads through the pointer,
  running past the caller's allocation.

Once a struct's `reserved[]` is exhausted, the next field needs a versioned
struct (`chd_..._opts_v2_t`) or a retrofitted leading `size_t struct_size` the
library validates — both larger changes best timed to a major bump.

New caller-populated option structs should ship a `reserved[]` tail from the
start, as `chd_nn_session_opts_t` and `chd_dropout_opts_t` both do (`reserved[4]`).

## Version probe

```c
#include <chromadec/version.h>

int major, minor, patch;
chd_version(&major, &minor, &patch);
// or:
const char *v = chd_version_string();   // e.g. "0.1.0"
```
