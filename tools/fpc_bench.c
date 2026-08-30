/*
 * Offline matching bench for the FPC1021 driver.
 *
 * Every question about matching so far has cost a full round of
 * fprintd-delete, a six-press enrollment, and several verifications, with a
 * real finger, for one data point. That is a bad way to explore a parameter
 * space. This runs the same pipeline over saved raw captures instead: no
 * device, no finger, no fprintd, and every sample scored against every
 * other in a second.
 *
 * It reproduces exactly what the driver hands libfprint -- enlargement,
 * scan resolution, blank-frame gate -- then builds prints through
 * libfprint's own fpi_print_add_from_image() and scores them with the same
 * bozorth3 entry point fpi_print_bz3_match() uses. The numbers are
 * therefore the numbers fprintd would report, not an approximation.
 *
 * Because those functions are internal (nothing fpi_* is exported from the
 * shared library), this links against a libfprint *build tree*'s static
 * archives. See tools/README.md for the build line.
 *
 * Usage:
 *   fpc_bench [options] <capture.bin> [capture.bin ...]
 *
 *   -e, --enlarge N     enlargement factor (default 3, as the driver ships)
 *   -t, --threshold N   bz3 threshold to report against (default 24)
 *   -g, --gate N        blank-frame tile-contrast gate (default 40, 0 disables)
 *   -s, --sigma F       unsharp sigma (0 with -a 0 disables sharpening)
 *   -a, --amount F      unsharp amount (0 disables sharpening)
 *   -m, --min-minutiae N  bozorth3's computable floor (default NIST's 10)
 *   -q, --reliability N   drop minutiae whose NBIS reliability is under N%
 *                         (0 keeps all, which is what libfprint does)
 *   -S, --subject NAME  label the captures that follow; see below
 *   -w, --width N       capture width (default 160)
 *   -h, --height N      capture height (default 160)
 *
 * Every capture is used once as a probe against all the others as a
 * template, which is the closest offline analogue of "enroll on some
 * presses, verify with another".
 *
 * Separation, not score, is the objective
 * ---------------------------------------
 * Every image parameter in this driver was once chosen by maximising
 * *genuine* scores without measuring impostors, and two of the
 * configurations that looked best that way score impostors higher than
 * genuine matches. Raising all the scores together is not progress.
 *
 * So the bench classifies pairs itself rather than leaving it to whoever
 * reads the matrix -- a hand classification got this wrong once already,
 * by dropping the second finger's own genuine pairs. Label each finger
 * with -S and every pair becomes genuine (same label) or impostor
 * (different label):
 *
 *   fpc_bench -S left-index shot1.bin place1.bin -S right-index other1.bin
 *
 * With two or more labels the bench reports mean, standard deviation and
 * d' for each class, and sweeps the threshold to find the operating point
 * with the best genuine-accept/false-accept trade. Filenames are never
 * parsed for this: shot* and place* are the same finger, and no naming
 * convention could know that.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "fpi-print.h"
#include "fpi-minutiae.h"
#include "fp-print-private.h"
#include "fpi-image.h"
#include "fpi-log.h"

#include "bozorth.h"

/* bozorth3's computable minutia floor.
 *
 * NIST shipped this as a command-line option -- bozorth3(1E) documents
 * "-A minminutiae=#", default 10, "can be changed to any non-zero integer"
 * -- and libfprint's re-vendoring script freezes it into a #define
 * (nbis/update-from-nbis.sh). libfprint-driver/bozorth-floor.patch restores
 * NIST's runtime control of it; with that applied the header defines
 * MIN_COMPUTABLE_BOZORTH_MINUTIAE_DEFAULT and -m works. Without it the
 * bench still builds and everything except -m behaves identically, which
 * keeps it usable against a stock libfprint checkout.
 */
#ifdef MIN_COMPUTABLE_BOZORTH_MINUTIAE_DEFAULT
#define FPC_FLOOR_SETTABLE 1
#else
#define FPC_FLOOR_SETTABLE 0
static gint bozorth_min_computable_minutiae = MIN_COMPUTABLE_BOZORTH_MINUTIAE;
#endif

#define DEFAULT_ENLARGE 3
#define DEFAULT_THRESHOLD 24
#define DEFAULT_GATE 40.0
#define QUALITY_TILE 16

typedef struct {
  char       *name;
  const char *subject;   /* which finger this is, from -S; NULL if unlabelled */
  FpPrint    *print;     /* one print, holding this capture's minutiae */
  guint       minutiae;
  guint       minutiae_raw;   /* before the reliability filter */
  gdouble     reliability;    /* median over the frame's minutiae */
  gdouble     contrast;
  gboolean    gated;     /* rejected by the blank-frame gate */
  guint8     *raw;       /* the frame as captured, before any sharpening */
  gfloat    **hp;        /* high-passed views, one per search angle */
} sample;

/* Unsharp mask, applied to the raw frame before enlargement.
 *
 * The sensor's ridges are soft-edged, and every later stage -- interpolation,
 * NBIS's block analysis -- loses more of that edge. Sharpening first is what
 * lets a verification image carry enough minutiae to be compared at all:
 * measured over ten saved captures (tools/fpc_bench.c), real frames go from
 * 8-11 minutiae to 13-148, so none of them fall under
 * MIN_COMPUTABLE_BOZORTH_MINUTIAE any more, and the share of captures that
 * match another capture of the same finger goes from 2 in 11 to 5 in 11.
 *
 * Clearing the floor is not the reason to keep it, though -- the floor can be
 * lowered instead (see -m), and doing that without sharpening reaches d' 0.08
 * against 0.58 with it. The sharpening earns its place on separation.
 *
 * That asymmetry is the point: enrollment retries until a stage is accepted,
 * so it quietly selects good frames, while a verification is scored on
 * whatever arrives. Sharpening raises the floor for the verification.
 *
 * sigma 1.5 with amount 2.5 is what the driver ships, and is the best cell of
 * a sweep of both against *separation* rather than genuine score. The
 * distinction matters: sigma 1.0 at the same amount scores 19.6 genuine
 * against 20.0 impostor, so it separates the wrong way while looking like an
 * improvement on genuine scores alone.
 */
/* Tunable from the command line in the bench; fixed constants in the driver.
 * The defaults track the driver, so a bare run measures what ships. */
static gdouble unsharp_sigma = 1.5;
static gdouble unsharp_amount = 2.5;
#define FPC_UNSHARP_SIGMA unsharp_sigma
#define FPC_UNSHARP_AMOUNT unsharp_amount
/* Kernel half-width, tied to sigma so the Gaussian is not silently
 * truncated when sigma changes -- 3 sigma is where it has effectively
 * died out. */
#define FPC_UNSHARP_RADIUS ((gint) ceil (3.0 * FPC_UNSHARP_SIGMA))
#define FPC_UNSHARP_MAX_RADIUS 12

static void
fpc_unsharp (guint8 *buf, gint width, gint height)
{
  const gint r = MIN (FPC_UNSHARP_RADIUS, FPC_UNSHARP_MAX_RADIUS);
  gdouble kernel[2 * FPC_UNSHARP_MAX_RADIUS + 1];
  gdouble sum = 0.0;

  /* Both zeros mean "no sharpening", which is a configuration worth
   * measuring rather than a mistake. Taken literally, sigma 0 divides by
   * zero in the Gaussian below and hands NBIS a frame of NaN, which reads
   * as a plausible zero-minutiae result instead of an obvious failure. */
  if (FPC_UNSHARP_AMOUNT == 0.0 || FPC_UNSHARP_SIGMA <= 0.0)
    return;
  g_autofree gdouble *tmp = g_new (gdouble, (gsize) width * height);
  g_autofree gdouble *blur = g_new (gdouble, (gsize) width * height);

  for (gint i = -r; i <= r; i++)
    {
      kernel[i + r] = exp (-(i * i) / (2.0 * FPC_UNSHARP_SIGMA * FPC_UNSHARP_SIGMA));
      sum += kernel[i + r];
    }
  for (gint i = 0; i < 2 * r + 1; i++)
    kernel[i] /= sum;

  /* Separable Gaussian: horizontal, then vertical. */
  for (gint y = 0; y < height; y++)
    for (gint x = 0; x < width; x++)
      {
        gdouble acc = 0.0;
        for (gint i = -r; i <= r; i++)
          acc += kernel[i + r] * buf[y * width + CLAMP (x + i, 0, width - 1)];
        tmp[y * width + x] = acc;
      }

  for (gint y = 0; y < height; y++)
    for (gint x = 0; x < width; x++)
      {
        gdouble acc = 0.0;
        for (gint i = -r; i <= r; i++)
          acc += kernel[i + r] * tmp[CLAMP (y + i, 0, height - 1) * width + x];
        blur[y * width + x] = acc;
      }

  for (gsize i = 0; i < (gsize) width * height; i++)
    {
      gdouble sharpened = buf[i] + FPC_UNSHARP_AMOUNT * (buf[i] - blur[i]);
      buf[i] = (guint8) CLAMP ((gint) (sharpened + 0.5), 0, 255);
    }
}

/* Catmull-Rom bicubic upscale.
 *
 * libfprint's fpi_image_resize() interpolates bilinearly, which softens
 * ridge edges just where NBIS is looking for them. Measured over ten saved
 * captures (tools/fpc_bench.c), swapping bilinear for Catmull-Rom at the
 * same 2x raises the best match score from 15 to 25 and is the only
 * configuration tried that puts any pair at or above the threshold of 24.
 * Catmull-Rom is the natural choice here: it is the interpolating cubic, so
 * it passes through the original samples rather than blurring them, and its
 * slight overshoot at an edge sharpens ridge boundaries instead of rounding
 * them off.
 */
static inline gdouble
fpc_cubic (gdouble a, gdouble b, gdouble c, gdouble d, gdouble t)
{
  const gdouble p = -0.5 * a + 1.5 * b - 1.5 * c + 0.5 * d;
  const gdouble q = a - 2.5 * b + 2.0 * c - 0.5 * d;
  const gdouble r = -0.5 * a + 0.5 * c;

  return ((p * t + q) * t + r) * t + b;
}

static inline guint8
fpc_sample (const guint8 *src, gint w, gint h, gint x, gint y)
{
  x = CLAMP (x, 0, w - 1);
  y = CLAMP (y, 0, h - 1);
  return src[y * w + x];
}

static void
fpc_resize_catrom (const guint8 *src, gint sw, gint sh, gint factor, guint8 *dst)
{
  const gint dw = sw * factor;
  const gint dh = sh * factor;

  for (gint dy = 0; dy < dh; dy++)
    {
      const gdouble sy = (dy + 0.5) / factor - 0.5;
      const gint iy = (gint) floor (sy);
      const gdouble ty = sy - iy;

      for (gint dx = 0; dx < dw; dx++)
        {
          const gdouble sx = (dx + 0.5) / factor - 0.5;
          const gint ix = (gint) floor (sx);
          const gdouble tx = sx - ix;
          gdouble col[4];

          for (gint k = 0; k < 4; k++)
            col[k] = fpc_cubic (fpc_sample (src, sw, sh, ix - 1, iy - 1 + k),
                                fpc_sample (src, sw, sh, ix + 0, iy - 1 + k),
                                fpc_sample (src, sw, sh, ix + 1, iy - 1 + k),
                                fpc_sample (src, sw, sh, ix + 2, iy - 1 + k),
                                tx);

          gdouble v = fpc_cubic (col[0], col[1], col[2], col[3], ty);
          dst[dy * dw + dx] = (guint8) CLAMP ((gint) (v + 0.5), 0, 255);
        }
    }
}

/* ---------------------------------------------------------------------
 * Normalised cross-correlation, as an alternative to matching minutiae.
 *
 * Why this exists: at 160x160 the frame carries 5-10 minutiae raw, where
 * bozorth3 wants ~12 and a full-size finger gives 40-80, and #22 measured the
 * whole minutiae pipeline at AUC 0.802 on 314 genuine and 1408 impostor pairs
 * -- better than the 0.563 that shipped, and still not an authenticator. Every
 * open-source project that got a sub-100mm2 press sensor working stopped
 * handing images to NBIS and matched the image directly; see
 * docs/research/other-projects-small-area.md. The one measured result in that
 * survey, libfprint MR !646 on a FocalTech FT9201 at 64x80, reached EER 0.07%
 * with the code below in about 120 lines, and its dominant parameter was not
 * any image-processing knob but the alignment search radius: EER 8.83% at
 * radius 3 against 0.07% at 16, because finger placement moves ~16 px between
 * presses and a +-3 px search never finds the overlap.
 *
 * This project has independent evidence for that mechanism from the other
 * direction. Two acquisitions milliseconds apart within one unbroken press --
 * a frame and the "ghost" the drain discards -- differ by a mean of 42 grey
 * levels per pixel, the same order as two separate presses at 25-51. So the
 * displacement is present even without lifting the finger, and a pixel-wise
 * operation cannot cope with it while a shift search is built for exactly it.
 *
 * Feed this the RAW frame. #22 measured that the driver's own pipeline
 * destroys signal here: enlargement and sharpening manufacture structure that
 * is common to any frame from this sensor, which is why 1x unsharpened beats
 * 2x sharpened. The high-pass below is the only preprocessing, and it plays
 * the role sharpening was reaching for without inventing ridges.
 */

/* Local-mean subtraction. Removes the slowly-varying pressure/contact term so
 * that correlation compares ridge structure rather than how hard the finger
 * was pressed -- synaspi's note on the same operation puts it exactly that
 * way. FT9201 inverts the frame first; that is a no-op here, because negating
 * both images leaves their correlation unchanged. */
static gfloat *
ncc_highpass (const guint8 *src, gint w, gint h, gint window)
{
  gfloat *out = g_new (gfloat, (gsize) w * h);
  const gint r = window / 2;

  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x++)
      {
        gint sum = 0, n = 0;

        for (gint j = -r; j <= r; j++)
          for (gint i = -r; i <= r; i++)
            { sum += fpc_sample (src, w, h, x + i, y + j); n++; }

        out[y * w + x] = (gfloat) src[y * w + x] - (gfloat) sum / n;
      }

  return out;
}

/* Correlation over the overlap at one integer offset. b is displaced by
 * (dx, dy) relative to a, so a[x, y] meets b[x - dx, y - dy].
 *
 * The minimum-overlap guard is not optional: without it a two-pixel corner
 * overlap scores 1.0 and wins the search outright. FT9201 requires half the
 * frame; synaspi sets an absolute floor for the same reason. */
static gdouble
ncc_at (const gfloat *a, const gfloat *b, gint w, gint h,
        gint dx, gint dy, gint min_overlap)
{
  const gint x0 = MAX (0, dx), x1 = MIN (w, w + dx);
  const gint y0 = MAX (0, dy), y1 = MIN (h, h + dy);
  gdouble sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
  gint n;

  if (x1 <= x0 || y1 <= y0) return -1.0;
  n = (x1 - x0) * (y1 - y0);
  if (n < min_overlap) return -1.0;

  for (gint y = y0; y < y1; y++)
    {
      const gfloat *pa = a + (gsize) y * w;
      const gfloat *pb = b + (gsize) (y - dy) * w - dx;

      for (gint x = x0; x < x1; x++)
        {
          const gdouble va = pa[x], vb = pb[x];

          sa += va; sb += vb;
          saa += va * va; sbb += vb * vb; sab += va * vb;
        }
    }

  {
    const gdouble ma = sa / n, mb = sb / n;
    const gdouble ca = saa - n * ma * ma;
    const gdouble cb = sbb - n * mb * mb;
    const gdouble cab = sab - n * ma * mb;

    if (ca <= 0.0 || cb <= 0.0) return -1.0;
    return cab / sqrt (ca * cb);
  }
}

/* Coarse-to-fine translation search: stride 2 over the whole radius, then
 * +-1 around the winner. FT9201 measured this as nearly lossless against the
 * exhaustive sweep -- mean score loss 0.004 over 70 pairs, and no genuine pair
 * that cleared the threshold stopped clearing it -- for a third of the cost.
 *
 * Both published implementations search translation only. Neither rotates,
 * neither estimates an angle first, and neither describes a finger guide; see
 * ncc_angles below for how that assumption is tested here rather than
 * inherited. */
static gdouble
ncc_search (const gfloat *a, const gfloat *b, gint w, gint h,
            gint radius, gint min_overlap, gint *out_dx, gint *out_dy)
{
  gdouble best = -1.0;
  gint bx = 0, by = 0;

  for (gint dy = -radius; dy <= radius; dy += 2)
    for (gint dx = -radius; dx <= radius; dx += 2)
      {
        const gdouble v = ncc_at (a, b, w, h, dx, dy, min_overlap);
        if (v > best) { best = v; bx = dx; by = dy; }
      }

  for (gint dy = by - 1; dy <= by + 1; dy++)
    for (gint dx = bx - 1; dx <= bx + 1; dx++)
      {
        const gdouble v = ncc_at (a, b, w, h, dx, dy, min_overlap);
        if (v > best) { best = v; bx = dx; by = dy; }
      }

  if (out_dx) *out_dx = bx;
  if (out_dy) *out_dy = by;
  return best;
}

/* Catmull-Rom at a fractional position, for the rotation axis. The filter is
 * the one fpc_resize_catrom already uses, and the reason is measured: swapping
 * bilinear for Catmull-Rom at the same 2x moved the best score from 15 to 25
 * with nothing else changed. That was inside the NBIS pipeline, which #22 has
 * since abandoned, so it does not carry as a result -- it carries as a warning
 * that interpolation on this sensor's frames is not neutral. */
static gdouble
fpc_sample_catrom (const guint8 *src, gint w, gint h, gdouble x, gdouble y)
{
  const gint ix = (gint) floor (x), iy = (gint) floor (y);
  const gdouble tx = x - ix, ty = y - iy;
  gdouble col[4];

  for (gint k = 0; k < 4; k++)
    col[k] = fpc_cubic (fpc_sample (src, w, h, ix - 1, iy - 1 + k),
                        fpc_sample (src, w, h, ix + 0, iy - 1 + k),
                        fpc_sample (src, w, h, ix + 1, iy - 1 + k),
                        fpc_sample (src, w, h, ix + 2, iy - 1 + k),
                        tx);

  return fpc_cubic (col[0], col[1], col[2], col[3], ty);
}

/* Rotate about the frame centre.
 *
 * Note what happens at exactly 0 degrees: the source coordinates come out
 * integral, Catmull-Rom at t = 0 returns the original sample, and the result
 * is a bit-exact copy. So routing 0 through this path does NOT equalise the
 * resampling cost across the angle axis, which is the obvious way to try to
 * make the sweep fair -- 0 is free and every other angle is not, whatever path
 * it takes. The cost is therefore measured rather than cancelled: sample the
 * angle axis finely near zero, and any step between 0 and the first non-zero
 * angle that does not continue as the angle grows is interpolation cost, not
 * rotation. */
static void
fpc_rotate_catrom (const guint8 *src, gint w, gint h, gdouble degrees, guint8 *dst)
{
  const gdouble rad = degrees * G_PI / 180.0;
  const gdouble c = cos (rad), s = sin (rad);
  const gdouble cx = (w - 1) / 2.0, cy = (h - 1) / 2.0;

  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x++)
      {
        const gdouble ox = x - cx, oy = y - cy;
        const gdouble sx =  c * ox + s * oy + cx;
        const gdouble sy = -s * ox + c * oy + cy;
        const gdouble v = fpc_sample_catrom (src, w, h, sx, sy);

        dst[(gsize) y * w + x] = (guint8) CLAMP ((gint) (v + 0.5), 0, 255);
      }
}

/* NCC configuration. Defaults track MR !646's tuned values, except the angle
 * axis, which upstream does not have. */
static gboolean ncc_mode = FALSE;
static gint     ncc_radius = 16;
static gint     ncc_window = 7;
static gboolean ncc_show_offsets = FALSE;
static gdouble *ncc_angles = NULL;   /* NULL means translation only */
static gint     ncc_n_angles = 1;

/* Mirrors fpc_frame_contrast() in the driver. */
static gdouble
frame_contrast (const guint8 *buf, gint width, gint height)
{
  gdouble total = 0.0;
  guint tiles = 0;

  for (gint ty = 0; ty + QUALITY_TILE <= height; ty += QUALITY_TILE)
    for (gint tx = 0; tx + QUALITY_TILE <= width; tx += QUALITY_TILE)
      {
        guint8 lo = 255, hi = 0;
        for (gint y = 0; y < QUALITY_TILE; y++)
          {
            const guint8 *row = buf + (ty + y) * width + tx;
            for (gint x = 0; x < QUALITY_TILE; x++)
              {
                if (row[x] < lo) lo = row[x];
                if (row[x] > hi) hi = row[x];
              }
          }
        total += hi - lo;
        tiles++;
      }

  return tiles ? total / tiles : 0.0;
}

/* fpi_print_add_from_image() reads minutiae that are already on the image;
 * on a real device libfprint runs detection before calling it. Detection is
 * async, so drive it to completion on a private main loop. */
static void
detect_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GMainLoop *loop = user_data;
  GError *error = NULL;

  if (!fp_image_detect_minutiae_finish (FP_IMAGE (source), res, &error))
    g_clear_error (&error);   /* no minutiae is a result, not a failure */

  g_main_loop_quit (loop);
}

static void
detect_minutiae_sync (FpImage *img)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);

  fp_image_detect_minutiae (img, NULL, detect_done, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
}

static guint8 *
read_raw (const char *path, gsize expected)
{
  FILE *f = fopen (path, "rb");
  guint8 *buf;
  gsize got;

  if (!f) { perror (path); return NULL; }

  buf = g_malloc (expected);
  got = fread (buf, 1, expected, f);
  fclose (f);

  if (got != expected)
    {
      fprintf (stderr, "%s: %zu bytes, expected %zu\n",
               path, (size_t) got, (size_t) expected);
      g_free (buf);
      return NULL;
    }
  return buf;
}

/* Per-minutia reliability: NBIS computes it, libfprint throws it away.
 *
 * mindtct scores every minutia it finds for how much it trusts it, and
 * libfprint's minutiae_to_xyt() carries that into c[i].col[3] -- and then
 * copies only col[0..2] into the xyt_struct that bozorth3 matches on
 * (fpi-print.c:138-152). NBIS ships bz_prune() and sort_quality_decreasing()
 * for exactly this, and neither is reachable from libfprint.
 *
 * That matters here because sharpening manufactures minutiae: a real frame
 * carries 2-10 of them and the same frame sharpened carries 39-127. If the
 * manufactured ones are the untrustworthy ones, dropping them should raise
 * separation. If they score as reliable as the real ones, that is a harder
 * and more interesting answer.
 *
 * The array belongs to the FpImage and libfprint says not to modify it. It
 * is modified here anyway, deliberately: the image is built and dropped
 * inside this function, and pruning before fpi_print_add_from_image() is the
 * only way to ask the question without a second copy of minutiae_to_xyt().
 */
static gint reliability_floor = 0;   /* percent; 0 keeps every minutia */

static gdouble
median_reliability (GPtrArray *m)
{
  g_autofree gdouble *v = NULL;
  gint n = m ? (gint) m->len : 0;

  if (!n) return 0.0;
  v = g_new (gdouble, n);
  for (gint i = 0; i < n; i++)
    v[i] = ((struct fp_minutia *) g_ptr_array_index (m, i))->reliability;

  /* Insertion sort: n is at most a few hundred. */
  for (gint i = 1; i < n; i++)
    {
      gdouble key = v[i];
      gint j = i - 1;
      while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; j--; }
      v[j + 1] = key;
    }
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

static void
prune_unreliable (GPtrArray *m)
{
  if (!m || reliability_floor <= 0) return;

  for (gint i = (gint) m->len - 1; i >= 0; i--)
    {
      struct fp_minutia *min = g_ptr_array_index (m, i);
      if (min->reliability * 100.0 < reliability_floor)
        g_ptr_array_remove_index (m, i);
    }
}

/* The driver's delivery path: raw frame -> gate -> enlarge -> ppmm. */
static gboolean
load_sample (sample *s, const char *path, const char *subject, gint w, gint h,
             gint enlarge, gdouble gate)
{
  g_autofree guint8 *raw = read_raw (path, (gsize) w * h);
  FpImage *img, *big;
  GError *error = NULL;

  if (!raw) return FALSE;

  s->name = g_path_get_basename (path);
  s->subject = subject;
  /* Gate on the raw frame, before sharpening, which is where the threshold
   * was calibrated and where a blank frame is unambiguous. */
  s->contrast = frame_contrast (raw, w, h);
  s->gated = (gate > 0.0 && s->contrast < gate);

  /* Kept before sharpening, because the NCC path wants the frame as the
   * sensor delivered it -- see the note above ncc_highpass(). */
  s->raw = g_memdup2 (raw, (gsize) w * h);
  if (ncc_mode)
    {
      s->hp = g_new0 (gfloat *, ncc_n_angles);

      if (!ncc_angles)
        {
          s->hp[0] = ncc_highpass (s->raw, w, h, ncc_window);
        }
      else
        {
          g_autofree guint8 *rot = g_new (guint8, (gsize) w * h);

          for (gint k = 0; k < ncc_n_angles; k++)
            {
              fpc_rotate_catrom (s->raw, w, h, ncc_angles[k], rot);
              s->hp[k] = ncc_highpass (rot, w, h, ncc_window);
            }
        }
    }

  fpc_unsharp (raw, w, h);

  img = fp_image_new (w, h);
  memcpy (img->data, raw, (gsize) w * h);

  if (enlarge > 1)
    {
      big = fp_image_new (w * enlarge, h * enlarge);
      fpc_resize_catrom (raw, w, h, enlarge, big->data);
    }
  else
    {
      big = g_object_ref (img);
    }
  g_object_unref (img);
  big->ppmm = (508.0 / 25.4) * enlarge;

  /* fp_print_new() wants a device, which there is none of here. Building
   * the object directly is what fp_print_deserialize() does, and gives a
   * print that the same internal helpers accept. FpPrint is a
   * GInitiallyUnowned, hence the sink. */
  detect_minutiae_sync (big);

  {
    GPtrArray *m = fp_image_get_minutiae (big);
    s->minutiae_raw = m ? m->len : 0;
    s->reliability = median_reliability (m);
    prune_unreliable (m);
  }

  s->print = g_object_ref_sink (g_object_new (FP_TYPE_PRINT,
                                              "driver", "fpc1021",
                                              "device-id", "bench",
                                              NULL));
  fpi_print_set_type (s->print, FPI_PRINT_NBIS);

  if (!fpi_print_add_from_image (s->print, big, &error))
    {
      /* No minutiae at all is a result, not a failure: it is exactly what a
       * blank or featureless frame produces, and worth reporting as zero. */
      g_clear_error (&error);
      s->minutiae = 0;
      g_object_unref (big);
      return TRUE;
    }
  g_object_unref (big);

  if (s->print->prints && s->print->prints->len == 1)
    {
      struct xyt_struct *xyt = g_ptr_array_index (s->print->prints, 0);
      s->minutiae = xyt->nrows;
    }

  return TRUE;
}

/* The scoring loop from fpi_print_bz3_match(), lifted so the score itself is
 * available rather than only the match/no-match verdict it returns. */
static gint
score_pair (sample *probe, sample *gallery)
{
  struct xyt_struct *p, *g;
  gint probe_len;

  if (!probe->print->prints || probe->print->prints->len != 1) return 0;
  if (!gallery->print->prints || gallery->print->prints->len != 1) return 0;

  p = g_ptr_array_index (probe->print->prints, 0);
  g = g_ptr_array_index (gallery->print->prints, 0);

  probe_len = bozorth_probe_init (p);
  return bozorth_to_gallery (probe_len, p, g);
}

/* The NCC counterpart of score_pair(), returning milli-NCC so that the
 * distribution, AUC and threshold machinery below is shared between the two
 * matchers unchanged and their numbers stay comparable pair for pair.
 *
 * The gallery is always searched unrotated and only the probe is turned,
 * which is what a verification actually looks like: the stored template is
 * fixed and the presented finger is whatever arrives. */
static gint
score_pair_ncc (sample *probe, sample *gallery, gint w, gint h,
                gint *out_dx, gint *out_dy, gdouble *out_angle)
{
  const gint min_overlap = w * h / 2;
  gdouble best = -1.0, ba = 0.0;
  gint bx = 0, by = 0;

  if (!probe->hp || !gallery->hp) return 0;

  for (gint k = 0; k < ncc_n_angles; k++)
    {
      gint dx, dy;
      const gdouble v = ncc_search (probe->hp[k], gallery->hp[0], w, h,
                                    ncc_radius, min_overlap, &dx, &dy);

      if (v > best)
        {
          best = v; bx = dx; by = dy;
          ba = ncc_angles ? ncc_angles[k] : 0.0;
        }
    }

  if (out_dx) *out_dx = bx;
  if (out_dy) *out_dy = by;
  if (out_angle) *out_angle = ba;

  return (gint) lround (best * 1000.0);
}

/* Genuine and impostor score distributions, and what separates them.
 *
 * d' is the separation in units of the pooled standard deviation --
 * (mean_genuine - mean_impostor) / sqrt((sd_g^2 + sd_i^2) / 2). It is the
 * number that decides whether any threshold is safe, and it is scale-free,
 * so it compares across configurations in a way raw scores do not. Usable
 * biometrics sit above 3.
 */
typedef struct {
  gint    n;
  gdouble sum, sumsq;
  gint   *scores;
  gint    cap;
} dist;

static void
dist_add (dist *d, gint score)
{
  if (d->n == d->cap)
    {
      d->cap = d->cap ? d->cap * 2 : 64;
      d->scores = g_renew (gint, d->scores, d->cap);
    }
  d->scores[d->n++] = score;
  d->sum += score;
  d->sumsq += (gdouble) score * score;
}

static gdouble
dist_mean (const dist *d)
{
  return d->n ? d->sum / d->n : 0.0;
}

static gdouble
dist_sd (const dist *d)
{
  gdouble m, var;

  if (d->n < 2) return 0.0;
  m = dist_mean (d);
  var = d->sumsq / d->n - m * m;
  return var > 0.0 ? sqrt (var) : 0.0;
}

static gint
dist_max (const dist *d)
{
  gint m = 0;
  for (gint i = 0; i < d->n; i++)
    if (d->scores[i] > m) m = d->scores[i];
  return m;
}

static gint
dist_at_or_above (const dist *d, gint t)
{
  gint n = 0;
  for (gint i = 0; i < d->n; i++)
    if (d->scores[i] >= t) n++;
  return n;
}

/* Probability that a random genuine pair outscores a random impostor pair,
 * with ties splitting the credit -- the area under the ROC curve, computed
 * directly rather than through a curve.
 *
 * d' assumes two Gaussians, and these distributions are not: below the
 * bozorth floor, and on a weak configuration generally, most scores are
 * exactly zero. A pile of ties at zero shrinks the standard deviations and
 * inflates d' -- 1x enlargement scores d' 0.28 while accepting 11% of
 * genuine attempts, which is not a better configuration, only a more
 * degenerate one. AUC has no distributional assumption and treats those
 * ties honestly: 0.5 is chance.
 */
static gdouble
auc (const dist *gen, const dist *imp)
{
  gdouble wins = 0.0;

  if (!gen->n || !imp->n) return 0.5;

  for (gint i = 0; i < gen->n; i++)
    for (gint j = 0; j < imp->n; j++)
      {
        if (gen->scores[i] > imp->scores[j]) wins += 1.0;
        else if (gen->scores[i] == imp->scores[j]) wins += 0.5;
      }

  return wins / ((gdouble) gen->n * imp->n);
}

static int
cmp_gint (const void *a, const void *b)
{
  const gint x = *(const gint *) a, y = *(const gint *) b;
  return (x > y) - (x < y);
}

static gint
dist_percentile (const dist *d, gdouble frac)
{
  g_autofree gint *copy = NULL;
  gint idx;

  if (d->n == 0) return 0;
  copy = g_memdup2 (d->scores, sizeof (gint) * d->n);
  qsort (copy, d->n, sizeof (gint), cmp_gint);
  idx = (gint) (frac * (d->n - 1) + 0.5);
  return copy[CLAMP (idx, 0, d->n - 1)];
}

static void
report_separation (const dist *gen, const dist *imp, gint threshold)
{
  gdouble mg = dist_mean (gen), mi = dist_mean (imp);
  gdouble sg = dist_sd (gen), si = dist_sd (imp);
  gdouble pooled = sqrt ((sg * sg + si * si) / 2.0);
  gint best_t = 0, hi = 0;
  gdouble best_margin = -1e9;

  printf ("\nseparation\n\n");
  printf ("  %-9s %5s %7s %7s %7s\n", "", "pairs", "mean", "sd", "max");
  printf ("  %-9s %5d %7.1f %7.1f %7d\n", "genuine", gen->n, mg, sg, dist_max (gen));
  printf ("  %-9s %5d %7.1f %7.1f %7d\n", "impostor", imp->n, mi, si, dist_max (imp));

  if (gen->n < 2 || imp->n < 2)
    {
      printf ("\n  too few pairs on one side to compute d'\n");
      return;
    }

  printf ("\n  d'  = %.2f", pooled > 0.0 ? (mg - mi) / pooled : 0.0);
  printf ("   (usable biometrics sit above 3)\n");
  printf ("  AUC = %.3f", auc (gen, imp));
  printf ("   (0.5 is chance; d' assumes Gaussians, AUC does not)\n");

  /* Sweep every threshold the scores actually reach. The best operating
   * point is the one maximising genuine-accept minus false-accept: with
   * distributions this close there is no threshold that is simply "right",
   * and the honest report is what the best available trade costs. */
  for (gint i = 0; i < gen->n; i++)
    if (gen->scores[i] > hi) hi = gen->scores[i];
  for (gint i = 0; i < imp->n; i++)
    if (imp->scores[i] > hi) hi = imp->scores[i];

  for (gint t = 1; t <= hi + 1; t++)
    {
      gdouble tar = (gdouble) dist_at_or_above (gen, t) / gen->n;
      gdouble far = (gdouble) dist_at_or_above (imp, t) / imp->n;
      if (tar - far > best_margin)
        {
          best_margin = tar - far;
          best_t = t;
        }
    }

  /* What a security decision actually needs: how many genuine attempts are
   * accepted at a threshold whose false-accept rate is capped. The "best
   * operating point" above maximises a margin and will happily sit at a FAR
   * no product could ship. */
  printf ("\n  genuine accepted at a threshold capping impostors:\n");
  {
    const gdouble caps[] = { 0.0, 0.001, 0.01 };
    for (gsize c = 0; c < G_N_ELEMENTS (caps); c++)
      {
        gint t_ok = -1;
        for (gint t = hi + 1; t >= 1; t--)
          {
            if ((gdouble) dist_at_or_above (imp, t) / imp->n <= caps[c])
              t_ok = t;
            else
              break;
          }
        if (t_ok < 0)
          printf ("    FAR <= %-5.1f%%   unreachable at any threshold\n", caps[c] * 100.0);
        else
          printf ("    FAR <= %-5.1f%%   threshold %-4d TAR %.0f%%\n",
                  caps[c] * 100.0, t_ok,
                  100.0 * dist_at_or_above (gen, t_ok) / gen->n);
      }
  }

  printf ("\n  at the shipped threshold %d:  accepts %.0f%% of genuine, %.0f%% of impostor\n",
          threshold,
          100.0 * dist_at_or_above (gen, threshold) / gen->n,
          100.0 * dist_at_or_above (imp, threshold) / imp->n);
  printf ("  best operating point %d:      accepts %.0f%% of genuine, %.0f%% of impostor\n",
          best_t,
          100.0 * dist_at_or_above (gen, best_t) / gen->n,
          100.0 * dist_at_or_above (imp, best_t) / imp->n);
}

int
main (int argc, char **argv)
{
  gint enlarge = DEFAULT_ENLARGE, threshold = DEFAULT_THRESHOLD;
  gint width = 160, height = 160;
  gdouble gate = DEFAULT_GATE;
  GPtrArray *samples = g_ptr_array_new ();
  const char *subject = NULL;
  gboolean threshold_set = FALSE;
  dist off_gen = { 0 }, off_imp = { 0 };
  int i;

  for (i = 1; i < argc; i++)
    {
      if ((!strcmp (argv[i], "-e") || !strcmp (argv[i], "--enlarge")) && i + 1 < argc)
        enlarge = atoi (argv[++i]);
      else if ((!strcmp (argv[i], "-t") || !strcmp (argv[i], "--threshold")) && i + 1 < argc)
        { threshold = atoi (argv[++i]); threshold_set = TRUE; }
      /* The NCC switches take effect at load time, so like -e and -s they
       * must precede the captures they apply to. */
      else if (!strcmp (argv[i], "-N") || !strcmp (argv[i], "--ncc"))
        ncc_mode = TRUE;
      else if ((!strcmp (argv[i], "-R") || !strcmp (argv[i], "--radius")) && i + 1 < argc)
        ncc_radius = atoi (argv[++i]);
      else if ((!strcmp (argv[i], "-W") || !strcmp (argv[i], "--window")) && i + 1 < argc)
        ncc_window = atoi (argv[++i]);
      else if (!strcmp (argv[i], "-O") || !strcmp (argv[i], "--offsets"))
        ncc_show_offsets = TRUE;
      /* -A MAX[:STEP] searches rotation as well as translation, over
       * -MAX..+MAX degrees. Neither published NCC implementation rotates at
       * all, so this is here to test that assumption rather than inherit it;
       * omitting it searches translation only and resamples nothing. */
      else if ((!strcmp (argv[i], "-A") || !strcmp (argv[i], "--angles")) && i + 1 < argc)
        {
          const char *spec = argv[++i];
          gdouble maxdeg = atof (spec), step = 1.0;
          const char *colon = strchr (spec, ':');

          if (colon) step = atof (colon + 1);
          if (maxdeg <= 0.0 || step <= 0.0)
            {
              fprintf (stderr, "-A wants MAX[:STEP] with both positive\n");
              return 2;
            }
          ncc_n_angles = 2 * (gint) floor (maxdeg / step + 1e-9) + 1;
          ncc_angles = g_new (gdouble, ncc_n_angles);
          for (gint k = 0; k < ncc_n_angles; k++)
            ncc_angles[k] = -maxdeg + k * step;
        }
      else if ((!strcmp (argv[i], "-s") || !strcmp (argv[i], "--sigma")) && i + 1 < argc)
        unsharp_sigma = atof (argv[++i]);
      else if ((!strcmp (argv[i], "-a") || !strcmp (argv[i], "--amount")) && i + 1 < argc)
        unsharp_amount = atof (argv[++i]);
      else if ((!strcmp (argv[i], "-g") || !strcmp (argv[i], "--gate")) && i + 1 < argc)
        gate = atof (argv[++i]);
      else if ((!strcmp (argv[i], "-m") || !strcmp (argv[i], "--min-minutiae")) && i + 1 < argc)
        {
          if (!FPC_FLOOR_SETTABLE)
            {
              fprintf (stderr, "-m needs libfprint-driver/bozorth-floor.patch "
                               "applied to the libfprint this was built against;\n"
                               "    without it the floor is a compile-time constant.\n");
              return 2;
            }
          bozorth_min_computable_minutiae = atoi (argv[++i]);
        }
      /* Applies to every capture after it, so one command line can carry
       * several fingers. */
      else if ((!strcmp (argv[i], "-S") || !strcmp (argv[i], "--subject")) && i + 1 < argc)
        subject = argv[++i];
      else if ((!strcmp (argv[i], "-q") || !strcmp (argv[i], "--reliability")) && i + 1 < argc)
        reliability_floor = atoi (argv[++i]);
      else if ((!strcmp (argv[i], "-w") || !strcmp (argv[i], "--width")) && i + 1 < argc)
        width = atoi (argv[++i]);
      else if ((!strcmp (argv[i], "-h") || !strcmp (argv[i], "--height")) && i + 1 < argc)
        height = atoi (argv[++i]);
      else
        {
          sample *s = g_new0 (sample, 1);
          if (load_sample (s, argv[i], subject, width, height, enlarge, gate))
            g_ptr_array_add (samples, s);
          else
            g_free (s);
        }
    }

  if (samples->len < 2)
    {
      fprintf (stderr,
               "usage: %s [-e N] [-t N] [-g N] [-S label] capture.bin capture.bin ...\n"
               "       %s -N [-R radius] [-W window] [-A MAX[:STEP]] [-O] ...\n"
               "  (at least two captures are needed to score anything)\n"
               "  -N matches raw frames by normalised cross-correlation instead of\n"
               "     minutiae; -R sets the alignment search radius, -A adds a rotation\n"
               "     axis, -O reports the peak offset per pair.\n", argv[0], argv[0]);
      return 2;
    }

  /* 0.50 is where MR !646 settled after review; as a milli-NCC integer that
   * is 500. Nothing about 24 means anything to a correlation score. */
  if (ncc_mode && !threshold_set)
    threshold = 500;

  if (ncc_mode)
    {
      printf ("NCC on raw %dx%d   high-pass %dx%d   radius +-%d   min overlap %d px",
              width, height, ncc_window, ncc_window, ncc_radius, width * height / 2);
      if (ncc_angles)
        printf ("   angles %+.2f..%+.2f in %d steps",
                ncc_angles[0], ncc_angles[ncc_n_angles - 1], ncc_n_angles);
      else
        printf ("   translation only");
      printf ("\n   threshold %d milli-NCC (%.3f)   gate %.0f\n\n",
              threshold, threshold / 1000.0, gate);
    }
  else
    printf ("enlarge %dx -> %dx%d   unsharp sigma %.2f amount %.2f   threshold %d   gate %.0f   floor %d\n\n",
            enlarge, width * enlarge, height * enlarge,
            unsharp_sigma, unsharp_amount, threshold, gate,
            bozorth_min_computable_minutiae);

  printf ("%-16s %-12s %9s %8s %6s %8s %s\n",
          "capture", "subject", "contrast", "minutiae", "kept", "med.rel", "");
  for (i = 0; i < (int) samples->len; i++)
    {
      sample *s = g_ptr_array_index (samples, i);
      const char *note = s->gated ? "  gated as blank"
                       : (gint) s->minutiae < bozorth_min_computable_minutiae
                         ? "  under bozorth floor" : "";
      printf ("%-16s %-12s %9.1f %8u %6u %8.2f%s\n", s->name,
              s->subject ? s->subject : "-", s->contrast,
              s->minutiae_raw, s->minutiae, s->reliability, note);
    }

  /* Every sample as probe against every other as gallery. */
  printf ("\nscores (row = probe, column = gallery)\n\n%16s", "");
  for (i = 0; i < (int) samples->len; i++)
    printf ("%6d", i);
  printf ("\n");

  gint best = 0, matches = 0, pairs = 0;
  const gint nsamp = (gint) samples->len;
  g_autofree gint *off_dx = ncc_show_offsets ? g_new0 (gint, (gsize) nsamp * nsamp) : NULL;
  g_autofree gint *off_dy = ncc_show_offsets ? g_new0 (gint, (gsize) nsamp * nsamp) : NULL;
  g_autofree gdouble *off_ang = ncc_show_offsets ? g_new0 (gdouble, (gsize) nsamp * nsamp) : NULL;
  gdouble total = 0;
  dist genuine = { 0 }, impostor = { 0 };
  gboolean labelled = FALSE;

  for (i = 0; i < (int) samples->len; i++)
    if (((sample *) g_ptr_array_index (samples, i))->subject)
      labelled = TRUE;

  for (i = 0; i < (int) samples->len; i++)
    {
      sample *probe = g_ptr_array_index (samples, i);
      printf ("%2d %-13s", i, probe->name);

      for (int j = 0; j < (int) samples->len; j++)
        {
          sample *gallery = g_ptr_array_index (samples, j);
          if (i == j) { printf ("     ."); continue; }

          gint dx = 0, dy = 0;
          gdouble ang = 0.0;
          gint sc = ncc_mode
                    ? score_pair_ncc (probe, gallery, width, height, &dx, &dy, &ang)
                    : score_pair (probe, gallery);
          printf ("%6d", sc);

          if (off_dx)
            {
              off_dx[(gsize) i * nsamp + j] = dx;
              off_dy[(gsize) i * nsamp + j] = dy;
              off_ang[(gsize) i * nsamp + j] = ang;
            }

          if (probe->gated || gallery->gated) continue;
          pairs++;
          total += sc;
          if (sc > best) best = sc;
          if (sc >= threshold) matches++;

          /* An unlabelled capture belongs to no finger, so it cannot be
           * classified either way; counting it as genuine is precisely the
           * mistake this labelling exists to prevent. */
          if (probe->subject && gallery->subject)
            {
              const gboolean same = !strcmp (probe->subject, gallery->subject);

              dist_add (same ? &genuine : &impostor, sc);

              /* Peak displacement in tenths of a pixel. This is the
               * measurement, not a by-product: the whole case for a shift
               * search rests on how far the finger actually moves, and the
               * search returns that for free. */
              if (ncc_mode)
                dist_add (same ? &off_gen : &off_imp,
                          (gint) lround (10.0 * sqrt ((gdouble) dx * dx +
                                                      (gdouble) dy * dy)));
            }
        }
      printf ("\n");
    }

  printf ("\nover %d ungated pairs:  best %d   mean %.1f   at or above %d: %d (%.0f%%)\n",
          pairs, best, pairs ? total / pairs : 0.0, threshold, matches,
          pairs ? 100.0 * matches / pairs : 0.0);

  if (labelled)
    report_separation (&genuine, &impostor, threshold);
  else
    printf ("\nno -S labels given, so no pair can be called genuine or "
            "impostor;\nseparation is the objective here, so label the "
            "fingers and re-run.\n");

  if (ncc_mode && (off_gen.n || off_imp.n))
    {
      const dist *cls[2] = { &off_gen, &off_imp };
      const char *names[2] = { "genuine", "impostor" };

      printf ("\npeak alignment offset |(dx, dy)|, pixels\n\n");
      printf ("  %-10s %7s %7s %8s %7s %7s\n",
              "", "pairs", "mean", "median", "p90", "max");
      for (gint c = 0; c < 2; c++)
        {
          const dist *d = cls[c];

          if (!d->n) continue;
          printf ("  %-10s %7d %7.1f %8.1f %7.1f %7.1f\n", names[c], d->n,
                  dist_mean (d) / 10.0, dist_percentile (d, 0.50) / 10.0,
                  dist_percentile (d, 0.90) / 10.0, dist_max (d) / 10.0);
        }
      printf ("\n  n is printed because it decides what these numbers can carry:\n"
              "  a median over a handful of pairs is an indication, not a distribution.\n");
    }

  if (off_dx)
    {
      printf ("\nper-pair peak offset (row = probe, column = gallery)\n\n");
      for (i = 0; i < nsamp; i++)
        {
          sample *probe = g_ptr_array_index (samples, i);

          printf ("%2d %-13s", i, probe->name);
          for (int j = 0; j < nsamp; j++)
            {
              if (i == j) { printf ("%12s", ".") ; continue; }
              if (ncc_angles)
                printf (" %+3d,%+3d@%+.1f", off_dx[(gsize) i * nsamp + j],
                        off_dy[(gsize) i * nsamp + j],
                        off_ang[(gsize) i * nsamp + j]);
              else
                printf ("  %+4d,%+4d", off_dx[(gsize) i * nsamp + j],
                        off_dy[(gsize) i * nsamp + j]);
            }
          printf ("\n");
        }
    }

  g_free (genuine.scores);
  g_free (impostor.scores);
  g_free (off_gen.scores);
  g_free (off_imp.scores);

  return 0;
}
