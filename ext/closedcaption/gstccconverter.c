/*
 * GStreamer
 * Copyright (C) 2018 Sebastian Dröge <sebastian@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */


#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>
#include <gst/base/base.h>
#include <gst/video/video.h>
#include <string.h>

#include "gstccconverter.h"

GST_DEBUG_CATEGORY_STATIC (gst_cc_converter_debug);
#define GST_CAT_DEFAULT gst_cc_converter_debug

/* Ordered by the amount of information they can contain */
#define CC_CAPS \
        "closedcaption/x-cea-708,format=(string) cdp; " \
        "closedcaption/x-cea-708,format=(string) cc_data; " \
        "closedcaption/x-cea-608,format=(string) s334-1a; " \
        "closedcaption/x-cea-608,format=(string) raw"

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (CC_CAPS));

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (CC_CAPS));

G_DEFINE_TYPE (GstCCConverter, gst_cc_converter, GST_TYPE_BASE_TRANSFORM);
#define parent_class gst_cc_converter_parent_class

static gboolean
gst_cc_converter_transform_size (GstBaseTransform * base,
    GstPadDirection direction,
    GstCaps * caps, gsize size, GstCaps * othercaps, gsize * othersize)
{
  /* We can't really convert from an output size to an input size */
  if (direction != GST_PAD_SINK)
    return FALSE;

  /* Assume worst-case here and over-allocate, and in ::transform() we then
   * downsize the buffer as needed. The worst-case is one CDP packet, which
   * can be up to MAX_CDP_PACKET_LEN bytes large */

  *othersize = MAX_CDP_PACKET_LEN;

  return TRUE;
}

static GstCaps *
gst_cc_converter_transform_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  static GstStaticCaps non_cdp_caps =
      GST_STATIC_CAPS ("closedcaption/x-cea-708, format=(string)cc_data; "
      "closedcaption/x-cea-608,format=(string) s334-1a; "
      "closedcaption/x-cea-608,format=(string) raw");
  static GstStaticCaps cdp_caps =
      GST_STATIC_CAPS ("closedcaption/x-cea-708, format=(string)cdp");
  static GstStaticCaps cdp_caps_framerate =
      GST_STATIC_CAPS ("closedcaption/x-cea-708, format=(string)cdp, "
      "framerate=(fraction){60/1, 60000/1001, 50/1, 30/1, 30000/1001, 25/1, 24/1, 24000/1001}");

  GstCCConverter *self = GST_CCCONVERTER (base);
  guint i, n;
  GstCaps *res, *templ;

  templ = gst_pad_get_pad_template_caps (base->srcpad);

  res = gst_caps_new_empty ();
  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    const GstStructure *s = gst_caps_get_structure (caps, i);
    const GValue *framerate = gst_structure_get_value (s, "framerate");

    if (gst_structure_has_name (s, "closedcaption/x-cea-608")) {

      if (direction == GST_PAD_SRC) {
        /* SRC direction: We produce upstream caps
         *
         * Downstream wanted CEA608 caps. If it had a framerate, we
         * also need upstream to provide exactly that same framerate
         * and otherwise we don't care.
         *
         * We can convert everything to CEA608.
         */
        res = gst_caps_merge (res, gst_static_caps_get (&cdp_caps_framerate));
        res = gst_caps_merge (res, gst_static_caps_get (&non_cdp_caps));
      } else {
        /* SINK: We produce downstream caps
         *
         * Upstream provided CEA608 caps. We can convert that to CDP if
         * also a CDP compatible framerate was provided, and we can convert
         * it to anything else regardless.
         *
         * If upstream provided a framerate we can pass that through, possibly
         * filtered for the CDP case.
         */
        if (framerate) {
          GstCaps *tmp;
          GstStructure *t;

          /* Create caps that contain the intersection of all framerates with
           * the CDP allowed framerates */
          tmp =
              gst_caps_make_writable (gst_static_caps_get
              (&cdp_caps_framerate));
          t = gst_caps_get_structure (tmp, 0);
          gst_structure_set_name (t, "closedcaption/x-cea-608");
          gst_structure_remove_field (t, "format");
          if (gst_structure_can_intersect (s, t)) {
            gst_caps_unref (tmp);

            tmp =
                gst_caps_make_writable (gst_static_caps_get
                (&cdp_caps_framerate));

            res = gst_caps_merge (res, tmp);
          } else {
            gst_caps_unref (tmp);
          }
          /* And we can convert to everything else with the given framerate */
          tmp = gst_caps_make_writable (gst_static_caps_get (&non_cdp_caps));
          gst_caps_set_value (tmp, "framerate", framerate);
          res = gst_caps_merge (res, tmp);
        } else {
          res = gst_caps_merge (res, gst_static_caps_get (&non_cdp_caps));
        }
      }
    } else if (gst_structure_has_name (s, "closedcaption/x-cea-708")) {
      if (direction == GST_PAD_SRC) {
        /* SRC direction: We produce upstream caps
         *
         * Downstream wanted CEA708 caps. If downstream wants *only* CDP we
         * either need CDP from upstream, or anything else with a CDP
         * framerate.
         * If downstream also wants non-CDP we can accept anything.
         *
         * We pass through any framerate as-is, except for filtering
         * for CDP framerates if downstream wants only CDP.
         */

        if (g_strcmp0 (gst_structure_get_string (s, "format"), "cdp") == 0) {
          /* Downstream wants only CDP */

          /* We need CDP from upstream in that case */
          res = gst_caps_merge (res, gst_static_caps_get (&cdp_caps_framerate));

          /* Or anything else with a CDP framerate */
          if (framerate) {
            GstCaps *tmp;
            GstStructure *t;
            const GValue *cdp_framerate;

            /* Create caps that contain the intersection of all framerates with
             * the CDP allowed framerates */
            tmp =
                gst_caps_make_writable (gst_static_caps_get
                (&cdp_caps_framerate));
            t = gst_caps_get_structure (tmp, 0);

            /* There's an intersection between the framerates so we can convert
             * into CDP with exactly those framerates from anything else */
            cdp_framerate = gst_structure_get_value (t, "framerate");
            tmp = gst_caps_make_writable (gst_static_caps_get (&non_cdp_caps));
            gst_caps_set_value (tmp, "framerate", cdp_framerate);
            res = gst_caps_merge (res, tmp);
          } else {
            GstCaps *tmp, *cdp_caps;
            const GValue *cdp_framerate;

            /* Get all CDP framerates, we can accept anything that has those
             * framerates */
            cdp_caps = gst_static_caps_get (&cdp_caps_framerate);
            cdp_framerate =
                gst_structure_get_value (gst_caps_get_structure (cdp_caps, 0),
                "framerate");

            tmp = gst_caps_make_writable (gst_static_caps_get (&non_cdp_caps));
            gst_caps_set_value (tmp, "framerate", cdp_framerate);
            gst_caps_unref (cdp_caps);

            res = gst_caps_merge (res, tmp);
          }
        } else {
          /* Downstream wants not only CDP, we can do everything */
          res = gst_caps_merge (res, gst_static_caps_get (&cdp_caps_framerate));
          res = gst_caps_merge (res, gst_static_caps_get (&non_cdp_caps));
        }
      } else {
        GstCaps *tmp;

        /* SINK: We produce downstream caps
         *
         * Upstream provided CEA708 caps. If upstream provided CDP we can
         * output CDP, no matter what (-> passthrough). If upstream did not
         * provide CDP, we can output CDP only if the framerate fits.
         * We can always produce everything else apart from CDP.
         *
         * If upstream provided a framerate we pass that through for non-CDP
         * output, and pass it through filtered for CDP output.
         */

        if (gst_structure_can_intersect (s,
                gst_caps_get_structure (gst_static_caps_get (&cdp_caps), 0))) {
          /* Upstream provided CDP caps, we can do everything independent of
           * framerate */
          res = gst_caps_merge (res, gst_static_caps_get (&cdp_caps_framerate));
        } else if (framerate) {
          const GValue *cdp_framerate;
          GstStructure *t;

          /* Upstream did not provide CDP. We can only do CDP if upstream
           * happened to have a CDP framerate */

          /* Create caps that contain the intersection of all framerates with
           * the CDP allowed framerates */
          tmp =
              gst_caps_make_writable (gst_static_caps_get
              (&cdp_caps_framerate));
          t = gst_caps_get_structure (tmp, 0);

          /* There's an intersection between the framerates so we can convert
           * into CDP with exactly those framerates */
          cdp_framerate = gst_structure_get_value (t, "framerate");
          gst_caps_set_value (tmp, "framerate", cdp_framerate);

          res = gst_caps_merge (res, tmp);
        }
        /* We can always convert CEA708 to all non-CDP formats */
        res = gst_caps_merge (res, gst_static_caps_get (&non_cdp_caps));
      }
    } else {
      g_assert_not_reached ();
    }
  }

  GST_DEBUG_OBJECT (self, "pre filter caps %" GST_PTR_FORMAT, res);

  /* We can convert anything into anything but it might involve loss of
   * information so always filter according to the order in our template caps
   * in the end */
  if (filter) {
    GstCaps *tmp;
    filter = gst_caps_intersect_full (templ, filter, GST_CAPS_INTERSECT_FIRST);

    tmp = gst_caps_intersect_full (filter, res, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (res);
    gst_caps_unref (filter);
    res = tmp;
  }

  gst_caps_unref (templ);

  GST_DEBUG_OBJECT (self, "Transformed in direction %s caps %" GST_PTR_FORMAT,
      direction == GST_PAD_SRC ? "src" : "sink", caps);
  GST_DEBUG_OBJECT (self, "filter %" GST_PTR_FORMAT, filter);
  GST_DEBUG_OBJECT (self, "to %" GST_PTR_FORMAT, res);

  return res;
}

static GstCaps *
gst_cc_converter_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * incaps, GstCaps * outcaps)
{
  GstCCConverter *self = GST_CCCONVERTER (base);
  const GstStructure *s;
  GstStructure *t;
  const GValue *framerate;
  GstCaps *intersection, *templ;

  GST_DEBUG_OBJECT (self, "Fixating in direction %s incaps %" GST_PTR_FORMAT,
      direction == GST_PAD_SRC ? "src" : "sink", incaps);
  GST_DEBUG_OBJECT (self, "and outcaps %" GST_PTR_FORMAT, outcaps);

  /* Prefer passthrough if we can */
  if (gst_caps_is_subset (incaps, outcaps)) {
    gst_caps_unref (outcaps);
    return GST_BASE_TRANSFORM_CLASS (parent_class)->fixate_caps (base,
        direction, incaps, gst_caps_ref (incaps));
  }

  /* Otherwise prefer caps in the order of our template caps */
  templ = gst_pad_get_pad_template_caps (base->srcpad);
  intersection =
      gst_caps_intersect_full (templ, outcaps, GST_CAPS_INTERSECT_FIRST);
  gst_caps_unref (outcaps);
  outcaps = intersection;

  outcaps =
      GST_BASE_TRANSFORM_CLASS (parent_class)->fixate_caps (base, direction,
      incaps, outcaps);

  /* remove any framerate that might've been added by basetransform due to
   * intersecting with downstream */
  s = gst_caps_get_structure (incaps, 0);
  framerate = gst_structure_get_value (s, "framerate");
  outcaps = gst_caps_make_writable (outcaps);
  t = gst_caps_get_structure (outcaps, 0);
  if (!framerate) {
    gst_structure_remove_field (t, "framerate");
  }

  GST_DEBUG_OBJECT (self,
      "Fixated caps %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT, incaps, outcaps);

  return outcaps;
}

static gboolean
gst_cc_converter_set_caps (GstBaseTransform * base, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstCCConverter *self = GST_CCCONVERTER (base);
  const GstStructure *s;
  gboolean passthrough;

  self->input_caption_type = gst_video_caption_type_from_caps (incaps);
  self->output_caption_type = gst_video_caption_type_from_caps (outcaps);

  if (self->input_caption_type == GST_VIDEO_CAPTION_TYPE_UNKNOWN ||
      self->output_caption_type == GST_VIDEO_CAPTION_TYPE_UNKNOWN)
    goto invalid_caps;

  s = gst_caps_get_structure (incaps, 0);
  if (!gst_structure_get_fraction (s, "framerate", &self->in_fps_n,
          &self->in_fps_d))
    self->in_fps_n = self->in_fps_d = 0;

  s = gst_caps_get_structure (outcaps, 0);
  if (!gst_structure_get_fraction (s, "framerate", &self->out_fps_n,
          &self->out_fps_d))
    self->out_fps_n = self->out_fps_d = 0;

  gst_video_time_code_clear (&self->current_output_timecode);

  /* Caps can be different but we can passthrough as long as they can
   * intersect, i.e. have same caps name and format */
  passthrough = gst_caps_can_intersect (incaps, outcaps);
  gst_base_transform_set_passthrough (base, passthrough);

  GST_DEBUG_OBJECT (self,
      "Got caps %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT " (passthrough %d)",
      incaps, outcaps, passthrough);

  return TRUE;

invalid_caps:
  {
    GST_ERROR_OBJECT (self,
        "Invalid caps: in %" GST_PTR_FORMAT " out: %" GST_PTR_FORMAT, incaps,
        outcaps);
    return FALSE;
  }
}

struct cdp_fps_entry
{
  guint8 fps_idx;
  guint fps_n, fps_d;
  guint max_cc_count;
};

static const struct cdp_fps_entry cdp_fps_table[] = {
  {0x1f, 24000, 1001, 25},
  {0x2f, 24, 1, 25},
  {0x3f, 25, 1, 24},
  {0x4f, 30000, 1001, 20},
  {0x5f, 30, 1, 20},
  {0x6f, 50, 1, 12},
  {0x7f, 60000, 1001, 10},
  {0x8f, 60, 1, 10},
};
static const struct cdp_fps_entry null_fps_entry = { 0, 0, 0, 0 };

static const struct cdp_fps_entry *
cdp_fps_entry_from_id (guint8 id)
{
  int i;
  for (i = 0; i < G_N_ELEMENTS (cdp_fps_table); i++) {
    if (cdp_fps_table[i].fps_idx == id)
      return &cdp_fps_table[i];
  }
  return &null_fps_entry;
}

static const struct cdp_fps_entry *
cdp_fps_entry_from_fps (guint fps_n, guint fps_d)
{
  int i;
  for (i = 0; i < G_N_ELEMENTS (cdp_fps_table); i++) {
    if (cdp_fps_table[i].fps_n == fps_n && cdp_fps_table[i].fps_d == fps_d)
      return &cdp_fps_table[i];
  }
  return &null_fps_entry;
}

static void
get_framerate_output_scale (GstCCConverter * self,
    const struct cdp_fps_entry *in_fps_entry, gint * scale_n, gint * scale_d)
{
  if (self->in_fps_n == 0 || self->out_fps_d == 0) {
    *scale_n = 1;
    *scale_d = 1;
    return;
  }

  /* compute the relative rates of the two framerates */
  if (!gst_util_fraction_multiply (in_fps_entry->fps_d, in_fps_entry->fps_n,
          self->out_fps_n, self->out_fps_d, scale_n, scale_d))
    /* we should never overflow */
    g_assert_not_reached ();
}

static gboolean
interpolate_time_code_with_framerate (GstCCConverter * self,
    const GstVideoTimeCode * tc, gint out_fps_n, gint out_fps_d,
    gint scale_n, gint scale_d, GstVideoTimeCode * out)
{
  gchar *tc_str;
  gint output_n, output_d;
  guint output_frame;
  GstVideoTimeCodeFlags flags;

  g_return_val_if_fail (tc != NULL, FALSE);
  g_return_val_if_fail (out != NULL, FALSE);
  /* out_n/d can only be 0 if scale_n/d are 1/1 */
  g_return_val_if_fail ((scale_n == 1 && scale_d == 1) || (out_fps_n != 0
          && out_fps_d != 0), FALSE);

  if (!tc || tc->config.fps_n == 0)
    return FALSE;

  gst_util_fraction_multiply (tc->frames, 1, scale_n, scale_d, &output_n,
      &output_d);

  tc_str = gst_video_time_code_to_string (tc);
  GST_TRACE_OBJECT (self, "interpolating time code %s with scale %d/%d "
      "to frame %d/%d", tc_str, scale_n, scale_d, output_n, output_d);
  g_free (tc_str);

  if (out_fps_n == 0 || out_fps_d == 0) {
    out_fps_n = tc->config.fps_n;
    out_fps_d = tc->config.fps_d;
  }

  flags = tc->config.flags;
  if ((flags & GST_VIDEO_TIME_CODE_FLAGS_DROP_FRAME) != 0 && out_fps_d != 1001
      && out_fps_n != 60000 && out_fps_n != 30000) {
    flags &= ~GST_VIDEO_TIME_CODE_FLAGS_DROP_FRAME;
  } else if ((flags & GST_VIDEO_TIME_CODE_FLAGS_DROP_FRAME) == 0
      && out_fps_d == 1001 && (out_fps_n == 60000 || out_fps_n == 30000)) {
    /* XXX: theoretically, not quite correct however this is an assumption
     * we have elsewhere that these framerates are always drop-framed */
    flags |= GST_VIDEO_TIME_CODE_FLAGS_DROP_FRAME;
  }

  output_frame = output_n / output_d;

  *out = (GstVideoTimeCode) GST_VIDEO_TIME_CODE_INIT;
  do {
    /* here we try to find the next available valid timecode.  The dropped
     * (when they exist) frames in time codes are that the beginning of each
     * minute */
    gst_video_time_code_clear (out);
    gst_video_time_code_init (out, out_fps_n, out_fps_d,
        tc->config.latest_daily_jam, flags, tc->hours, tc->minutes,
        tc->seconds, output_frame, tc->field_count);
    output_frame++;
  } while ((flags & GST_VIDEO_TIME_CODE_FLAGS_DROP_FRAME) != 0
      && output_frame < 10 && !gst_video_time_code_is_valid (out));

  tc_str = gst_video_time_code_to_string (out);
  GST_TRACE_OBJECT (self, "interpolated to %s", tc_str);
  g_free (tc_str);

  return TRUE;
}

/* remove padding bytes from a cc_data packet. Returns the length of the new
 * data in @cc_data */
static guint
compact_cc_data (guint8 * cc_data, guint cc_data_len)
{
  gboolean started_ccp = FALSE;
  guint out_len = 0;
  guint i;

  if (cc_data_len % 3 != 0) {
    GST_WARNING ("Invalid cc_data buffer size");
    cc_data_len = cc_data_len - (cc_data_len % 3);
  }

  for (i = 0; i < cc_data_len / 3; i++) {
    gboolean cc_valid = (cc_data[i * 3] & 0x04) == 0x04;
    guint8 cc_type = cc_data[i * 3] & 0x03;

    if (!started_ccp && cc_valid && (cc_type == 0x00 || cc_type == 0x01)) {
      /* skip over 608 data */
      cc_data[out_len++] = cc_data[i * 3];
      cc_data[out_len++] = cc_data[i * 3 + 1];
      cc_data[out_len++] = cc_data[i * 3 + 2];
      continue;
    }

    if (cc_type & 0x10)
      started_ccp = TRUE;

    if (!cc_valid)
      continue;

    cc_data[out_len++] = cc_data[i * 3];
    cc_data[out_len++] = cc_data[i * 3 + 1];
    cc_data[out_len++] = cc_data[i * 3 + 2];
  }

  return out_len;
}

/* takes cc_data and cc_data_len and attempts to fit it into a hypothetical
 * output packet.  Any leftover data is stored for later addition.  Returns
 * the number of bytes of @cc_data to place in a new output packet */
static gint
fit_and_scale_cc_data (GstCCConverter * self,
    const struct cdp_fps_entry *in_fps_entry,
    const struct cdp_fps_entry *out_fps_entry, const guint8 * cc_data,
    guint cc_data_len, const GstVideoTimeCode * tc)
{
  if (!in_fps_entry || in_fps_entry->fps_n == 0) {
    in_fps_entry = cdp_fps_entry_from_fps (self->in_fps_n, self->in_fps_d);
    if (!in_fps_entry || in_fps_entry->fps_n == 0)
      g_assert_not_reached ();
  }

  /* This is slightly looser than checking for the exact framerate as the cdp
   * spec allow for 0.1% difference between framerates to be considered equal */
  if (in_fps_entry->max_cc_count == out_fps_entry->max_cc_count) {
    if (tc && tc->config.fps_n != 0)
      interpolate_time_code_with_framerate (self, tc, out_fps_entry->fps_n,
          out_fps_entry->fps_d, 1, 1, &self->current_output_timecode);
  } else {
    int input_frame_n, input_frame_d, output_frame_n, output_frame_d;
    int output_time_cmp, scale_n, scale_d, rate_cmp;

    /* TODO: handle input discont */

    /* compute the relative frame count for each */
    if (!gst_util_fraction_multiply (self->in_fps_d, self->in_fps_n,
            self->input_frames, 1, &input_frame_n, &input_frame_d))
      /* we should never overflow */
      g_assert_not_reached ();

    if (!gst_util_fraction_multiply (self->out_fps_d, self->out_fps_n,
            self->output_frames + 1, 1, &output_frame_n, &output_frame_d))
      /* we should never overflow */
      g_assert_not_reached ();

    output_time_cmp = gst_util_fraction_compare (input_frame_n, input_frame_d,
        output_frame_n, output_frame_d);

    /* compute the relative rates of the two framerates */
    get_framerate_output_scale (self, in_fps_entry, &scale_n, &scale_d);

    rate_cmp = gst_util_fraction_compare (scale_n, scale_d, 1, 1);

    GST_TRACE_OBJECT (self, "performing framerate conversion at scale %d/%d "
        "of cc data", scale_n, scale_d);

    if (rate_cmp == 0) {
      /* we are not scaling. Should never happen with current conditions
       * above */
      g_assert_not_reached ();
    } else if (output_time_cmp == 0) {
      /* we have completed a cycle and can reset our counters to avoid
       * overflow. Anything that fits into the output packet will be written */
      GST_LOG_OBJECT (self, "cycle completed, resetting frame counters");
      self->scratch_len = 0;
      self->input_frames = self->output_frames = 0;
      if (tc->config.fps_n != 0) {
        interpolate_time_code_with_framerate (self, tc, out_fps_entry->fps_n,
            out_fps_entry->fps_d, scale_n, scale_d,
            &self->current_output_timecode);
      }
    } else if (output_time_cmp < 0) {
      /* we can't generate an output yet */
      self->scratch_len = cc_data_len;
      GST_DEBUG_OBJECT (self, "holding cc_data of len %u until next input "
          "buffer", self->scratch_len);
      memcpy (self->scratch, cc_data, self->scratch_len);
      return -1;
    } else if (rate_cmp != 0) {
      /* we are changing the framerate and may overflow the max output packet
       * size. Split them where necessary. */

      if (cc_data_len / 3 > out_fps_entry->max_cc_count) {
        /* packet would overflow, push extra bytes into the next packet */
        self->scratch_len = cc_data_len - 3 * out_fps_entry->max_cc_count;
        GST_DEBUG_OBJECT (self, "buffer would overflow by %u bytes (max "
            "length %u)", self->scratch_len, 3 * out_fps_entry->max_cc_count);
        memcpy (self->scratch, &cc_data[3 * out_fps_entry->max_cc_count],
            self->scratch_len);
        cc_data_len = 3 * out_fps_entry->max_cc_count;
      } else {
        GST_DEBUG_OBJECT (self, "packet length of %u fits within max output "
            "packet size %u", cc_data_len, 3 * out_fps_entry->max_cc_count);
        self->scratch_len = 0;
      }
    } else {
      g_assert_not_reached ();
    }

    if (tc && tc->config.fps_n != 0)
      interpolate_time_code_with_framerate (self, tc, out_fps_entry->fps_n,
          out_fps_entry->fps_d, scale_n, scale_d,
          &self->current_output_timecode);
  }

  return cc_data_len;
}

/* Converts raw CEA708 cc_data and an optional timecode into CDP */
static guint
convert_cea708_cc_data_cea708_cdp_internal (GstCCConverter * self,
    const guint8 * cc_data, guint cc_data_len, guint8 * cdp, guint cdp_len,
    const GstVideoTimeCode * tc, const struct cdp_fps_entry *fps_entry)
{
  GstByteWriter bw;
  guint8 flags, checksum;
  guint i, len;

  GST_DEBUG_OBJECT (self, "writing out cdp packet from cc_data with length %u",
      cc_data_len);

  gst_byte_writer_init_with_data (&bw, cdp, cdp_len, FALSE);
  gst_byte_writer_put_uint16_be_unchecked (&bw, 0x9669);
  /* Write a length of 0 for now */
  gst_byte_writer_put_uint8_unchecked (&bw, 0);

  gst_byte_writer_put_uint8_unchecked (&bw, fps_entry->fps_idx);

  if (cc_data_len / 3 > fps_entry->max_cc_count) {
    GST_WARNING_OBJECT (self, "Too many cc_data triplet for framerate: %u > %u",
        cc_data_len / 3, fps_entry->max_cc_count);
    cc_data_len = 3 * fps_entry->max_cc_count;
  }

  /* ccdata_present | caption_service_active */
  flags = 0x42;

  /* time_code_present */
  if (tc && tc->config.fps_n > 0)
    flags |= 0x80;

  /* reserved */
  flags |= 0x01;

  gst_byte_writer_put_uint8_unchecked (&bw, flags);

  gst_byte_writer_put_uint16_be_unchecked (&bw, self->cdp_hdr_sequence_cntr);

  if (tc && tc->config.fps_n > 0) {
    gst_byte_writer_put_uint8_unchecked (&bw, 0x71);
    gst_byte_writer_put_uint8_unchecked (&bw, 0xc0 |
        (((tc->hours % 10) & 0x3) << 4) |
        ((tc->hours - (tc->hours % 10)) & 0xf));

    gst_byte_writer_put_uint8_unchecked (&bw, 0x80 |
        (((tc->minutes % 10) & 0x7) << 4) |
        ((tc->minutes - (tc->minutes % 10)) & 0xf));

    gst_byte_writer_put_uint8_unchecked (&bw,
        (tc->field_count <
            2 ? 0x00 : 0x80) | (((tc->seconds %
                    10) & 0x7) << 4) | ((tc->seconds -
                (tc->seconds % 10)) & 0xf));

    gst_byte_writer_put_uint8_unchecked (&bw,
        ((tc->config.flags & GST_VIDEO_TIME_CODE_FLAGS_DROP_FRAME) ? 0x80 :
            0x00) | (((tc->frames % 10) & 0x3) << 4) | ((tc->frames -
                (tc->frames % 10)) & 0xf));
  }

  gst_byte_writer_put_uint8_unchecked (&bw, 0x72);
  gst_byte_writer_put_uint8_unchecked (&bw, 0xe0 | fps_entry->max_cc_count);
  gst_byte_writer_put_data_unchecked (&bw, cc_data, cc_data_len);
  while (fps_entry->max_cc_count > cc_data_len / 3) {
    gst_byte_writer_put_uint8_unchecked (&bw, 0xf8);
    gst_byte_writer_put_uint8_unchecked (&bw, 0x00);
    gst_byte_writer_put_uint8_unchecked (&bw, 0x00);
    cc_data_len += 3;
  }

  gst_byte_writer_put_uint8_unchecked (&bw, 0x74);
  gst_byte_writer_put_uint16_be_unchecked (&bw, self->cdp_hdr_sequence_cntr);
  self->cdp_hdr_sequence_cntr++;
  /* We calculate the checksum afterwards */
  gst_byte_writer_put_uint8_unchecked (&bw, 0);

  len = gst_byte_writer_get_pos (&bw);
  gst_byte_writer_set_pos (&bw, 2);
  gst_byte_writer_put_uint8_unchecked (&bw, len);

  checksum = 0;
  for (i = 0; i < len; i++) {
    checksum += cdp[i];
  }
  checksum &= 0xff;
  checksum = 256 - checksum;
  cdp[len - 1] = checksum;

  return len;
}

/* Converts CDP into raw CEA708 cc_data */
static guint
convert_cea708_cdp_cea708_cc_data_internal (GstCCConverter * self,
    const guint8 * cdp, guint cdp_len, guint8 cc_data[MAX_CDP_PACKET_LEN],
    GstVideoTimeCode * tc, const struct cdp_fps_entry **out_fps_entry)
{
  GstByteReader br;
  guint16 u16;
  guint8 u8;
  guint8 flags;
  guint len = 0;
  const struct cdp_fps_entry *fps_entry;

  *out_fps_entry = &null_fps_entry;
  memset (tc, 0, sizeof (*tc));

  /* Header + footer length */
  if (cdp_len < 11)
    return 0;

  gst_byte_reader_init (&br, cdp, cdp_len);
  u16 = gst_byte_reader_get_uint16_be_unchecked (&br);
  if (u16 != 0x9669)
    return 0;

  u8 = gst_byte_reader_get_uint8_unchecked (&br);
  if (u8 != cdp_len)
    return 0;

  u8 = gst_byte_reader_get_uint8_unchecked (&br);
  fps_entry = cdp_fps_entry_from_id (u8);
  if (!fps_entry || fps_entry->fps_n == 0)
    return 0;

  flags = gst_byte_reader_get_uint8_unchecked (&br);
  /* No cc_data? */
  if ((flags & 0x40) == 0)
    return 0;

  /* cdp_hdr_sequence_cntr */
  gst_byte_reader_skip_unchecked (&br, 2);

  /* time_code_present */
  if (flags & 0x80) {
    guint8 hours, minutes, seconds, frames, fields;
    gboolean drop_frame;

    if (gst_byte_reader_get_remaining (&br) < 5)
      return 0;
    if (gst_byte_reader_get_uint8_unchecked (&br) != 0x71)
      return 0;

    u8 = gst_byte_reader_get_uint8_unchecked (&br);
    if ((u8 & 0xc) != 0xc)
      return 0;

    hours = ((u8 >> 4) & 0x3) * 10 + (u8 & 0xf);

    u8 = gst_byte_reader_get_uint8_unchecked (&br);
    if ((u8 & 0x80) != 0x80)
      return 0;
    minutes = ((u8 >> 4) & 0x7) * 10 + (u8 & 0xf);

    u8 = gst_byte_reader_get_uint8_unchecked (&br);
    if (u8 & 0x80)
      fields = 2;
    else
      fields = 1;
    seconds = ((u8 >> 4) & 0x7) * 10 + (u8 & 0xf);

    u8 = gst_byte_reader_get_uint8_unchecked (&br);
    if (u8 & 0x40)
      return 0;

    drop_frame = ! !(u8 & 0x80);
    frames = ((u8 >> 4) & 0x3) * 10 + (u8 & 0xf);

    gst_video_time_code_init (tc, fps_entry->fps_n, fps_entry->fps_d, NULL,
        drop_frame ? GST_VIDEO_TIME_CODE_FLAGS_DROP_FRAME :
        GST_VIDEO_TIME_CODE_FLAGS_NONE, hours, minutes, seconds, frames,
        fields);
  }

  /* ccdata_present */
  if (flags & 0x40) {
    guint8 cc_count;

    if (gst_byte_reader_get_remaining (&br) < 2)
      return 0;
    if (gst_byte_reader_get_uint8_unchecked (&br) != 0x72)
      return 0;

    cc_count = gst_byte_reader_get_uint8_unchecked (&br);
    if ((cc_count & 0xe0) != 0xe0)
      return 0;
    cc_count &= 0x1f;

    len = 3 * cc_count;
    if (gst_byte_reader_get_remaining (&br) < len)
      return 0;

    memcpy (cc_data, gst_byte_reader_get_data_unchecked (&br, len), len);
  }

  *out_fps_entry = fps_entry;

  /* skip everything else we don't care about */
  return len;
}

static guint
cdp_to_cc_data (GstCCConverter * self, GstBuffer * inbuf, guint8 * out,
    guint out_size, GstVideoTimeCode * out_tc,
    const struct cdp_fps_entry **out_fps_entry)
{
  GstMapInfo in;
  guint len = 0;

  if (self->scratch_len > 0) {
    GST_DEBUG_OBJECT (self, "copying from previous scratch buffer of %u bytes",
        self->scratch_len);
    memcpy (&out[len], self->scratch, self->scratch_len);
    len += self->scratch_len;
  }

  if (inbuf) {
    guint cc_data_len;

    gst_buffer_map (inbuf, &in, GST_MAP_READ);

    cc_data_len =
        convert_cea708_cdp_cea708_cc_data_internal (self, in.data, in.size,
        &out[len], out_tc, out_fps_entry);
    if (cc_data_len / 3 > (*out_fps_entry)->max_cc_count) {
      GST_WARNING_OBJECT (self, "Too many cc_data triples in CDP packet %u",
          cc_data_len / 3);
      cc_data_len = 3 * (*out_fps_entry)->max_cc_count;
    }
    cc_data_len = compact_cc_data (&out[len], cc_data_len);
    len += cc_data_len;

    gst_buffer_unmap (inbuf, &in);
    self->input_frames++;
  }

  return len;
}

static GstFlowReturn
convert_cea608_raw_cea608_s334_1a (GstCCConverter * self, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstMapInfo in, out;
  guint i, n;

  n = gst_buffer_get_size (inbuf);
  if (n & 1) {
    GST_WARNING_OBJECT (self, "Invalid raw CEA608 buffer size");
    gst_buffer_set_size (outbuf, 0);
    return GST_FLOW_OK;
  }

  n /= 2;

  if (n > 3) {
    GST_WARNING_OBJECT (self, "Too many CEA608 pairs %u", n);
    n = 3;
  }

  gst_buffer_set_size (outbuf, 3 * n);

  gst_buffer_map (inbuf, &in, GST_MAP_READ);
  gst_buffer_map (outbuf, &out, GST_MAP_WRITE);

  /* We have to assume that each value is from the first field and
   * don't know from which line offset it originally is */
  for (i = 0; i < n; i++) {
    out.data[i * 3] = 0x80;
    out.data[i * 3 + 1] = in.data[i * 2];
    out.data[i * 3 + 2] = in.data[i * 2 + 1];
  }

  gst_buffer_unmap (inbuf, &in);
  gst_buffer_unmap (outbuf, &out);

  return GST_FLOW_OK;
}

static GstFlowReturn
convert_cea608_raw_cea708_cc_data (GstCCConverter * self, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstMapInfo in, out;
  guint i, n;

  n = gst_buffer_get_size (inbuf);
  if (n & 1) {
    GST_WARNING_OBJECT (self, "Invalid raw CEA608 buffer size");
    gst_buffer_set_size (outbuf, 0);
    return GST_FLOW_OK;
  }

  n /= 2;

  if (n > 3) {
    GST_WARNING_OBJECT (self, "Too many CEA608 pairs %u", n);
    n = 3;
  }

  gst_buffer_set_size (outbuf, 3 * n);

  gst_buffer_map (inbuf, &in, GST_MAP_READ);
  gst_buffer_map (outbuf, &out, GST_MAP_WRITE);

  /* We have to assume that each value is from the first field and
   * don't know from which line offset it originally is */
  for (i = 0; i < n; i++) {
    out.data[i * 3] = 0xfc;
    out.data[i * 3 + 1] = in.data[i * 2];
    out.data[i * 3 + 2] = in.data[i * 2 + 1];
  }

  gst_buffer_unmap (inbuf, &in);
  gst_buffer_unmap (outbuf, &out);

  return GST_FLOW_OK;
}

static GstFlowReturn
convert_cea608_raw_cea708_cdp (GstCCConverter * self, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstMapInfo in, out;
  guint i, n;
  gint len;
  guint8 cc_data[MAX_CDP_PACKET_LEN];
  const GstVideoTimeCodeMeta *tc_meta;
  const struct cdp_fps_entry *fps_entry;

  n = gst_buffer_get_size (inbuf);
  if (n & 1) {
    GST_WARNING_OBJECT (self, "Invalid raw CEA608 buffer size");
    gst_buffer_set_size (outbuf, 0);
    return GST_FLOW_OK;
  }

  n /= 2;

  if (n > 3) {
    GST_WARNING_OBJECT (self, "Too many CEA608 pairs %u", n);
    n = 3;
  }

  gst_buffer_map (inbuf, &in, GST_MAP_READ);
  gst_buffer_map (outbuf, &out, GST_MAP_WRITE);

  for (i = 0; i < n; i++) {
    cc_data[i * 3] = 0xfc;
    cc_data[i * 3 + 1] = in.data[i * 2];
    cc_data[i * 3 + 2] = in.data[i * 2 + 1];
  }

  fps_entry = cdp_fps_entry_from_fps (self->out_fps_n, self->out_fps_d);
  if (!fps_entry || fps_entry->fps_n == 0)
    g_assert_not_reached ();

  tc_meta = gst_buffer_get_video_time_code_meta (inbuf);

  len = fit_and_scale_cc_data (self, NULL, fps_entry, cc_data,
      n * 3, tc_meta ? &tc_meta->tc : NULL);
  if (len >= 0) {
    len =
        convert_cea708_cc_data_cea708_cdp_internal (self, cc_data, len,
        out.data, out.size, &self->current_output_timecode, fps_entry);
  } else {
    len = 0;
  }

  gst_buffer_unmap (inbuf, &in);
  gst_buffer_unmap (outbuf, &out);

  gst_buffer_set_size (outbuf, len);

  return GST_FLOW_OK;
}

static GstFlowReturn
convert_cea608_s334_1a_cea608_raw (GstCCConverter * self, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstMapInfo in, out;
  guint i, n;
  guint cea608 = 0;

  n = gst_buffer_get_size (inbuf);
  if (n % 3 != 0) {
    GST_WARNING_OBJECT (self, "Invalid S334-1A CEA608 buffer size");
    n = n - (n % 3);
  }

  n /= 3;

  if (n > 3) {
    GST_WARNING_OBJECT (self, "Too many S334-1A CEA608 triplets %u", n);
    n = 3;
  }

  gst_buffer_map (inbuf, &in, GST_MAP_READ);
  gst_buffer_map (outbuf, &out, GST_MAP_WRITE);

  for (i = 0; i < n; i++) {
    if (in.data[i * 3] & 0x80) {
      out.data[i * 2] = in.data[i * 3 + 1];
      out.data[i * 2 + 1] = in.data[i * 3 + 2];
      cea608++;
    }
  }

  gst_buffer_unmap (inbuf, &in);
  gst_buffer_unmap (outbuf, &out);

  gst_buffer_set_size (outbuf, 2 * cea608);

  return GST_FLOW_OK;
}

static GstFlowReturn
convert_cea608_s334_1a_cea708_cc_data (GstCCConverter * self, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstMapInfo in, out;
  guint i, n;

  n = gst_buffer_get_size (inbuf);
  if (n % 3 != 0) {
    GST_WARNING_OBJECT (self, "Invalid S334-1A CEA608 buffer size");
    n = n - (n % 3);
  }

  n /= 3;

  if (n > 3) {
    GST_WARNING_OBJECT (self, "Too many S334-1A CEA608 triplets %u", n);
    n = 3;
  }

  gst_buffer_set_size (outbuf, 3 * n);

  gst_buffer_map (inbuf, &in, GST_MAP_READ);
  gst_buffer_map (outbuf, &out, GST_MAP_WRITE);

  for (i = 0; i < n; i++) {
    out.data[i * 3] = (in.data[i * 3] & 0x80) ? 0xfc : 0xfd;
    out.data[i * 3 + 1] = in.data[i * 3 + 1];
    out.data[i * 3 + 2] = in.data[i * 3 + 2];
  }

  gst_buffer_unmap (inbuf, &in);
  gst_buffer_unmap (outbuf, &out);

  return GST_FLOW_OK;
}

static GstFlowReturn
convert_cea608_s334_1a_cea708_cdp (GstCCConverter * self, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstMapInfo in, out;
  guint i, n;
  gint len;
  guint8 cc_data[MAX_CDP_PACKET_LEN];
  const GstVideoTimeCodeMeta *tc_meta;
  const struct cdp_fps_entry *fps_entry;

  n = gst_buffer_get_size (inbuf);
  if (n % 3 != 0) {
    GST_WARNING_OBJECT (self, "Invalid S334-1A CEA608 buffer size");
    n = n - (n % 3);
  }

  n /= 3;

  if (n > 3) {
    GST_WARNING_OBJECT (self, "Too many S334-1A CEA608 triplets %u", n);
    n = 3;
  }

  gst_buffer_map (inbuf, &in, GST_MAP_READ);
  gst_buffer_map (outbuf, &out, GST_MAP_WRITE);

  for (i = 0; i < n; i++) {
    cc_data[i * 3] = (in.data[i * 3] & 0x80) ? 0xfc : 0xfd;
    cc_data[i * 3 + 1] = in.data[i * 3 + 1];
    cc_data[i * 3 + 2] = in.data[i * 3 + 2];
  }

  fps_entry = cdp_fps_entry_from_fps (self->out_fps_n, self->out_fps_d);
  if (!fps_entry || fps_entry->fps_n == 0)
    g_assert_not_reached ();

  tc_meta = gst_buffer_get_video_time_code_meta (inbuf);

  len = fit_and_scale_cc_data (self, NULL, fps_entry, cc_data,
      n * 3, tc_meta ? &tc_meta->tc : NULL);
  if (len >= 0) {
    len =
        convert_cea708_cc_data_cea708_cdp_internal (self, cc_data, len,
        out.data, out.size, &self->current_output_timecode, fps_entry);
  } else {
    len = 0;
  }

  gst_buffer_unmap (inbuf, &in);
  gst_buffer_unmap (outbuf, &out);

  gst_buffer_set_size (outbuf, len);

  return GST_FLOW_OK;
}

static GstFlowReturn
convert_cea708_cc_data_cea608_raw (GstCCConverter * self, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstMapInfo in, out;
  guint i, n;
  guint cea608 = 0;

  n = gst_buffer_get_size (inbuf);
  if (n % 3 != 0) {
    GST_WARNING_OBJECT (self, "Invalid raw CEA708 buffer size");
    n = n - (n % 3);
  }

  n /= 3;

  if (n > 25) {
    GST_WARNING_OBJECT (self, "Too many CEA708 triplets %u", n);
    n = 25;
  }

  gst_buffer_map (inbuf, &in, GST_MAP_READ);
  gst_buffer_map (outbuf, &out, GST_MAP_WRITE);

  for (i = 0; i < n; i++) {
    /* We can only really copy the first field here as there can't be any
     * signalling in raw CEA608 and we must not mix the streams of different
     * fields
     */
    if (in.data[i * 3] == 0xfc) {
      out.data[cea608 * 2] = in.data[i * 3 + 1];
      out.data[cea608 * 2 + 1] = in.data[i * 3 + 2];
      cea608++;
    }
  }

  gst_buffer_unmap (inbuf, &in);
  gst_buffer_unmap (outbuf, &out);

  gst_buffer_set_size (outbuf, 2 * cea608);

  return GST_FLOW_OK;
}

static GstFlowReturn
convert_cea708_cc_data_cea608_s334_1a (GstCCConverter * self, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstMapInfo in, out;
  guint i, n;
  guint cea608 = 0;

  n = gst_buffer_get_size (inbuf);
  if (n % 3 != 0) {
    GST_WARNING_OBJECT (self, "Invalid raw CEA708 buffer size");
    n = n - (n % 3);
  }

  n /= 3;

  if (n > 25) {
    GST_WARNING_OBJECT (self, "Too many CEA708 triplets %u", n);
    n = 25;
  }

  gst_buffer_map (inbuf, &in, GST_MAP_READ);
  gst_buffer_map (outbuf, &out, GST_MAP_WRITE);

  for (i = 0; i < n; i++) {
    if (in.data[i * 3] == 0xfc || in.data[i * 3] == 0xfd) {
      /* We have to assume a line offset of 0 */
      out.data[cea608 * 3] = in.data[i * 3] == 0xfc ? 0x80 : 0x00;
      out.data[cea608 * 3 + 1] = in.data[i * 3 + 1];
      out.data[cea608 * 3 + 2] = in.data[i * 3 + 2];
      cea608++;
    }
  }

  gst_buffer_unmap (inbuf, &in);
  gst_buffer_unmap (outbuf, &out);

  gst_buffer_set_size (outbuf, 3 * cea608);

  return GST_FLOW_OK;
}

static GstFlowReturn
convert_cea708_cc_data_cea708_cdp (GstCCConverter * self, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstMapInfo in, out;
  guint n;
  gint len;
  const GstVideoTimeCodeMeta *tc_meta;
  const struct cdp_fps_entry *fps_entry;

  n = gst_buffer_get_size (inbuf);
  if (n % 3 != 0) {
    GST_WARNING_OBJECT (self, "Invalid raw CEA708 buffer size");
    n = n - (n % 3);
  }

  n /= 3;

  if (n > 25) {
    GST_WARNING_OBJECT (self, "Too many CEA708 triplets %u", n);
    n = 25;
  }

  gst_buffer_map (inbuf, &in, GST_MAP_READ);
  gst_buffer_map (outbuf, &out, GST_MAP_WRITE);

  fps_entry = cdp_fps_entry_from_fps (self->out_fps_n, self->out_fps_d);
  if (!fps_entry || fps_entry->fps_n == 0)
    g_assert_not_reached ();

  tc_meta = gst_buffer_get_video_time_code_meta (inbuf);

  len = fit_and_scale_cc_data (self, NULL, fps_entry, in.data,
      in.size, tc_meta ? &tc_meta->tc : NULL);
  if (len >= 0) {
    len =
        convert_cea708_cc_data_cea708_cdp_internal (self, in.data, len,
        out.data, out.size, &self->current_output_timecode, fps_entry);
  } else {
    len = 0;
  }

  gst_buffer_unmap (inbuf, &in);
  gst_buffer_unmap (outbuf, &out);

  gst_buffer_set_size (outbuf, len);

  return GST_FLOW_OK;
}

static GstFlowReturn
convert_cea708_cdp_cea608_raw (GstCCConverter * self, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstMapInfo out;
  GstVideoTimeCode tc = GST_VIDEO_TIME_CODE_INIT;
  guint i, cea608 = 0;
  gint len = 0;
  const struct cdp_fps_entry *in_fps_entry = NULL, *out_fps_entry;
  guint8 cc_data[MAX_CDP_PACKET_LEN] = { 0, };

  len =
      cdp_to_cc_data (self, inbuf, cc_data, sizeof (cc_data), &tc,
      &in_fps_entry);

  out_fps_entry = cdp_fps_entry_from_fps (self->out_fps_n, self->out_fps_d);
  if (!out_fps_entry || out_fps_entry->fps_n == 0)
    out_fps_entry = in_fps_entry;

  len = fit_and_scale_cc_data (self, in_fps_entry, out_fps_entry, cc_data, len,
      &tc);
  if (len > 0) {
    len /= 3;

    gst_buffer_map (outbuf, &out, GST_MAP_WRITE);

    for (i = 0; i < len; i++) {
      /* We can only really copy the first field here as there can't be any
       * signalling in raw CEA608 and we must not mix the streams of different
       * fields
       */
      if (cc_data[i * 3] == 0xfc) {
        out.data[cea608 * 2] = cc_data[i * 3 + 1];
        out.data[cea608 * 2 + 1] = cc_data[i * 3 + 2];
        cea608++;
      }
    }

    gst_buffer_unmap (outbuf, &out);
  }

  gst_buffer_set_size (outbuf, 2 * cea608);

  if (self->current_output_timecode.config.fps_n != 0
      && !gst_buffer_get_video_time_code_meta (inbuf)) {
    gst_buffer_add_video_time_code_meta (outbuf,
        &self->current_output_timecode);
    gst_video_time_code_increment_frame (&self->current_output_timecode);
  }

  return GST_FLOW_OK;
}

static GstFlowReturn
convert_cea708_cdp_cea608_s334_1a (GstCCConverter * self, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstMapInfo out;
  GstVideoTimeCode tc = GST_VIDEO_TIME_CODE_INIT;
  guint i, cea608 = 0;
  gint len = 0;
  const struct cdp_fps_entry *in_fps_entry = NULL, *out_fps_entry;
  guint8 cc_data[MAX_CDP_PACKET_LEN] = { 0, };

  len =
      cdp_to_cc_data (self, inbuf, cc_data, sizeof (cc_data), &tc,
      &in_fps_entry);

  out_fps_entry = cdp_fps_entry_from_fps (self->out_fps_n, self->out_fps_d);
  if (!out_fps_entry || out_fps_entry->fps_n == 0)
    out_fps_entry = in_fps_entry;

  len = fit_and_scale_cc_data (self, in_fps_entry, out_fps_entry, cc_data, len,
      &tc);
  if (len > 0) {
    len /= 3;

    gst_buffer_map (outbuf, &out, GST_MAP_WRITE);
    for (i = 0; i < len; i++) {
      if (cc_data[i * 3] == 0xfc || cc_data[i * 3] == 0xfd) {
        /* We have to assume a line offset of 0 */
        out.data[cea608 * 3] = cc_data[i * 3] == 0xfc ? 0x80 : 0x00;
        out.data[cea608 * 3 + 1] = cc_data[i * 3 + 1];
        out.data[cea608 * 3 + 2] = cc_data[i * 3 + 2];
        cea608++;
      }
    }
    gst_buffer_unmap (outbuf, &out);
    self->output_frames++;
  }

  gst_buffer_set_size (outbuf, 3 * cea608);

  if (self->current_output_timecode.config.fps_n != 0
      && !gst_buffer_get_video_time_code_meta (inbuf)) {
    gst_buffer_add_video_time_code_meta (outbuf,
        &self->current_output_timecode);
    gst_video_time_code_increment_frame (&self->current_output_timecode);
  }

  return GST_FLOW_OK;
}

static GstFlowReturn
convert_cea708_cdp_cea708_cc_data (GstCCConverter * self, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstMapInfo out;
  GstVideoTimeCode tc = GST_VIDEO_TIME_CODE_INIT;
  gint len = 0;
  const struct cdp_fps_entry *in_fps_entry = NULL, *out_fps_entry;
  guint8 cc_data[MAX_CDP_PACKET_LEN] = { 0, };

  len =
      cdp_to_cc_data (self, inbuf, cc_data, sizeof (cc_data), &tc,
      &in_fps_entry);

  out_fps_entry = cdp_fps_entry_from_fps (self->out_fps_n, self->out_fps_d);
  if (!out_fps_entry || out_fps_entry->fps_n == 0)
    out_fps_entry = in_fps_entry;

  len = fit_and_scale_cc_data (self, in_fps_entry, out_fps_entry, cc_data, len,
      &tc);
  if (len >= 0) {
    gst_buffer_map (outbuf, &out, GST_MAP_WRITE);
    memcpy (out.data, cc_data, len);
    gst_buffer_unmap (outbuf, &out);
    self->output_frames++;
  } else {
    len = 0;
  }

  if (self->current_output_timecode.config.fps_n != 0
      && !gst_buffer_get_video_time_code_meta (inbuf)) {
    gst_buffer_add_video_time_code_meta (outbuf,
        &self->current_output_timecode);
    gst_video_time_code_increment_frame (&self->current_output_timecode);
  }

  gst_buffer_set_size (outbuf, len);

  return GST_FLOW_OK;
}

static GstFlowReturn
convert_cea708_cdp_cea708_cdp (GstCCConverter * self, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstMapInfo out;
  GstVideoTimeCode tc = GST_VIDEO_TIME_CODE_INIT;
  gint len = 0;
  const struct cdp_fps_entry *in_fps_entry = NULL, *out_fps_entry;
  guint8 cc_data[MAX_CDP_PACKET_LEN] = { 0, };

  len =
      cdp_to_cc_data (self, inbuf, cc_data, sizeof (cc_data), &tc,
      &in_fps_entry);

  out_fps_entry = cdp_fps_entry_from_fps (self->out_fps_n, self->out_fps_d);
  if (!out_fps_entry || out_fps_entry->fps_n == 0)
    out_fps_entry = in_fps_entry;

  len = fit_and_scale_cc_data (self, in_fps_entry, out_fps_entry, cc_data, len,
      &tc);
  if (len >= 0) {
    gst_buffer_map (outbuf, &out, GST_MAP_WRITE);
    len =
        convert_cea708_cc_data_cea708_cdp_internal (self, cc_data, len,
        out.data, out.size, &self->current_output_timecode, out_fps_entry);

    gst_buffer_unmap (outbuf, &out);
    self->output_frames++;
  } else {
    len = 0;
  }

  gst_buffer_set_size (outbuf, len);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_cc_converter_transform (GstCCConverter * self, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstVideoTimeCodeMeta *tc_meta = NULL;
  GstFlowReturn ret = GST_FLOW_OK;

  GST_DEBUG_OBJECT (self, "Converting %" GST_PTR_FORMAT " from %u to %u", inbuf,
      self->input_caption_type, self->output_caption_type);

  if (inbuf)
    tc_meta = gst_buffer_get_video_time_code_meta (inbuf);

  if (tc_meta) {
    if (self->current_output_timecode.config.fps_n <= 0) {
      /* XXX: this assumes the input time codes are well-formed and increase
       * at the rate of one frame for each input buffer */
      const struct cdp_fps_entry *in_fps_entry;
      gint scale_n, scale_d;

      in_fps_entry = cdp_fps_entry_from_fps (self->in_fps_n, self->in_fps_d);
      if (!in_fps_entry || in_fps_entry->fps_n == 0)
        scale_n = scale_d = 1;
      else
        get_framerate_output_scale (self, in_fps_entry, &scale_n, &scale_d);

      if (tc_meta)
        interpolate_time_code_with_framerate (self, &tc_meta->tc,
            self->out_fps_n, self->out_fps_d, scale_n, scale_d,
            &self->current_output_timecode);
    }
  }

  switch (self->input_caption_type) {
    case GST_VIDEO_CAPTION_TYPE_CEA608_RAW:

      switch (self->output_caption_type) {
        case GST_VIDEO_CAPTION_TYPE_CEA608_S334_1A:
          ret = convert_cea608_raw_cea608_s334_1a (self, inbuf, outbuf);
          break;
        case GST_VIDEO_CAPTION_TYPE_CEA708_RAW:
          ret = convert_cea608_raw_cea708_cc_data (self, inbuf, outbuf);
          break;
        case GST_VIDEO_CAPTION_TYPE_CEA708_CDP:
          ret = convert_cea608_raw_cea708_cdp (self, inbuf, outbuf);
          break;
        case GST_VIDEO_CAPTION_TYPE_CEA608_RAW:
        default:
          g_assert_not_reached ();
          break;
      }

      break;
    case GST_VIDEO_CAPTION_TYPE_CEA608_S334_1A:

      switch (self->output_caption_type) {
        case GST_VIDEO_CAPTION_TYPE_CEA608_RAW:
          ret = convert_cea608_s334_1a_cea608_raw (self, inbuf, outbuf);
          break;
        case GST_VIDEO_CAPTION_TYPE_CEA708_RAW:
          ret = convert_cea608_s334_1a_cea708_cc_data (self, inbuf, outbuf);
          break;
        case GST_VIDEO_CAPTION_TYPE_CEA708_CDP:
          ret = convert_cea608_s334_1a_cea708_cdp (self, inbuf, outbuf);
          break;
        case GST_VIDEO_CAPTION_TYPE_CEA608_S334_1A:
        default:
          g_assert_not_reached ();
          break;
      }

      break;
    case GST_VIDEO_CAPTION_TYPE_CEA708_RAW:

      switch (self->output_caption_type) {
        case GST_VIDEO_CAPTION_TYPE_CEA608_RAW:
          ret = convert_cea708_cc_data_cea608_raw (self, inbuf, outbuf);
          break;
        case GST_VIDEO_CAPTION_TYPE_CEA608_S334_1A:
          ret = convert_cea708_cc_data_cea608_s334_1a (self, inbuf, outbuf);
          break;
        case GST_VIDEO_CAPTION_TYPE_CEA708_CDP:
          ret = convert_cea708_cc_data_cea708_cdp (self, inbuf, outbuf);
          break;
        case GST_VIDEO_CAPTION_TYPE_CEA708_RAW:
        default:
          g_assert_not_reached ();
          break;
      }

      break;
    case GST_VIDEO_CAPTION_TYPE_CEA708_CDP:

      switch (self->output_caption_type) {
        case GST_VIDEO_CAPTION_TYPE_CEA608_RAW:
          ret = convert_cea708_cdp_cea608_raw (self, inbuf, outbuf);
          break;
        case GST_VIDEO_CAPTION_TYPE_CEA608_S334_1A:
          ret = convert_cea708_cdp_cea608_s334_1a (self, inbuf, outbuf);
          break;
        case GST_VIDEO_CAPTION_TYPE_CEA708_RAW:
          ret = convert_cea708_cdp_cea708_cc_data (self, inbuf, outbuf);
          break;
        case GST_VIDEO_CAPTION_TYPE_CEA708_CDP:
          ret = convert_cea708_cdp_cea708_cdp (self, inbuf, outbuf);
          break;
        default:
          g_assert_not_reached ();
          break;
      }

      break;
    default:
      g_assert_not_reached ();
      break;
  }

  if (ret != GST_FLOW_OK) {
    GST_DEBUG_OBJECT (self, "returning %s", gst_flow_get_name (ret));
    return ret;
  }

  GST_DEBUG_OBJECT (self, "Converted to %" GST_PTR_FORMAT, outbuf);

  if (gst_buffer_get_size (outbuf) > 0) {
    if (self->current_output_timecode.config.fps_n > 0) {
      gst_buffer_add_video_time_code_meta (outbuf,
          &self->current_output_timecode);
      /* XXX: discont handling? */
      gst_video_time_code_increment_frame (&self->current_output_timecode);
    }

    return GST_FLOW_OK;
  } else {
    return GST_BASE_TRANSFORM_FLOW_DROPPED;
  }
}

static gboolean
gst_cc_converter_transform_meta (GstBaseTransform * base, GstBuffer * outbuf,
    GstMeta * meta, GstBuffer * inbuf)
{
  const GstMetaInfo *info = meta->info;

  /* we do this manually for framerate scaling */
  if (info->api == GST_VIDEO_TIME_CODE_META_API_TYPE)
    return FALSE;

  return GST_BASE_TRANSFORM_CLASS (parent_class)->transform_meta (base, outbuf,
      meta, inbuf);
}

static gboolean
can_generate_output (GstCCConverter * self)
{
  int input_frame_n, input_frame_d, output_frame_n, output_frame_d;
  int output_time_cmp;

  if (self->in_fps_n == 0 || self->out_fps_n == 0)
    return FALSE;

  /* compute the relative frame count for each */
  if (!gst_util_fraction_multiply (self->in_fps_d, self->in_fps_n,
          self->input_frames, 1, &input_frame_n, &input_frame_d))
    /* we should never overflow */
    g_assert_not_reached ();

  if (!gst_util_fraction_multiply (self->out_fps_d, self->out_fps_n,
          self->output_frames + 1, 1, &output_frame_n, &output_frame_d))
    /* we should never overflow */
    g_assert_not_reached ();

  output_time_cmp = gst_util_fraction_compare (input_frame_n, input_frame_d,
      output_frame_n, output_frame_d);

  /* if the next output frame is at or before the current input frame */
  if (output_time_cmp >= 0)
    return TRUE;

  return FALSE;
}

static GstFlowReturn
gst_cc_converter_generate_output (GstBaseTransform * base, GstBuffer ** outbuf)
{
  GstBaseTransformClass *bclass = GST_BASE_TRANSFORM_GET_CLASS (base);
  GstCCConverter *self = GST_CCCONVERTER (base);
  GstBuffer *inbuf = base->queued_buf;
  GstFlowReturn ret;

  *outbuf = NULL;
  base->queued_buf = NULL;
  if (!inbuf && !can_generate_output (self)) {
    return GST_FLOW_OK;
  }

  if (gst_base_transform_is_passthrough (base)) {
    *outbuf = inbuf;
    ret = GST_FLOW_OK;
  } else {
    *outbuf = gst_buffer_new_allocate (NULL, MAX_CDP_PACKET_LEN, NULL);
    if (*outbuf == NULL)
      goto no_buffer;

    if (inbuf)
      gst_buffer_replace (&self->previous_buffer, inbuf);

    if (bclass->copy_metadata) {
      if (!bclass->copy_metadata (base, self->previous_buffer, *outbuf)) {
        /* something failed, post a warning */
        GST_ELEMENT_WARNING (self, STREAM, NOT_IMPLEMENTED,
            ("could not copy metadata"), (NULL));
      }
    }

    ret = gst_cc_converter_transform (self, inbuf, *outbuf);

    if (inbuf)
      gst_buffer_unref (inbuf);
  }

  return ret;

no_buffer:
  {
    if (inbuf)
      gst_buffer_unref (inbuf);
    *outbuf = NULL;
    GST_WARNING_OBJECT (self, "could not allocate buffer");
    return GST_FLOW_ERROR;
  }
}

static gboolean
gst_cc_converter_sink_event (GstBaseTransform * trans, GstEvent * event)
{
  GstCCConverter *self = GST_CCCONVERTER (trans);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
      GST_DEBUG_OBJECT (self, "received EOS");

      while (self->scratch_len > 0 || can_generate_output (self)) {
        GstBuffer *outbuf;
        GstFlowReturn ret;

        outbuf = gst_buffer_new_allocate (NULL, MAX_CDP_PACKET_LEN, NULL);

        ret = gst_cc_converter_transform (self, NULL, outbuf);
        if (ret == GST_BASE_TRANSFORM_FLOW_DROPPED) {
          /* try to move the output along */
          self->input_frames++;
          gst_buffer_unref (outbuf);
          continue;
        } else if (ret != GST_FLOW_OK)
          break;

        ret = gst_pad_push (GST_BASE_TRANSFORM_SRC_PAD (trans), outbuf);
        if (ret != GST_FLOW_OK)
          break;
      }
      /* fallthrough */
    case GST_EVENT_FLUSH_START:
      self->scratch_len = 0;
      self->input_frames = 0;
      self->output_frames = 0;
      gst_video_time_code_clear (&self->current_output_timecode);
      gst_clear_buffer (&self->previous_buffer);
      break;
    default:
      break;
  }

  return GST_BASE_TRANSFORM_CLASS (parent_class)->sink_event (trans, event);
}

static gboolean
gst_cc_converter_start (GstBaseTransform * base)
{
  GstCCConverter *self = GST_CCCONVERTER (base);

  /* Resetting this is not really needed but makes debugging easier */
  self->cdp_hdr_sequence_cntr = 0;
  self->current_output_timecode = (GstVideoTimeCode) GST_VIDEO_TIME_CODE_INIT;
  self->input_frames = 0;
  self->output_frames = 0;
  self->scratch_len = 0;

  return TRUE;
}

static gboolean
gst_cc_converter_stop (GstBaseTransform * base)
{
  GstCCConverter *self = GST_CCCONVERTER (base);

  gst_video_time_code_clear (&self->current_output_timecode);
  gst_clear_buffer (&self->previous_buffer);

  return TRUE;
}

static void
gst_cc_converter_class_init (GstCCConverterClass * klass)
{
  GstElementClass *gstelement_class;
  GstBaseTransformClass *basetransform_class;

  gstelement_class = (GstElementClass *) klass;
  basetransform_class = (GstBaseTransformClass *) klass;

  gst_element_class_set_static_metadata (gstelement_class,
      "Closed Caption Converter",
      "Filter/ClosedCaption",
      "Converts Closed Captions between different formats",
      "Sebastian Dröge <sebastian@centricular.com>");

  gst_element_class_add_static_pad_template (gstelement_class, &sinktemplate);
  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);

  basetransform_class->start = GST_DEBUG_FUNCPTR (gst_cc_converter_start);
  basetransform_class->stop = GST_DEBUG_FUNCPTR (gst_cc_converter_stop);
  basetransform_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_cc_converter_sink_event);
  basetransform_class->transform_size =
      GST_DEBUG_FUNCPTR (gst_cc_converter_transform_size);
  basetransform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_cc_converter_transform_caps);
  basetransform_class->fixate_caps =
      GST_DEBUG_FUNCPTR (gst_cc_converter_fixate_caps);
  basetransform_class->set_caps = GST_DEBUG_FUNCPTR (gst_cc_converter_set_caps);
  basetransform_class->transform_meta =
      GST_DEBUG_FUNCPTR (gst_cc_converter_transform_meta);
  basetransform_class->generate_output =
      GST_DEBUG_FUNCPTR (gst_cc_converter_generate_output);
  basetransform_class->passthrough_on_same_caps = TRUE;

  GST_DEBUG_CATEGORY_INIT (gst_cc_converter_debug, "ccconverter",
      0, "Closed Caption converter");
}

static void
gst_cc_converter_init (GstCCConverter * self)
{
}
