# Small-area fingerprint matching: is NBIS the wrong tool for an 8x8mm sensor?

Primary-source research note, 2026-08-29. Context and measurements:
[`../../libfprint-driver/README.md`](../../libfprint-driver/README.md).

## Summary

Minutiae matching is the wrong tool at this sensor size, but that is only half
the answer — the measured d' of 0.26 is *also* far worse than published results
at comparable sensor areas, so it is not purely a hardware ceiling.

Three primary measurements bracket the problem. NIST measured commercial
matchers on center-cropped 500 ppi images and concluded that "image sizes below
320 pixels by 320 pixels should not be used"; at 180x180 px (9.1x9.1 mm, close
to this sensor's 8x8 mm) the best commercial matcher still reached a true-accept
rate of 0.762 at FAR 0.001 ([NISTIR 7201][7201], Table 1). FVC2006's DB1 was a
96x96 px, 250 dpi electric-field sensor — a 9.8x9.8 mm window, slightly *larger*
than ours — and the best of the 42 algorithms scored in the Open category
reached 5.564% EER there
against 0.021% on the full-size optical DB2 ([FVC2006 databases][fvcdb],
[Open DB1][fvcdb1], [Open DB2][fvcdb2]). And FPC's own biometric module, using
FPC's own algorithm on an FPC1020 (192x192 at 508 dpi, 9.6x9.6 mm), specifies
FRR 3% at FAR 0.002% ([FPC-BM Product Specification rev C, Table 5][fpcbm]).

Under an equal-variance Gaussian assumption those correspond to d' of roughly
3.8, 3.2 and 6.0 respectively (my arithmetic, not the sources'). This pipeline
measures 0.26. So the published record says a ~8-10 mm window with a competent
algorithm gets a d' somewhere between 3 and 6; it does not say 0.26. A ~65 mm²
window is a genuinely hard, roughly 100x-EER-penalty regime, and NBIS is the
wrong matcher for it — but the gap between 0.26 and 3.2 is too large to be
explained by sensor area alone, and part of it belongs to the pipeline.

The specific things the sources say are wrong with the current approach:
bozorth3's public NIST documentation describes the 10-minutia floor as an
adjustable default (`-A minminutiae=#`) that libfprint has frozen into a
`#define`; MINDTCT is documented as tuned for 500 ppi *full-size* images and a
finger is stated to "typically" have 40-80 minutiae, where this sensor yields 8;
FPC's own product requires eight finger placements per enrolment and matches
against a "multi-template"; Apple states outright that Touch ID discards
"finger minutiae data" and uses ridge-flow angle mapping instead; and every
libfprint driver for a comparably small sensor is a *swipe* driver that
assembles many frames into one larger image, rather than matching a single small
press. This driver is the only press-type image driver in libfprint with a
sub-100 mm² single frame.

The honest ranking of what could be done is in [section 8](#8-conclusion). The
short version: nothing available closes the gap to a safe threshold in one step,
the highest-payoff change is to stop matching a single 8x8mm frame, and the
cheapest experiments (lifting the minutia floor, multi-template score fusion) are
worth doing but will not on their own reach d' = 3.

---

## 1. Sensor area vs. minutiae-based matching in the literature

### NIST's own measurement of the image-size floor

The most directly applicable primary source is NISTIR 7201, *Effect of Image
Size and Compression on One-to-One Fingerprint Matching* (Watson and Wilson,
NIST, 2005) [PDF][7201]. NIST center-cropped 500 ppi live-scan images from
368x368 px down to 180x180 px and ran three commercial SDK matchers over them,
including "the vendor currently used in the US-VISIT system" (p. 3).

The abstract states: "The results of the study show that image cropping quickly
degrade matcher performance. ... The conclusion from this study is that Image
sizes below 320 pixels by 320 pixels should not be used." (p. 2)

TAR at FAR 0.001, DOS-C dataset, as a function of cropped size (Table 1, p. 7):

| image size (px @ 500 ppi) | physical window | right index TAR | left index TAR |
|---|---|---|---|
| 368 | 18.7 x 18.7 mm | 0.986 | 0.969 |
| 320 | 16.3 x 16.3 mm | 0.981 | 0.959 |
| 280 | 14.2 x 14.2 mm | 0.972 | 0.944 |
| 200 | 10.2 x 10.2 mm | 0.839 | 0.764 |
| 180 | 9.1 x 9.1 mm | 0.762 | 0.688 |

NIST's own commentary on the small end: "even with a high quality commercial
fingerprint matcher such as SDK-F small images such as the 180 pixel images
would result in a 0.144 reduction in TAR which would place the fingerprint
matching results far outside the required performance needed for most
applications." (p. 7)

This sensor's 160x160 at 508 dpi is an 8.0x8.0 mm window (64 mm²), below the
smallest size NIST tested and a quarter of the area of the 320 px floor NIST
recommends. **That is the load-bearing finding for "minutiae matching is the
wrong tool here": NIST, evaluating the best commercial minutiae matchers of the
day, says images this small should not be used.**

But note the other half: at 180 px those matchers still delivered TAR 0.762 at
FAR 0.001 — d' ≈ 3.8 under an equal-variance Gaussian assumption (my
conversion). Degraded, unfit for a border-control application, but not
indistinguishable.

### FVC: what the field's best algorithms achieve on a small-area sensor

The Fingerprint Verification Competitions are the standing benchmark. FVC2006
included a database collected on a small-area sensor. From the competition's own
database page [FVC2006 Databases][fvcdb]:

| database | sensor type | image size | resolution | implied window |
|---|---|---|---|---|
| DB1 | Electric Field sensor | 96x96 (9 Kpixels) | 250 dpi | 9.8 x 9.8 mm |
| DB2 | Optical Sensor | 400x560 (224 Kpixels) | 569 dpi | 17.9 x 25.0 mm |
| DB3 | Thermal sweeping Sensor | 400x500 (200 Kpixels) | 500 dpi | 20.3 x 25.4 mm |
| DB4 | SFinGe v3.0 (synthetic) | 288x384 (108 Kpixels) | about 500 dpi | — |

Best EER achieved by any participant, Open category (unconstrained resources;
42 algorithms scored):

| database | best EER | second | source |
|---|---|---|---|
| DB1 (small area) | **5.564%** (P017) | 5.978% | [Open DB1][fvcdb1] |
| DB2 (optical) | 0.021% (P088) | 0.032% | [Open DB2][fvcdb2] |
| DB3 (thermal sweep) | 1.534% (P015) | 1.608% | [Open DB3][fvcdb3] |
| DB4 (synthetic) | 0.269% (P009) | 0.453% | [Open DB4][fvcdb4] |

The same finger population and the same algorithms across all four databases:
the only variable is the sensor. Small area costs a factor of ~265 in EER
against the optical database.

This is not a compute-budget artefact. FVC2006's Light category constrained
enrolment to 0.3 s, matching to 0.1 s, model size to 2 KB and memory to 4 MB
([FVC2006 Categories][fvccat]), and the best of the 24 Light-category algorithms
scored 5.356% EER on DB1 ([Light DB1][fvcldb1]) — *marginally better* than the unconstrained
category. The barrier at 96x96 is information, not cycles.

FVC2006's data collection is also notable for being *unforced*: "Data collection
in FVC2006 was performed without deliberately introducing difficulties ... but
the population is more heterogeneous and also includes manual workers and
elderly people" ([FVC2006 Databases][fvcdb]). FVC2004 by contrast deliberately
perturbed its data; its DB3 (thermal sweep, 300x480 @ 512 dpi) and DB1 (optical,
640x480 @ 500 dpi) are both far larger windows than ours ([FVC2004
Databases][fvc2004db]).

Converting DB1's 5.564% EER gives d' ≈ 3.18 (my conversion). So: at a window
*slightly larger* than this sensor's, the best algorithm in the world in 2006
reached d' ≈ 3.2, and the field regarded that as poor.

### Is d' = 0.26 the expected result?

No. Set against the two sourced anchors above — d' ≈ 3.2 at 9.8x9.8 mm from
FVC2006 DB1, d' ≈ 3.8 at 9.1x9.1 mm from NISTIR 7201 — a d' of 0.26 at 8.0x8.0
mm is an order of magnitude worse than the published record at comparable area.
Sensor area is a real and severe handicap; it is not a sufficient explanation.
Sections 2 and 7 identify where the rest of the gap plausibly lives.

Caveat on the arithmetic: converting EER and TAR/FAR pairs to d' assumes
equal-variance Gaussian genuine and impostor score distributions. None of the
sources make that assumption or report d'. The conversions are mine and are
included only to put figures on one scale; the sourced quantities are the EERs
and TARs themselves.

### ISO/IEC 19794-2 and 19794-4

I could not obtain these. Both are paywalled ([19794-4:2011][iso19794-4],
[19794-2:2011][iso19794-2]) and ISO's public abstracts describe only the record
formats — sampling rate, bit depth, impression codes for Part 4; minutiae record
layouts for Part 2 — without a quotable statement about minimum capture area or
minimum minutia counts. **I am recording this as a gap rather than paraphrasing
the abstracts as if they contained requirements they may not contain.** NISTIR
7201 and the FVC results above cover the same ground with numbers I can cite.

## 2. What NBIS itself says about its operating envelope

Sources: NISTIR 7392, *User's Guide to NIST Biometric Image Software (NBIS)*
(Watson, Garris, Tabassi, Wilson, McCabe, Janet, Ko; NIST, 2007) [PDF][7392];
the `bozorth3(1E)` man page from the NBIS distribution (dated 2004-09-24, NIST,
Stanley A. Janet) [as distributed][bz3man]; and the NBIS sources libfprint
vendors, at `libfprint/nbis/` in the checkout.

### What MINDTCT is designed for

NISTIR 7392 §5.2 (p. 48): "It should be noted that the algorithms and software
parameters have been designed and set to optimally process images scanned at
19.69 pixels per millimeter (ppmm) (500 pixels per inch) and quantized to 256
levels of gray."

That statement is about *resolution*, not size, and this sensor satisfies it —
508 dpi, 8 bpp. libfprint hardcodes the same default: `DEFAULT_PPI 500` at
`libfprint/nbis/include/lfs.h:278`.

On expected minutia counts, §3.2.1 (p. 10): "Typically, there are on the order of
100 minutiae on a tenprint." The `bozorth3(1E)` man page is more specific: "A
finger typically has 40-80 minutiae."

This driver's raw frames yield 7-11 minutiae, and 0 on some placements. The
sharpened-and-upscaled frames yield 13-148, but the README's own measurements
show that count is dominated by interpolation artefacts that do not correspond
between two views of the same finger. Either way the sensor is an order of
magnitude below what the tool's documentation describes as normal.

### Where MIN_COMPUTABLE_BOZORTH_MINUTIAE comes from

NISTIR 7392 does **not** document bozorth3's algorithm at all. §4.2 (p. 17):
"BOZORTH3 source code is subject to U.S. export laws. This package is only
available on CD-ROM upon request. The BOZORTH3 package detail information and
the algorithmic description is provided on the CD-ROM." The public NBIS user's
guide covers PCASYS, MINDTCT and NFIQ only (§5, Table of Contents). **There is
no published NIST algorithmic description of bozorth3 that I could obtain.**

The `bozorth3(1E)` man page, which ships with the distribution, does describe the
floor, and describes it as a *default*:

> "To compute a match score between two fingerprints, both sets must have at
> least a minimum number of minutiae. That number is 10 by default, and can be
> changed to any non-zero integer. Otherwise the computation returns a match
> score of 0."

and documents the option that changes it:

> `-A minminutiae=#`
> Set minimum number of minutiae required for the match score to be more than 0 [10].

The man page also documents the upper bound as tunable — `-n max-minutiae`,
default 150, legal range [0,200] — and warns that "Using more than is necessary
typically reduces the accuracy of the matcher and increases its run time", which
is exactly the failure mode the README observed when upscaling past 2x inflated
the count.

**libfprint removed the tunability.** `libfprint/nbis/update-from-nbis.sh:83-87`
deletes NIST's command-line-option globals from `bozorth.h` and converts them to
compile-time constants:

```sh
for i in m1_xyt max_minutiae min_computable_minutiae verbose_bozorth verbose_main verbose_load; do
	sed -i "/$i/d" include/bozorth.h
done
rename_variable max_minutiae DEFAULT_BOZORTH_MINUTIAE
rename_variable m1_xyt 0
rename_variable min_computable_minutiae MIN_COMPUTABLE_BOZORTH_MINUTIAE
```

which is why `libfprint/nbis/include/bozorth.h:120-125` reads:

```c
#define DEFAULT_BOZORTH_MINUTIAE	150
#define MAX_BOZORTH_MINUTIAE		200
#define MIN_BOZORTH_MINUTIAE		0
#define MIN_COMPUTABLE_BOZORTH_MINUTIAE	10
#define ZERO_MATCH_SCORE		0
```

and `libfprint/nbis/bozorth3/bozorth3.c:642-671` returns `ZERO_MATCH_SCORE`
whenever either side is under it. libfprint exposes no way to change it:
`fpi_print_bz3_match()` (`libfprint/fpi-print.c:230-268`) calls
`bozorth_probe_init()` / `bozorth_to_gallery()` with no parameter for the floor.

So the answer to "where does it come from" is: it is NIST's *default* value for
a documented command-line option, which libfprint froze into an immutable
`#define` when vendoring the code. Nothing in the NIST documentation presents 10
as a correctness limit.

### Is bozorth3 documented as unsuitable below some size?

Not in anything I could obtain. NISTIR 7392 does not document bozorth3 at all,
and the man page discusses minutia counts, not image dimensions. The nearest
NIST statement about size is NISTIR 7201's 320x320 floor (§1 above), which was
measured on *commercial* matchers, not on bozorth3.

Two further things libfprint's vendoring drops that are worth noting: `m1_xyt`
is renamed to the constant `0`, so libfprint never uses the ANSI INCITS 378-2004
minutiae representation that `-m1` selects, and the verbose diagnostics that
would print "too few minutiae" are compiled out (`if ( 0 )` guards at
`libfprint/nbis/bozorth3/bozorth3.c:644-670`) — which is why the short-circuit
was invisible until this project measured it.

## 3. The right matcher class for small-area sensors

### Why minutiae fail at small area, stated explicitly

Jain, Ross and Prabhakar, "Fingerprint Matching Using Minutiae and Texture
Features", *Proc. ICIP 2001*, pp. 282-285 [author's copy][ross01], abstract:

> "The advent of solid-state fingerprint sensors presents a fresh challenge to
> traditional fingerprint matching algorithms. These sensors provide a small
> contact area (≈ 0.6" × 0.6") for the fingertip and, therefore, sense only a
> limited portion of the fingerprint. Thus multiple impressions of the same
> fingerprint may have only a small region of overlap. Minutiae-based matching
> algorithms, which consider ridge activity only in the vicinity of minutiae
> points, are not likely to perform well on these images due to the insufficient
> number of corresponding points in the input and template images."

Two mechanisms, both named: **too few minutiae**, and **too little overlap
between two partial views**. §1 quantifies both:

> "The solid-state sensors provide only a small contact area (≈ 0.6" × 0.6") for
> the fingertip and, therefore, sample only a limited portion of the fingerprint
> pattern (300 × 300 pixels at 500 dpi). An optical sensor, on the other hand,
> has a contact area of 1" × 1", resulting in images of size 480 × 508 pixels at
> 500 dpi. Hence, the number of minutiae points that can be extracted from a
> fingerprint sample acquired using a solid-state sensor is smaller compared to
> that acquired using an optical sensor"

Figure 1's caption gives the counts: 17 and 21 minutiae on the solid-state
images, 39 on the optical one.

**The calibration matters.** The sensor this 2001 paper calls small — the one
whose minutia shortage motivated an entire alternative feature class — is
15.2 x 15.2 mm, 232 mm². This project's sensor is 8.0 x 8.0 mm, 64 mm²: about
28% of that area, yielding 8 minutiae where the paper's "problem" sensor yielded
17-21.

### Texture / ridge-feature matching (Gabor filterbanks, FingerCode)

The same paper's answer is a Gabor filterbank descriptor. §3.3: the image is
tessellated into cells, each filtered by eight Gabor filters at a fixed
frequency chosen from the ridge spacing ("This frequency is chosen based on the
average inter-ridge distance in the fingerprints (which is ∼ 10 pixels)"), and
the absolute average deviation per filtered cell becomes a feature, giving a
648-dimensional vector. Matching is a sum of squared differences over valid
cells, fused with the minutiae score by the sum rule.

Measured result, §4 on a 160-user, 2560-image Veridicom database: "at a 1% FAR,
the hybrid matcher gives a Genuine Accept Rate of 92% while the minutiae-based
matcher gives a Genuine Accept Rate of 72%."

Note the ridge-spacing figure — ~10 px at 500 dpi — is exactly what this
project's README measured on real FPC1021 captures. The scale assumption
underlying FingerCode holds for this sensor.

The underlying descriptor is Jain, Prabhakar, Hong and Pankanti,
"Filterbank-Based Fingerprint Matching", *IEEE Trans. Image Processing* 9(5),
pp. 846-859, May 2000 (cited as [5] in the above). I did not obtain that paper
directly; the description above is from the ICIP paper that uses it.

### Correlation / phase-based matching

Ito, Nakajima, Kobayashi, Aoki and Higuchi, "A Fingerprint Matching Algorithm
Using Phase-Only Correlation", *IEICE Trans. Fundamentals* E87-A(3),
pp. 682-691, 2004, is the seminal reference for phase-based fingerprint
matching, and the band-limited variant (BLPOC) is the standard refinement. **I
was not able to obtain either paper's full text from a primary host** — the
copies I found are on aggregator sites. I therefore record the approach as
existing and relevant, with the citation, but make no quantitative claim about
its performance.

Correlation-based matching is attractive here in principle for a reason
independent of any paper: it uses the whole image rather than a sparse point
set, so it does not care that only 8 minutiae exist. Whether it survives the
elastic distortion of a pressed finger at this scale is exactly what the
literature would have to answer, and I could not source it.

### Level-3 features (pores, ridge contours)

Jain, Chen and Demirkus, "Pores and Ridges: High-Resolution Fingerprint Matching
Using Level 3 Features", *IEEE TPAMI* 29(1), 2007, is the standard reference.
It is not applicable here: level-3 features require ~1000 ppi imagery to resolve
pores, and this sensor delivers 508 dpi. I note it to close the option rather
than to recommend it, and I did not obtain the paper.

### Multi-frame / mosaicking / template fusion

This is the best-documented answer and the one with numbers.

Ross, Shah and Jain, "Image versus feature mosaicing: A case study in
fingerprints", *Proc. SPIE Biometric Technology for Human Identification III*,
pp. 620208-1 - 620208-12, April 2006 [author's copy][mosaic06]. §1:

> "[solid-state sensors provide] information (e.g., fewer minutiae points)
> compared to rolled fingerprints ... Thus, multiple impressions of the same
> finger may have only a small region of overlap, thereby degrading the matching
> performance"

The paper compares four consolidation strategies on FVC2002 DB1 with a
minutiae-based COTS matcher. From the ROC legend (Fig. 8) and Table 1:

| scheme | EER | GAR @ FAR 1% | GAR @ FAR 0.1% |
|---|---|---|---|
| single impression I1 vs probe | 5.12% | — | — |
| single impression I2 vs probe | 2.09% | 94.6% | 94.4% |
| image mosaicing + sum rule | 1.24% | 98.6% | 98.0% |
| feature (minutiae-set) mosaicing + sum rule | 0.53% | 99.6% | 98.8% |
| **score-level sum rule over both impressions** | **0.22%** | — | — |

The paper's own conclusion on that last row: "The ROC also indicates that fusion
at the match score level results in the best performance. However, unlike
mosaicing, score level fusion requires storing multiple templates per user in the
database thereby increasing storage requirements and the time needed for
matching."

Two things follow for this project. First, consolidating multiple partial views
is worth roughly an order of magnitude in EER on a *minutiae* matcher — no
change of feature class required. Second, the cheapest form of it (keep several
templates, fuse the scores) beat the expensive forms (stitch the images or the
minutiae sets). libfprint already stores multiple templates per finger
(§7) but combines them with a max-and-threshold, not a fusion rule.

The earlier and more frequently cited work is Jain and Ross, "Fingerprint
Mosaicking", *Proc. ICASSP 2002* — I did not obtain its full text and rely on
the 2006 paper above, which is by overlapping authors and reports the numbers.

A 2026 technical report, Ye et al., "Toward the Whole Picture: Accumulative
Fingerprint Mapping and Reconstruction for Small-Area Mobile Sensors",
[arXiv:2606.15574][accum], targets exactly this problem — building a unified
fingerprint state progressively from a sequence of small-area touches — but
**reports no quantitative results**: it states that "the present technical report
emphasizes formulation and system design more than exhaustive empirical
validation" and that "a complete set of quantitative experiments remains future
work". It is evidence that the problem is live and that accumulation is the
consensus direction; it is not evidence that any particular gain is achievable.

### Learned fixed-length representations

The modern alternative is a CNN producing a fixed-length embedding rather than a
variable-length minutiae set: Engelsma, Cao and Jain, "Learning a Fixed-Length
Fingerprint Representation" ([arXiv:1909.09901][deepprint]); and for the
partial-area case specifically, work on localized/dense descriptors
([arXiv:2311.18576][fdd]). These are the direction the field has moved, and they
address partial overlap directly. They also require a trained model and a
training corpus, which puts them outside what a libfprint driver can carry — see
§8.

## 4. What FPC's own documentation says about the FPC1021

### The sensor specification says nothing about matching

FPC's *Product Specification FPC1020* rev PB3, dated 2014-04-28, document number
`710-FPC1020_A_Product Specification` [publicly hosted PDF][fpc1020ps] (the
document carries FPC's "CONFIDENTIAL" marking; it is cited here as found
publicly hosted by a distributor). Its Overview (p. 4) lists:

> - Fingerprint area sensor
> - Superior 3D image quality
> - 508 dpi resolution
> - 192 x 192 pixels with 8 bit depth
> - High-speed SPI interface

and Table 1 (p. 4) repeats "Size sensing array 192*192 Pixel", "Pixel resolution
256 gray scale levels 8 Bit", "Resolution 508 DPI".

This confirms the 508 dpi figure this project assumed and the family's 8-bit
depth. It is a pure silicon datasheet: **the words "algorithm", "match",
"template", "enrol" and "minutia" do not appear anywhere in the document.** The
only occurrence of anything adjacent is a note that internal test functionality
exists "to verify the image quality and establish a proper signal-to-noise ratio
(SNR)" (p. 23). The sensor is specified as an imager and nothing more.

I could not find an FPC1021-specific datasheet. FPC's current product page for
the FPC1021 [fpc.com][fpc1021page] no longer carries specifications; the only
FPC1021 mention on it is a case study describing an "FPC1021 sensor embedded in
lock + licensed biometric matching" — which is itself the answer to whether FPC
treats matching as a separate, licensed product.

### FPC's own driver does not touch the image

`https://github.com/fingerprint-cards/capacitive_device_driver`, single commit
`1e23d52` (2020-06-30). The whole repository is 4,931 lines of platform glue
across seven SoC targets. Every variant's file header says the same thing —
from `qcom/spi_driver/fpc1020_tee.c:1-23`:

> "This driver will control the platform resources that the FPC fingerprint
> sensor needs to operate. The major things are probing the sensor to check that
> it is actually connected ... enabling and disabling of regulators, enabling and
> disabling of platform clocks, controlling GPIOs such as SPI chip select, sensor
> reset line, sensor IRQ line, MISO and MOSI lines.
> ...
> **This driver will NOT send any SPI commands to the sensor it only controls the
> electrical parts.**"

The sysfs interface it exposes is exactly that: `clk_enable`, `pinctl_set`,
`regulator_enable`, `hw_reset`, `device_prepare`, `wakeup_enable`,
`handle_wakelock`, `irq`. Grepping the entire repository for `image`, `template`,
`minutia`, `enroll` or `algorithm` returns only `of_match_table` matches. The
comment at `qcom/platform_driver/fpc1020_platform_tee.c:350` names where the
real work happens: "sensor driver eg. the TEE driver needs to do a _SOFT_ reset".

**So the vendor's published Linux code does not read the image, does not process
it, and does not match. Acquisition and matching live in a proprietary
trusted-execution-environment blob that is not published.** That is stated
plainly here because it closes off "read the vendor driver to see what it does
with the image" as an avenue: there is nothing there to read.

### FPC's algorithm, as specified in FPC's own product

FPC's *Product Specification FPC-BM* rev C, dated 2017-03-14, document number
`710-FPC-BM` [publicly hosted PDF][fpcbm], specifies a biometric module built
around FPC's own sensors and algorithm. This is FPC documenting its own matching
product, and it answers the sub-questions directly.

**FPC ships its own matcher.** The module "can be used in conjunction with either
an FPC1020AM touch fingerprint sensor, or an FPC1011F3 area fingerprint sensor"
(p. 4), performs enrolment and verification on-board, and exposes an algorithm
configuration API — `API_SECURITY_LEVEL_RAM` / `_STATIC` selecting "high
convenience / standard / high security", and `API_SET_DYNAMIC_UPDATE` toggling
template update (Table 25, p. 18).

**Multi-template enrolment is FPC's stated design, not an add-on.** The feature
list (p. 4) reads:

> - One-to-one verification mode
>   - Matching against 1 **multi-template**
> - Identify (Few) verification mode
>   - 50 multi-templates
> - On-board template storage
>   - Max. 50 multi-templates

and Table 31 (p. 21) gives the enrolment cost per sensor:

| Sensor | Number of finger placements during enroll |
|---|---|
| FPC1011 (area sensor) | 1 |
| **FPC1020 (touch sensor)** | **8** |

One placement suffices for the large area sensor; the 9.6x9.6 mm touch sensor
needs eight. FPC also caps template updates: "Each multi-template ... can be
updated a maximum of 20 times (per enrolled finger)" (p. 8), i.e. the template
keeps absorbing new views after enrolment.

**FPC's stated accuracy with its own algorithm** (Table 5, p. 8, one-to-one
verification, 1 user):

| security level | FRR | FAR |
|---|---|---|
| High Convenience (default) | 2.5% | 0.01% |
| Normal | 3% | 0.002% |
| High Security | 4% | 0.001% |

The "Normal" row corresponds to d' ≈ 6.0 (my conversion, equal-variance Gaussian
assumption). That is what a 9.6x9.6 mm FPC sensor achieves with FPC's own
algorithm and an eight-placement multi-template.

**The matcher is proprietary and unavailable.** It ships as firmware on the
FPC-BM module and as TEE code on phone platforms. Nothing in FPC's published
material describes the algorithm, and nothing in FPC's published source contains
it. There is no path to obtaining it for a Linux driver.

## 5. What WinBio "Basic" mode implies

Sources are Microsoft's own documentation on learn.microsoft.com.

**What "Basic" means.** From the `WINBIO_SENSOR_MODE` constants
([winbio-sensor-mode-constants][wbmode]):

> **WINBIO_SENSOR_BASIC_MODE** — "Operate the sensor in basic mode. The sensor
> acts only as a capture device."
>
> **WINBIO_SENSOR_ADVANCED_MODE** — "Operate the sensor in advanced mode. The
> sensor can capture samples and perform matching and storage functions."

So Basic mode says the *sensor* does not match. It does not say Windows does the
matching with a Microsoft algorithm.

**Which host component matches.** The Windows Biometric Framework splits the
biometric unit into three adapter plug-ins. From the Biometric Framework
overview ([biometric-framework-overview][wbfoverview]): "You can also use this
API to extend the framework and create biometric sensor adapters, matching
engines, and storage components." The engine adapter's own documentation
([engine-adapter-functions][wbengine]) states it "generates biometric templates
from captured samples, matches samples to existing templates, and indexes
templates", and that "The following functions must be implemented by the adapter
developer."

**Who supplies each adapter.** The decisive text is Microsoft's own sample INF,
in *Installing a Biometric Driver* ([installing-a-biometric-driver][wbinstall]),
which is the canonical registration for a WBDI fingerprint driver:

```inf
[DriverPlugInAddReg]
HKR,WinBio\Configurations,DefaultConfiguration,,"0"
HKR,WinBio\Configurations\0,SensorMode,0x10001,1                                ; Basic - 1, Advanced - 2
HKR,WinBio\Configurations\0,SystemSensor,0x10001,1                              ; UAC/Winlogon - 1
HKR,WinBio\Configurations\0,SensorAdapterBinary,,"WinBioSensorAdapter.DLL"      ; Windows built-in WBDI sensor adapter.
HKR,WinBio\Configurations\0,EngineAdapterBinary,,"EngineAdapter.DLL"            ; Vendor engine
HKR,WinBio\Configurations\0,StorageAdapterBinary,,"WinBioStorageAdapter.DLL"    ; Windows built-in storage adapter
```

Microsoft's own comments label the split: the sensor adapter and storage adapter
are "Windows built-in", and the engine adapter — the component that extracts
features and matches — is the "**Vendor engine**". This is the sample for a
`SensorMode` of 1, i.e. Basic. Microsoft's page also notes "The sample uses the
Microsoft-provided sensor adapter and storage adapter", pointedly not the engine.

**What this means for this project.** "Basic sensor mode" tells us that the
FPC1021 in this Type Cover does no on-chip matching and that a Linux driver
therefore only has to reproduce image capture — which is what
[`PROTOCOL.md`](../../PROTOCOL.md) records, and which is correct. It does *not*
mean Windows matched these images with a stock Microsoft algorithm that a Linux
stack could reproduce. In this architecture the matching was done by a
vendor-supplied engine adapter DLL. Combined with §4 — FPC's matcher is
proprietary, licensed separately, and never published in source — the conclusion
is that **the Windows driver for this exact Type Cover depended on a proprietary
matcher we cannot obtain, and Linux is not failing to reproduce something that
was ever available to reproduce.**

I did not verify the Surface Type Cover's own INF registry values; the claim that
this device runs in Basic mode is taken as established from
[`PROTOCOL.md`](../../PROTOCOL.md).

## 6. What non-libfprint stacks do for small-area sensors

### Android: the bar, stated numerically

AOSP's *Measure biometric unlock security* ([source.android.com][androidmeasure])
sets the thresholds a fingerprint implementation must meet to be a Class 3
("Strong") biometric — the only class that may gate KeyStore keys for third-party
apps:

| class | SAR | FAR | FRR |
|---|---|---|---|
| Class 3 (Strong) | 0-7% | 1/50k | 10% |
| Class 2 (Weak) | 7-20% | 1/50k | 10% |
| Class 1 (Convenience) | 20-30% | 1/50k | 10% |

The FAR requirement is identical across all three classes: **1 in 50,000, at an
FRR of 10%.** That operating point corresponds to d' ≈ 5.4 (my conversion). Every
fingerprint sensor shipped in an Android phone — most of them smaller than 8x8 mm
— is required to hit it. The AOSP page does not describe the algorithm used, and
Android's fingerprint HAL deliberately leaves feature extraction and matching to
the vendor implementation, so this tells us the bar and not the method.

### Apple: explicitly not minutiae, and explicitly multi-view

Apple's *Platform Security* guide (August 2026 edition, [PDF][appleps]) is
unusually specific for a vendor document, and it says two things that bear
directly on this project.

Touch ID does not use minutiae (p. 20):

> "While the fingerprint scan is being vectorized for analysis, the raster scan
> is temporarily stored in encrypted memory within the Secure Enclave and then
> it's discarded. The analysis uses **subdermal ridge flow angle mapping**, a
> lossy process that **discards "finger minutiae data"** that would be required
> to reconstruct the user's actual fingerprint. During enrollment, the resulting
> map of nodes is stored in an encrypted format..."

And the template accumulates views over time (p. 20):

> "This technology reads fingerprint data from any angle and learns more about a
> user's fingerprint over time, with the sensor continuing to **expand the
> fingerprint map as additional overlapping nodes are identified with each use**."

Apple's support note *About Touch ID advanced security technology*
([support.apple.com/en-us/105095][apple105095]) repeats the incremental update
in plainer terms: "Touch ID will incrementally update the mathematical
representation of enrolled fingerprints over time to improve matching accuracy",
and describes the capture as taking "a high-resolution image from small sections
of your fingerprint from the subepidermal layers of your skin."

Apple's stated false-match figure (Platform Security, p. 23): "For a user's iPad,
iPhone, Mac models with Touch ID, and those paired with a Magic Keyboard with
Touch ID, it's less than 1 in 50,000. This probability increases with multiple
enrolled fingerprints (up to 1 in 10,000 with five fingerprints)."

**Apple's published position is therefore: a ridge-flow feature class rather than
minutiae, a template built from many overlapping partial views, and a template
that keeps growing after enrolment.** Apple does not publish the sensor's pixel
dimensions or ppi in the current guide; earlier claims of "500 ppi, 88x88" are
not in the document I read and I do not repeat them.

### SourceAFIS: the same feature class, but without the floor

SourceAFIS is the main open-source alternative to NBIS. Its algorithm page
([sourceafis.machinezoo.com/algorithm][safisalgo]) states that its "high-level
abstractions are *minutiae*, or ridge endings and bifurcations", plus an *edge*
abstraction connecting minutia pairs with rotation- and translation-invariant
length and angle properties. That is the same feature class as bozorth3 and the
same pairwise-edge matching idea; **it is not a different answer to the
small-area problem.**

It does differ from libfprint's frozen NBIS in three ways that matter here, read
from `robertvazan/sourceafis-java`:

- **No minimum-minutiae short circuit.** `Parameters.java` has `MAX_MINUTIAE = 100`
  but no minimum; nothing in the matcher returns early on a sparse template.
- **Calibrated thresholds.** `Parameters.java:69-76` publishes score thresholds
  tied to false-match rates (`THRESHOLD_FMR_100 = 18.22`,
  `THRESHOLD_FMR_1000 = 22.39`, `THRESHOLD_FMR_10_000 = 27.24`), so an operating
  point can be chosen by target FMR rather than by a hand-picked integer.
- **Explicit scale handling.** `FingerprintImageOptions.java:27-38`: "SourceAFIS
  algorithm is not scale-invariant. Fingerprints with incorrectly configured DPI
  may fail to match. Check your fingerprint reader specification for correct DPI
  value. Default DPI is 500."

The project makes no accuracy claim — its front page says only that "Accuracy and
speed of matching are sufficient for most applications" and that it "delivers
decent accuracy" ([sourceafis.machinezoo.com][safis]) — and says nothing about
small images or sensor size. One parameter is worth flagging as a risk rather
than a finding: `MIN_ROOT_EDGE_LENGTH = 58` (`Parameters.java:58`) requires
matching to be seeded from minutia pairs at least 58 units apart, and this
sensor's whole frame is ~157 units wide at 500 dpi. I did not verify how that
interacts with an 8 mm image and I am not claiming it breaks; it is the first
thing to check if SourceAFIS is tried.

### Summary of §6

| stack | feature class | small-area strategy | published bar |
|---|---|---|---|
| Android (CDD) | vendor's choice, unspecified | vendor's choice | FAR 1/50k @ FRR 10% |
| Apple Touch ID | ridge-flow angle map, minutiae explicitly discarded | template expands with overlapping views on every use | <1 in 50,000 |
| FPC (own product) | proprietary | 8 placements into one multi-template, dynamic update | FRR 3% @ FAR 0.002% |
| SourceAFIS | minutiae + edges | none stated | none published |
| libfprint | minutiae (NBIS) | none | none |

Nobody who ships small-area fingerprint authentication at scale does it with a
single small press through a plain minutiae matcher.

## 7. What libfprint does for its own small sensors

Read from the libfprint checkout at
`/home/cw/dev/surface-typecover-fpc1021/.work/libfprint` (git `3a41fa7`). Paths
below are relative to that root.

### There is exactly one image-matching path

`FpiPrintType` has two values (`libfprint/fpi-print.h:12-19`): `FPI_PRINT_RAW`,
"a raw print where the data is directly compared", and `FPI_PRINT_NBIS`, "NBIS
minutiae comparison". Every match-on-chip driver — `synaptics`, `goodixmoc`,
`elanmoc`, `egismoc`, `fpcmoc`, `focaltech_moc`, `mafpmoc`, `realtek` — uses
`FPI_PRINT_RAW` and lets the sensor match. Every `FpImageDevice` driver uses
`FPI_PRINT_NBIS` and goes through `fpi_print_bz3_match()`. **libfprint has no
non-NBIS matching path for images at all**; grepping for `bozorth` outside
`nbis/` returns only `fpi-print.c`, `fpi-image-device.h`, `meson.build` and this
project's own `drivers/fpc1021.c` comments.

### Multiple templates per finger already exist

`fp-image-device-private.h:24` sets `IMG_ENROLL_STAGES 5`, and
`fpi-image-device.c:301-320` calls `fpi_print_add_print()` once per accepted
stage, so an enrolled finger holds five independent minutiae sets.
`fpi_print_bz3_match()` (`fpi-print.c:255-267`) then loops the probe against each
and returns success on the **first** gallery print that clears the threshold:

```c
for (i = 0; i < print_template->prints->len; i++)
  {
    ...
    score = bozorth_to_gallery (probe_len, pstruct, gstruct);
    fp_dbg ("score %d/%d", score, bz3_threshold);
    if (score >= bz3_threshold)
      return FPI_MATCH_SUCCESS;
  }
```

That is a max-over-templates rule, not the score-level *fusion* that §3 found to
be the strongest consolidation scheme. Only `elanspi` raises the stage count
(`elanspi.c:1703`: `dev_class->nr_enroll_stages = 7; /* these sensors are very
hit or miss, may as well record a few extras */`); `upekts` lowers it to 3.

### Per-driver comparison

`bz3_threshold` defaults to `BOZORTH3_DEFAULT_THRESHOLD 40`
(`fp-image-device.c:25`) when a driver leaves it unset. `ppmm` defaults to 0.0
and only two drivers set it. `FPI_IMAGE_PARTIAL` sets
`lfsparms->remove_perimeter_pts` (`fp-image.c:319`).

| driver | scan | frame the sensor gives | assembles frames? | delivered image | upscale | PARTIAL | ppmm | bz3_thr |
|---|---|---|---|---|---|---|---|---|
| `aes4000` | press | 96x96, in 6 strips of 96x16 | strips of one press (`aes3k.c:113-120`) | 288x288 | 3x bilinear (`aes3k.c:124`) | no | 0 | **9** (`aes3k.c:269`) |
| `aes3500` | press | 128x128, in 8 strips of 128x16 | strips of one press | 256x256 | 2x bilinear | no | 0 | **9** |
| `aes2501` | swipe | 192x16 stripes | yes, movement estimation | 288 x variable | no | yes (`aes2501.c:460`) | 0 | 40 (default) |
| `aes1610` | swipe | 128x?? stripes | yes | 192 x variable | no | yes (`aes1610.c:615`) | 0 | 20 |
| `aes2550` | swipe | 192x?? stripes | yes | 288 x variable | no | yes (`aes2550.c:254`) | 0 | 20 |
| `aes1660`/`aes2660` | swipe | 128x?? / 192x?? | yes (`aesx660.c`) | 1.5x frame width | no | yes (`aesx660.c:335`) | 0 | 20 |
| `egis0570` | swipe | 114x57, using 114x17 band | yes (`egis0570.c:209-211`) | 152 x variable, then 2x | 2x bilinear | yes | 0 | **25** |
| `elan` | swipe | sensor-reported width, height capped at 50 (`elan.h:57`) | yes, 7-30 frames (`elan.h:53-54`) | 1.5x frame width x variable | no | yes (`elan.c:335`) | 0 | 24 |
| `elanspi` | swipe | die up to **160x160** (`elanspi.h:46`), cropped to ≤43 rows/frame (`elanspi.h:376`) | yes, 8-21 frames (`elanspi.h:373-374`) | 1.5x frame width x variable, then 2x | 2x bilinear (`elanspi.c:1352`) | yes (`elanspi.c:1354`) | 0 | 24 |
| `vfs0050` | swipe | 100-px lines | yes | 100 x variable (max 3000) | no | no | 0 | 24 |
| `vfs5011` | swipe | 160-px lines | yes (`vfs5011.c:395`) | 160 x variable | no | no | 0 | 20 |
| `vfs101` | swipe | 200-px lines | yes | 200 x variable | no | no | 0 | 24 |
| `vfs301` | swipe | 200-px lines | yes | 200 x variable | no | no | 0 | 24 |
| `upeksonly` | swipe | lines | yes | variable | no | no | 0 | 25 |
| `upektc_img` | swipe | 108x384 … 192x270 by model | yes | as above | no | yes (`upektc_img.c:358`) | 0 | 20 |
| `vfs7552` | press | **112x112** | no | 112x112 | no | no | 0 | 20 |
| `upektc` | press | 208x288 | no | 208x288 | no | no | 0 | 30 |
| `nb1010` | press | 256x180 | no | 256x180 | no | no | 0 | 24 |
| `vcom5s` | press | 300x288 | no | 300x288 | no | no | 0 | 40 (default) |
| `uru4000` | press | 384x290 | no | 384x290 | no | no | 0 | 40 (default) |
| `secugen` | press | 300x400 | no | 300x400 | no | no | **19.685** (`secugen.c:1682`) | 24 |
| **`fpc1021` (this driver)** | **press** | **160x160** | **no** | **320x320** | **2x Catmull-Rom + unsharp** | **no** | **40.0** (`fpc1021.c:549`) | **24** |

Four observations from the table.

**Every small libfprint sensor is a swipe sensor.** Of the 25 `scan_type`
declarations across the image drivers, 17 are `FP_SCAN_TYPE_SWIPE` and 8 are
`FP_SCAN_TYPE_PRESS`; the swipe drivers assemble many frames into one taller image via
`fpi_assemble_frames()` / `fpi_assemble_lines()`. The press drivers with
comparable frame areas — `aes3500` at 128x128 and `aes4000` at 96x96 — are also
assembled, just from strips of a single press rather than from a moving finger.
**`fpc1021` and `vfs7552` are the only image drivers in libfprint that hand
bozorth3 a single, un-assembled, sub-100 mm² frame.** `vfs7552` (112x112, no
upscale, no partial flag, threshold 20) declares no resolution, so I cannot say
what physical area it covers.

**`elanspi` is the closest sibling and it does the opposite thing.** Its sensor
table (`elanspi.h:46`) includes `{0x2, 0xA0, 0xA0, ...  "eFSA160S"}` — a
**160x160** die, exactly this sensor's geometry. `elanspi` does not press-match
it. `elanspi.c:400-410` crops every sensor to at most `ELANSPI_MAX_FRAME_HEIGHT`
= 43 rows per frame, and `elanspi.c:1338-1355` stitches 8-21 such frames from a
swipe into an image 1.5x the frame width and however tall the swipe was, then
upscales 2x and flags it partial. It also enrols 7 stages instead of 5. **The one
libfprint driver with the same silicon dimensions as ours treats it as a swipe
sensor and never asks bozorth3 to match a single 160x160 frame.**

**The thresholds are set by feel and one driver says so.** `egis0570.h:164-170`
is the most candid comment in the tree:

```c
/*
 * This sensor is small so I decided to reduce bz3_threshold from
 * 40 to 10 to have more success to fail ratio
 * Bozorth3 Algorithm seems not fine at the end
 * foreget about security :))
 */

#define EGIS0570_BZ3_THRESHOLD 25 /* and even less What a joke */
```

`aes3k.c:269` sets 9 for its 96x96 and 128x128 sensors with no comment. Neither
number is derived from any measured impostor distribution, and the README's own
impostor measurement is, as far as I can tell, the first such measurement anyone
has published for a libfprint driver. This is worth stating plainly: **the fact
that sibling drivers ship thresholds of 9 and 25 is not evidence that those
thresholds are safe. It is evidence that nobody measured.**

**Upscaling is an acknowledged hack, not a technique.** `aes3k.c:122-124`:

```c
/* FIXME: this is an ugly hack to make the image big enough for NBIS
 * to process reliably */
img = fpi_image_resize (tmp, cls->enlarge_factor, cls->enlarge_factor);
```

Our driver's Catmull-Rom variant is a better-motivated version of the same hack,
and the README's own numbers show it buys real but bounded improvement.

## 8. Conclusion

### (a) Is d' = 0.26 the expected ceiling, or a fixable defect?

**Neither cleanly. It is a hard hardware handicap made much worse by the
pipeline.**

The hardware handicap is real and documented. NIST says images below 320x320 at
500 ppi "should not be used" ([NISTIR 7201][7201], p. 2), and this sensor
delivers 160x160 at 508 dpi — a quarter of that area. The 2001 paper that
introduced texture matching as the answer to small sensors was writing about a
sensor 3.6x the area of this one ([Jain, Ross, Prabhakar][ross01], §1). FVC2006's
best-in-world algorithms lost a factor of 265 in EER moving from a full-size
optical sensor to a 9.8 mm one. Nothing will make an 8x8mm press comparable to a
full print.

But the published record at comparable area does not sit anywhere near 0.26:
FVC2006 DB1's winner is d' ≈ 3.2, NISTIR 7201's 180 px row is d' ≈ 3.8, and FPC's
own algorithm on a 9.6 mm sensor is d' ≈ 6.0 (all my conversions). A gap of that
size is not sensor area. Concrete, sourced candidates for where the rest of it
lives:

1. **Single frame, single view.** Every comparable sensor in every stack examined
   here — libfprint's own `elanspi` on the identical 160x160 die, FPC's own
   FPC1020 module with 8 placements, Apple's expanding node map — consolidates
   many partial views. This driver matches one 8x8mm press against five stored
   presses with a max rule. Ross et al. measured that consolidation is worth
   roughly 10x in EER on a minutiae matcher.
2. **A frozen matcher default.** libfprint hardcodes NIST's *adjustable* default
   minimum of 10 minutiae into an immutable `#define`
   (`nbis/update-from-nbis.sh:83-87`), and the sharpening that this project added
   exists only to climb over that number. Some of the minutiae it manufactures
   are interpolation artefacts — the README already measured that they do not
   correspond between two views of the same finger — so the pipeline is currently
   trading real precision for a floor that NIST never intended to be immovable.
3. **An uncalibrated threshold.** `bz3_threshold` is an integer with no stated
   relationship to any false-match rate anywhere in libfprint. SourceAFIS at least
   publishes thresholds keyed to FMR.

The measurement caveat from the README stands and matters: the impostor side rests
on 28 pairs from two images. That is enough to say "nowhere near safe" and not
enough to rank interventions by measured effect. **Any work below should begin by
enlarging the impostor set in `tools/fpc_bench.c`, because every conclusion after
that depends on it.**

### (b) Concrete alternatives, ranked

Ranked by expected payoff per unit of effort. Every one of these is a hypothesis
with a sourced rationale, not a promised gain.

| # | change | rationale (source) | effort | expected payoff |
|---|---|---|---|---|
| 1 | **Enlarge the impostor dataset and re-measure everything** | current d' rests on 2 impostor images; §8(a) | low | none directly — but nothing else is decidable without it |
| 2 | **Score-level fusion over the 5 enrolled templates instead of max** | best-performing consolidation in [Ross et al. 2006][mosaic06], Table 1 / Fig. 8 (EER 2.09% → 0.22%) | medium (needs an upstream libfprint change; `fpi_print_bz3_match` returns on first hit) | plausibly the largest single gain available without leaving NBIS |
| 3 | **Multi-view enrolment: more stages, wider finger placement** | FPC requires 8 placements for its 9.6 mm sensor ([FPC-BM][fpcbm] Table 31); `elanspi` already raises stages to 7 | low (one line + guidance) | improves overlap probability; free to try |
| 4 | **Lift or expose the 10-minutia floor; drop the artefact-generating sharpening** | NIST documents it as `-A minminutiae=#`, default 10, "can be changed to any non-zero integer" ([bozorth3(1E)][bz3man]) | low locally, medium upstream | removes the short circuit *without* manufacturing false minutiae — a cleaner baseline to measure from |
| 5 | **Try SourceAFIS as an alternative matcher** | no minutia floor; FMR-calibrated thresholds; explicit DPI handling (`Parameters.java`, `FingerprintImageOptions.java`) | medium (out-of-tree harness; Java/.NET/Rust ports, not C) | unknown — same feature class, so it fixes the plumbing, not the physics. Check `MIN_ROOT_EDGE_LENGTH` first |
| 6 | **Frame accumulation: capture several frames as the finger settles/rolls and mosaic them** | `elanspi` does exactly this on the same 160x160 die (`elanspi.c:1338-1355`); the sensor already emits an extra unrequested frame per capture (see driver README) | high | the change most likely to actually move d', and the one every comparable product makes |
| 7 | **Texture / Gabor filterbank descriptor fused with minutiae** | GAR 72% → 92% at FAR 1% on a *larger* small sensor ([Jain, Ross, Prabhakar 2001][ross01], §4) | high (new matcher, no libfprint hook) | real, sourced gain — but measured at 15 mm, not 8 mm |
| 8 | **Learned fixed-length embedding** | [DeepPrint][deepprint], [FDD][fdd] | very high | out of scope: needs a model and a training corpus a driver cannot carry |

Not viable, recorded to close them off: level-3 pore features (needs ~1000 ppi,
sensor is 508); obtaining FPC's matcher (proprietary TEE blob, §4); reproducing
what Windows did (vendor engine adapter DLL, §5).

### (c) Does any of this fit libfprint's architecture?

- **#1 and #3 fit today.** The bench is ours; `nr_enroll_stages` is a driver
  field.
- **#2 and #4 require upstream changes.** `fpi_print_bz3_match()` returns on the
  first template that clears the threshold (`fpi-print.c:262-265`) and has no
  fusion mode; `MIN_COMPUTABLE_BOZORTH_MINUTIAE` is a `#define` in vendored NBIS
  and libfprint's re-vendoring script actively strips NIST's runtime control of
  it (`nbis/update-from-nbis.sh:83-87`). Both are small, well-motivated patches
  that would benefit `aes3k`, `egis0570` and `elanspi` too — which is the right
  way to pitch them.
- **#6 fits the architecture well.** `fpi-assembling.h` / `fpi_assemble_frames()`
  is a public internal API, `elanspi` shows the pattern on the same silicon, and
  the sensor's unexplained second frame per capture is a hint that multi-frame
  capture is natural for this hardware. It changes the driver from a press to
  something closer to a swipe, with the user-experience cost that implies.
- **#5 and #7 do not fit at all.** libfprint has exactly one image-matching path
  and it is bozorth3 (§7). Adding a second matcher class is an architectural
  change upstream would have to want, and neither SourceAFIS nor a Gabor
  filterbank has a C implementation libfprint could vendor as it vendored NBIS.

### The honest bottom line

**No available, implementable change closes the gap to a safe threshold, and this
should be stated as a negative result rather than softened.** The sourced targets
are d' ≥ 3 to be comparable with a 2006 small-sensor algorithm and d' ≈ 5.4 to
meet Android's Class 3 bar; the measurement is 0.26. Options #1-#4 are cheap,
principled and worth doing, and might plausibly get this into the low single
digits of d' — the mosaicking literature supports roughly an order of magnitude
in EER from consolidation alone — but none of them is *known* to. Option #6 is
the one every comparable product actually chose, and it is a substantial rewrite
of the capture path. Options #7-#8 are research, not driver work.

The driver README's current position — "capture, the wedge fix, enrollment and
the diagnostics are solid and worth upstreaming; verification is not" — survives
this research pass unchanged, and this note supplies the citations for why.

---

## Sources

Primary sources consulted. Where I could not obtain a source, that is stated in
the relevant section rather than papered over.

**NIST**
- [NISTIR 7201, *Effect of Image Size and Compression on One-to-One Fingerprint Matching*, Watson & Wilson, 2005][7201]
- [NISTIR 7392, *User's Guide to NIST Biometric Image Software (NBIS)*, 2007][7392]
- [`bozorth3(1E)` man page, NIST, 2004-09-24, as distributed with NBIS][bz3man] (fetched from a mirror of the NIST distribution; NIST does not serve the export-controlled BOZORTH3 package from nist.gov)
- [NFIQ 2 documentation, NIST][nfiq2] — states it targets "optical and ink 500 PPI plain impression fingerprints"; no image-size requirement stated

**FVC (Biometric System Laboratory, University of Bologna)**
- [FVC2006 Databases][fvcdb] · [Categories][fvccat] · [Open DB1][fvcdb1] [DB2][fvcdb2] [DB3][fvcdb3] [DB4][fvcdb4] · [Light DB1][fvcldb1]
- [FVC2004 Databases][fvc2004db]

**Fingerprint Cards AB**
- [*Product Specification FPC1020*, rev PB3, 2014-04-28, doc 710-FPC1020_A][fpc1020ps]
- [*Product Specification FPC-BM*, rev C, 2017-03-14, doc 710-FPC-BM][fpcbm]
- [`fingerprint-cards/capacitive_device_driver`][fpcdriver], commit `1e23d52`
- [FPC1021 product page][fpc1021page]

**Microsoft**
- [Biometric Framework overview][wbfoverview] · [Installing a Biometric Driver][wbinstall] · [WINBIO_SENSOR_MODE constants][wbmode] · [Engine adapter functions][wbengine]

**Apple / Google**
- [Apple Platform Security, August 2026][appleps] · [About Touch ID advanced security technology][apple105095]
- [AOSP, Measure biometric unlock security][androidmeasure]

**Papers**
- [Jain, Ross & Prabhakar, "Fingerprint Matching Using Minutiae and Texture Features", ICIP 2001][ross01]
- [Ross, Shah & Jain, "Image versus feature mosaicing: A case study in fingerprints", SPIE 2006][mosaic06]
- [Ye et al., "Toward the Whole Picture: Accumulative Fingerprint Mapping and Reconstruction for Small-Area Mobile Sensors", arXiv:2606.15574][accum] — formulation only, no results
- [Engelsma, Cao & Jain, "Learning a Fixed-Length Fingerprint Representation", arXiv:1909.09901][deepprint]
- [Fixed-length Dense Descriptor for partial fingerprints, arXiv:2311.18576][fdd]
- Ito, Nakajima, Kobayashi, Aoki & Higuchi, "A Fingerprint Matching Algorithm Using Phase-Only Correlation", *IEICE Trans. Fundamentals* E87-A(3), 682-691, 2004 — **not obtained**
- Jain, Prabhakar, Hong & Pankanti, "Filterbank-Based Fingerprint Matching", *IEEE TIP* 9(5), 846-859, 2000 — **not obtained**
- Jain, Chen & Demirkus, "Pores and Ridges: High-Resolution Fingerprint Matching Using Level 3 Features", *IEEE TPAMI* 29(1), 2007 — **not obtained**; ruled out on resolution grounds

**Software read directly**
- libfprint, git `3a41fa7`, at `/home/cw/dev/surface-typecover-fpc1021/.work/libfprint`
- [SourceAFIS for Java][safisrepo] · [algorithm description][safisalgo] · [project page][safis]

**Not obtained**
- ISO/IEC 19794-2:2011 and ISO/IEC 19794-4:2011 — paywalled; public abstracts do not state capture-area or minutia-count requirements

[7201]: https://nvlpubs.nist.gov/nistpubs/Legacy/IR/nistir7201.pdf
[7392]: https://nvlpubs.nist.gov/nistpubs/Legacy/IR/nistir7392.pdf
[bz3man]: https://raw.githubusercontent.com/lessandro/nbis/master/man/man1/bozorth3.1
[nfiq2]: https://pages.nist.gov/NFIQ2/docs/v2.3.0/index.html
[fvcdb]: http://bias.csr.unibo.it/fvc2006/databases.asp
[fvccat]: http://bias.csr.unibo.it/fvc2006/categories.asp
[fvcdb1]: http://bias.csr.unibo.it/fvc2006/results/O_res_db1_a.asp
[fvcdb2]: http://bias.csr.unibo.it/fvc2006/results/O_res_db2_a.asp
[fvcdb3]: http://bias.csr.unibo.it/fvc2006/results/O_res_db3_a.asp
[fvcdb4]: http://bias.csr.unibo.it/fvc2006/results/O_res_db4_a.asp
[fvcldb1]: http://bias.csr.unibo.it/fvc2006/results/l_res_db1_a.asp
[fvc2004db]: http://bias.csr.unibo.it/fvc2004/databases.asp
[fpc1020ps]: http://www.shenzhen2u.com/doc/Module/Fingerprint/710-FPC1020_PB3_Product-Specification.pdf
[fpcbm]: https://site.eet-china.com/arrow/technote/Fingerprints/710-FPC-BM_Product%20Specification_C.PDF
[fpcdriver]: https://github.com/fingerprint-cards/capacitive_device_driver
[fpc1021page]: https://www.fpc.com/technology/hardware/sensors/fpc1021/
[wbfoverview]: https://learn.microsoft.com/en-us/windows/win32/secbiomet/biometric-framework-overview
[wbinstall]: https://learn.microsoft.com/en-us/windows-hardware/drivers/biometric/installing-a-biometric-driver
[wbmode]: https://learn.microsoft.com/en-us/windows/win32/secbiomet/winbio-sensor-mode-constants
[wbengine]: https://learn.microsoft.com/en-us/windows/win32/secbiomet/engine-adapter-functions
[appleps]: https://help.apple.com/pdf/security/en_US/apple-platform-security-guide.pdf
[apple105095]: https://support.apple.com/en-us/105095
[androidmeasure]: https://source.android.com/docs/security/features/biometric/measure
[ross01]: https://www.cse.msu.edu/~rossarun/pubs/RossMinTexture_ICIP01.pdf
[mosaic06]: https://www.cse.msu.edu/~rossarun/pubs/RossMosaicing_SPIE06.pdf
[accum]: https://arxiv.org/abs/2606.15574
[deepprint]: https://arxiv.org/abs/1909.09901
[fdd]: https://arxiv.org/abs/2311.18576
[safis]: https://sourceafis.machinezoo.com/
[safisalgo]: https://sourceafis.machinezoo.com/algorithm
[safisrepo]: https://github.com/robertvazan/sourceafis-java
[iso19794-2]: https://www.iso.org/standard/50864.html
[iso19794-4]: https://www.iso.org/standard/50866.html
