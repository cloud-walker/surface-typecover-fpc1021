# FPC1021 USB protocol (Surface Type Cover with Fingerprint ID)

This document describes the USB wire protocol spoken by the fingerprint sensor
built into the Microsoft Surface Pro Type Cover with Fingerprint ID. It was
reconstructed via static analysis of Microsoft's own, publicly-downloadable
Surface driver package, then independently validated end-to-end against a
real device on Linux via `libusb`. No Windows installation was used at any
point.

## Hardware

- USB composite device `045E:09C2` ("Microsoft Corp. Surface Type Cover").
  Interface 0 is the ordinary HID keyboard/touchpad; **interface 1** is the
  fingerprint sensor.
- Interface 1: vendor-specific class (`0xFF`), two bulk endpoints:
  - `0x04` OUT (host -> sensor, commands)
  - `0x83` IN (sensor -> host, replies)
  - Both `wMaxPacketSize = 64`, full-speed (12 Mbps).
- Sensor chip: Fingerprint Cards **FPC1021**, a capacitive area/touch sensor,
  **160x160 px, 8 bits/pixel grayscale**.
- The same USB "shell"/hardware ID also covers other Type Cover fingerprint
  revisions using sibling FPC chips (see the auto-detection table below) —
  this document focuses on FPC1021, the chip found in this Surface Pro 7's
  Type Cover.

## Command format

Every command is a **2-byte little-endian opcode** written to the bulk OUT
endpoint. Each command produces **exactly one reply packet** on the bulk IN
endpoint — reads and writes must be paired 1:1; do not write two commands
before reading, or you will read the wrong command's reply.

### Reply packet layout

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | `status` | `0x1000 \| opcode`, i.e. it echoes which command this is acknowledging. Not a generic "ready/busy" flag. |
| 2 | 2 | `substatus` | `0` = OK. Nonzero = error/reject. `5` has been observed as a transient "not ready yet" value — treat as retryable. |
| 4 | 2 | `length` | Only present on replies longer than 4 bytes (e.g. capture). Total payload size to expect. |
| 6+ | up to 58 | `payload` | First chunk of payload, if any. |

A short (4-byte) reply is normal for commands with no payload (e.g. reset) —
it's just `status`+`substatus` with no `length`/`payload` fields.

### Known opcodes

| Opcode | Name | Reply size | Description |
|---|---|---|---|
| `0x0001` | Get Chip ID | 6 bytes | Returns the sensor's chip-ID word; see auto-detection table below. |
| `0x0007` | Capture | 6 + streamed payload | Triggers an image capture and streams it back. |
| `0x0008` | Reset | 4 bytes | Resets/aborts the capture engine. Send this before every capture attempt. |
| `0x0005` | *unknown* | 4 bytes | Seen in the Windows driver but not required for a basic single-shot capture; purpose not yet determined. |

## Capture flow

1. Write `08 00` (Reset). Read its reply (`status` will be `0x1008`) and discard it.
2. Write `07 00` (Capture).
3. Read the first reply packet (up to 64 bytes).
   - `status` should be `0x1007`.
   - If `substatus` is `5`, or `status`/`substatus` otherwise indicate "not ready" (e.g. no finger present yet), wait briefly and retry from step 1.
   - If `substatus` is some other nonzero value, the capture was rejected (bad image quality, finger removed too soon, etc).
   - Otherwise take `length` (bytes 4-5) as the total image size — for FPC1021 this is `25600` (`160*160`).
4. The first packet's bytes 6..63 are the first 58 bytes of payload.
5. Keep reading further packets until `length` bytes are collected. For each subsequent read, request `min(bytes_remaining + 2, 64)` bytes; **the first 2 bytes of every such read are a per-packet marker and must be discarded** — only the remaining bytes are payload.
6. The assembled buffer is the final image: a flat, row-major, 8-bit grayscale raster of `width x height` pixels. No further decoding, compression, or bit-packing is involved — it can be dumped directly as a raw grayscale image (verified: `magick -size 160x160 -depth 8 gray:capture.bin out.png` produces a clearly recognizable fingerprint).

## Chip auto-detection (Get Chip ID, opcode `0x0001`)

Write `01 00`, read the 6-byte reply. The chip-ID word is at reply offset 4.
Mask it with `0xFFF0` (except FPC1022, masked with `0xFF0F`) and compare:

| Masked chip ID | Chip | Resolution (WxH) | Model number |
|---|---|---|---|
| `0x0200` | FPC1020 | 192x192 | 1020 |
| `0x0210` | **FPC1021** | **160x160** | **1021** |
| `0x1400` | FPC1140 | 192x56 | 1140 |
| `0x1500` | FPC1150 | 208x80 | 1150 |
| `0x0101` (masked `&0xFF0F`) | FPC1022 | 88x112 | 1022 |

Any other value is an unsupported/unrecognized chip.

## Timing

Observed in the Windows driver; useful for a well-behaved retry loop:

- Wait up to ~15s for a capture-engine lock before giving up on a single capture attempt.
- Enforce a ~3s cooldown between capture attempts.
- After writing Reset, wait ~15ms before writing Capture; wait ~10ms after writing Capture before reading.
- When waiting for the reset command to complete, poll for up to ~500ms.
- When retrying a capture due to poor image quality (see below), pace retries about ~810ms apart.

## Image quality (not yet ported)

The Windows driver classifies each captured frame into one of four buckets —
"Perfect", "Hideous", "Too Dark", "Too Soft" — using a block-based contrast
analysis (computing min/max pixel value over small tiles across the image)
plus a separate finger-presence check, and retries the capture loop until an
acceptable frame is obtained or a cap is hit. This logic has been identified
but not yet reimplemented here.

## Open questions

- The purpose of opcode `0x0005` is unknown; not required for a basic capture.
- Enrollment/template-storage flow (as opposed to a single raw image capture) has not been explored — this device operates in WinBio's "Basic" sensor mode, meaning matching happens host-side, so a Linux driver only needs to reproduce image capture, not any on-chip enrollment protocol.
- No encryption or attestation (e.g. Microsoft's SDCP) is involved anywhere in this path.

## Related

- [linux-surface/linux-surface#353](https://github.com/linux-surface/linux-surface/issues/353) — tracks community interest in Linux support for this exact hardware.
- [fingerprint-cards/capacitive_device_driver](https://github.com/fingerprint-cards/capacitive_device_driver) — FPC's own published Linux driver code for a different (I2C) FPC1xxx product line; not directly usable here (different transport) but a useful cross-reference for the vendor's conventions.
