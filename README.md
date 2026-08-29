# surface-typecover-fpc1021

Reverse-engineered USB protocol, and an in-progress Linux driver, for the
**FPC1021** fingerprint sensor built into the Microsoft **Surface Pro Type
Cover with Fingerprint ID**.

## Status

- ✅ Protocol fully reverse-engineered — see [`PROTOCOL.md`](PROTOCOL.md)
- ✅ Validated end-to-end against real hardware: a working standalone capture
  tool (`src/fpc_capture.c`) pulls a real, recognizable fingerprint image via
  plain `libusb` on Linux.
- ✅ Working libfprint `FpImageDevice` driver — see
  [`libfprint-driver/`](libfprint-driver/). Builds cleanly against upstream
  libfprint and has captured a real, clean fingerprint image end-to-end
  through libfprint's own APIs (device probe → open → activate → capture →
  minutiae detection), not just the raw prototype above.
- ⬜ Not yet upstreamed to libfprint/linux-surface.

## Background

[linux-surface/linux-surface#353](https://github.com/linux-surface/linux-surface/issues/353)
has tracked missing Linux support for this exact fingerprint reader since
2021, without anyone starting the reverse-engineering work (confirmed with a
maintainer as recently as mid-2026). This repo exists to close that gap.

## How this was reverse-engineered

Entirely via **static analysis of Microsoft's own, publicly-downloadable
Surface driver package** (the `.msi` from Microsoft's download center) — no
Windows installation, USB sniffing hardware, or dual-boot was used. The
resulting protocol model was then validated live against the real sensor via
`libusb` on Linux. See [`PROTOCOL.md`](PROTOCOL.md) for the full writeup.

Only the protocol facts (command bytes, packet layout, timing) are
documented/reimplemented here — no decompiled code or binaries from
Microsoft's/Fingerprint Cards' driver are included in this repository.

## Trying it yourself

Requires a C compiler and `libusb-1.0` development headers.

```sh
make
sudo ./build/fpc_capture
```

Place a finger on the sensor; it retries for a few seconds and then writes a
raw `WxH` 8-bit grayscale image (160x160 for FPC1021) to `capture.bin`. View
it with e.g.:

```sh
magick -size 160x160 -depth 8 gray:capture.bin capture.png
```

Pass `-v` to trace every USB transfer, and `-t trace.jsonl` to record one.

## Diagnostics

`build/fpc_probe` is an interactive probe over the protocol: send an
arbitrary opcode, run captures back to back, annotate the timeline, reset the
bus — with every transfer traced to stderr and to a JSONL file that two runs
can be diffed against each other. It's the tool for the wedge bug that
currently blocks enrollment.

```sh
sudo ./build/fpc_probe
fpc> loop 10
```

See [`tools/README.md`](tools/README.md).

## Roadmap

- [x] Reverse-engineer the wire protocol
- [x] Validate against real hardware
- [x] Port to a proper libfprint `FpImageDevice` driver
- [x] Diagnostic tracing + interactive probe (`tools/`) for investigating
      sensor behaviour over time
- [ ] Root-cause the wedge: the sensor stops answering after a handful of
      captures until a full USB reset, which aborts any in-progress
      enrollment — the blocker before this driver is usable day to day
- [ ] Image-quality / finger-presence checks (the Windows driver's logic for
      this has been identified but not yet ported — see `PROTOCOL.md`)
- [ ] Tune the "await finger" retry/cooldown timing against extended real use
- [ ] Test enroll/verify via `fprintd`, not just raw capture
- [ ] Upstream to libfprint and linux-surface

Contributions and testing on other Type Cover / FPC-chip revisions welcome —
see the auto-detection table in `PROTOCOL.md` for the other chips this USB
device ID family covers.

## License

LGPL-2.1-or-later, to match libfprint's license and ease eventual upstreaming.
