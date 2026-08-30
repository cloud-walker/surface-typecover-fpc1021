/*
 * FPC1021 driver for libfprint
 *
 * Driver for the Fingerprint Cards FPC1021 sensor built into the Microsoft
 * Surface Pro Type Cover with Fingerprint ID (USB 045e:09c2, interface 1).
 *
 * The wire protocol was reverse-engineered via static analysis of
 * Microsoft's own, publicly-downloadable Surface driver package, then
 * validated live against real hardware. See ../../PROTOCOL.md (in the
 * surface-typecover-fpc1021 project this driver was developed in) for the
 * full protocol writeup.
 *
 * Copyright (C) 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "fpc1021"

#include <math.h>

#include "drivers_api.h"

/* Interface 1 of the composite USB device is the fingerprint sensor;
 * interface 0 is the ordinary HID keyboard/touchpad and is left alone. */
#define FPC_USB_INTERFACE 1
#define FPC_EP_OUT (4 | FPI_USB_ENDPOINT_OUT)
#define FPC_EP_IN (3 | FPI_USB_ENDPOINT_IN)
#define FPC_USB_TIMEOUT 3000
#define FPC_MAX_PACKET 64

/* All commands are 2-byte little-endian opcodes; see PROTOCOL.md. */
#define CMD_GET_CHIP_ID 0x0001
#define CMD_CAPTURE 0x0007
#define CMD_RESET 0x0008

/* Pacing between capture attempts while waiting for a finger, and cooldown
 * after a successful capture, in milliseconds. Not yet tuned against real
 * hardware beyond confirming the protocol itself works; see PROTOCOL.md's
 * "Timing" section for the (larger, more conservative) values observed in
 * the original Windows driver. */
#define FPC_RETRY_DELAY_MS 200
/* After this many back-to-back timeouts (~this many * (500ms poll + delay)
 * seconds) with no reply at all, assume the sensor firmware is wedged and
 * issue a real USB bus reset -- see consecutive_timeouts in the struct. */
#define FPC_MAX_CONSECUTIVE_TIMEOUTS 6

/* Draining the IN endpoint before a capture. A stale image is a header plus
 * 412 stream packets, so the cap is set above that with room to spare; the
 * timeout only has to be long enough for a packet that is already queued to
 * come back, not for the sensor to produce anything. */
/* NBIS's block-based minutiae detector works on a fixed 8px grid, so on a
 * small sensor the ridge structure is averaged away before it can be seen.
 * libfprint's other small square press sensors enlarge before handing the
 * image over -- aes4000 (96x96) by 3, aes3500 (128x128) by 2, both with the
 * comment that it is "to make the image big enough for NBIS to process
 * reliably". At 160x160 this sensor is the largest of the three, so 2 puts
 * it at 320x320 and 3 at 480x480, both in the range those two deliver.
 *
 * Detection below MIN_COMPUTABLE_BOZORTH_MINUTIAE (10) is what made every
 * comparison score exactly 0. At 2 the floor was cleared and bozorth3
 * started producing real scores for the first time -- but only 3 and 6
 * against a threshold of 24, weaker even than the 9 aes3k settles for. 3 is
 * the next step up, and what aes4000 uses. Offline the extra factor cuts
 * both ways (one sample 9 -> 18 minutiae, another 12 -> 2), so this is
 * worth keeping only if the scores say so, and they say 2.
 *
 * Measured with tools/fpc_bench.c over ten saved captures, varying only this
 * factor: 1 -> best score 0, 2 -> 15, 3 -> 10, 4 -> 0. An earlier live
 * comparison appeared to favour 3, but it re-enrolled and re-pressed between
 * factors, so it compared conditions as much as factors.
 *
 * Past 2 the count keeps rising while correspondence collapses: three
 * captures of one finger that was never lifted yield 11/20/23 minutiae at 2x
 * and score 6, 7, 15 against each other, but 4/21/41 at 3x and score zero.
 * Enlargement buys resolution for NBIS's fixed 8px block grid; beyond that it
 * amplifies sensor noise into minutiae that differ between two images of the
 * same finger, which is worse than having too few.
 * See ../libfprint-driver/README.md. */
#define FPC_ENLARGE_FACTOR 2

/* Frame quality gate. The sensor happily returns blank frames -- all-white
 * or all-black -- and handing one to libfprint puts a useless print in the
 * enrolled template and wastes a verification attempt. The Windows driver
 * classifies each frame by a block-based contrast analysis before accepting
 * it; this is the same idea, calibrated on ten real captures:
 *
 *   mean per-tile (max - min) over 16x16 tiles
 *     blank white / blank black frames :   3.4 .. 17.7
 *     frames carrying a real print     :  77.7 .. 235.2
 *
 * The gap is wide enough that the exact threshold hardly matters; 40 sits
 * well clear of both sides. This only rejects frames that are unusable, not
 * merely poor -- two real captures scoring 80-88 yielded 9 minutiae, just
 * under what bozorth3 can work with, and are deliberately still accepted
 * rather than second-guessed here. */
#define FPC_QUALITY_TILE 16
#define FPC_MIN_TILE_CONTRAST 40.0

#define FPC_DRAIN_TIMEOUT_MS 50
#define FPC_DRAIN_MAX_PACKETS 512
#define FPC_CAPTURE_COOLDOWN_MS 3000

struct _FpiDeviceFpc1021
{
  FpImageDevice parent;

  gint     width;
  gint     height;

  guint8  *image_buf;
  gsize    image_len;
  gsize    image_have;

  gboolean deactivating;

  /* The sensor firmware sometimes stops answering the capture-reply read
   * entirely (every CAPTURE_READ_HEADER attempt times out, even with a
   * finger present) after a handful of captures. Its own "reset" opcode
   * (0x0008) does not clear this; only a real USB bus reset does (verified
   * empirically -- unplugging/replugging the Type Cover always fixes it).
   * Track consecutive timeouts and issue a real reset after too many. */
  guint    consecutive_timeouts;

  /* Reads left unconsumed on the IN endpoint desynchronise everything after
   * them, because this protocol pairs reads and writes 1:1. Counted per
   * capture so an unusual drain gets reported once rather than silently. */
  guint    drained;

  /* The drained packets are a whole unrequested image, and nobody has ever
   * looked at one. When FPC1021_GHOST_DIR is set they are reassembled the
   * same way a real capture is and written there as raw frames. Off by
   * default and never affects a capture either way -- the frame is still
   * discarded, it is just written down first. See fpc_ghost_feed(). */
  guint8  *ghost_buf;
  gsize    ghost_len;
  gsize    ghost_have;
  guint    ghost_seq;
};

G_DECLARE_FINAL_TYPE (FpiDeviceFpc1021, fpi_device_fpc1021, FPI,
                      DEVICE_FPC1021, FpImageDevice);
G_DEFINE_TYPE (FpiDeviceFpc1021, fpi_device_fpc1021, FP_TYPE_IMAGE_DEVICE);

static void start_capture (FpImageDevice *idev);
static void fpc_drain_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                          gpointer user_data, GError *error);

/* ---------------------------------------------------------------------- */
/* Low-level protocol helpers                                             */
/* ---------------------------------------------------------------------- */

static void
fpc_write_cmd (FpiSsm *ssm, FpDevice *dev, guint16 opcode)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
  guint8 *buf = g_malloc (2);

  buf[0] = opcode & 0xff;
  buf[1] = (opcode >> 8) & 0xff;

  fpi_usb_transfer_fill_bulk_full (transfer, FPC_EP_OUT, buf, 2, g_free);
  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (transfer, FPC_USB_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
fpc_read_reply_timeout (FpiSsm *ssm, FpDevice *dev, gsize len,
                        guint timeout_ms, FpiUsbTransferCallback cb)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (transfer, FPC_EP_IN, len);
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (transfer, timeout_ms, NULL, cb, NULL);
}

static void
fpc_read_reply (FpiSsm *ssm, FpDevice *dev, gsize len,
                FpiUsbTransferCallback cb)
{
  fpc_read_reply_timeout (ssm, dev, len, FPC_USB_TIMEOUT, cb);
}

/* ---------------------------------------------------------------------- */
/* Open: identify the exact FPC chip and its resolution (Get Chip ID)     */
/* ---------------------------------------------------------------------- */

enum open_states {
  /* An image the sensor produced but nobody read survives a close/open
   * cycle: the very first fprintd-verify after a completed enrollment read
   * a stale capture header here and reported "unrecognised FPC chip ID
   * 0x6400" -- 0x6400 being 25600, the image length field of that header.
   * Drain before trusting anything on this endpoint. */
  OPEN_DRAIN,
  OPEN_GET_CHIP_ID,
  OPEN_READ_CHIP_ID,
  OPEN_NUM_STATES,
};

static void
open_read_chip_id_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                      gpointer user_data, GError *error)
{
  FpiDeviceFpc1021 *self = FPI_DEVICE_FPC1021 (dev);
  guint16 chip_id, masked, status;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (transfer->actual_length < 6)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "short Get-Chip-ID reply (%" G_GSSIZE_FORMAT " bytes)",
                                                     transfer->actual_length));
      return;
    }

  /* Replies carry status = 0x1000 | opcode, so a reply that does not
   * acknowledge Get Chip ID is somebody else's -- the endpoint is
   * desynchronised and every byte after this is meaningless. Say so, rather
   * than decoding a capture header's length field as a chip ID. */
  status = transfer->buffer[0] | (transfer->buffer[1] << 8);
  if (status != (0x1000 | CMD_GET_CHIP_ID))
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "desynchronised reply: expected status 0x%04x, got 0x%04x",
                                                     0x1000 | CMD_GET_CHIP_ID, status));
      return;
    }

  chip_id = transfer->buffer[4] | (transfer->buffer[5] << 8);
  masked = chip_id & 0xfff0;

  /* Table from PROTOCOL.md. Only FPC1021 (this driver's namesake) has been
   * validated against real hardware; the others are transcribed from the
   * Windows driver but untested here. */
  if (masked == 0x0200)
    {
      self->width = 192; self->height = 192; /* FPC1020 */
    }
  else if (masked == 0x0210)
    {
      self->width = 160; self->height = 160; /* FPC1021 */
    }
  else if (masked == 0x1400)
    {
      self->width = 192; self->height = 56; /* FPC1140 */
    }
  else if (masked == 0x1500)
    {
      self->width = 208; self->height = 80; /* FPC1150 */
    }
  else if ((chip_id & 0xff0f) == 0x0101)
    {
      self->width = 88; self->height = 112; /* FPC1022 */
    }
  else
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED,
                                                     "unrecognised FPC chip ID 0x%04x", chip_id));
      return;
    }

  fp_dbg ("FPC sensor detected: chip id 0x%04x, %dx%d", chip_id,
          self->width, self->height);
  fpi_ssm_mark_completed (transfer->ssm);
}

static void
open_run_state (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OPEN_DRAIN:
      fpc_read_reply_timeout (ssm, dev, FPC_MAX_PACKET, FPC_DRAIN_TIMEOUT_MS,
                              fpc_drain_cb);
      break;

    case OPEN_GET_CHIP_ID:
      fpc_write_cmd (ssm, dev, CMD_GET_CHIP_ID);
      break;

    case OPEN_READ_CHIP_ID:
      fpc_read_reply (ssm, dev, FPC_MAX_PACKET, open_read_chip_id_cb);
      break;
    }
}

static void
open_sm_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  fpi_image_device_open_complete (FP_IMAGE_DEVICE (dev), error);
}

static void
dev_open (FpImageDevice *idev)
{
  FpiDeviceFpc1021 *self = FPI_DEVICE_FPC1021 (idev);
  GError *error = NULL;
  FpiSsm *ssm;

  self->drained = 0;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (idev)),
                                     FPC_USB_INTERFACE, 0, &error))
    {
      fpi_image_device_open_complete (idev, error);
      return;
    }

  ssm = fpi_ssm_new (FP_DEVICE (idev), open_run_state, OPEN_NUM_STATES);
  fpi_ssm_start (ssm, open_sm_complete);
}

static void
dev_close (FpImageDevice *idev)
{
  GError *error = NULL;
  FpiDeviceFpc1021 *self = FPI_DEVICE_FPC1021 (idev);

  g_clear_pointer (&self->image_buf, g_free);
  g_clear_pointer (&self->ghost_buf, g_free);

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (idev)),
                                  FPC_USB_INTERFACE, 0, &error);
  fpi_image_device_close_complete (idev, error);
}

/* ---------------------------------------------------------------------- */
/* Capture: reset, trigger, stream the image back                        */
/* ---------------------------------------------------------------------- */

enum capture_states {
  CAPTURE_DRAIN,
  CAPTURE_RESET,
  CAPTURE_RESET_ACK,
  CAPTURE_TRIGGER,
  CAPTURE_READ_HEADER,
  CAPTURE_STREAM,
  CAPTURE_NUM_STATES,
};

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
 * That asymmetry is the point: enrollment retries until a stage is accepted,
 * so it quietly selects good frames, while a verification is scored on
 * whatever arrives. Sharpening raises the floor for the verification.
 *
 * sigma 1.5 with amount 2.5 was the best of a grid over both, and sits on a
 * plateau rather than a spike; past sigma 2.0 the score collapses, which is
 * the mechanism showing itself -- blur too much before sharpening and there
 * is nothing left to sharpen.
 */
#define FPC_UNSHARP_SIGMA 1.5
#define FPC_UNSHARP_AMOUNT 2.5
/* Kernel half-width, tied to sigma so the Gaussian is not silently
 * truncated -- 3 sigma is where it has effectively died out. */
#define FPC_UNSHARP_RADIUS ((gint) ceil (3.0 * FPC_UNSHARP_SIGMA))

static void
fpc_unsharp (guint8 *buf, gint width, gint height)
{
  const gint r = FPC_UNSHARP_RADIUS;
  gdouble kernel[2 * FPC_UNSHARP_RADIUS + 1];
  gdouble sum = 0.0;
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

/* Mean per-tile contrast, the frame-quality proxy described above. */
static gdouble
fpc_frame_contrast (const guint8 *buf, gint width, gint height)
{
  gdouble total = 0.0;
  guint tiles = 0;

  for (gint ty = 0; ty + FPC_QUALITY_TILE <= height; ty += FPC_QUALITY_TILE)
    {
      for (gint tx = 0; tx + FPC_QUALITY_TILE <= width; tx += FPC_QUALITY_TILE)
        {
          guint8 lo = 255, hi = 0;

          for (gint y = 0; y < FPC_QUALITY_TILE; y++)
            {
              const guint8 *row = buf + (ty + y) * width + tx;
              for (gint x = 0; x < FPC_QUALITY_TILE; x++)
                {
                  if (row[x] < lo) lo = row[x];
                  if (row[x] > hi) hi = row[x];
                }
            }
          total += hi - lo;
          tiles++;
        }
    }

  return tiles ? total / tiles : 0.0;
}

static void
deliver_image (FpDevice *dev)
{
  FpiDeviceFpc1021 *self = FPI_DEVICE_FPC1021 (dev);
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);
  FpImage *enlarged;
  gdouble contrast;

  contrast = fpc_frame_contrast (self->image_buf, self->width, self->height);
  if (contrast < FPC_MIN_TILE_CONTRAST)
    {
      fp_dbg ("fpc1021: rejecting blank frame (tile contrast %.1f < %.1f)",
              contrast, FPC_MIN_TILE_CONTRAST);
      g_clear_pointer (&self->image_buf, g_free);
      fpi_image_device_retry_scan (idev, FP_DEVICE_RETRY_GENERAL);
      fpi_image_device_report_finger_status (idev, FALSE);
      return;
    }

  /* Sharpen the raw frame before it is enlarged; the gate above deliberately
   * ran on the unsharpened frame, where a blank frame is unambiguous and
   * where the threshold was calibrated. */
  fpc_unsharp (self->image_buf, self->width, self->height);

  enlarged = fp_image_new (self->width * FPC_ENLARGE_FACTOR,
                           self->height * FPC_ENLARGE_FACTOR);
  fpc_resize_catrom (self->image_buf, self->width, self->height,
                     FPC_ENLARGE_FACTOR, enlarged->data);

  /* Scan resolution, used by NBIS to size the neighbourhood it scores each
   * minutia's reliability over (RADIUS_MM * ppmm in mindtct/quality.c).
   * Left unset it is 0.0, collapsing that radius to zero pixels. The FPC
   * capacitive family is specified at 508 dpi, and the ridge period measured
   * on real captures is 10px = 0.50mm at that scale, which is the normal
   * human value -- so 508 dpi is confirmed, not assumed. Scaled by the
   * enlargement so the radius stays the same physical distance. */
  enlarged->ppmm = (508.0 / 25.4) * FPC_ENLARGE_FACTOR;

  fpi_image_device_image_captured (idev, enlarged);
  fpi_image_device_report_finger_status (idev, FALSE);

  g_clear_pointer (&self->image_buf, g_free);
}

/* Real USB bus reset (not the sensor's own soft "reset" opcode) to recover
 * from the sensor going unresponsive. Synchronous, matching the pattern
 * other libfprint drivers (e.g. vfs7552.c) use for device-level resets.
 *
 * KNOWN LIMITATION (2026-08-29 live testing): this is the only thing found
 * so far that actually restores the sensor's responsiveness once it stops
 * answering touches (matches manually unplugging/replugging the Type
 * Cover, which also always fixed it). A lighter CLEAR_FEATURE(ENDPOINT_HALT)
 * on both endpoints was tried first and does NOT restore touch detection
 * at all -- so whatever wedges the sensor is not a simple USB pipe stall.
 * The real cost of the full reset: it makes libfprint think the device
 * was disconnected, which aborts whatever enroll/verify session was in
 * progress. So this recovers the *device* but not the *session* -- a
 * multi-stage enrollment currently cannot survive one of these. The actual
 * root cause (why the sensor wedges after ~2-3 captures at all) is still
 * unknown; see PROTOCOL.md / project memory for the likely next lead (an
 * uninvestigated vtable call in the original Windows driver's reset
 * routine, on a different object than the one this function already
 * mirrors). Shipping the full reset anyway because never recovering at all
 * is worse than recovering with a session restart. */
static void
fpc_recover_wedged_device (FpDevice *dev)
{
  GUsbDevice *usb_dev = fpi_device_get_usb_device (dev);
  GError *error = NULL;

  fp_warn ("fpc1021: sensor stopped responding after %u timeouts, issuing USB reset",
           FPC_MAX_CONSECUTIVE_TIMEOUTS);

  g_usb_device_release_interface (usb_dev, FPC_USB_INTERFACE, 0, NULL);

  if (!g_usb_device_reset (usb_dev, &error))
    {
      fp_warn ("fpc1021: USB reset failed: %s", error->message);
      g_error_free (error);
    }

  if (!g_usb_device_claim_interface (usb_dev, FPC_USB_INTERFACE, 0, &error))
    {
      fp_warn ("fpc1021: could not re-claim interface after reset: %s", error->message);
      g_error_free (error);
    }
}

static void
capture_header_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                   gpointer user_data, GError *error)
{
  FpiDeviceFpc1021 *self = FPI_DEVICE_FPC1021 (dev);
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);
  guint16 status, substatus, length;
  gsize first_chunk;

  if (error)
    {
      /* The sensor does not answer this read at all until a finger is
       * actually on it -- a USB read timeout here is the normal "not ready
       * yet" signal (matches the Windows driver's own pending/retry
       * sentinel on this exact read), not a failure. Anything else is a
       * real error. */
      if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          g_error_free (error);
          self->consecutive_timeouts++;
          if (self->consecutive_timeouts >= FPC_MAX_CONSECUTIVE_TIMEOUTS)
            {
              fpc_recover_wedged_device (dev);
              self->consecutive_timeouts = 0;
            }
          fpi_ssm_mark_completed_delayed (transfer->ssm, FPC_RETRY_DELAY_MS);
          return;
        }
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  self->consecutive_timeouts = 0;

  if (transfer->actual_length < 4)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "short capture reply (%" G_GSSIZE_FORMAT " bytes)",
                                                     transfer->actual_length));
      return;
    }

  status = transfer->buffer[0] | (transfer->buffer[1] << 8);
  substatus = transfer->buffer[2] | (transfer->buffer[3] << 8);

  /* status is 0x1000 | <opcode>; 0x1007 acknowledges our capture (0x0007)
   * command specifically. Anything else (or substatus 5) means the sensor
   * isn't ready yet -- most likely no finger is present. This has not been
   * empirically distinguished from other "not ready" cases on real
   * hardware; treat it as retryable rather than an error. */
  if (status != 0x1007 || substatus == 5)
    {
      fpi_ssm_mark_completed_delayed (transfer->ssm, FPC_RETRY_DELAY_MS);
      return;
    }

  if (substatus != 0)
    {
      fp_warn ("capture rejected, substatus=0x%04x", substatus);
      fpi_image_device_retry_scan (idev, FP_DEVICE_RETRY_GENERAL);
      fpi_ssm_mark_completed_delayed (transfer->ssm, FPC_RETRY_DELAY_MS);
      return;
    }

  if (transfer->actual_length < 6)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "capture reply missing length field"));
      return;
    }

  length = transfer->buffer[4] | (transfer->buffer[5] << 8);
  if (length == 0 || length > (guint16) (self->width * self->height))
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "implausible capture length %u", length));
      return;
    }

  g_clear_pointer (&self->image_buf, g_free);
  self->image_len = length;
  self->image_buf = g_malloc0 (length);
  self->image_have = 0;

  first_chunk = transfer->actual_length - 6;
  if (first_chunk > length)
    first_chunk = length;
  if (first_chunk > 0)
    {
      memcpy (self->image_buf, transfer->buffer + 6, first_chunk);
      self->image_have = first_chunk;
    }

  fpi_image_device_report_finger_status (idev, TRUE);
  fpi_ssm_next_state (transfer->ssm);
}

static void
capture_stream_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                   gpointer user_data, GError *error)
{
  FpiDeviceFpc1021 *self = FPI_DEVICE_FPC1021 (dev);
  gssize usable;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* The first 2 bytes of every continuation packet are a per-packet marker,
   * not payload -- see PROTOCOL.md. */
  usable = transfer->actual_length - 2;
  if (usable < 0)
    usable = 0;
  if ((gsize) usable > self->image_len - self->image_have)
    usable = self->image_len - self->image_have;
  if (usable > 0)
    memcpy (self->image_buf + self->image_have, transfer->buffer + 2, usable);
  self->image_have += usable;

  fpi_ssm_jump_to_state (transfer->ssm, CAPTURE_STREAM);
}

/* Reassembles a drained image, when asked to.
 *
 * The drain throws away 413 packets before most captures: one capture header
 * plus 412 stream packets, which is a complete unrequested 160x160 frame.
 * Where it comes from is still unexplained, and the frame itself has never
 * been examined -- it has only ever been counted and dropped.
 *
 * It is worth examining for a second reason. Two frames of one press cover
 * the same skin, so averaging them cannot add area, but it can average away
 * sensor noise -- and noise amplified into minutiae that differ between two
 * views of a finger is the failure this driver's matching actually has. The
 * offline bench can answer whether that helps, given the frames.
 *
 * Set FPC1021_GHOST_DIR to a writable directory to collect them. Unset, this
 * costs one comparison per drained packet and changes nothing.
 */
static void
fpc_ghost_feed (FpiDeviceFpc1021 *self, const guint8 *buf, gint len)
{
  const gchar *dir = g_getenv ("FPC1021_GHOST_DIR");

  if (!dir || len <= 0)
    return;

  /* A capture header starts a frame; anything else before one is some other
   * command's stale reply and is not ours to interpret. */
  if (!self->ghost_buf)
    {
      guint16 status, length;

      if (len < 6)
        return;
      status = buf[0] | (buf[1] << 8);
      if (status != (0x1000 | CMD_CAPTURE))
        return;

      length = buf[4] | (buf[5] << 8);
      if (length == 0 || length > (guint16) (self->width * self->height))
        return;

      self->ghost_len = length;
      self->ghost_buf = g_malloc0 (length);
      self->ghost_have = MIN ((gsize) (len - 6), self->ghost_len);
      memcpy (self->ghost_buf, buf + 6, self->ghost_have);
    }
  else
    {
      /* Continuation packets carry a 2-byte marker, as in capture_stream_cb. */
      gsize usable = len > 2 ? (gsize) (len - 2) : 0;

      usable = MIN (usable, self->ghost_len - self->ghost_have);
      if (usable)
        memcpy (self->ghost_buf + self->ghost_have, buf + 2, usable);
      self->ghost_have += usable;
    }

  if (self->ghost_have < self->ghost_len)
    return;

  {
    g_autofree gchar *path = g_strdup_printf ("%s/ghost-%u-%u.bin", dir,
                                              (guint) getpid (), self->ghost_seq++);
    g_autoptr(GError) error = NULL;

    if (g_file_set_contents (path, (const gchar *) self->ghost_buf,
                             (gssize) self->ghost_len, &error))
      fp_warn ("fpc1021: wrote ghost frame %s (%zu bytes)", path, self->ghost_len);
    else
      fp_warn ("fpc1021: could not write ghost frame: %s", error->message);
  }

  g_clear_pointer (&self->ghost_buf, g_free);
  self->ghost_have = 0;
  self->ghost_len = 0;
}

/* Empties the IN endpoint before starting a capture.
 *
 * Root cause this defends against (2026-08-29, usbmon trace of a real
 * fprintd-enroll): after two captures that were each read out in full, the
 * sensor delivered a third capture header nobody had asked for. Because the
 * protocol pairs reads and writes 1:1, that one unread packet put every
 * later reply behind ~412 stale stream packets: Reset answered by pixel
 * data, Get Chip ID answered by stream packets, and finally silence and
 * endless timeouts -- the state this driver had been calling a wedge and
 * "recovering" from with a USB reset that aborts the enrollment.
 *
 * Where that extra frame comes from is still unknown; it did not reproduce
 * over plain libusb in 21 captures, including four paced to match
 * libfprint's own ~7.5ms/packet drain. Draining does not need to know: it
 * makes an unread packet harmless instead of fatal, which is what stops the
 * enrollment dying.
 */
static void
fpc_drain_cb (FpiUsbTransfer *transfer, FpDevice *dev,
              gpointer user_data, GError *error)
{
  FpiDeviceFpc1021 *self = FPI_DEVICE_FPC1021 (dev);

  if (error)
    {
      /* A timeout is the expected outcome: the endpoint is empty, which is
       * the normal case on every capture after the first. */
      if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          g_error_free (error);
          if (self->drained)
            fp_warn ("fpc1021: drained %u stale packet(s) before capture",
                     self->drained);
          fpi_ssm_next_state (transfer->ssm);
          return;
        }
      /* Anything else is not worth failing a capture over -- the capture
       * itself will report a real problem soon enough. */
      g_error_free (error);
      fpi_ssm_next_state (transfer->ssm);
      return;
    }

  fpc_ghost_feed (self, transfer->buffer, (gint) transfer->actual_length);

  self->drained++;
  if (self->drained >= FPC_DRAIN_MAX_PACKETS)
    {
      fp_warn ("fpc1021: still draining after %u packets, giving up",
               self->drained);
      fpi_ssm_next_state (transfer->ssm);
      return;
    }

  fpi_ssm_jump_to_state (transfer->ssm, fpi_ssm_get_cur_state (transfer->ssm));
}

static void
capture_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFpc1021 *self = FPI_DEVICE_FPC1021 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPTURE_DRAIN:
      /* Short timeout: this only has to collect what is already queued. */
      fpc_read_reply_timeout (ssm, dev, FPC_MAX_PACKET, FPC_DRAIN_TIMEOUT_MS,
                              fpc_drain_cb);
      break;

    case CAPTURE_RESET:
      fpc_write_cmd (ssm, dev, CMD_RESET);
      break;

    case CAPTURE_RESET_ACK:
      /* Consume the reset command's own reply (status 0x1008) before
       * continuing; its content isn't needed. */
      fpc_read_reply (ssm, dev, FPC_MAX_PACKET, fpi_ssm_usb_transfer_cb);
      break;

    case CAPTURE_TRIGGER:
      fpc_write_cmd (ssm, dev, CMD_CAPTURE);
      break;

    case CAPTURE_READ_HEADER:
      /* Short timeout: this read simply doesn't complete until a finger is
       * on the sensor, so a "timeout" here is the normal polling case, not
       * an error (see capture_header_cb). Keeping it short makes the
       * await-finger retry loop responsive. */
      fpc_read_reply_timeout (ssm, dev, FPC_MAX_PACKET, 3000, capture_header_cb);
      break;

    case CAPTURE_STREAM:
      {
        gsize remaining = self->image_len - self->image_have;

        if (remaining == 0)
          {
            deliver_image (dev);
            fpi_ssm_mark_completed_delayed (ssm, FPC_CAPTURE_COOLDOWN_MS);
            break;
          }

        gsize reqlen = remaining + 2;
        if (reqlen > FPC_MAX_PACKET)
          reqlen = FPC_MAX_PACKET;

        FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
        fpi_usb_transfer_fill_bulk (transfer, FPC_EP_IN, reqlen);
        transfer->ssm = ssm;
        transfer->short_is_error = TRUE;
        fpi_usb_transfer_submit (transfer, FPC_USB_TIMEOUT, NULL,
                                 capture_stream_cb, NULL);
        break;
      }
    }
}

static void
capture_sm_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);
  FpiDeviceFpc1021 *self = FPI_DEVICE_FPC1021 (dev);

  if (self->deactivating)
    {
      self->deactivating = FALSE;
      g_clear_error (&error);
      fpi_image_device_deactivate_complete (idev, NULL);
      return;
    }

  if (error)
    {
      fpi_image_device_session_error (idev, error);
      return;
    }

  /* No image this round (not ready / rejected) or one was just delivered --
   * either way, loop back and try again while the device stays active. */
  start_capture (idev);
}

static void
start_capture (FpImageDevice *idev)
{
  FpiDeviceFpc1021 *self = FPI_DEVICE_FPC1021 (idev);
  FpiSsm *ssm;

  g_clear_pointer (&self->image_buf, g_free);
  self->image_len = 0;
  self->image_have = 0;
  self->drained = 0;

  /* A drain that stopped mid-frame leaves a partial ghost behind; it belongs
   * to the previous capture and must not be continued into this one. */
  g_clear_pointer (&self->ghost_buf, g_free);
  self->ghost_len = 0;
  self->ghost_have = 0;

  ssm = fpi_ssm_new (FP_DEVICE (idev), capture_run_state, CAPTURE_NUM_STATES);
  fpi_ssm_start (ssm, capture_sm_complete);
}

static void
dev_activate (FpImageDevice *idev)
{
  FpiDeviceFpc1021 *self = FPI_DEVICE_FPC1021 (idev);

  self->deactivating = FALSE;
  fpi_image_device_activate_complete (idev, NULL);
  start_capture (idev);
}

static void
dev_deactivate (FpImageDevice *idev)
{
  FpiDeviceFpc1021 *self = FPI_DEVICE_FPC1021 (idev);

  /* The currently running capture SSM will notice this flag once it next
   * completes (at most ~FPC_CAPTURE_COOLDOWN_MS away) and call
   * fpi_image_device_deactivate_complete() instead of looping again. There
   * is nothing chip-specific to send on deactivate in the protocol as
   * currently understood. */
  self->deactivating = TRUE;
}

/* ---------------------------------------------------------------------- */
/* Class/type boilerplate                                                 */
/* ---------------------------------------------------------------------- */

static const FpIdEntry id_table[] = {
  { .vid = 0x045e, .pid = 0x09c2 },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

static void
fpi_device_fpc1021_init (FpiDeviceFpc1021 *self)
{
}

static void
fpi_device_fpc1021_class_init (FpiDeviceFpc1021Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id = "fpc1021";
  dev_class->full_name = "FPC1021 (Surface Type Cover with Fingerprint ID)";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  img_class->img_open = dev_open;
  img_class->img_close = dev_close;
  img_class->activate = dev_activate;
  img_class->deactivate = dev_deactivate;

  /* Not a tuned threshold: a refusal.
   *
   * This sensor does not separate fingers. Over 146 genuine and 160 impostor
   * comparisons of well-placed captures, the highest genuine score and the
   * highest impostor score are the same number -- 92 -- so no threshold
   * admits any genuine match without also admitting the best impostor. At
   * the 24 this driver used to carry, good captures were accepted 97% of the
   * time and *strangers* 92% of the time; the apparent safety of 24 came
   * from poor captures scoring low, not from the threshold doing anything.
   *
   * G_MAXINT rather than a large-looking number because the intent must not
   * read as tuning. A comparison of an image against itself scores 1415-1606
   * on these frames, so anything in the hundreds is reachable and would be a
   * guess dressed as a bound.
   *
   * Do not lower this to make verification "work". What would justify a real
   * threshold is a separation measurement -- tools/fpc_bench.c reports TAR at
   * a capped FAR -- and the ones taken so far say there is nothing to
   * separate. See libfprint-driver/README.md.
   */
  img_class->bz3_threshold = G_MAXINT;

  /* Resolution varies by exact FPC chip; determined at img_open time via
   * Get Chip ID and stored per-instance (self->width/height), not here. */
  img_class->img_width = -1;
  img_class->img_height = -1;
}
