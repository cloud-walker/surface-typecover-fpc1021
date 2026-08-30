# What other open-source projects actually *do* about small-area matching

Primary-source research note, 2026-08-30. Complement to
[`small-area-matching.md`](small-area-matching.md), which covered the published
literature. This one covers code: what shipped, what is in review, what was
measured, and what of it is portable here.

## Summary

**Every open-source project that got a sub-100 mm² press sensor working on Linux
in the last four years stopped handing the image to NBIS and matched the image
directly.** Two matcher classes exist today, both inside libfprint merge
requests, both LGPL-2.1-or-later: SIFT keypoint matching (SIGFM, [!418][mr418] →
[!530][mr530] → [!570][mr570]) and normalised cross-correlation (`focaltech_moh`,
[!572][mr572] → [!646][mr646]). A third, band-limited phase-only correlation, is
a week old and unreviewed ([#869][is869]).

The single most useful source is [!646][mr646] and its report,
[`INFORME-MR572.md`][informe]. On a FocalTech FT9201 — a **64x80** sensor,
smaller than ours — retuning an NCC matcher against a measured 25-genuine /
24-impostor set moved it from unusable to **EER 0.07%**, and the dominant
parameter was not any image-processing knob but the **alignment search radius**:

```
radius  3: EER 8.83%      radius 12: EER 4.26%
radius  8: EER 4.23%      radius 16: EER 0.07%
radius 20: EER 0.15%   (no gain, 1.5x the cost)
```

That driver is an `FpDevice` that stores preprocessed raw images as
`FPI_PRINT_RAW` and matches them itself. **It needs no libfprint change at all** —
it works against master today.

Three findings contradict or sharpen the previous note's ranking:

1. **Alignment, not features.** The previous note ranked feature-class changes
   and consolidation. The one measured sweep in the field says the binding
   constraint on a small press sensor is that finger placement moves ~16 px
   between presses, and a matcher that does not search that far never finds the
   overlap. Our pipeline hands bozorth3 two images and lets bozorth3 do the
   alignment from 8-minutia point sets — the worst case of exactly this problem.
2. **Two of the previous note's ranked options have measured negatives
   elsewhere.** `synaspi` (Synaptics SYNA8002, 144x40, [#868][is868]) records
   that "more enrolment views does *not* fix this — impostor scores rise faster
   than the genuine floor — and Gabor ridge enhancement raises impostor scores
   more than genuine ones" ([README][synareadme]). Those were options #3 and #7.
3. **Dataset construction can dominate the result.** The FT9201 report: an
   earlier set collected by deliberately *varying* finger position "produced EER
   ~45% for every matcher tried, including BLPOC ... Datasets for this device
   have to be collected with natural, repeated placement or the results are
   meaningless" ([INFORME-MR572.md][informe]). Our bench's next dataset has to be
   designed around that. **Since done** — see the update below.

On the negative side, and stated plainly: **nobody patches bozorth3.** A GitHub
code search for `minminutiae` returns zero results; five sampled libfprint forks
all carry `MIN_COMPUTABLE_BOZORTH_MINUTIAE 10` unchanged; Debian, Fedora and Arch
all ship libfprint with no patches at all. Nobody has implemented score-level
fusion over enrolled templates either — every matcher found here, NBIS and
non-NBIS alike, returns max-over-templates. And **nobody else is working on this
device**: `045e:09c2` appears in no code on GitHub outside this repository, and
is not even on libfprint's unsupported-devices wiki.

Ranked portability is in [section 5](#5-what-is-actually-portable-cheapest-first),
and what has happened to that ranking since is in
[the update](#update-what-master-settled-after-this-note-was-researched) directly
below.

---

## Update: what master settled after this note was researched

This note was researched against `96c591b` (#5). By the time it landed,
`origin/master` was at `a5afd1b` (#22) — six commits of measurement done in
parallel worktrees, against a far larger capture set than existed when section 5
was written. The survey of other projects is unaffected; the *ranking* is not.
Recorded here rather than by editing section 5, so that what the sources said
stays separable from what this project then measured.

**Row 1 (rebuild the dataset) is done.** #18 (`a625642`) and #22 (`a5afd1b`)
collected 42 captures across five fingers with natural, repeated placement —
"placed as if unlocking a laptop rather than deliberately varied" — giving **314
genuine and 1408 impostor pairs**. That is the protocol [INFORME-MR572.md][informe]
demands, arrived at independently. The gate this note called blocking is open.

**Row 10 (do not tune bozorth3) is contradicted by measurement.** #6 (`7fd0981`)
added `libfprint-driver/bozorth-floor.patch`, restoring NIST's runtime control of
the minutia floor; #22 then measured, at 1x with everything else fixed, **AUC
0.792 at floor 4 against 0.645 at floor 10**. The floor is the enabling change,
not a dead end. The reasoning in row 10 was sound about the *ecosystem* — nobody
patches bozorth3, and that survey stands — but "nobody does it" was never
evidence about what works on this sensor, and it should not have been ranked as
if it were.

**Row 3 (percentile normalisation instead of the unsharp mask) is superseded.**
#22 measured the raw 1x frame as the best input of the group: sharpening does not
help at 1x at all. The right pipeline here is not a gentler filter but none, so
the question of *which* filter no longer arises.

**The metric changed.** #18 moved from d' to AUC, because on weak configurations
most scores are exactly zero, which shrinks the standard deviations and inflates
d'. The **d' 0.26 that this note's Summary and section 5 argue from is therefore
not the current figure of merit**; the comparable numbers are AUC 0.563 for the
configuration shipped at the time and **AUC 0.802 (TAR@FAR1% 18%)** at 1x, no
sharpening, floor 4.

**The driver now refuses verification by construction.** #20 (`63e1e55`) found
that on well-placed frames the shipped `bz3_threshold` of 24 accepted 97% of
genuine attempts *and 92% of impostors* — the apparent safety had come from poor
captures scoring under it, not from the threshold. #21 (`32dea3c`) set
`bz3_threshold` to `G_MAXINT`.

**What still stands, and is now cheaper than this note assumed.** Row 5 — the NCC
matcher — remains untried, and it was ranked behind a prerequisite that has since
been satisfied: the dataset exists, and `fpc_bench` has grown per-subject labels,
per-class statistics and AUC. One convergence is worth flagging, because the two
lines of work reached it independently: master measured the **raw** frame as the
best input, and NCC consumes raw pixels through a local-mean high-pass rather
than minutiae extracted from a sharpened image. Rows 6, 8 and 9 are untouched by
any of the above.

---

## 1. libfprint's own drivers, read as source

Read from a fresh clone of `https://gitlab.freedesktop.org/libfprint/libfprint`
at `3f1e2817ed01660d0b422a0283effc0e5d3e4b80` (2026-08-04). Paths below are
relative to that checkout. The previous note's §7 already tabulates scan type,
image size, upscale factor and `bz3_threshold` per driver; that table is not
repeated. What follows is the part it did not cover: **what these drivers do to
the pixels, and why.**

### 1.1 elanspi, on the identical 160x160 die

`elanspi.h:46` lists `{0x2, 0xA0, 0xA0, 0x0, 0x0, "eFSA160S"}` — 160x160, the
same geometry as the FPC1021. The driver's pipeline, in order:

**Background calibration and subtraction.** A no-finger frame is captured at
open and stored; every subsequent frame has it subtracted, and pixels that go
negative are counted (`elanspi.c:1154-1173`):

```c
/* in place correct image, returning number of invalid pixels */
static gint
elanspi_correct_with_bg (FpiDeviceElanSpi *self, guint16 *raw_image)
{
  gint count = 0;
  for (int i = 0; i < self->sensor_width * self->sensor_height; i += 1)
    {
      if (raw_image[i] < self->bg_image[i])
        { count += 1; raw_image[i] = 0; }
      else
        raw_image[i] -= self->bg_image[i];
    }
  return count;
}
```

**Finger presence by a two-of-two vote.** `elanspi_guess_image()`
(`elanspi.c:1199-1251`) computes that invalid-pixel percentage and the squared
standard deviation of the frame, scores each against a threshold, and returns
`FINGERPRINT`, `EMPTY` or `UNKNOWN` — an `UNKNOWN` frame is discarded rather than
forced into a decision. Thresholds, `elanspi.h:364-370`:

```c
#define ELANSPI_MIN_EMPTY_INVALID_PERCENT 6
#define ELANSPI_MAX_REAL_INVALID_PERCENT 3
#define ELANSPI_MIN_REAL_STDDEV (592 * 592)
#define ELANSPI_MAX_EMPTY_STDDEV (350 * 350)
#define ELANSPI_MIN_FRAMES_DEBOUNCE 2
```

Note the shape: two independent statistics, an explicit "don't know" verdict, and
a two-frame debounce. This driver's blank-frame gate is a single one-sided
threshold on tile contrast.

**Percentile contrast stretch, not a sharpening filter.**
`elanspi_process_frame()` (`elanspi.c:1279-1325`) sorts the frame, takes the 0th,
30th, 65th and 100th percentiles as `lvl0..lvl3`, and maps each band linearly
onto a different output range:

```c
  qsort (data_in_sorted, frame_size, 2, cmp_u16);
  guint16 lvl0 = data_in_sorted[0];
  guint16 lvl1 = data_in_sorted[frame_size * 3 / 10];
  guint16 lvl2 = data_in_sorted[frame_size * 65 / 100];
  guint16 lvl3 = data_in_sorted[frame_size - 1];
  ...
              if (lvl0 <= px && px < lvl1)
                px = (px - lvl0) * 99 / (lvl1 - lvl0);
              else if (lvl1 <= px && px < lvl2)
                px = 99 + ((px - lvl1) * 56 / (lvl2 - lvl1));
              else /* (lvl2 <= px && px <= lvl3) */
                px = 155 + ((px - lvl2) * 100 / (lvl3 - lvl2));
```

This is copied from the USB `elan` driver, where it carries the rationale
(`elan.c:164-197`):

> "Elantech recommends 2-step non-linear normalization in order to reduce 2^14
> ADC resolution to 2^8 image: 1. background is subtracted (done here) 2. pixels
> are grouped in 3 groups by intensity and each group is mapped separately onto
> the normalized frame (done in `elan_process_frame_*`)"
>
> "For some devices we don't do 2. but instead do a simple linear mapping because
> it seems to produce better results (or at least as good)"

So: a **vendor-recommended** normalisation, and a driver that switched one device
family to plain min/max linear stretching because it measured better
(`elan.c:938-945`, `ELAN_0907`). Two concrete, in-tree, LGPL alternatives to the
unsharp mask this driver currently applies.

**Duplicate-frame rejection.** A frame whose difference from the previous one is
too small is dropped (`elanspi.c:1447-1454`, threshold
`ELANSPI_MIN_FRAME_TO_FRAME_DIFF (250 * 250)` at `elanspi.h:377`):

```c
          gint difference = elanspi_get_frame_diff_stddev_sq (self, self->last_image, self->prev_frame_image);
          if (difference < ELANSPI_MIN_FRAME_TO_FRAME_DIFF)
            { fp_dbg ("<fp_frame> ignoring b.c. difference is too small"); break; }
```

**Then it crops the die and treats it as a swipe.** Frames are cut to at most
`ELANSPI_MAX_FRAME_HEIGHT` 43 rows (`elanspi.h:376`, applied at
`elanspi.c:400-410`), 8-21 of them are collected
(`ELANSPI_MIN_FRAMES_SWIPE`/`MAX`, `elanspi.h:372-374`), and
`elanspi_fp_frame_stitch_and_submit()` (`elanspi.c:1333-1361`) runs
`fpi_do_movement_estimation()` + `fpi_assemble_frames()`, then 2x bilinear and
`FPI_IMAGE_PARTIAL | FPI_IMAGE_COLORS_INVERTED`. Seven enrolment stages
(`elanspi.c:1703`), `bz3_threshold` 24.

**The author says why, in his own words.** The reverse-engineering repository
that became this driver, `mincrmatt12/elan-spi-fingerprint`, README at commit
[`627f7bb`][elanspiro] (2021-06-25):

> "We also treat the sensors as swipe-style ones, since the libfprint image
> matching algorithm is not designed to deal with such small sensors."

and, on finger detection:

> "The current implementation uses two techniques to determine if an image is
> empty, which works well enough right now: - stddev - number of pixels below the
> background image"

That is the primary source for the previous note's inference. It is not that
elanspi could not press-match; it is that the author judged libfprint's matcher
unfit for the size and worked around it in the capture path.

### 1.2 vfs7552 — the other sub-100 mm² press driver

The previous note recorded vfs7552 as 112x112 with no upscale. That is wrong at
current master, and worth correcting: `capture_complete()`
(`vfs7552.c:765-800`) allocates `fp_image_new (2 * VFS7552_IMAGE_WIDTH, 2 *
VFS7552_IMAGE_HEIGHT)` and writes each source pixel into a 2x2 block — a
**nearest-neighbour 2x upscale**, hand-rolled rather than via
`fpi_image_resize()`. It also still declares `img_class->img_width = 112` while
submitting 224 (`vfs7552.c:1069-1071`); harmless, because `img_width`/`img_height`
are effectively vestigial — only `vcom5s.c:219` reads them anywhere in the tree.

Its preprocessing (`clean_image()`, `vfs7552.c:345-372`) is background
subtraction with a fixed 4x gain and saturation:

```c
      if (self->background[i] < self->image[i])
        self->image[i] = 0;
      else
        self->image[i] = self->background[i] - self->image[i];
      if ((int) (self->image[i]) * 4 > 255)
        self->image[i] = 255;
      else
        self->image[i] *= 4;
```

and its finger gate is **two-sided** (`vfs7552.c:32-34`, `585-599`):

```c
#define CAPTURE_VARIANCE_THRESHOLD 1200
#define FINGER_OFF_VARIANCE_THRESHOLD 100
#define NOISE_VARIANCE_THRESHOLD 4000
...
          // ... Additionally we want to ensure
          // that we don't capture prints with a way too high noise level (this sometimes happens).
          if (variance_after > CAPTURE_VARIANCE_THRESHOLD && variance_after < NOISE_VARIANCE_THRESHOLD)
```

A noise *ceiling*, not just a blank floor. This driver's gate has no ceiling; a
noisy frame that clears the contrast floor is enrolled and matched like any
other. libfprint issue [#373][is373], "False negatives with vfs7552", is open and
has been since 2021 — this is not a driver whose approach is validated by
results.

### 1.3 The assembly helpers cannot average a still finger

`fpi_assemble_frames()` (`fpi-assembling.c:255-313`) blits each frame over the
canvas with `aes_blit_stripe()` (`fpi-assembling.c:207-243`), which does a plain
assignment:

```c
      img->data[ix + (iy * img->width)] = ctx->get_pixel (ctx, stripe, fx, fy);
```

There is no averaging of overlapping content: the last frame written wins. And
`find_overlap()` (`fpi-assembling.c:98-130`) begins its search at `dy = 2`:

```c
  /* Seeking in horizontal and vertical dimensions,
   * for horizontal dimension we'll check only 8 pixels
   * in both directions. For vertical direction diff is
   * rarely less than 2, so start with it.
   */
  for (dy = 2; dy < ctx->frame_height; dy++)
    for (dx = -8; dx < 8; dx++)
```

so a stationary finger cannot even be *expressed* — the minimum representable
inter-frame motion is 2 rows, and horizontal search is capped at ±8 px. **The
existing libfprint frame machinery is a swipe stitcher, not a multi-frame
averager, and it is not the tool for "capture several frames of one press".** If
this driver ever accumulates frames, that code has to be written, not called.

For completeness on how the swipe drivers gate frames: `vfs5011.c:315-370` drops
lines whose `fpi_std_sq_dev` is below `DEVIATION_THRESHOLD = 15 * 15`, stops
after `STOP_CHECK_LINES = 50` consecutive empty ones, and records a line only if
`fpi_mean_sq_diff_norm` against the previous recorded line is at least
`DIFFERENCE_THRESHOLD = 600`. Same three ideas as elanspi, at line granularity.

### 1.4 What libfprint does with the minutiae it extracts

Two things the previous note did not reach, both in `fpi-print.c`.

**libfprint takes the first 200 minutiae in detection order, not the best 200.**
`minutiae_to_xyt()` (`fpi-print.c:116-154`) carries an in-tree admission:

```c
/* XXX: This is the old version, but wouldn't it be smarter to instead
 * use the highest quality mintutiae? Possibly just using bz_prune from
 * upstream? */
static void
minutiae_to_xyt (...)
{
  ...
  int nmin = min (minutiae->num, MAX_BOZORTH_MINUTIAE);
  for (i = 0; i < nmin; i++)
    { minutia = minutiae->list[i]; ... }
```

`bz_prune` is NIST's quality-ranked selection, and libfprint's re-vendoring
script deletes it outright (`nbis/update-from-nbis.sh:114`,
`remove_function bz_prune bozorth3/bz_io.c`). NIST's own doc comment for the
loader survives in the tree with the function it described removed
(`nbis/bozorth3/bz_io.c:143-153`):

> "A maximum of MAX_BOZORTH_MINUTIAE minutiae can be returned -- fewer if
> `DEFAULT_BOZORTH_MINUTIAE` is smaller. If the file contains more minutiae than
> are to be returned, the highest-quality minutiae are returned."

This matters here specifically: the unsharp mask lifts frames to 13-148
minutiae, and libfprint's selection among them is *arbitrary* where NIST's was
quality-ranked. The reliability values are computed — `c[i].col[3] = sround
(minutia->reliability * 100.0)` at `fpi-print.c:138` — and then never used to
choose.

**`fpi_print_bz3_match()` is unchanged** from what the previous note quoted
(`fpi-print.c:228-269`): first template over threshold wins. And the reason
multi-template enrolment exists at all is stated in the commit that introduced
it, `e215b050` (Vasily Khoruzhick, 2013-02-18): *"imgdev: perform 5 scans for
enrollment — This feature dramatically improves matching rate on devices with
small sensors."* A year later the same author removed a lowered threshold from
aes1660 with the rationale (`e1728e7c`):

> "Since 5 scans for enroll was introduced it's not necessary to lower
> bz3_threshold anymore, there's a good probability that scan to verify matches
> with at least one enrolled sample."

So upstream's *stated* model of the five templates is "more chances to hit", i.e.
explicitly a maximum, offered as an alternative to lowering the threshold.

It has been revisited exactly once, on paper. libfprint [#271][is271], "Improve
enrollment for imaging devices" (Vasily Khoruzhick, 2020-06-08, still open):

> "Besides being quite inefficient (we're doing 5 comparisons instead of 1) it's
> also not very reliable on small sensors. There's no guarantee that user didn't
> scan his finger the same way 5 times in a row, and different way during
> verification (and we'll get false rejection in this case). What we should do
> instead is merging minutiae information during enrollment and do as many scans
> as we need to get acceptable number of minutiae."

and its sibling [#272][is272], "Improve feature extraction", same day:

> "MINDTCT extracts only minutiae information such as ridge end or ridge
> bifurcation. Unfortunately it's not enough for accurate fingerprint matching
> for sensors with small area. We can implement extraction of representative
> ridge points or ridge shape features to increase number of feature points"

Both are open, unassigned, and six years old. The person who wrote libfprint's
five-stage enrolment identified both of this project's structural problems —
feature-set merging instead of max, and minutiae being the wrong feature class at
small area — and nothing was built. That is the state of the upstream plan.

### 1.5 Thresholds and stage counts, across the tree

The previous note showed that thresholds are set by feel. The commit log says so
in the maintainers' own words:

| commit | driver | change | stated reason |
|---|---|---|---|
| `71e4bb39` (2007) | aes4000 | first per-driver threshold | "aes4000 detects fewer minutiae and hence returns lower scores" |
| `8e0e8e43` (2007) | aes1610 | 15 → 10 | "I think it is ok for the moment" |
| `a1f36c71` (2016) | upeksonly | lowered for `147e:1001` | "Its width is only 216 pixels, and it appears not to be enough for matching at default threshold" |
| `4ff97e7c` (2018) | elan | → 24 | "Based on experience. Values more than 24 seem to work just after enrollment, but it becomes very hard to verify in a day or so" |

Not one references an impostor measurement.

Enrolment stage counts are more interesting, because the match-on-chip drivers
are a window onto what the *vendors'* own matchers ask for. Every value below is
`nr_enroll_stages` at `3f1e281`:

| driver | matching | stages | source |
|---|---|---|---|
| image devices (default) | NBIS, host | **5** | `fp-image-device-private.h:24` |
| `upekts` | — | 3 | |
| `elanspi` | NBIS, host | 7 | `elanspi.c:1703` |
| `synaptics` | on chip | 8 | `synaptics.h:48` |
| `goodixmoc` | on chip | 8 | `goodix.c:32` |
| `realtek` | on chip | 8 | `realtek.h:50` |
| `egismoc` | on chip | 10 | `egismoc.h:53` |
| `focaltech_moc` | on chip | 10 | `focaltech_moc.h:33` |
| `egis_etu905` | on chip | 12 | `egis_etu905.h:44` |
| `mafpmoc` | on chip | 12 | `mafpmoc.h:80` |
| **`fpcmoc`** | on chip | **25** | `fpc.c:24` |
| `focaltech_moh` ([!646][mr646]) | NCC, host | **15** | `focaltech_moh.h:44` |

`fpcmoc` is FPC's own driver, contributed by an FPC employee (`cca2b6a6`, Haowei
Lo `<haowei.lo@fingerprints.com>`, 2022-07-25), and it asks for up to 25
placements. The previous note's FPC-BM finding — 8 placements for a 9.6 mm touch
sensor — is if anything conservative about what FPC asks of users on newer parts.
libfprint's image path asks for 5, and this driver asks for 5.

### 1.6 One unmerged upstream branch is directly relevant

`origin/benzea/self-match-check`, tip `2a9ad74e` (Benjamin Berg, 2022-05-06),
never merged to master. It adds a self-match gate at enrolment:

```c
  probe_len = bozorth_probe_init (xyt);
  score = bozorth_to_gallery (probe_len, xyt, xyt);
  fp_dbg ("self-match score %d/%d", score, bz3_threshold);
  if (score <= bz3_threshold)
    {
      g_set_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_GENERAL,
                   "Not enough minutiae to generate a match!");
```

commit message: *"If the score of a print matching itself is too low to match,
then reject it. It can never match and it is therefore completely useless."*

This is a maintainer's answer to the asymmetry this project measured
independently — enrolment silently selects good frames, verification is scored on
whatever arrives — and it is four years old and unlanded. It is also a *free*
diagnostic for us: the self-match score of a frame is computable offline in
`tools/fpc_bench.c` today and gives a per-frame upper bound with no second finger
required.

## 2. Microsoft Surface hardware specifically

**linux-surface#353 is exactly where this repo's README says it is.** Read via
the GitHub API: opened 2020-12-05 by `@zccrs`, 19 comments, still open, last
comment 2026-07-25. The technical content is one `lsusb -v` dump identifying
`045e:09c2` with `iInterface 4 FPC TSD TX` on the vendor-specific interface, and
a maintainer response ([@qzed][qzed353], 2022-03-27) pointing at FPC's Android
kernel drivers and noting "Neither works with USB as an interface though, so
those links may not be very helpful." The most recent maintainer statement on
progress ([@qzed][qzed353], 2024-08-01) is "I don't think any one has started
looking into this yet." Later comments (2026-03-19, 2026-07-25) are two more
users posting descriptors and offering to help. **No protocol work, no code, no
capture.**

Other Microsoft fingerprint hardware, all unsupported:

| device | product | status |
|---|---|---|
| `045e:09c2` | Surface Type Cover with Fingerprint ID (this one) | no driver anywhere; **not even listed** on libfprint's [Unsupported Devices][unsup] wiki |
| `045e:0815` | Microsoft Modern Keyboard with Fingerprint ID | libfprint [#432][is432], opened and closed the same day (2021-11-03), one comment; listed on the wiki |
| `04f3:0c5a` | Surface Laptop Go (ELAN:ARM-M4) | on the wiki, no driver; matches no `id_table` in the tree |
| `04f3:0c80` | Surface Laptop Go 2 (ELAN:ARM-M4) | on the wiki, no driver |

I could not read the comment threads on libfprint's GitLab: the instance is
behind an Anubis proof-of-work challenge for HTML, and the anonymous REST API
returns `401 Unauthorized` for `/issues/:iid/notes` while serving issue and MR
bodies fine. **Everything quoted from GitLab in this note is from issue/MR
descriptions, which are readable; comment threads are not, and where a
discussion's outcome mattered I have said so rather than guessed.**

The one piece of Microsoft-fingerprint discussion I could reach is the fprint
mailing list, May 2018, on the Modern Keyboard. Igor Filatov — author of the
`elan` driver — [wrote][ml1010]:

> "Sorry for a bit of off-topic but it looks like the new ms keyborad as a small
> touch sensor which is unreliable with the current matching algo. That's the
> problem we really need competent people and/or sponsorship for first. Even if
> someone writes a driver for that kb, the experience will still be so-so."

Eight years old, about a Microsoft small touch sensor, and it is this project's
conclusion in two sentences. In the preceding message Julien Nicoulaud
[offered][ml1009] to buy the hardware for anyone competent who would work on it.
Nobody did.

**Is anyone else doing this?** No. GitHub's code search for `045e:09c2` returns
zero results outside this repository; a repository search for "surface type cover
fingerprint" returns exactly one repository, this one. `045e:0815` likewise
returns zero code hits.

## 3. Outside NBIS: matchers people actually shipped

### 3.1 The landscape, with the numbers each project has

| project | sensor | window | matcher | measured | licence |
|---|---|---|---|---|---|
| [SIGFM][sigfmrepo] / libfprint [!418][mr418], [!530][mr530] | Goodix 5110, ELAN | "meant to work with 64x80" | SIFT + geometric consistency | none published | MIT (standalone); **LGPL-2.1-or-later** in the MR |
| `fpcmoh` [!570][mr570] | **FPC1022** 10a5:9200 | 112x88 @ 508 dpi (24.6 mm², my conversion) | SIGFM | "Verification: 8/8 match rate"; no impostor figure | LGPL-2.1-or-later |
| `focaltech_moh` [!572][mr572]/[!646][mr646] | FocalTech FT9201 | 64x80 (see note below) | NCC, ±16 px search | **EER 0.07%**, 25 genuine / 24 impostor; live 8/8 genuine, 0/10 impostor | LGPL-2.1-or-later |
| [`fingerprint-ocv`][focv] | FPC 10a5:9201 | 112x88 | SIFT homography + **image mosaicking** + MSSIM | none published | **AGPL-3.0** |
| libfprint [#869][is869] | FPC 10a5:9200/9201 | 112x88 | BLPOC (kiss_fft) | none published | proposed for libfprint |
| [`synaspi`][synareadme] | Synaptics SYNA8002 | 144x40 (~14.8 mm², my conversion) | NCC on locally-normalised images | genuine floor +0.4307, impostor max +0.3402 over 21 impostor views | MIT |
| [OpenAFIS][openafis] | n/a (matcher only) | n/a | minutiae triplets | test suite "30%"; no published EER | BSD-2-Clause |
| [FingerJetFX OSE][fjfx] | n/a (extractor only) | n/a | — (MINEX-compliant extractor) | MINEX compliance claimed for the DigitalPersona contribution | LGPL-**3**-or-later |
| **this driver** | FPC1021 | 160x160 @ 508 dpi (64 mm²) | NBIS/bozorth3 | **d' 0.26**, 28 pairs (superseded: AUC 0.563 over 1408 impostor pairs — see the update) | LGPL-2.1-or-later |

Note on the FT9201's size: [!572][mr572] describes it as "64×80 pixels" with
"native resolution (~250 DPI)", while [!646][mr646] calls it "a 3×4 mm sensor".
Those are inconsistent — 64x80 px at 250 dpi is 6.5x8.1 mm (my arithmetic), 53
mm²; 3x4 mm would imply ~540 dpi and 12 mm². I have not resolved which is right.
**Either way it is a smaller window than this sensor's 64 mm², and it reaches EER
0.07% where we measure d' 0.26.**

### 3.2 SIGFM: SIFT keypoints, and the only non-NBIS matcher inside libfprint

Origin: `goodix-fp-linux-dev/sigfm` (formerly `mpi3d/sigfm`), whose README is
blunt about what it is — "SIGFM stands for 'SIFT Is Good For Matching' ... a new
fingerprint matcher algorithm designed for low resolution sensors ... SIGFM is
meant to work with 64x80 images". Integration was proposed as libfprint
[!418][mr418] (2022-11-24, `0x00002a`), picked up and rebased as [!530][mr530]
(2025-04-04, `Tooniis`), and carried again inside [!570][mr570] (2026-03-11,
`ssubbotin`). Four years open.

The algorithm, read from `libfprint/sigfm/sigfm.cpp` on !570's branch
(`ssubbotin/libfprint`, `ba10c93`), header `SPDX-License-Identifier:
LGPL-2.1-or-later`:

- `sigfm_extract()`: `cv::SIFT::create()->detectAndCompute(img, roi, pts, descs)`
  over the whole image.
- `sigfm_match_score()`: brute-force `knnMatch(..., 2)` with Lowe's ratio
  (`distance_match = 0.75`); then for every *pair* of surviving matches, check
  that the distance between the two probe points and between the two gallery
  points agree within `length_match = 0.05`; then count pairs of such pairs whose
  implied rotation agrees within `angle_match = 0.05`. That count is the score.
- Bails out returning 0 if fewer than `min_match = 5` matches or angle pairs.

So the score is a count of geometrically consistent keypoint correspondences —
scale-free, no minutiae, no floor of the `MIN_COMPUTABLE_BOZORTH_MINUTIAE` kind.

The libfprint integration in !530/!570 is the shape the previous note said would
be needed: `FpImageDeviceClass` gains an `algorithm` field
(`FPI_DEVICE_ALGO_SIGFM`), `bz3_threshold` is renamed `score_threshold`, prints
gain an `FPI_PRINT_SIGFM` type, and `fpi_print_sigfm_match()` mirrors
`fpi_print_bz3_match()` — including, note, **the same first-over-threshold rule**:

```c
      int score = sigfm_match_score (pinfo, against);
      ...
      if (score >= score_threshold)
        return FPI_MATCH_SUCCESS;
```

`fpcmoh` (!570) sets `score_threshold = 10` and `algorithm =
FPI_DEVICE_ALGO_SIGFM`, upscales 2x nearest-neighbour before extraction, and
enrols in 5 stages. Its commit message states the position plainly:

> "This sensor's capture area (112×88 pixels) is too small for reliable
> minutiae-based matching — NBIS/Bozorth3 produces essentially zero stable
> minutiae on images this size. After extensive testing, SIFT-based matching
> proved to be the only approach that works reliably for small-area sensors."

Integration was *asked about* before it was written: libfprint [#485][is485]
(2022-06-15, `mpi3d`) opens with "I'm working on a new matcher called SIGFM to
solve issues on small sensors ... I'm wondering how I should implement that" and
lists four architectural options, the first of which ("Add a matcher entry in
`FpImageDeviceClass`") is what !418 went on to build. The issue is still open. I
could not read its replies (see §2).

In the meantime the practical distribution channel is a fork:
`goodix-fp-linux-dev/libfprint`, branch `sigfm`, tip `07306bbc` (2023-01-04,
"Switch to OpenCV 4.5") — which is the tree the [#867][is867] reporter built.

Field reports exist. libfprint [#867][is867] (2026-08-22) is a user running an
ELAN `eFSA80SC` 80x80 on elanspi with SIGFM at threshold 100: "Once image capture
was working, NBIS/Bozorth3 matching was not reliable on this sensor. Repeated
same-finger verification frequently produced `verify-no-match` ... With SIGFM:
enrolled-finger verification became reliable, a different/non-enrolled finger was
rejected". No numbers.

Costs, stated honestly: it drags **OpenCV** into libfprint (resolved as an
optional dependency in !570), and its parameters are still tuned by feel — a
community fork raises `min_match` 5 → 15 and tightens the ratio 0.75 → 0.70 with
the note "These values are subjective" ([patch 0006][lfd55b4] in
`jedbillyb/linux-fingerprint-drivers`).

### 3.3 An evaluation harness worth stealing, twice over

!570 also adds `libfprint/sigfm/sigfm-bench.cpp` (LGPL-2.1-or-later), which is
the closest thing in this whole survey to `tools/fpc_bench.c`. Its header:

> "Measures FAR/FRR of the shipped SIGFM implementation by calling the same public
> entry points a driver does ... A large-area reference print is reduced to a
> sequence of small windows that approximate what a small-area sensor sees: a
> random placement (offset plus rotation) of the finger over the capture window,
> optionally upscaled the way a driver does before extraction."

It reads PGM files named by the FVC/NIST convention (`101_1.pgm`), builds a
gallery of `--stages` windows per subject and `--probes` more as verification
attempts, scores each probe against every gallery taking the best, and prints
TAR/FRR/FAR plus a CSV of `is_genuine,score`. The CSV format is deliberately
compatible with a second project, [`wl2776/sigfm-eval`][sigfmeval] (GitLab), which
does the same thing in Python with a per-device "sensor simulator" — the CS9711
one crops a 68x118 window at a random rotation and applies CLAHE before
extraction (`eval_sigfm_1_n.py:68-101`).

**Neither publishes results.** `sigfm-eval` has no results files and no README;
`sigfm-bench` is "not built by default and not part of the test suite, as it
needs a dataset that cannot be shipped here". They are methodology, not evidence.

### 3.4 NCC on raw pixels: the one result with a real sweep

`focaltech_moh` began as [!572][mr572] (2026-03-15, `0xCoDSnet`): an `FpDevice`
(not `FpImageDevice`) for the FocalTech FT9201, doing its own matching because
"the sensor's native resolution ... is too low for NBIS/bozorth3 minutiae
matching (designed for 500 DPI — only 0-3 unstable minutiae detected)". As
submitted it did not work: genuine scores 0.31-0.47 against impostors 0.05-0.29,
threshold 0.30.

[!646][mr646] (2026-08-20, `dgmtnz.2019`) is the continuation that measured it.
From [`INFORME-MR572.md`][informe]:

| | !572 | proposed |
|---|---|---|
| `FT9201_SEARCH_RADIUS` | 3 | **16** |
| `FT9201_NUM_ENROLL_STAGES` | 5 | **15** |
| `FT9201_NCC_THRESHOLD` | 0.30 | **0.55** (later relaxed to 0.50 after review) |

> "Real finger placement on a 3x4 mm sensor varies by roughly 16 px between
> presses, so a +-3 px search never finds the alignment."

Radius sweep at 15 templates: EER 8.83% (r=3), 4.23% (8), 4.26% (12), **0.07%
(16)**, 0.15% (20). Stage sweep at radius 16: 7.4% (5 templates), 1.6% (10),
0.07% (15). Live after the change: 8/8 genuine accepted at 0.874-0.951, 0/10
impostors accepted at 0.111-0.446, against 4/9 genuine and overlapping ranges
before.

The matcher itself is ~120 lines of plain C, quoted below from the tuned copy in
`driver/focaltech_moh.c` in the [ft9201 repository][ft9201repo]. Preprocessing
(`ft9201_preprocess()`) is bitwise inversion plus subtraction of a 7x7 local
mean — a high-pass filter, `FT9201_LOCAL_MEAN_WINDOW 7`. Scoring
(`ft9201_ncc()`) is textbook normalised cross-correlation over the overlap of
two images at a given `(dx, dy)`, with an overlap guard:

```c
  if (n < w * h / 2)
    return -1.0;
```

and the search is coarse-to-fine with a documented cost/accuracy measurement:

```c
/* Coarse-to-fine translation search. A +-16 exhaustive sweep is 1089 NCCs per
 * template, which at 15 templates is enough CPU to be noticeable; striding by 2
 * and then refining +-1 around the peak costs 296 instead. The correlation
 * surface is broad enough that this is nearly lossless: over 70 measured pairs
 * the mean score loss is 0.004 (median 0.000, worst 0.053) and no genuine pair
 * that cleared the threshold stopped clearing it. */
```

Three further findings in that report bear directly on us:

- **NBIS was measured, not assumed, before being abandoned.** "`nbis-bench`
  (patch 2) runs libfprint's own NBIS over raw frames. This sensor yields 2-4
  minutiae per image where bozorth3 needs ~12, so every score in the matrix is 0.
  That holds across ppmm from 8 to 40 and both ridge polarities, so it is an area
  limit, not a tuning problem."
- **BLPOC was tried and lost.** From the repository README: "BLPOC (band-limited
  phase correlation) works correctly — verified with sanity tests — but performs
  worse than NCC on this sensor." (Translated from Spanish; the original reads
  "rinde peor que la NCC en este sensor".) That is the only head-to-head between
  the two correlation families I found.
- **The thermal model counts idle polling as activity.** "On a press-type reader,
  `fpi_device_update_temp` accumulates heat while the device sits idle polling
  INT_STATUS ... It cost 72 consecutive failed captures before it was obvious."
  This repository hit the same wall from the other side (see commit `6082d5c`);
  it is a libfprint-wide problem for anyone collecting a dataset through the
  driver.

Caveats the author states himself, which should be carried whenever the 0.07% is
quoted: "The EER figures come from one finger and three impostor fingers on a
single machine. That is enough to show the old parameters could not work, but it
is thin for a security threshold."

**How it fits libfprint with no upstream change.** `focaltech_moh` is an
`FpDevice` implementing `enroll`/`verify`/`identify` itself; the enrolled
template is the preprocessed raw images packed into a `GVariant` of type
`"(ya(ay))"` and stored with `fpi_print_set_type (print, FPI_PRINT_RAW)`
(`driver/focaltech_moh.c:540-551` in the [ft9201 repo][ft9201repo]; the same code
is patches 1-6 of [!646][mr646]). That is the "fake MOC" pattern libfprint [#869][is869]
asks about — using the container built for on-chip template handles to carry
host-side data. It works today, on released libfprint, with no patch. The cost is
real and should be said: **the stored template is a set of fingerprint images on
disk**, 15 x 5120 bytes here, where an NBIS template is a minutiae list.

### 3.5 Correlation again, independently: `synaspi`

libfprint [#868][is868] (2026-08-22) is a reverse-engineering report for the
Synaptics SYNA8002 (SPI, ThinkPad X1 Tablet Gen 3), code at
[`sandbranch/x1-tablet-gen3-fingerprint`][synareadme], MIT. Image is 144x40 at
~500 dpi. The matcher (`synaspi/matcher.py`) is:

- `preprocess()`: per-pixel local mean/variance normalisation over a 5x5
  neighbourhood (`blur=1`, `r = blur + 1`) — "Local normalisation is what makes correlation robust to how
  hard the finger is pressed: it removes the slowly-varying illumination/pressure
  term and scales ridge contrast to unit variance."
- `ncc()`: normalised cross-correlation with `min_overlap=1200` — "without that
  guard a 2-pixel corner overlap can score 1.0 and dominate the search."
- `best_match()`: coarse-to-fine translation search over `max_dx=48, max_dy=24`.
- `Template.score()`: best NCC against **any** enrolled view. Max again.

Same family as FT9201, arrived at independently, with a much wider search window
on a wider sensor. Measured: genuine floor +0.4307, impostor max +0.3402 over 21
impostor views from 5 fingers, FRR/FAR 0%/0% at threshold 0.35-0.40 — and the
author bounds it himself: "Zero false accepts out of 21 impostor views bounds FAR
at roughly 14% with 95% confidence (rule of three)".

The two recorded negatives are the valuable part:

> "Two things that turned out to be false, recorded because they cost real time:
> more enrolment views does *not* fix this — impostor scores rise faster than the
> genuine floor — and Gabor ridge enhancement raises impostor scores more than
> genuine ones."

The Gabor path is implemented (`synaspi/enhance.py`: structure-tensor orientation
field, oriented Gabor bank, per-pixel selection, `RIDGE_PERIOD = 9.0`) and
`eval_enroll.py` blends it in with an `--alpha` parameter — the measurement was
made with the code present, not by assumption, and alpha 0.0 won.

Note the tension with §3.4: FT9201 measured stage count 5 → 15 as worth two
orders of magnitude in EER; `synaspi` measured more views as *not* helping. The
difference is plausibly which failure dominates — overlap probability on a
64x80 window versus impostor collision on a 144x40 strip — but neither author
addresses the other's result, and I am not going to reconcile them from outside.
**For this project it means "more stages" is a hypothesis to measure, not a
known-good change.**

### 3.6 Mosaicking, implemented: `fingerprint-ocv`

[`vrolife/fingerprint-ocv`][focv] is a standalone daemon for the FPC
`10a5:9201`, and it is the only open implementation I found of the image-mosaicking
approach the previous note ranked at #6. `src/cvext.cpp`:

- `get_transform_matrix()`: SIFT + `BFMatcher::knnMatch(..., 2)` with ratio 0.6,
  then `findHomography` from the surviving pairs.
- `merge()`: warps the new partial into the accumulated print's frame, requires
  `MSSIM(new_img1, new_img2, overlap == 2)[0] >= 0.3` on the overlap before
  accepting, blends with fixed weights 0.9/0.1, and grows the canvas to the union
  of the two footprints.
- `match()`: warps the probe onto the accumulated print, masks to the valid
  region, and scores with **MSSIM** — optionally after block-wise Gabor filtering
  (`garbor_filter_block_wise(fpr, 32, 24)`).

So the enrolled template is a progressively-built larger fingerprint image with a
validity mask, exactly the "accumulative mapping" the previous note found
described in arXiv:2606.15574 without results. There are no published numbers
here either. And the licence is **AGPL-3.0**, which rules out reuse in an
LGPL-2.1 driver; it can be read for method, not copied.

libfprint [#869][is869] (2026-08-23) proposes a third route for the same
hardware — a "dependency-free" C BLPOC engine over `kiss_fftnd` — crediting
vrolife's protocol work. It is a week old, self-describes as "heavily written by
AI", asks "Is this fake moc driver hack allowed or not", and has no maintainer
response and no measurements. Recorded for completeness; it is not evidence of
anything yet, and the one project that benchmarked BLPOC against NCC (§3.4) found
BLPOC worse.

### 3.7 Things that look relevant and are not

- **[OpenAFIS][openafis]** (BSD-2-Clause, C++17): "this library is focused on the
  matching problem. It does not currently extract minutiae from images." It
  consumes ISO/IEC 19794-2 minutiae templates and matches them with minutiae
  triplets. Same feature class as bozorth3, so it inherits the same 8-minutiae
  problem; its own status table lists "Test suite 30% — EER, FMR100, FMR1000,
  ZeroFMR" and "Certification/evaluation 0%", i.e. **no published accuracy
  numbers**. Last commit 2022-02. Not portable.
- **[FingerJetFX OSE][fjfx]**: a minutiae *extractor*, not a matcher — a possible
  `mindtct` replacement rather than a `bozorth3` replacement. Two problems: it is
  licensed LGPL **v3** or later (`README.txt`), which a project that is
  LGPL-2.1-or-later cannot absorb without moving the combined work to LGPL-3+;
  and extracting better minutiae does not create minutiae that are not there. If
  the FT9201 measurement generalises — 2-4 minutiae is an area limit, not a
  tuning problem — a better extractor does not help.
- **[python-validity][pyval]**: cited by `synaspi` as the ancestor of its TLS
  layer, and it is genuinely the reference for out-of-tree Linux fingerprint
  daemons. But its Synaptics sensors match on-chip: there is no host matcher in
  `validitysensor/` to port. Relevant as an architectural precedent (a userspace
  daemon speaking to `fprintd` via `open-fprintd`, when a driver cannot go
  in-tree), not as matcher source.
- **The FPC1150 libfprint driver in the wild.** `404698-FDU/pro5-android`
  carries `legacy/device-meizu-m86-cm14/libfprint/fpc1150.c` (faust93, 2016,
  LGPL-2.1-or-later) — an actual FPC-family image driver for old libfprint. What
  it does is a warning, not a model: the sensor gives 416x80, and `capture()`
  builds a 416x160 image by stacking the frame with a **vertically flipped copy
  of itself**, then declares `ENROLL_STAGES 35` and `BZ3_THRESHOLD 35`. Mirroring
  adds no information; it manufactures a symmetric duplicate minutiae set to get
  over the floor. Its `contrast()` and `luminosity()` helpers are dead code.
  This is the same failure mode this project already measured with over-scaling.

## 4. Patches, forks and distro packaging

**No distribution patches libfprint's matching.** Debian's packaging
(`salsa.debian.org/debian/libfprint`, branch `debian`, at 1:1.94.10-1) has **no
`debian/patches` directory at all** — the only `.patch` files in the tree are
libfprint's own NBIS vendoring patches. Fedora's `libfprint.spec` (rawhide)
declares no `Patch` lines. Arch's `PKGBUILD` builds an unmodified signed tag.

**Nobody has re-exposed bozorth3's `minminutiae`.** A GitHub code search for
`minminutiae` returns **0 results**. Five libfprint forks sampled directly
(`FrameworkComputer`, `deepin-community`, `joshuagrisham`,
`TenSeventy7/libfprint-egismoc-sdcp`, `SamSeven777/libfprint-fte3600`) all carry
`#define MIN_COMPUTABLE_BOZORTH_MINUTIAE 10` and `#define
DEFAULT_BOZORTH_MINUTIAE 150` unchanged. The re-vendoring script that strips
NIST's runtime control (`nbis/update-from-nbis.sh:81-88`) is intact in every one.

**Nobody has implemented score-level fusion.** Searching libfprint's issues for
"fusion" returns one unrelated driver request. Every matcher surveyed here —
`fpi_print_bz3_match()`, `fpi_print_sigfm_match()`, `focaltech_moh`'s identify,
`synaspi`'s `Template.score()`, `fingerprint-ocv`'s `match()` — takes a maximum
or first-over-threshold over the gallery. The mosaicking literature's
best-performing consolidation rule has no implementation anywhere in this
ecosystem.

**What people patch instead:** thresholds and preprocessing, per device.

- libfprint [!576][mr576] (2026-03-21, `phoehnel`) adds `upektc_img_enhance()`:
  "contrast stretching and 2x nearest-neighbor upscaling", lowers `upeksonly`'s
  threshold to 20, **removes `FPI_IMAGE_PARTIAL`** ("Cropping 2x10px on a 144px
  wide image is significant"), and raises enrolment to 10 stages — reporting
  minutiae counts going from "at most 18, averaging ~15" to "consistently reaches
  20+". Genuine counts only; no impostor measurement. The author notes the
  enhancement function "was written by Claude Opus 4.6" and asks for
  corresponding scrutiny.
- libfprint [!217][mr217] (2020-12-13, `ppiastucki`, still open after six years)
  for the ELAN touch sensors: more calibration loops, `0xaf` handling, "bz3_threshold
  was decreased due to the reader's low resolution and poor image quality", "the
  original image is resized to make minutiae extraction work" and "a simple
  sharpening convolution filter is applied to the resized image to improve
  minutiae extraction (usually doubles the number of extracted minutiae)".
  **That is precisely the resize-plus-sharpen this driver arrived at
  independently, and it too was justified by minutiae count rather than by
  separation.** It has never been merged.
- `jedbillyb/linux-fingerprint-drivers` (LGPL-2.1) is a community hub of
  per-device patch series for unsupported sensors, and is a useful *index* of the
  ecosystem — but its accuracy content is the SIGFM parameter tightening quoted in
  §3.2, whose own commit message calls the values "subjective".

## 5. What is actually portable, cheapest first

Every row is a hypothesis with a source, not a promised gain. The measurement
that gates all of them is still the one the previous note named: the impostor set
is 28 pairs from two images, and nothing below is decidable until that grows.
**That has since happened** — 314 genuine and 1408 impostor pairs on master — so
row 1 is complete and rows 3 and 10 have been measured. See
[the update](#update-what-master-settled-after-this-note-was-researched).

| # | change | source | effort | honest verdict |
|---|---|---|---|---|
| 1 | **Collect the dataset the way the FT9201 report says to** — natural, repeated placement; many fingers; raw frames saved before any processing | [INFORME-MR572.md][informe] "Method" and its EER ~45% warning | low | Prerequisite for everything. Our current set may be *adversarial* in exactly the way that produces meaningless numbers |
| 2 | **Score every frame against itself** (`bozorth_to_gallery(probe, xyt, xyt)`) in `fpc_bench` | unmerged libfprint branch `benzea/self-match-check`, `2a9ad74e` | very low | Free upper bound per frame; separates "bad capture" from "bad match" without a second finger. Also tells us whether the enrol/verify asymmetry is still live |
| 3 | **Swap the unsharp mask for percentile band normalisation** (elanspi/elan "thirds"), and try plain min/max linear as the control | `elan.c:164-197` (vendor-recommended), `elanspi.c:1279-1325` | low | ~40 lines, in-tree, LGPL. Judge on separation, not minutiae count — the count metric is what produced the current sharpening |
| 4 | **Add a noise ceiling to the frame gate**, and an explicit "unknown" verdict with a debounce | `vfs7552.c:32-34,595`; `elanspi.c:1199-1251` | low | Our gate is one-sided. A noisy frame that clears the contrast floor is currently indistinguishable from a good one |
| 5 | **Implement an NCC matcher in `fpc_bench` and sweep the search radius** on the new dataset | `focaltech_moh.c` `ft9201_ncc()`/`ft9201_match_score()`, LGPL-2.1-or-later; independently `synaspi/matcher.py` | medium | **The highest-value experiment available.** ~120 lines of C, no dependencies, measurable offline before any driver change. Two independent projects converged on it; one has a sweep showing radius is the dominant parameter |
| 6 | **If (5) separates, ship it as an `FpDevice` with `FPI_PRINT_RAW` templates** | `focaltech_moh.c:540-551`; the pattern [#869][is869] asks about | medium | Works on released libfprint with no upstream patch. Cost: the template becomes fingerprint images on disk — say so in the README before doing it |
| 7 | **Raise enrolment stages** (to 10-15) once (5) exists to measure it | `focaltech_moh.h:44` (5→15 worth 7.4%→0.07% EER); `fpcmoc` asks 25 | low | Cheap but **contested**: `synaspi` measured more views making separation *worse*. Measure, do not assume |
| 8 | **SIGFM via [!530][mr530]** as a second matcher to compare against NCC | LGPL-2.1-or-later; `sigfm.cpp` | medium-high | Real option, and the only non-NBIS matcher with any libfprint integration written. But OpenCV, and four years unmerged — building on it means building on an MR |
| 9 | **Frame accumulation / mosaicking** | `fingerprint-ocv` `cvext.cpp` `merge()` proves it is implementable | high | Would have to be written from scratch: AGPL rules out copying, and `fpi_assemble_frames()` blits without averaging and cannot represent a stationary finger (`fpi-assembling.c:116`, `dy` starts at 2) |
| 10 | **Tuning bozorth3** — lower `bz3_threshold`, expose `minminutiae`, better minutiae selection | — | — | **Do not.** Nobody in the ecosystem does it; the two projects that measured NBIS on a smaller sensor both concluded it is an area limit, not a tuning problem ([INFORME-MR572.md][informe]; [!570][mr570]). The one place it might still be worth a patch is quality-ranked selection (`fpi-print.c:116-118`), and that is an upstream cleanup, not a fix for us |

Two things this note does **not** support, despite being tempting:

- **"Other drivers ship threshold 9, so 24 is too high."** Still false, and now
  with more evidence: `focaltech_moh` shipped a threshold that made genuine and
  impostor scores overlap, and it took a 49-capture measurement to notice. A
  threshold nobody measured is not a precedent.
- **"Someone else will solve this."** linux-surface#353 has been open for five
  years with no protocol work by anyone; the mailing list identified the
  small-sensor matching problem for a Microsoft device in 2018 and it was never
  picked up. The matcher work that did happen happened because individual owners
  of individual laptops measured their own sensors.

---

## Sources

Software read directly (clone + read, not summaries):

- **libfprint**, `gitlab.freedesktop.org/libfprint/libfprint`, at
  `3f1e2817ed01660d0b422a0283effc0e5d3e4b80` (2026-08-04), including branch
  `origin/benzea/self-match-check` (`2a9ad74e`)
- **libfprint !570 branch**, `gitlab.freedesktop.org/ssubbotin/libfprint`,
  `fpcmoh-sigfm` at `ba10c93` / `1195dce` — SIGFM library, `fpcmoh` driver,
  `sigfm-bench`
- [`Dgmtnz/ft9201-fingerprint-linux`][ft9201repo] — tuned `focaltech_moh` driver,
  patch series and `upstream/INFORME-MR572.md`
- [`sandbranch/x1-tablet-gen3-fingerprint`][synareadme] — `synaspi`, MIT
- [`vrolife/fingerprint-ocv`][focv] — AGPL-3.0
- [`wl2776/sigfm-eval`][sigfmeval] (GitLab) — SIGFM evaluation scripts
- [`uunicorn/python-validity`][pyval]
- [`jedbillyb/linux-fingerprint-drivers`][lfd] — community patch hub
- `404698-FDU/pro5-android`, `legacy/device-meizu-m86-cm14/libfprint/fpc1150.c`
- [`mincrmatt12/elan-spi-fingerprint`][elanspiro] README at `627f7bb`
- Debian `salsa.debian.org/debian/libfprint` (branch `debian`), Fedora
  `src.fedoraproject.org/rpms/libfprint` (rawhide spec), Arch
  `gitlab.archlinux.org/archlinux/packaging/packages/libfprint` — all checked for
  matching-related patches; none found

Issue trackers and merge requests (descriptions read via the GitLab/GitHub REST
APIs):

- libfprint MRs: [!217][mr217] · [!418][mr418] · [!530][mr530] · [!570][mr570] ·
  [!572][mr572] · [!576][mr576] · [!646][mr646]
- libfprint issues: [#271][is271] · [#272][is272] · [#373][is373] ·
  [#376][is376] · [#432][is432] · [#485][is485] · [#867][is867] · [#868][is868] ·
  [#869][is869] · [Unsupported Devices wiki][unsup]
- [linux-surface#353][ls353] (full comment thread, via the GitHub API) ·
  [linux-surface#1380][ls1380]
- fprint mailing list, May 2018: [msg01009][ml1009] · [msg01010][ml1010]

**Not obtained.** libfprint's GitLab comment threads: the web UI is behind an
Anubis proof-of-work challenge and the anonymous REST API returns 401 for
`/issues/:iid/notes` and `/discussions`. That means the *outcome* of several
relevant discussions is unknown to me — in particular why [!418][mr418] and
[!530][mr530] have sat unmerged for four years, what the single comment on
[#432][is432] said, and any maintainer response to [#868][is868] or
[#869][is869]. I have quoted only descriptions, which are readable, and have not
inferred maintainer positions from silence.

[mr217]: https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/217
[mr418]: https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/418
[mr530]: https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/530
[mr570]: https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/570
[mr572]: https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/572
[mr576]: https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/576
[mr646]: https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/646
[is271]: https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/271
[is272]: https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/272
[is373]: https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/373
[is376]: https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/376
[is432]: https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/432
[is485]: https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/485
[is867]: https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/867
[is868]: https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/868
[is869]: https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/869
[unsup]: https://gitlab.freedesktop.org/libfprint/wiki/-/wikis/Unsupported-Devices
[ls353]: https://github.com/linux-surface/linux-surface/issues/353
[ls1380]: https://github.com/linux-surface/linux-surface/issues/1380
[qzed353]: https://github.com/linux-surface/linux-surface/issues/353
[ml1009]: https://www.mail-archive.com/fprint@lists.freedesktop.org/msg01009.html
[ml1010]: https://www.mail-archive.com/fprint@lists.freedesktop.org/msg01010.html
[sigfmrepo]: https://github.com/goodix-fp-linux-dev/sigfm
[sigfmeval]: https://gitlab.com/wl2776/sigfm-eval
[ft9201repo]: https://github.com/Dgmtnz/ft9201-fingerprint-linux
[informe]: https://github.com/Dgmtnz/ft9201-fingerprint-linux/blob/main/upstream/INFORME-MR572.md
[synareadme]: https://github.com/sandbranch/x1-tablet-gen3-fingerprint
[focv]: https://github.com/vrolife/fingerprint-ocv
[pyval]: https://github.com/uunicorn/python-validity
[lfd]: https://github.com/jedbillyb/linux-fingerprint-drivers
[lfd55b4]: https://github.com/jedbillyb/linux-fingerprint-drivers/blob/main/devices/27c6:55b4/patches/0006-sigfm-tighten-matching-thresholds-to-reduce-false-po.patch
[elanspiro]: https://github.com/mincrmatt12/elan-spi-fingerprint/blob/627f7bb0b21574c61b7068821001fe1e3fa8bb39/README.md
[openafis]: https://github.com/neilharan/openafis
[fjfx]: https://github.com/FingerJetFXOSE/FingerJetFXOSE
