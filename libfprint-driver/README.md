# libfprint driver

`fpc1021.c` is a working `FpImageDevice` driver for libfprint, implementing
the protocol documented in [`../PROTOCOL.md`](../PROTOCOL.md).

## Status

**Working, and enrollment completes end to end** (2026-08-29): built against
a fresh checkout of [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint),
correctly detects the device, identifies the FPC1021 chip via Get Chip ID,
waits for a finger, and captures a full clean 160x160 image — confirmed via
libfprint's own `examples/img-capture` tool, which wrote a real, recognizable
fingerprint to `finger.pgm` and even ran libfprint's minutiae detection on it
successfully.

**Fixed (2026-08-29): the wedge was a reply-queue desynchronisation, and
enrollment now completes.**

The symptom: after roughly 2-3 successful captures the sensor appeared to
stop answering the capture-reply read entirely, until the whole USB device
was reset — which made libfprint think the device had disconnected and
aborted whatever `fprintd-enroll` session was in progress, so a multi-stage
enrollment could never finish.

The cause, found by tracing a real enrollment at the kernel URB level with
`tools/usbmon-watch.sh`: **the sensor emits a second, unrequested image
after most captures.** Because this protocol pairs reads and writes 1:1,
that unread image puts every later reply behind 413 stale packets (one
header plus 412 stream packets). Reset gets answered by pixel data, Get Chip
ID by stream packets, and once the backlog runs out the device goes quiet
and every read times out. Nothing is actually stuck; the host is simply
reading one conversation behind.

The fix is a `CAPTURE_DRAIN` state that empties the IN endpoint before each
capture, reading with a 50ms timeout until it comes back empty. Measured on
a completed enrollment:

```
fpc1021: drained 413 stale packet(s) before capture     (x6)
capture commands sent (0700):  8
capture headers received:     14
```

Six extra headers, six drains, exact accounting. `Enroll result:
enroll-completed` — five stages plus two retry-scans, print saved.

**The drain has to cover open too.** The first `fprintd-verify` after a
completed enrollment failed with `unrecognised FPC chip ID 0x6400` — and
0x6400 is 25600, the image-length field of a capture header being decoded as
a chip ID. The extra image survives a close/open cycle, so the reply to
Get Chip ID at open time was a stale header. `OPEN_DRAIN` now runs before it.

That failure also showed a gap worth closing generally: the driver believed
a reply that did not acknowledge the command it had sent. Replies carry
`status = 0x1000 | opcode` precisely so that can be checked, and the chip-ID
path now rejects a mismatch with "desynchronised reply" instead of decoding
whatever bytes arrived.

**Still open: where the extra image comes from.** It did not reproduce over
plain libusb in 21 captures, including four paced to match libfprint's own
~7.5ms/packet drain, and a bare read after a capture with the finger held
down always timed out. Whatever triggers it, the driver no longer needs to
know: draining makes an unread image harmless instead of fatal. The
never-ported opcode `0x0005` remains the natural suspect for a "stop
capture" command that would prevent the frame rather than discard it.

Two earlier observations, kept because they cost time to establish:
`CLEAR_FEATURE(ENDPOINT_HALT)` on both endpoints does not restore touch
detection, and `libusb_reset_device()` returned `ok` without clearing the
stuck state while a physical unplug/replug cleared it instantly. The USB
reset recovery path is retained as a last resort but should no longer
trigger in normal use.

## Matching

`fprintd-verify` returned `verify-no-match` four times in a row against a
freshly completed enrollment, on captures that look excellent — sharp dark
ridges on light valleys, correct polarity for NBIS. So the problem was not
acquisition.

`tools/fpc_minutiae.c` counts what libfprint actually extracts from a raw
capture, which turns that into a measurement:

```
image: 160x160, 0.00 px/mm
minutiae detected: 11
```

Two candidate defects behind those numbers. **Both were tried and reverted**
— see below — but they are recorded because the reasoning still holds and
the second is probably still worth fixing on its own:

- **No `FPI_IMAGE_PARTIAL`.** On a 160x160 patch the image edge is not the
  finger's edge, so every ridge running out of frame reads as a ridge
  ending. The flag makes NBIS drop those perimeter artefacts; `elan.c` and
  `elanspi.c` both set it for the same reason.
- **`ppmm` left at 0.0.** NBIS sizes the neighbourhood it scores each
  minutia's reliability over as `RADIUS_MM * ppmm`, so zero collapses it to
  no pixels at all. The FPC capacitive family is specified at 508 dpi.

**Setting both made enrollment worse, not better.** Captures still succeeded
(`CAPTURE_NUM_STATES completed successfully`) but no enroll stage ever
passed — libfprint accepted none of the images, and the enrollment sat there
until it was interrupted. Starting from 11 minutiae, `FPI_IMAGE_PARTIAL`
evidently removes enough of them to fall under whatever libfprint needs to
accept a stage. The flag is right in principle and wrong at this minutia
count.

Reverted to the state where enrollment completes. Worth retrying separately:
`ppmm` alone, since it only affects reliability scoring and was never the
suspect for stage rejection, and the two were changed together — which made
it impossible to tell which one did the damage.

The real lever is likely upstream of all this: 11 minutiae is thin, bozorth3
needs overlap between two sets, and nothing currently stops a poor frame
entering the template. That is the image-quality gating below.

### Why verification returns no match: the bozorth3 floor

`fprintd-verify` fails with `score 0/24` on **every** comparison — 25 of 25
across five attempts, never a low score, always exactly zero. The reason is
in `nbis/include/bozorth.h`:

```c
#define MIN_COMPUTABLE_BOZORTH_MINUTIAE 10
```

`bz_match_score()` returns `ZERO_MATCH_SCORE` without attempting a
comparison if *either* side has fewer than 10 minutiae. The stored template
holds five prints with 7, 5, 8, 2 and 7 minutiae; a verification image
carries about 8. Every comparison is short-circuited before matching starts.

So `bz3_threshold` is irrelevant here — the score is never computed — and
this is not a question of poor overlap between prints. The sensor simply
does not clear the floor.

Measured minutiae per capture, ten samples (`tools/fpc_minutiae.c`):

```
11, 8, 8, 8, 8, 7, 0, 0, 0     one of ten reaches 10
```

Three placements yielded *no* minutiae at all. Three captures of one
unmoved finger gave 8, 8, 8 with positions stable to within a few pixels,
so extraction is repeatable and these are real features, not noise — there
are just very few of them. 160x160 at 508 dpi is 8x8mm, and ~8 minutiae is
about what that area of a finger holds.

Ruled out along the way: contrast processing (normalize, equalize,
auto-level all leave the count unchanged) and a resolution mismatch — the
ridge period measures 10px, i.e. 0.50mm at 508dpi, which is the normal human
value and confirms both the scale NBIS assumes and the `ppmm` set above.

Upscaling, as `elanspi.c` does with `fpi_image_resize (img, 2, 2)`, changes
counts wildly and inconsistently: one image went 7 -> 67 minutiae at 2x,
another 11 -> 2 at 3x. Interpolation cannot add information, so most of that
is likely spurious. **Counting minutiae is the wrong measurement** — false
minutiae inflate the count and hurt matching. What matters is whether two
captures of the same finger score against each other, and that needs an
offline matching harness (libfprint's `virtual-image` driver, not currently
built here) rather than an enroll/verify cycle against a real finger.

Where this leaves the driver: capture and enrollment work; verification
cannot until captures reliably clear 10 minutiae. Image-quality gating would
remove the zero-minutiae frames and the 2-minutia print now in the template,
which is necessary but may not be sufficient, since the best sample seen is
11.

### Enlargement vs match score, measured

| factor | delivered | best bozorth3 score |
|---|---|---|
| 1x | 160x160 | 0 — under MIN_COMPUTABLE_BOZORTH_MINUTIAE, never computed |
| **2x** | 320x320 | **15** |
| 3x | 480x480 | 10 |
| 4x | 640x640 | 0 |

Threshold is 24; `aes3k` settles for 9. At 3x the distribution over 25
comparisons was `0 x18, 3 x3, 6 x1, 9 x1, 11 x1, 12 x1` — three would clear
aes3k's bar, none clear ours.

**2x is the optimum**, and the numbers above supersede an earlier live
comparison that appeared to favour 3x. That comparison re-enrolled and
re-pressed the finger between factors, so it varied conditions as much as
factors; these come from `tools/fpc_bench.c` replaying the same ten saved
captures through the same pipeline, with only the factor changing.

What goes wrong past 2x is visible on three captures of a finger that was
never lifted between them:

| factor | minutiae | scores against each other |
|---|---|---|
| 1x | 8, 8, 8 | all 0 (under the floor) |
| **2x** | 11, 20, 23 | **6, 7, 15** |
| 3x | 4, 21, 41 | all 0 |
| 4x | 17, 7, 20 | all 0 |

At 1x the counts are stable but too low. At 2x they clear the floor and stay
consistent. Past that the count keeps climbing while consistency collapses —
4 against 41 minutiae from near-identical images — because interpolation is
amplifying sensor noise into minutiae that differ between two views of the
same finger. More minutiae that do not correspond is worse than fewer that
do, which is why counting minutiae was the wrong measurement all along.

Enlargement alone is spent as a lever at 15 — but the *interpolation* is not.

### Interpolation: Catmull-Rom instead of bilinear

`fpi_image_resize()` interpolates bilinearly, which softens ridge edges
exactly where NBIS looks for them. Swapping the filter at the same 2x, over
the same ten captures:

| filter | best | mean | pairs at or above 24 |
|---|---|---|---|
| bilinear (`fpi_image_resize`) | 15 | 4.0 | 0 |
| Point / nearest | 15 | 3.1 | 0 |
| Mitchell | 20 | 2.7 | 0 |
| Lanczos | 22 | 2.7 | 0 |
| **Catmull-Rom** | **25** | 2.8 | **2** |

Catmull-Rom is the only configuration tried that puts any pair at or above
the threshold — the first genuine matches this driver has produced. It is
also the principled choice: it is the interpolating cubic, so it passes
through the original samples rather than averaging them away, and its slight
overshoot at an edge sharpens a ridge boundary instead of rounding it off.

Larger targets do not help with it either — Catmull-Rom at 400px scores 9 and
at 480px scores 11, against 25 at 320px — which is the same 2x optimum found
above, now confirmed under a second filter.

The driver carries its own `fpc_resize_catrom()` rather than calling
`fpi_image_resize()`. Making the shared helper offer a sharper filter would
be the better fix, and is worth proposing upstream.

### Sharpening before enlargement

A user observation pointed at this one: `fprintd-verify` returned no-match
*instantly*, as though nothing were really being compared. It wasn't. The
enrolled template was healthy — 35, 51, 29, 65 and 24 minutiae, all five
clear of the floor — so the short-circuit had to be on the other side: the
verification image was landing under 10 minutiae, and bozorth3 was returning
zero without comparing anything.

That asymmetry has a cause. **Enrollment quietly selects good frames** — a
stage only passes if libfprint accepts the image, so poor ones are retried —
while a verification is scored on whatever arrives. The verification image is
the one that needs help.

An unsharp mask on the raw frame, before enlargement, provides it. Real
frames go from 8–11 minutiae to 13–148, so none fall under the floor any
more, and the share of captures that match another capture of the same finger
roughly triples:

| configuration | captures matching another | mean best score per capture |
|---|---|---|
| enlargement only | 2 of 11 | 7.0 |
| + unsharp (sigma 1.5, amount 2.5) | **6 of 11** | **17.4** |

The parameters come from a grid over both, and sit on a plateau rather than a
spike. Past sigma 2.0 the score collapses to 0–2 of 11, which is the
mechanism showing itself: blur too much before sharpening and there is
nothing left to sharpen.

The gate still runs on the *unsharpened* frame, where a blank frame is
unambiguous and where its threshold was calibrated.

Ten samples is a small dataset and the optimum is loosely determined; the
plateau is the reassuring part, not the peak.

On the device, this removed the short-circuit entirely. Twenty comparisons
across several verifications, before and after:

```
before:  0 x20
after :  7 x3   9 x1   10 x4   11 x3   12 x3   13 x3   15 x1   17 x1   20 x1
```

Every comparison now produces a real score, and the best is 20 against a
threshold of 24.

### The threshold question, and the measurement it needs

With genuine scores clustering between 7 and 20, a threshold of 24 rejects
everything, and `aes3k` settles for 9. But lowering it is a security decision,
not a tuning one, and it cannot be made from genuine scores alone: what
matters is the *separation* between a genuine comparison and an impostor.

**That measurement has now been taken, and the answer is the unwelcome one.**
Captures of a second finger, scored against the first through
`tools/fpc_bench.c`:

| configuration | genuine | impostor | d' |
|---|---|---|---|
| 2x, no sharpening | 4.0 | 4.9 | -0.14 |
| 2x + unsharp 1.0/1.0 | 10.2 | 9.1 | +0.13 |
| **2x + unsharp 1.5/2.5 (shipped)** | **14.4** | **11.8** | **+0.26** |
| 2x + unsharp 1.0/3.0 | 14.9 | 17.4 | -0.22 |
| 3x + unsharp 1.5/2.5 | 8.1 | 12.7 | -0.54 |

The shipped configuration is the best of those tried and does separate the
two fingers — but barely. At its best operating point, a threshold of 16, it
would accept 45% of genuine attempts and **14% of a stranger's finger**. A
d' of 0.26 is nothing: usable biometrics sit above 3.

**No threshold makes this safe**, and `bz3_threshold` should stay at 24 —
which in practice means verification rejects everything, and that is the
honest state of it.

Note what this exposes about the tuning above: every image-processing
parameter here was chosen by maximising *genuine* scores, with impostor
scores never measured. That optimises the wrong objective — raising all
scores together looks like progress and is not. The separation column is the
one that matters, and it should gate any future change to this pipeline.

Caveat: only two impostor captures were usable, so the impostor side rests on
28 pairs from two images. The margin would have to be very different, not
slightly different, for the conclusion to change.

### Where that leaves the driver

Capture, the wedge fix, enrollment and the diagnostics are solid and worth
upstreaming. Verification is not, and should be presented as such rather than
propped up with a lowered threshold: this sensor's 8x8mm window, through
NBIS, does not currently separate fingers well enough to authenticate
anybody.

A literature pass against primary sources — [`../docs/research/small-area-matching.md`](../docs/research/small-area-matching.md)
— confirms that position and sharpens it in two ways. First, minutiae matching
really is the wrong tool at this area: NIST's own conclusion is that "image
sizes below 320 pixels by 320 pixels should not be used" (NISTIR 7201), and
this sensor delivers a quarter of that. Second, and less comfortably, d' 0.26
is far below what the published record reports at a comparable window — FVC2006
DB1 at 9.8mm, NISTIR 7201 at 9.1mm and FPC's own algorithm at 9.6mm work out to
d' of roughly 3.2, 3.8 and 6.0. Area alone does not explain the gap; part of it
belongs to this pipeline.

Two findings there bear directly on the code above. The ten-minutia floor is a
NIST *default*, adjustable via `-A minminutiae=#`, which libfprint freezes into
a `#define` — its re-vendoring script strips NIST's runtime control of it
(`nbis/update-from-nbis.sh:83-87`). The unsharp mask exists only to climb over
that number, and it manufactures minutiae that the enlargement measurements
above already showed do not correspond between two views of the same finger.
And `fpi_print_bz3_match()` returns on the first stored template that clears
the threshold (`fpi-print.c:262-265`), so the five enrolled prints are used as
a maximum rather than fused — where the mosaicking literature puts score-level
fusion at roughly an order of magnitude in EER.

Neither closes the gap. The note ranks what might, and is explicit that nothing
available does it in one step.

### Testing that: the floor is not the lever, and the sharpening earns its place

The cheapest item on that ranking was to lift the floor and drop the sharpening
that only existed to climb over it, on the reasoning that manufactured minutiae
cost precision. **Measured, that is wrong**, and the pipeline stays as it is.

The measurement needed two things first. `tools/fpc_bench.c` now classifies pairs
itself from `-S` labels instead of leaving the score matrix to be read by hand,
and reports mean, standard deviation and d' per class — the hand classification
had already gone wrong once. And `bozorth-floor.patch` restores NIST's runtime
control of the floor to the vendored NBIS, so `-m` can move it at all.

Over nine captures of one finger and two of a second, 32 genuine and 24 impostor
pairs, at 2x:

| configuration | genuine | impostor | d' |
|---|---|---|---|
| **unsharp 1.5/2.5, floor 10 (shipped)** | **18.9** | **13.5** | **0.58** |
| unsharp 1.5/2.5, floor 8 / 6 / 4 / 2 | 18.9 | 13.5 | 0.58 |
| no sharpening, floor 10 | 4.8 | 5.8 | -0.12 |
| no sharpening, floor 8 | 5.4 | 5.9 | -0.07 |
| no sharpening, floor 6 / 4 / 2 | 6.4 | 5.9 | 0.08 |

Two things fall out. **The floor is a non-lever while sharpening is on** — every
frame already clears 10 by a wide margin, so moving it to 2 changes nothing at
all. And **removing the sharpening costs half a d'**, which the floor cannot buy
back: the best unsharpened configuration reaches 0.08 against 0.58.

So the sharpening is not merely inflating minutia counts to beat a threshold.
Whatever it does to the ridge structure survives the separation objective, which
is the objective the rest of this pipeline was tuned against and should have been.

Re-tuning both parameters on that objective, rather than on genuine scores:

| sigma | amount | genuine | impostor | d' |
|---|---|---|---|---|
| 1.5 | 1.5 | 17.7 | 12.2 | 0.54 |
| 1.5 | 2.0 | 15.9 | 12.5 | 0.36 |
| **1.5** | **2.5** | **18.9** | **13.5** | **0.58** |
| 1.5 | 3.0 | 20.4 | 15.3 | 0.49 |
| 1.5 | 4.0 | 20.6 | 16.0 | 0.45 |
| 1.0 | 2.5 | 19.6 | 20.0 | -0.05 |
| 2.0 | 2.5 | 12.5 | 11.6 | 0.12 |

The shipped 1.5/2.5 is the best cell of both sweeps, which is luck rather than
method — it was chosen by maximising genuine scores and happens to survive the
right objective. The rows around it show why that is luck: amount 3.0 and 4.0
score *higher* genuine than the shipped setting and separate worse, and sigma
1.0 — which was this bench's own default — scores 19.6 genuine against 20.0
impostor, i.e. it prefers a stranger's finger.

None of this closes anything. d' moves from 0.08 to 0.58 against a target above
3, and the differences among the sharpened rows are smaller than 24 impostor
pairs from two images can resolve. What it does settle is that the floor and the
sharpening are answered, and that the next thing to spend effort on is the
impostor set — which the research note already ranked first, for this reason.

### How fragile all of this is, measured

Falling out of the same runs, and worth more than the result they were for.
Splitting the genuine side by how the finger was placed, against the *same two*
impostor captures throughout:

| genuine captures | genuine | impostor | d' |
|---|---|---|---|
| `shot1-3` — one finger, never lifted | 22.2 | 8.2 | **2.24** |
| `place1,3,4` — same finger, varied placements | 19.5 | 18.8 | **0.08** |
| all nine mixed | 18.9 | 13.5 | 0.58 |

The genuine means barely move: 22.2, 19.5, 18.9. **The entire swing is on the
impostor side**, from 8.2 to 18.8 — against an impostor set that never changed.
Certain placements of the enrolled finger score against a stranger's finger as
highly as against themselves; `place3` scores 31 and 39 against the two impostor
captures, above most of its genuine pairs.

So the similarity this pipeline measures is driven at least as much by *how the
finger was placed* as by *whose finger it is*. That is a hypothesis about
mechanism — plausibly the interpolation and sharpening manufacturing structure
common to any frame from this sensor — but the measurement itself is not in
doubt.

The practical consequence is immediate: **d' on this dataset ranges from 0.08 to
2.24 depending on which captures you choose**, so no number in any table above
can rank two configurations against each other. They can only say that
sharpening beats no sharpening by more than that range, which they do. Every
finer comparison has to wait on a real impostor set. The research note ranked
that first on the reasoning that nothing else is decidable without it; this is
that reasoning demonstrated instead of asserted.

### The real impostor set, and what it settles

Five fingers of one hand, eight presses each with the placement deliberately
varied, no blank frames: 40 captures, 280 genuine and 1280 impostor pairs, up
from 32 and 24. Every configuration above was re-measured against it.

**The pipeline is at chance.**

| configuration | genuine | impostor | d' | AUC |
|---|---|---|---|---|
| 1x, unsharp 1.5/2.5 | 0.7 | 0.2 | 0.28 | 0.534 |
| **2x, unsharp 1.5/2.5 (shipped)** | **7.6** | **6.4** | **0.26** | **0.563** |
| 3x, unsharp 1.5/2.5 | 7.0 | 6.7 | 0.11 | 0.525 |
| 4x, unsharp 1.5/2.5 | 4.2 | 3.4 | 0.23 | 0.558 |
| 2x, no sharpening | 0.1 | 0.0 | 0.17 | 0.511 |
| 2x, unsharp 1.5/3.0 | 7.6 | 6.5 | 0.25 | 0.583 |
| 2x, unsharp 1.0/2.5 | 7.1 | 6.7 | 0.10 | 0.525 |

Nothing tried — enlargement 1x to 4x, unsharp amount 0 to 4.0, sigma 0.5 to 2.5,
the bozorth floor from 10 down to 2 — moves AUC outside 0.511 to 0.583. **0.5 is
a coin flip.** At the shipped threshold of 24 the pipeline accepts 1% of genuine
attempts and 0% of impostors; at the threshold with the best available trade it
accepts 81% of genuine and 69% of impostors.

AUC was added for this dataset because d' stops being trustworthy here. Most
scores are exactly zero on a weak configuration, which shrinks the standard
deviations and inflates d' — 1x scores d' 0.28 while accepting 11% of genuine
attempts, which is not a better configuration, only a more degenerate one. AUC
makes no distributional assumption and splits ties honestly.

**The machinery is not the problem.** An identical image scored against itself
returns **191**, so extraction, enlargement, sharpening and bozorth3 all work.
The finding is about the data reaching them.

Eight presses of one finger, scored against each other:

```
     3  5  5  10  4  3  3
        9   4  7  5  4  12
            6  8 12  5   8      best 12, mean 6.1
```

Against an impostor mean of 6.4. **Two presses of the same finger share almost
nothing bozorth3 can align**, which is the whole result: the ~100 minutiae per
sharpened frame are not reproducible between presses.

That the earlier two-finger dataset gave AUC 0.687 rather than 0.563 is the
small-sample effect this was run to remove, and it also corrects the section
above: the claim that sharpening "earns its place on separation" was measured at
d' 0.08 against 0.58 on 56 pairs. On 1560 pairs it is AUC 0.511 against 0.563 —
the same direction, a tenth of the size, and both ends are chance. Sharpening
moves the scores; it does not move the discrimination.

### Two confounds in that dataset, stated because they are real

The new captures carry far fewer *real* minutiae than the earlier ones. Measured
unsharpened, where nothing is manufactured:

```
earlier batch:   7, 29, 33, 8, 57, 15
new batch:       5,  7,  7, 2, 10,  6
```

After sharpening the new frames reach 39-127 minutiae, so **almost all of what
the matcher sees on them is manufactured**, which is a sufficient explanation for
why it does not correspond between two presses. Tile contrast does not predict
this — `index1` has higher contrast than `shot1` and fewer real minutiae — so the
gate cannot currently tell a weak frame from a strong one.

And the placement variation was deliberate, on instruction from this analysis,
after varied placements were found to drive the earlier impostor scores. On an
8x8mm window a rotated press may share very little skin with the previous one,
so the protocol may be harder than what a user unlocking a laptop produces. The
earlier batch's varied placements scored a mean of 18.2 against the new batch's
6.3.

Neither confound rescues the conclusion — both sit inside a measurement that is
at chance, and the earlier, more favourable batch was itself only AUC 0.687 — but
they do bound what has been shown. What is established: **at 8x8mm, through NBIS,
with realistic placement variation, this pipeline cannot distinguish two presses
of one finger from two different fingers.** What is not established is how much
of that is the sensor, how much is frame quality, and how much is a placement
protocol harsher than real use. Separating those needs a capture protocol that
holds placement to what a user actually does, and a frame-quality measure better
than tile contrast.

### The better quality measure exists, is discarded by libfprint, and does not help

NBIS already computes the measure the section above asked for. `mindtct` scores
every minutia it finds for how much it trusts it, and libfprint's
`minutiae_to_xyt()` carries that into `c[i].col[3]` — and then copies only
`col[0..2]` into the `xyt_struct` bozorth3 matches on
(`fpi-print.c:138-152`). NBIS ships `bz_prune()` and `sort_quality_decreasing()`
for precisely this use and neither is reachable from libfprint.

That is worth reporting upstream on its own. It also has a side effect nothing
here hits but another driver might: when a frame yields more than
`MAX_BOZORTH_MINUTIAE` (200), libfprint keeps the first 200 of a list `mindtct`
sorted by *position* (`minutia.c:533`, `sort_minutiae_y_x`), so the discarded
minutiae are the ones lowest on the image rather than the ones least trusted.
Our frames top out at 155.

The hypothesis it enables is direct: if the minutiae sharpening manufactures are
the untrustworthy ones, dropping them should raise separation. `fpc_bench -q N`
drops minutiae under N% reliability before the template is built.

| minimum reliability | minutiae kept (mean) | genuine | impostor | AUC |
|---|---|---|---|---|
| **0% (what libfprint does)** | **77** | **7.6** | **6.4** | **0.563** |
| 10% | 70 | 7.2 | 6.0 | 0.557 |
| 20% | 40 | 6.0 | 5.8 | 0.498 |
| 30% | 39 | 5.7 | 5.6 | 0.498 |
| 40% | 13 | 0.7 | 0.4 | 0.530 |
| 50% | 7 | 0.1 | 0.0 | 0.505 |

**It gets worse, monotonically at first and never better.** Rejecting whole
frames on the same signal, rather than individual minutiae, fails the same way —
keeping only the 25 captures whose median reliability clears 0.25 gives AUC
0.509, and the best 18 give 0.522, against 0.563 for all 40.

So NBIS scores the manufactured minutiae as trustworthy as the real ones, and
pruning on reliability removes signal and noise together. **The frame-quality
lever is spent**: the better measure than tile contrast exists, it is NIST's
own, and it carries nothing that helps this sensor match.

Median reliability does still describe the *frames*. The earlier batch sits
uniformly at 0.40-0.44 while the new one spreads 0.15-0.47, which is the batch
quality difference recorded above showing up in a second, independent
measurement. It is a real quality signal about a capture. It is just not one
that predicts whether two captures will match.

That leaves the capture protocol as the only untested item on the list.

### `fprintd-identify` appears to hang

It is not hanging on the driver. `fprintd-identify` keeps capturing until it
recognises a finger, so while matching fails it never stops — and libfprint's
thermal model then disables the device:

```
Updated temperature model ... FP_TEMPERATURE_HOT
Device reported an error during verify: Device disabled to prevent overheating.
verify_cb: result verify-disconnected
```

`DEFAULT_TEMP_HOT_SECONDS` is `3 * 60` in `fp-device-private.h`: three
minutes of continuous activity disables any device, driver-independent.
Cooling back to cold takes `DEFAULT_TEMP_COLD_SECONDS`, nine minutes —
or restart `fprintd`, since the model is per-process state.

While matching is unreliable, use `fprintd-verify`, which captures once and
reports, rather than `fprintd-identify`, which loops. The same session
confirmed the drain still doing its job under a long run:
`drained 413 stale packet(s) before capture`.

Also not yet done:
- The "await finger" retry-loop timing (`FPC_RETRY_DELAY_MS`,
  `FPC_CAPTURE_COOLDOWN_MS`, the 3000ms read timeout in
  `CAPTURE_READ_HEADER`) reflects what got a full raw-image capture
  working, not necessarily what's needed for reliable back-to-back
  captures — see the bug above.
- Not yet submitted upstream.

## Applying this to a libfprint checkout

```sh
git clone https://gitlab.freedesktop.org/libfprint/libfprint.git
cp fpc1021.c libfprint/libfprint/drivers/
cd libfprint
git apply ../meson.build.patch   # registers the driver in both meson.build files
meson setup build -Ddoc=false
ninja -C build
```

Then, with the Surface Type Cover attached:

```sh
sudo env LD_LIBRARY_PATH=./build/libfprint ./build/examples/img-capture
```

(root is needed only because there's no udev rule yet granting user access
to this specific USB device; a proper udev rule is one of the remaining
upstreaming steps.)
