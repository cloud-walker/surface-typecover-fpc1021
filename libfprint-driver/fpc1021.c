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

static void
deliver_image (FpDevice *dev)
{
  FpiDeviceFpc1021 *self = FPI_DEVICE_FPC1021 (dev);
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);
  FpImage *img = fp_image_new (self->width, self->height);

  memcpy (img->data, self->image_buf, self->image_len);

  /* 160x160 over roughly 8mm of finger is a partial scan: the edge of the
   * image is not the edge of the finger, so every ridge running out of
   * frame looks like a ridge ending. FPI_IMAGE_PARTIAL tells NBIS to drop
   * those perimeter artefacts instead of matching on them -- the same thing
   * elan.c and elanspi.c do for their small press sensors. Without it the
   * spurious minutiae pollute both the enrolled template and the image
   * being verified against it. */
  img->flags |= FPI_IMAGE_PARTIAL;

  /* Scan resolution, used by NBIS to size the neighbourhood it scores each
   * minutia's reliability over. Left unset it is 0.0, which collapses that
   * radius to zero pixels. The FPC capacitive family is specified at
   * 508 dpi, i.e. 160 px across ~8mm; worth re-checking against the
   * datasheet if matching stays weak. */
  img->ppmm = 508.0 / 25.4;

  fpi_image_device_image_captured (idev, img);
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

  img_class->bz3_threshold = 24;

  /* Resolution varies by exact FPC chip; determined at img_open time via
   * Get Chip ID and stored per-instance (self->width/height), not here. */
  img_class->img_width = -1;
  img_class->img_height = -1;
}
