/*
 * Counts the minutiae libfprint extracts from a raw capture.
 *
 * Matching on this sensor is bozorth3 over minutiae -- ridge endings and
 * bifurcations -- not over the image. A capture can look excellent to the
 * eye and still be unmatchable if the patch of finger it covers has almost
 * no minutiae in it, which is the failure mode small 160x160 press sensors
 * are prone to. This turns "verify-no-match" from a guess into a number.
 *
 * Usage: fpc_minutiae capture.bin [width] [height]     (default 160x160)
 *
 * Rough reading of the result: bozorth3 needs a useful overlap between two
 * prints' minutiae sets. Under ~10 minutiae in an image, a match is mostly
 * luck; a healthy press capture is usually several tens.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fprint.h>

typedef struct {
    GMainLoop *loop;
    int status;
} ctx;

static void
detect_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    FpImage *img = FP_IMAGE (source);
    ctx *c = user_data;
    GError *error = NULL;

    if (!fp_image_detect_minutiae_finish (img, res, &error)) {
        fprintf (stderr, "minutiae detection failed: %s\n",
                 error ? error->message : "unknown error");
        g_clear_error (&error);
        c->status = 1;
        g_main_loop_quit (c->loop);
        return;
    }

    GPtrArray *minutiae = fp_image_get_minutiae (img);
    guint n = minutiae ? minutiae->len : 0;

    printf ("minutiae detected: %u\n", n);
    for (guint i = 0; i < n && i < 40; i++) {
        gint x, y;
        fp_minutia_get_coords (g_ptr_array_index (minutiae, i), &x, &y);
        printf ("  %2u: (%3d, %3d)\n", i, x, y);
    }
    if (n > 40) printf ("  ... %u more\n", n - 40);

    if (n == 0)
        puts ("\nNo minutiae: this image cannot match anything.");
    else if (n < 10)
        puts ("\nVery few minutiae -- matching will be unreliable at best.");

    c->status = 0;
    g_main_loop_quit (c->loop);
}

int main (int argc, char **argv)
{
    if (argc < 2) {
        fprintf (stderr, "usage: %s capture.bin [width] [height]\n", argv[0]);
        return 2;
    }

    int width  = (argc > 2) ? atoi (argv[2]) : 160;
    int height = (argc > 3) ? atoi (argv[3]) : 160;

    FILE *f = fopen (argv[1], "rb");
    if (!f) { perror (argv[1]); return 1; }

    gsize expected = (gsize) width * (gsize) height;
    guchar *raw = g_malloc (expected);
    gsize got = fread (raw, 1, expected, f);
    fclose (f);

    if (got != expected) {
        fprintf (stderr, "%s holds %zu bytes, expected %zu for %dx%d\n",
                 argv[1], (size_t) got, (size_t) expected, width, height);
        g_free (raw);
        return 1;
    }

    FpImage *img = fp_image_new (width, height);
    gsize len = 0;
    /* The buffer belongs to the image and is ours to fill; the getter is
     * const only because callers normally read finished images. */
    guchar *data = (guchar *) fp_image_get_data (img, &len);
    if (!data || len != expected) {
        fprintf (stderr, "unexpected image buffer (%zu bytes)\n", (size_t) len);
        return 1;
    }
    memcpy (data, raw, expected);
    g_free (raw);

    printf ("image: %dx%d, %.2f px/mm\n", width, height, fp_image_get_ppmm (img));

    ctx c = { g_main_loop_new (NULL, FALSE), 1 };
    fp_image_detect_minutiae (img, NULL, detect_done, &c);
    g_main_loop_run (c.loop);

    g_main_loop_unref (c.loop);
    g_object_unref (img);
    return c.status;
}
