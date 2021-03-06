/*
 * GStreamer
 * Copyright (C) 2009 Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>

#include "gstvdpvideobuffer.h"
#include "gstvdpvideoyuv.h"

GST_DEBUG_CATEGORY_STATIC (gst_vdp_video_yuv_debug);
#define GST_CAT_DEFAULT gst_vdp_video_yuv_debug

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0
};

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VDP_VIDEO_CAPS));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw-yuv, "
        "framerate = (fraction) [ 0, MAX ], "
        "width = (int) [ 1, MAX ], " "height = (int) [ 1, MAX ]"));

#define DEBUG_INIT(bla) \
    GST_DEBUG_CATEGORY_INIT (gst_vdp_video_yuv_debug, "vdpauvideoyuv", 0, "VDPAU VdpSurface to YUV");

GST_BOILERPLATE_FULL (GstVdpVideoYUV, gst_vdp_video_yuv, GstElement,
    GST_TYPE_ELEMENT, DEBUG_INIT);

static void gst_vdp_video_yuv_finalize (GObject * object);
static void gst_vdp_video_yuv_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_vdp_video_yuv_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

GstFlowReturn
gst_vdp_video_yuv_chain (GstPad * pad, GstBuffer * buffer)
{
  GstVdpVideoYUV *video_yuv;
  GstVdpDevice *device;
  VdpVideoSurface surface;
  GstBuffer *outbuf = NULL;
  GstFlowReturn result = GST_FLOW_ERROR;

  video_yuv = GST_VDP_VIDEO_YUV (GST_OBJECT_PARENT (pad));
  device = GST_VDP_VIDEO_BUFFER (buffer)->device;
  surface = GST_VDP_VIDEO_BUFFER (buffer)->surface;

  GST_LOG_OBJECT (video_yuv, "Received buffer format %" GST_FOURCC_FORMAT,
      GST_FOURCC_ARGS (video_yuv->format));

  switch (video_yuv->format) {
    case GST_MAKE_FOURCC ('Y', 'V', '1', '2'):
    {
      gint size;
      VdpStatus status;
      guint8 *data[3];
      guint32 stride[3];

      size =
          gst_video_format_get_size (GST_VIDEO_FORMAT_YV12, video_yuv->width,
          video_yuv->height);
      result =
          gst_pad_alloc_buffer_and_set_caps (video_yuv->src,
          GST_BUFFER_OFFSET_NONE, size, GST_PAD_CAPS (video_yuv->src), &outbuf);
      if (G_UNLIKELY (result != GST_FLOW_OK)) {
        GST_DEBUG_OBJECT (video_yuv, "Pad alloc_buffer returned %d", result);
        goto done;
      }

      data[0] = GST_BUFFER_DATA (outbuf) +
          gst_video_format_get_component_offset (GST_VIDEO_FORMAT_YV12,
          0, video_yuv->width, video_yuv->height);
      data[1] = GST_BUFFER_DATA (outbuf) +
          gst_video_format_get_component_offset (GST_VIDEO_FORMAT_YV12,
          2, video_yuv->width, video_yuv->height);
      data[2] = GST_BUFFER_DATA (outbuf) +
          gst_video_format_get_component_offset (GST_VIDEO_FORMAT_YV12,
          1, video_yuv->width, video_yuv->height);

      stride[0] = gst_video_format_get_row_stride (GST_VIDEO_FORMAT_YV12,
          0, video_yuv->width);
      stride[1] = gst_video_format_get_row_stride (GST_VIDEO_FORMAT_YV12,
          2, video_yuv->width);
      stride[2] = gst_video_format_get_row_stride (GST_VIDEO_FORMAT_YV12,
          1, video_yuv->width);

      status =
          device->vdp_video_surface_get_bits_ycbcr (surface,
          VDP_YCBCR_FORMAT_YV12, (void *) data, stride);
      GST_LOG_OBJECT (video_yuv,
          "Got status %d from vdp_video_surface_get_bits_ycbcr", status);
      if (G_UNLIKELY (status != VDP_STATUS_OK)) {
        GST_ELEMENT_ERROR (video_yuv, RESOURCE, READ,
            ("Couldn't get data from vdpau"),
            ("Error returned from vdpau was: %s",
                device->vdp_get_error_string (status)));
        goto done;
      }
      break;
    }
    case GST_MAKE_FOURCC ('I', '4', '2', '0'):
    {
      gint size;
      VdpStatus status;
      guint8 *data[3];
      guint32 stride[3];

      size =
          gst_video_format_get_size (GST_VIDEO_FORMAT_YV12, video_yuv->width,
          video_yuv->height);
      result =
          gst_pad_alloc_buffer_and_set_caps (video_yuv->src,
          GST_BUFFER_OFFSET_NONE, size, GST_PAD_CAPS (video_yuv->src), &outbuf);
      if (G_UNLIKELY (result != GST_FLOW_OK)) {
        GST_DEBUG_OBJECT (video_yuv, "Pad alloc_buffer returned %d", result);
        goto done;
      }

      data[0] = GST_BUFFER_DATA (outbuf) +
          gst_video_format_get_component_offset (GST_VIDEO_FORMAT_I420,
          0, video_yuv->width, video_yuv->height);
      data[1] = GST_BUFFER_DATA (outbuf) +
          gst_video_format_get_component_offset (GST_VIDEO_FORMAT_I420,
          2, video_yuv->width, video_yuv->height);
      data[2] = GST_BUFFER_DATA (outbuf) +
          gst_video_format_get_component_offset (GST_VIDEO_FORMAT_I420,
          1, video_yuv->width, video_yuv->height);

      stride[0] = gst_video_format_get_row_stride (GST_VIDEO_FORMAT_I420,
          0, video_yuv->width);
      stride[1] = gst_video_format_get_row_stride (GST_VIDEO_FORMAT_I420,
          2, video_yuv->width);
      stride[2] = gst_video_format_get_row_stride (GST_VIDEO_FORMAT_I420,
          1, video_yuv->width);

      status =
          device->vdp_video_surface_get_bits_ycbcr (surface,
          VDP_YCBCR_FORMAT_YV12, (void *) data, stride);
      GST_LOG_OBJECT (video_yuv,
          "Got status %d from vdp_video_surface_get_bits_ycbcr", status);
      if (G_UNLIKELY (status != VDP_STATUS_OK)) {
        GST_ELEMENT_ERROR (video_yuv, RESOURCE, READ,
            ("Couldn't get data from vdpau"),
            ("Error returned from vdpau was: %s",
                device->vdp_get_error_string (status)));
        goto done;
      }
      break;
    }
    case GST_MAKE_FOURCC ('N', 'V', '1', '2'):
    {
      gint size;
      VdpStatus status;
      guint8 *data[2];
      guint32 stride[2];

      size =
          video_yuv->width * video_yuv->height +
          video_yuv->width * video_yuv->height / 2;
      GST_LOG_OBJECT (video_yuv, "Entering buffer_alloc");
      result =
          gst_pad_alloc_buffer_and_set_caps (video_yuv->src,
          GST_BUFFER_OFFSET_NONE, size, GST_PAD_CAPS (video_yuv->src), &outbuf);
      if (G_UNLIKELY (result != GST_FLOW_OK)) {
        GST_DEBUG_OBJECT (video_yuv, "Pad alloc_buffer returned %d", result);
        goto done;
      }

      data[0] = GST_BUFFER_DATA (outbuf);
      data[1] = GST_BUFFER_DATA (outbuf) + video_yuv->width * video_yuv->height;

      stride[0] = video_yuv->width;
      stride[1] = video_yuv->width;

      GST_LOG_OBJECT (video_yuv, "Entering vdp_video_surface_get_bits_ycbcr");
      status =
          device->vdp_video_surface_get_bits_ycbcr (surface,
          VDP_YCBCR_FORMAT_NV12, (void *) data, stride);
      GST_LOG_OBJECT (video_yuv,
          "Got status %d from vdp_video_surface_get_bits_ycbcr", status);
      if (G_UNLIKELY (status != VDP_STATUS_OK)) {
        GST_ELEMENT_ERROR (video_yuv, RESOURCE, READ,
            ("Couldn't get data from vdpau"),
            ("Error returned from vdpau was: %s",
                device->vdp_get_error_string (status)));
        goto done;
      }
      break;
    }
    default:
      break;
  }

  gst_buffer_unref (buffer);

  gst_buffer_copy_metadata (outbuf, buffer, GST_BUFFER_COPY_TIMESTAMPS);
  GST_LOG_OBJECT (video_yuv, "Pushing buffer with ts %" GST_TIME_FORMAT,
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (outbuf)));
  return gst_pad_push (video_yuv->src, outbuf);

done:
  if (outbuf)
    gst_buffer_unref (outbuf);
  gst_buffer_unref (buffer);
  return result;
}

static GstCaps *
gst_vdp_video_yuv_get_caps (GstVdpVideoYUV * video_yuv,
    GstVdpDevice * device, gint chroma_type, gint width, gint height,
    gint framerate_numerator, gint framerate_denominator, gint par_numerator,
    gint par_denominator)
{
  GstCaps *caps;
  gint i;

  caps = gst_caps_new_empty ();

  for (i = 0; i < N_FORMATS; i++) {
    VdpStatus status;
    VdpBool is_supported;

    if (formats[i].chroma_type != chroma_type)
      continue;

    status =
        device->vdp_video_surface_query_ycbcr_capabilities (device->device,
        chroma_type, formats[i].format, &is_supported);
    if (status != VDP_STATUS_OK && status != VDP_STATUS_INVALID_Y_CB_CR_FORMAT) {
      GST_ELEMENT_ERROR (video_yuv, RESOURCE, READ,
          ("Could not query VDPAU YCbCr capabilites"),
          ("Error returned from vdpau was: %s",
              device->vdp_get_error_string (status)));

      return NULL;
    }
    if (is_supported) {
      GstCaps *format_caps;

      format_caps = gst_caps_new_simple ("video/x-raw-yuv",
          "format", GST_TYPE_FOURCC, formats[i].fourcc,
          "width", G_TYPE_INT, width,
          "height", G_TYPE_INT, height,
          "framerate", GST_TYPE_FRACTION, framerate_numerator,
          framerate_denominator, "pixel-aspect-ratio", GST_TYPE_FRACTION,
          par_numerator, par_denominator, NULL);
      gst_caps_append (caps, format_caps);
    }
  }

  if (gst_caps_is_empty (caps)) {
    gst_caps_unref (caps);
    return NULL;
  }

  return caps;
}

static gboolean
gst_vdp_video_yuv_sink_set_caps (GstPad * pad, GstCaps * caps)
{
  GstVdpVideoYUV *video_yuv = GST_VDP_VIDEO_YUV (GST_OBJECT_PARENT (pad));

  GstCaps *src_caps, *new_caps;
  GstStructure *structure;
  const GValue *value;
  GstVdpDevice *device;
  gint chroma_type;
  gint width, height;
  gint framerate_numerator, framerate_denominator;
  gint par_numerator, par_denominator;
  guint32 fourcc_format;
  gboolean res;

  structure = gst_caps_get_structure (caps, 0);
  value = gst_structure_get_value (structure, "device");
  device = g_value_get_object (value);

  gst_structure_get_int (structure, "chroma-type", &chroma_type);
  gst_structure_get_int (structure, "width", &width);
  gst_structure_get_int (structure, "height", &height);
  gst_structure_get_fraction (structure, "framerate",
      &framerate_numerator, &framerate_denominator);
  gst_structure_get_fraction (structure, "pixel-aspect-ratio",
      &par_numerator, &par_denominator);

  src_caps =
      gst_vdp_video_yuv_get_caps (video_yuv, device, chroma_type, width,
      height, framerate_numerator, framerate_denominator, par_numerator,
      par_denominator);
  if (G_UNLIKELY (!src_caps))
    return FALSE;

  video_yuv->src_caps = src_caps;

  src_caps = gst_pad_get_allowed_caps (video_yuv->src);
  if (G_UNLIKELY (!src_caps || !gst_caps_get_size (src_caps)))
    return FALSE;

  GST_DEBUG_OBJECT (video_yuv,
      "Selecting first caps from set: %" GST_PTR_FORMAT, src_caps);

  new_caps = gst_caps_copy_nth (src_caps, 0);
  gst_caps_unref (src_caps);
  if (G_UNLIKELY (!new_caps))
    return FALSE;

  structure = gst_caps_get_structure (new_caps, 0);
  gst_structure_get_fourcc (structure, "format", &fourcc_format);

  gst_pad_fixate_caps (video_yuv->src, new_caps);
  res = gst_pad_set_caps (video_yuv->src, new_caps);

  gst_caps_unref (new_caps);

  if (G_UNLIKELY (!res))
    return FALSE;

  video_yuv->width = width;
  video_yuv->height = height;
  video_yuv->framerate_numerator = framerate_numerator;
  video_yuv->framerate_denominator = framerate_denominator;
  video_yuv->format = fourcc_format;

  return TRUE;
}

static GstCaps *
gst_vdp_video_yuv_src_getcaps (GstPad * pad)
{
  GstVdpVideoYUV *video_yuv;

  video_yuv = GST_VDP_VIDEO_YUV (GST_OBJECT_PARENT (pad));

  if (video_yuv->src_caps)
    return gst_caps_copy (video_yuv->src_caps);

  if (GST_PAD_CAPS (video_yuv->src))
    return gst_caps_copy (GST_PAD_CAPS (video_yuv->src));

  return gst_caps_copy (gst_pad_get_pad_template_caps (video_yuv->src));
}

/* GObject vmethod implementations */

static void
gst_vdp_video_yuv_base_init (gpointer klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_set_details_simple (element_class,
      "VdpauVideoYUV",
      "Covideo_yuv/Decoder/Video",
      "VDPAU video surface to YUV",
      "Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>");

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_template));
}

static void
gst_vdp_video_yuv_class_init (GstVdpVideoYUVClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->finalize = gst_vdp_video_yuv_finalize;
  gobject_class->set_property = gst_vdp_video_yuv_set_property;
  gobject_class->get_property = gst_vdp_video_yuv_get_property;
}

static void
gst_vdp_video_yuv_init (GstVdpVideoYUV * video_yuv, GstVdpVideoYUVClass * klass)
{
  video_yuv->src_caps = NULL;

  video_yuv->height = 0;
  video_yuv->width = 0;
  video_yuv->framerate_numerator = 0;
  video_yuv->framerate_denominator = 0;
  video_yuv->par_numerator = 1;
  video_yuv->par_denominator = 1;

  video_yuv->src = gst_pad_new_from_static_template (&src_template, "src");
  gst_pad_set_getcaps_function (video_yuv->src, gst_vdp_video_yuv_src_getcaps);
  gst_element_add_pad (GST_ELEMENT (video_yuv), video_yuv->src);

  video_yuv->sink = gst_pad_new_from_static_template (&sink_template, "sink");
  gst_pad_set_setcaps_function (video_yuv->sink,
      gst_vdp_video_yuv_sink_set_caps);
  gst_pad_set_chain_function (video_yuv->sink, gst_vdp_video_yuv_chain);
  gst_element_add_pad (GST_ELEMENT (video_yuv), video_yuv->sink);
  gst_pad_set_active (video_yuv->sink, TRUE);
}

static void
gst_vdp_video_yuv_finalize (GObject * object)
{
  GstVdpVideoYUV *video_yuv = (GstVdpVideoYUV *) object;

  if (video_yuv->src_caps)
    gst_caps_unref (video_yuv->src_caps);
}

static void
gst_vdp_video_yuv_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vdp_video_yuv_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}
