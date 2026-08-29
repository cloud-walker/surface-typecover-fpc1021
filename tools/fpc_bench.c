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
 *   -w, --width N       capture width (default 160)
 *   -h, --height N      capture height (default 160)
 *
 * Every capture is used once as a probe against all the others as a
 * template, which is the closest offline analogue of "enroll on some
 * presses, verify with another".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fpi-print.h"
#include "fp-print-private.h"
#include "fpi-image.h"
#include "fpi-log.h"

#include "bozorth.h"

#define DEFAULT_ENLARGE 3
#define DEFAULT_THRESHOLD 24
#define DEFAULT_GATE 40.0
#define QUALITY_TILE 16

typedef struct {
  char    *name;
  FpPrint *print;        /* one print, holding this capture's minutiae */
  guint    minutiae;
  gdouble  contrast;
  gboolean gated;        /* rejected by the blank-frame gate */
} sample;

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

/* The driver's delivery path: raw frame -> gate -> enlarge -> ppmm. */
static gboolean
load_sample (sample *s, const char *path, gint w, gint h,
             gint enlarge, gdouble gate)
{
  g_autofree guint8 *raw = read_raw (path, (gsize) w * h);
  FpImage *img, *big;
  GError *error = NULL;

  if (!raw) return FALSE;

  s->name = g_path_get_basename (path);
  s->contrast = frame_contrast (raw, w, h);
  s->gated = (gate > 0.0 && s->contrast < gate);

  img = fp_image_new (w, h);
  memcpy (img->data, raw, (gsize) w * h);

  big = (enlarge > 1) ? fpi_image_resize (img, enlarge, enlarge) : g_object_ref (img);
  g_object_unref (img);
  big->ppmm = (508.0 / 25.4) * enlarge;

  /* fp_print_new() wants a device, which there is none of here. Building
   * the object directly is what fp_print_deserialize() does, and gives a
   * print that the same internal helpers accept. FpPrint is a
   * GInitiallyUnowned, hence the sink. */
  detect_minutiae_sync (big);

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

int
main (int argc, char **argv)
{
  gint enlarge = DEFAULT_ENLARGE, threshold = DEFAULT_THRESHOLD;
  gint width = 160, height = 160;
  gdouble gate = DEFAULT_GATE;
  GPtrArray *samples = g_ptr_array_new ();
  int i;

  for (i = 1; i < argc; i++)
    {
      if ((!strcmp (argv[i], "-e") || !strcmp (argv[i], "--enlarge")) && i + 1 < argc)
        enlarge = atoi (argv[++i]);
      else if ((!strcmp (argv[i], "-t") || !strcmp (argv[i], "--threshold")) && i + 1 < argc)
        threshold = atoi (argv[++i]);
      else if ((!strcmp (argv[i], "-g") || !strcmp (argv[i], "--gate")) && i + 1 < argc)
        gate = atof (argv[++i]);
      else if ((!strcmp (argv[i], "-w") || !strcmp (argv[i], "--width")) && i + 1 < argc)
        width = atoi (argv[++i]);
      else if ((!strcmp (argv[i], "-h") || !strcmp (argv[i], "--height")) && i + 1 < argc)
        height = atoi (argv[++i]);
      else
        {
          sample *s = g_new0 (sample, 1);
          if (load_sample (s, argv[i], width, height, enlarge, gate))
            g_ptr_array_add (samples, s);
          else
            g_free (s);
        }
    }

  if (samples->len < 2)
    {
      fprintf (stderr, "usage: %s [-e N] [-t N] [-g N] capture.bin capture.bin ...\n"
                       "  (at least two captures are needed to score anything)\n", argv[0]);
      return 2;
    }

  printf ("enlarge %dx  ->  %dx%d      threshold %d      blank gate %.0f\n\n",
          enlarge, width * enlarge, height * enlarge, threshold, gate);

  printf ("%-16s %9s %8s %s\n", "capture", "contrast", "minutiae", "");
  for (i = 0; i < (int) samples->len; i++)
    {
      sample *s = g_ptr_array_index (samples, i);
      const char *note = s->gated ? "  gated as blank"
                       : s->minutiae < MIN_COMPUTABLE_BOZORTH_MINUTIAE
                         ? "  under bozorth floor" : "";
      printf ("%-16s %9.1f %8u%s\n", s->name, s->contrast, s->minutiae, note);
    }

  /* Every sample as probe against every other as gallery. */
  printf ("\nscores (row = probe, column = gallery)\n\n%16s", "");
  for (i = 0; i < (int) samples->len; i++)
    printf ("%6d", i);
  printf ("\n");

  gint best = 0, matches = 0, pairs = 0;
  gdouble total = 0;

  for (i = 0; i < (int) samples->len; i++)
    {
      sample *probe = g_ptr_array_index (samples, i);
      printf ("%2d %-13s", i, probe->name);

      for (int j = 0; j < (int) samples->len; j++)
        {
          sample *gallery = g_ptr_array_index (samples, j);
          if (i == j) { printf ("     ."); continue; }

          gint sc = score_pair (probe, gallery);
          printf ("%6d", sc);

          if (probe->gated || gallery->gated) continue;
          pairs++;
          total += sc;
          if (sc > best) best = sc;
          if (sc >= threshold) matches++;
        }
      printf ("\n");
    }

  printf ("\nover %d ungated pairs:  best %d   mean %.1f   at or above %d: %d (%.0f%%)\n",
          pairs, best, pairs ? total / pairs : 0.0, threshold, matches,
          pairs ? 100.0 * matches / pairs : 0.0);

  return 0;
}
