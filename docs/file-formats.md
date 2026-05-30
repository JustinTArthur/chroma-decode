# File formats

libchromadec reads two families of composite-video capture: the ld-decode
**`.tbc`** format, and the **CVBS file format**. This page describes both and
maps each to the right opening function. The opening functions are named by
signal layout (composite vs Y/C), not by which family the file came from: an
ld-decode `.tbc` and a CVBS `.composite` both open with `chd_video_open_composite`.

## Reader matrix

| On disk | Open with | Sidecar | Notes |
|---|---|---|---|
| `capture.tbc` | [`chd_video_open_composite`](api-reference.md#chd_video_open_composite) | `capture.tbc.db` (SQLite) or `capture.tbc.json` | ld-decode output. Composite, time-base-corrected. |
| `luma.tbc` + `chroma.tbc` | [`chd_video_open_yc`](api-reference.md#chd_video_open_yc) | `luma.tbc.db` + `chroma.tbc.db` | vhs-decode Y/C-separated pair (e.g. S-Video). Decoded per plane, then merged. |
| `capture.composite` | [`chd_video_open_composite`](api-reference.md#chd_video_open_composite) | `capture.meta` (SQLite) | CVBS single-file composite. |
| `capture.y` + `capture.c` | [`chd_video_open_yc`](api-reference.md#chd_video_open_yc) | `capture.meta` (SQLite) | CVBS dual-file luma/chroma pair. |

In every case the **sample data and the metadata live in separate files**: the
data file is a bare stream of samples with no header, and all the information
needed to interpret it (geometry, levels, standard, field order) comes from the
sidecar. A data file without its sidecar cannot be decoded.

## The TBC format

A `.tbc` ("time-base-corrected") file is the output of ld-decode's RF
demodulation and time-base correction stage. It has historically been
under-documented, so this section is deliberately thorough.

### On-disk layout

A `.tbc` file is a **raw, headerless stream of unsigned 16-bit little-endian
samples**. There is no magic number, no version field, and no structure marker
anywhere in the file: it is purely sample values, back to back.

The samples are **field-sequential**. The file is a concatenation of fields,
each field being exactly `fieldWidth × fieldHeight` samples, stored row-major
(line by line). Because every field is the same fixed size, the field count is
simply:

```
numberOfFields = fileSizeInBytes / (fieldWidth * fieldHeight * 2)
```

and any field or line can be located by arithmetic alone (the format is fully
random-access). The `× 2` is because each sample is two bytes.

Two consecutive fields make an interlaced frame; which field of the pair comes
first is recorded in the metadata (`isFirstFieldFirst`), not implied by file
order. (The `reverse_field_order` decoder option flips this; see the
[option registry](api-reference.md#option-registry).)

### Sample encoding

Each 16-bit word holds a **10-bit sample value left-shifted by 6 bits** (the low
6 bits are zero). In the terms of the CVBS specification below, this is exactly
the `U16_4FSC` sample encoding. Two consequences worth knowing:

- The usable range is `0..1023` scaled into `0..65472` in steps of 64.
- Sampling is **4×f<sub>SC</sub>** (four samples per colour-subcarrier cycle),
  and the signal is **composite**: luma and chroma are interleaved in the same
  samples, exactly as the decoder expects to separate them.

The black, white, and blanking reference levels are *not* fixed by the format;
they are recorded per-capture in the sidecar as `black16bIre`, `white16bIre`,
and `blanking16bIre` (16-bit IRE reference points). Older sidecars predate
`blanking16bIre`; when it is absent the library falls back to `black16bIre`.

### The metadata sidecar

Everything the raw samples omit lives in a sidecar file next to the `.tbc`:

- **`capture.tbc.json`** is the original JSON sidecar.
- **`capture.tbc.db`** is the newer SQLite sidecar (ld-decode migrated to
  SQLite for large captures).

[`chd_video_open_composite`](api-reference.md#chd_video_open_composite) auto-locates either
(`.tbc.db` preferred, then `.tbc.json`) when you pass `NULL` for the sidecar
path, or you can pass an explicit path to either form. The library reads both;
the file extension (case-insensitive) selects the parser.

The sidecar carries two kinds of information:

**Capture-wide video parameters**, including:

| Field | Meaning |
|---|---|
| `fieldWidth`, `fieldHeight` | Field geometry, in samples and lines. |
| `sampleRate`, `fSC` | Sample rate and colour-subcarrier frequency (Hz). |
| `isSubcarrierLocked` | Whether sampling is locked to the subcarrier. |
| `isWidescreen` | 16:9 flag. |
| `colourBurstStart` / `End` | Burst window, in samples. |
| `activeVideoStart` / `End` | Active-line sample range. |
| `firstActiveFieldLine` / `lastActiveFieldLine` | Active field-line range. |
| `firstActiveFrameLine` / `lastActiveFrameLine` | Active frame-line range. |
| `white16bIre`, `black16bIre`, `blanking16bIre` | 16-bit IRE reference levels. |
| `numberOfSequentialFields` | Field count recorded by the capturer. |

**Per-field records** (one per field, keyed by a sequential number), including
the field's `isFirstField` flag, sync confidence, median burst IRE, subcarrier
phase ID, dropout ranges (start/end sample per line), and optional VBI / VITC /
closed-caption / NTSC-specific / PCM-audio sub-records. Dropout ranges from
these records drive [dropout correction](api-reference.md#dropout-correction).

### What the format does not contain

There is no audio in the `.tbc` itself (PCM-audio parameters in the sidecar
describe a separate stream), no colour-decoded output, and no per-frame index
beyond the implicit field arithmetic. The decoder synthesises component
Y'CbCr output from these composite samples at decode time; the `.tbc` is the
raw composite signal, nothing more.

## The CVBS file format

The CVBS file format captures composite video more flexibly than `.tbc`: it
adds dual-file Y/C, several sample encodings, and a structured `.meta` sidecar.
libchromadec reads it through
[`chd_video_open_composite`](api-reference.md#chd_video_open_composite)
and [`chd_video_open_yc`](api-reference.md#chd_video_open_yc).

Rather than restate the specification, this page summarises the shape and defers
to the authoritative source:

!!! info "Authoritative specification"
    The CVBS File Format Specification is published at
    **[simoninns.github.io/cvbs-file-format-specification](https://simoninns.github.io/cvbs-file-format-specification)**.
    Treat it as the source of truth; the summary here is orientation only.

### Shape of the format

The format separates three **independent preset axes**, so a capture is
described by one choice on each axis rather than a single monolithic mode:

1. **Video Standard** (e.g. the line standard and colour system).
2. **Sample Encoding** (how samples are quantised and packed).
3. **Signal State** (TBC-locked, TBC-unlocked, raw, and standard vs
   non-standard variants).

It defines **two file layouts**:

- **Composite** (`.composite`): a single file carrying the full CVBS signal.
- **Dual-file Y/C** (`.y` + `.c`): luma and chroma in separate files, for
  S-Video-style sources where the two are already separated.

Metadata lives in a **`.meta` SQLite sidecar** (the specification pins the
schema version). As with `.tbc`, the data files hold samples and the `.meta`
file holds everything needed to interpret them; pass `NULL` to the open
function to auto-locate `capture.meta`, or give an explicit path.

### Sample encodings libchromadec accepts

The library recognises the CVBS sample encodings below. An ld-decode `.tbc`
reads as `U16_4FSC` (its on-disk layout). The exact bit layouts are specified
in the document above; in brief:

| Encoding | Summary |
|---|---|
| `U16_4FSC` | Unsigned 16-bit, 4×f<sub>SC</sub>. Same packing the `.tbc` format uses. |
| `U10_4FSC` | Unsigned 10-bit, 4×f<sub>SC</sub>. |
| `S16_28M` / `S16_40M` | Signed 16-bit raw composite at 28.6 MHz / 40 MHz sample rates. |
| `TPG21_4FSC` | Test-pattern-generator encoding at 4×f<sub>SC</sub>. |

When metadata is absent or you need to force parameters, both CVBS open
functions accept a
[`chd_video_params_t`](api-reference.md#chd_video_params_t) override.