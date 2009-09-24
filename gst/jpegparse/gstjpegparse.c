/* GStreamer
 *
 * jpegparse: a parser for JPEG streams
 *
 * Copyright (C) <2009> Arnout Vandecappelle (Essensium/Mind) <arnout@mind.be>
 *                      Víctor Manuel Jáquez Leal <vjaquez@igalia.com>
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-jpegparse
 * @short_description: JPEG parser
 *
 * Parses a JPEG stream into JPEG images.  It looks for EOI boundaries to
 * split a continuous stream into single-frame buffers. Also reads the
 * image header searching for image properties such as width and height
 * among others.
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v souphttpsrc location=... ! jpegparse ! matroskamux ! filesink location=...
 * ]|
 * The above pipeline fetches a motion JPEG stream from an IP camera over
 * HTTP and stores it in a matroska file.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstjpegparse.h"

enum
{
	PROP_0,
	PROP_THUMBNAILONLY,
};


#define GST_JPEGPARSE_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_JPEG_PARSE, GstJpegParsePrivate))

struct _GstJpegParsePrivate
{
	gint width;
	gint height;
	gboolean thumbnail_only;
	gint thumbnail_offset;
	gint thumbnail_length;
};



static const GstElementDetails gst_jpeg_parse_details =
GST_ELEMENT_DETAILS ("JPEG stream parser",
    "Codec/Parser/Video",
    "Parse JPEG images into single-frame buffers",
    "Arnout Vandecappelle (Essensium/Mind) <arnout@mind.be>");

static GstStaticPadTemplate gst_jpeg_parse_src_pad_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("image/jpeg, "
        "format = (fourcc) { I420, UYVY }, "
        "width = (int) [ 0, MAX ],"
        "height = (int) [ 0, MAX ], "
        "interlaced = (boolean) { true, false }, "
        "framerate = (fraction) [ 0/1, MAX ]")
    );

static GstStaticPadTemplate gst_jpeg_parse_sink_pad_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("image/jpeg")
    );

GST_DEBUG_CATEGORY_STATIC (jpeg_parse_debug);
#define GST_CAT_DEFAULT jpeg_parse_debug
#define DEFAULT_THUMBNAIL_ONLY	FALSE

static GstElementClass *parent_class;   /* NULL */

static void gst_jpeg_parse_base_init (gpointer g_class);
static void gst_jpeg_parse_class_init (GstJpegParseClass * klass);
static void gst_jpeg_parse_dispose (GObject * object);

static GstFlowReturn gst_jpeg_parse_chain (GstPad * pad, GstBuffer * buffer);
static gboolean gst_jpeg_parse_sink_setcaps (GstPad * pad, GstCaps * caps);
static gboolean gst_jpeg_parse_sink_event (GstPad * pad, GstEvent * event);
static gboolean gst_jpeg_parse_src_event (GstPad * pad, GstEvent * event);
static GstStateChangeReturn gst_jpeg_parse_change_state (GstElement * element,
    GstStateChange transition);
static void gst_jpeg_parse_set_property (GObject* object, guint prop_id,
    const GValue* value, GParamSpec* pspec);
static void gst_jpeg_parse_get_property (GObject* object, guint prop_id,
    GValue* value, GParamSpec* pspec);

#define DEBUG_INIT(bla) \
  GST_DEBUG_CATEGORY_INIT (jpeg_parse_debug, "jpegparse", 0, "JPEG parser");

GST_BOILERPLATE_FULL (GstJpegParse, gst_jpeg_parse, GstElement,
    GST_TYPE_ELEMENT, DEBUG_INIT);

static void
gst_jpeg_parse_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_jpeg_parse_src_pad_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_jpeg_parse_sink_pad_template));
  gst_element_class_set_details (element_class, &gst_jpeg_parse_details);
}

static void
gst_jpeg_parse_class_init (GstJpegParseClass * klass)
{
  GstElementClass *gstelement_class;
  GObjectClass *gobject_class;
  GObjectClass* g_klass;
  GParamSpec* spec;


	g_klass = G_OBJECT_CLASS (klass);

  gstelement_class = (GstElementClass *) klass;
  gobject_class = (GObjectClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  g_type_class_add_private (klass, sizeof (GstJpegParsePrivate));

  g_klass->set_property =
		GST_DEBUG_FUNCPTR (gst_jpeg_parse_set_property);

  g_klass->get_property =
		GST_DEBUG_FUNCPTR (gst_jpeg_parse_get_property);

  spec = g_param_spec_boolean ("thumbnail-only", "Thumbnail-only",
		  "Selects thumbnail as output frame", DEFAULT_THUMBNAIL_ONLY,
		  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_property (g_klass, PROP_THUMBNAILONLY, spec);

  gobject_class->dispose = gst_jpeg_parse_dispose;

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_jpeg_parse_change_state);
}

static void
gst_jpeg_parse_reset (GstJpegParse * parse)
{
  if (parse->adapter) {
    gst_adapter_clear (parse->adapter);
    gst_object_unref (parse->adapter);
    parse->adapter = NULL;
  }
  parse->srcpad = NULL;
  parse->sinkpad = NULL;
  parse->next_ts = GST_CLOCK_TIME_NONE;
}

static void
gst_jpeg_parse_init (GstJpegParse * parse, GstJpegParseClass * g_class)
{
  GstJpegParsePrivate* priv = GST_JPEGPARSE_GET_PRIVATE (parse);

  gst_jpeg_parse_reset (parse);

  priv->width = -1;
  priv->height = -1;
  priv->thumbnail_only = FALSE;
  priv->thumbnail_offset = 0;
  priv->thumbnail_length = 0;

  /* create the sink and src pads */
  parse->sinkpad =
      gst_pad_new_from_static_template (&gst_jpeg_parse_sink_pad_template,
      "sink");
  gst_pad_set_chain_function (parse->sinkpad,
      GST_DEBUG_FUNCPTR (gst_jpeg_parse_chain));
  gst_pad_set_event_function (parse->sinkpad,
      GST_DEBUG_FUNCPTR (gst_jpeg_parse_sink_event));
  gst_pad_set_setcaps_function (parse->sinkpad,
      GST_DEBUG_FUNCPTR (gst_jpeg_parse_sink_setcaps));
  gst_element_add_pad (GST_ELEMENT (parse), parse->sinkpad);

  parse->srcpad =
      gst_pad_new_from_static_template (&gst_jpeg_parse_src_pad_template,
      "src");
  gst_pad_set_event_function (parse->srcpad,
      GST_DEBUG_FUNCPTR (gst_jpeg_parse_src_event));
  gst_element_add_pad (GST_ELEMENT (parse), parse->srcpad);

  parse->adapter = gst_adapter_new ();
}


static void
gst_jpeg_parse_set_property (GObject* object, guint prop_id,
			      const GValue* value, GParamSpec* pspec)
{
	GstJpegParsePrivate* priv = GST_JPEGPARSE_GET_PRIVATE (object);
	/* GstJpegParse *parse = GST_JPEG_PARSE (object);  */

	switch (prop_id)
	{
	case PROP_THUMBNAILONLY:
		priv->thumbnail_only = g_value_get_boolean (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}

	return;
}

static void
gst_jpeg_parse_get_property (GObject* object, guint prop_id,
			      GValue* value, GParamSpec* pspec)
{
	GstJpegParsePrivate* priv = GST_JPEGPARSE_GET_PRIVATE (object);
	/*  GstJpegParse *parse = GST_JPEG_PARSE (object);  */

	switch (prop_id)
	{
	case PROP_THUMBNAILONLY:
		g_value_set_boolean (value, priv->thumbnail_only);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
	return;
}



static void
gst_jpeg_parse_dispose (GObject * object)
{
  GstJpegParse *parse = GST_JPEG_PARSE (object);

  gst_jpeg_parse_reset (parse);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static gboolean
gst_jpeg_parse_sink_setcaps (GstPad * pad, GstCaps * caps)
{
  GstJpegParse *parse = GST_JPEG_PARSE (GST_OBJECT_PARENT (pad));
  GstStructure *s = gst_caps_get_structure (caps, 0);
  const GValue *framerate;

  if ((framerate = gst_structure_get_value (s, "framerate")) != NULL) {
    if (GST_VALUE_HOLDS_FRACTION (framerate)) {
      parse->framerate_numerator = gst_value_get_fraction_numerator (framerate);
      parse->framerate_denominator =
          gst_value_get_fraction_denominator (framerate);
      parse->packetized = TRUE;
      GST_DEBUG_OBJECT (parse, "got framerate of %d/%d fps => packetized mode",
          parse->framerate_numerator, parse->framerate_denominator);
    }
  }

  return TRUE;
}

/* Flush everything until the next JPEG header.  The header is considered
 * to be the a start marker (0xff 0xd8) followed by any other marker (0xff ...).
 * Returns TRUE if the header was found, FALSE if more data is needed. */
static gboolean
gst_jpeg_parse_skip_to_jpeg_header (GstJpegParse * parse)
{
  guint available, flush;
  gboolean ret = TRUE;

  available = gst_adapter_available (parse->adapter);
  if (available < 4)
    return FALSE;
  flush = gst_adapter_masked_scan_uint32 (parse->adapter, 0xffffff00,
      0xffd8ff00, 0, available);
  if (flush == -1) {
    flush = available - 3;      /* Last 3 bytes + 1 more may match header. */
    ret = FALSE;
  }
  if (flush > 0) {
    GST_LOG_OBJECT (parse, "Skipping %u bytes.", flush);
    gst_adapter_flush (parse->adapter, flush);
  }
  return ret;
}

static inline gboolean
gst_jpeg_parse_parse_tag_has_entropy_segment (guint8 tag)
{
  if (tag == 0xda || (tag >= 0xd0 && tag <= 0xd7))
    return TRUE;
  return FALSE;
}

/* Find the next marker, based on the marker at data.  data[0] must be 0xff.
 * Returns the offset of the next valid marker.  Returns -1 if adapter doesn't
 * have enough data. */
static guint
gst_jpeg_parse_match_next_marker (const guint8 * data, guint size)
{
  guint marker_len;
  guint8 tag;

  g_return_val_if_fail (data[0] == 0xff, -1);
  g_return_val_if_fail (size >= 2, -1);
  tag = data[1];

  if (tag >= 0xd0 && tag <= 0xd9)
    marker_len = 2;
  else if (G_UNLIKELY (size < 4))
    return -1;
  else
    marker_len = GST_READ_UINT16_BE (data + 2) + 2;
  /* Need marker_len for this marker, plus two for the next marker. */
  if (G_UNLIKELY (marker_len + 2 >= size))
    return -1;
  if (G_UNLIKELY (gst_jpeg_parse_parse_tag_has_entropy_segment (tag))) {
    while (!(data[marker_len] == 0xff && data[marker_len + 1] != 0x00)) {
      if (G_UNLIKELY (marker_len + 2 >= size))
        return -1;
      ++marker_len;
    }
  }
  return marker_len;
}

/* Returns the position beyond the end marker, -1 if insufficient data and -2
   if marker lengths are inconsistent. data must start with 0xff. */
static guint
gst_jpeg_parse_find_end_marker (GstJpegParse * parse, const guint8 * data,
    guint size)
{
  guint offset = 0;
  GstJpegParsePrivate* priv = GST_JPEGPARSE_GET_PRIVATE (parse);

  while (1) {
    guint marker_len;
    guint8 tag;

    if (offset + 1 >= size)
      return -1;

    if (data[offset] != 0xff)
      return -2;

    /* Skip over extra 0xff */
    while (G_UNLIKELY ((tag = data[offset + 1]) == 0xff)) {
      ++offset;
      if (G_UNLIKELY (offset + 1 >= size))
        return -1;
    }
    /* Check for EOI */
    if (G_UNLIKELY (tag == 0xd9)) {
      GST_DEBUG_OBJECT (parse, "EOI at %u", offset);
      return offset + 2;
    }
    /* Skip over this marker. */
    marker_len = gst_jpeg_parse_match_next_marker (data + offset,
        size - offset);
    if (G_UNLIKELY (marker_len == -1)) {
      return -1;
    } else {
      GST_LOG_OBJECT (parse, "At offset %u: marker %02x, length %u", offset,
          tag, marker_len);
      if((tag == 0xe1) && (priv->thumbnail_length == 0)) {
          priv->thumbnail_offset = offset +2 ;
          priv->thumbnail_length = marker_len;
          GST_DEBUG_OBJECT (parse, "  Buffer %02x : offset %u | mark_length %u",
                  tag, priv->thumbnail_offset, priv->thumbnail_length);
      }
      offset += marker_len;
    }
  }
}

/* scan until EOI, by interpreting marker + length */
static guint
gst_jpeg_parse_get_image_length (GstJpegParse * parse)
{
  const guint8 *data;
  guint size, offset, start = 2;

  size = gst_adapter_available (parse->adapter);
  if (size < 4) {
    GST_DEBUG_OBJECT (parse, "Insufficient data for end marker.");
    return 0;
  }
  data = gst_adapter_peek (parse->adapter, size);

  g_return_val_if_fail (data[0] == 0xff && data[1] == 0xd8, 0);

  GST_DEBUG_OBJECT (parse, "Parsing jpeg image data (%u bytes)", size);

  /* skip start marker */
  offset = gst_jpeg_parse_find_end_marker (parse, data + 2, size - 2);

  if (offset == -1) {
    GST_DEBUG_OBJECT (parse, "Insufficient data.");
    return 0;
  } else if (G_UNLIKELY (offset == -2)) {
    GST_DEBUG_OBJECT (parse, "Lost sync, resyncing.");
    /* FIXME does this make sense at all?  This can only happen for broken
     * images, and the most likely breakage is that it's truncated.  In that
     * case, however, we should be looking for a new start marker... */
    while (offset == -2 || offset == -1) {
      start++;
      while (start + 1 < size && data[start] != 0xff)
        start++;
      if (G_UNLIKELY (start + 1 >= size)) {
        GST_DEBUG_OBJECT (parse, "Insufficient data while resyncing.");
        return 0;
      }
      GST_LOG_OBJECT (parse, "Resyncing from offset %u.", start);
      offset = gst_jpeg_parse_find_end_marker (parse, data + start, size -
          start);
    }
  }

  return start + offset;
}

static gint
gst_jpeg_parse_get_color_format (const guint8 * data)
{
  gint j, i, temp;
  gshort H[4], V[4];
  guint8 Nf = data[7];

  if (Nf != 3)
    goto done;

  for (j = 0; j < Nf; j++) {
    i = j * 3 + 7 + 2;
    /* H[j]: upper 4 bits of a byte, horizontal sampling factor. */
    H[j] = (0x0f & (data[i] >> 4));

    /* V[j]: lower 4 bits of a byte, vertical sampling factor.   */
    V[j] = (0x0f & data[i]);
  }

  temp = (V[0] * H[0]) / (V[1] * H[1]);

  if (temp == 4 && H[0] == 2)
    return GST_MAKE_FOURCC ('I', '4', '2', '0');

done:
  return GST_MAKE_FOURCC ('U', 'Y', 'V', 'Y');
}

static gboolean
gst_jpeg_parse_read_header (GstJpegParse * parse, GstBuffer * buffer)
{
  guint8 *data;
  gint section;

  data = GST_BUFFER_DATA (buffer);
  data += 2;                    /* skip start marker */

  for (section = 0; section < 19; section++) {
    gint a, len;
    guint8 marker;

    for (a = 0; a < 7; a++) {
      marker = *data++;
      if (marker != 0xff)
        break;
    }

    if (a >= 6 || marker == 0xff)
      return FALSE;             /* reached max number of sections */

    len = GST_READ_UINT16_BE (data);
    GST_INFO_OBJECT (parse, "marker = %x, length = %d", marker, len);

    if (len < 2)
      return FALSE;             /* invalid marker */

    switch (marker) {
      case 0xda:               /* start of scan (begins compressed data) */
        return TRUE;
        break;
      case 0xd9:               /* end of image (end of datastream) */
        GST_INFO_OBJECT (parse, "Premature EOI");
        return FALSE;           /* premature EOI */
        break;
      case 0xfe:               /* comment section */
      case 0xed:               /* non exif image tag */
      case 0xe1:               /* exif image tag */
        break;
      case 0xc2:               /* interlaced jpeg */
        parse->interlaced = TRUE;
      case 0xc0:               /* start of frame N */
      case 0xc1:               /* N indicates which compression process */
      case 0xc3:
      case 0xc5:               /* 0xc4 and 0xcc are not sof markers */
      case 0xc6:
      case 0xc7:
      case 0xc9:
      case 0xca:
      case 0xcb:
      case 0xcd:
      case 0xce:
      case 0xcf:
        parse->height = GST_READ_UINT16_BE (data + 3);
        parse->width = GST_READ_UINT16_BE (data + 5);
        parse->fourcc = gst_jpeg_parse_get_color_format (data);
        break;
      default:
        break;
    }

    data += len;
  }

  return FALSE;
}

static GstFlowReturn
gst_jpeg_parse_push_buffer (GstJpegParse * parse, guint len)
{
  GstBuffer *outbuf;
  GstFlowReturn ret = GST_FLOW_OK;

  outbuf = gst_adapter_take_buffer (parse->adapter, len);
  if (outbuf == NULL) {
    GST_ELEMENT_ERROR (parse, STREAM, DECODE,
        ("Failed to take buffer of size %u", len),
        ("Failed to take buffer of size %u", len));
    return GST_FLOW_ERROR;
  }

  if (gst_jpeg_parse_read_header (parse, outbuf)) {
    if (parse->width != parse->caps_width || parse->height != parse->height ||
        parse->framerate_numerator != parse->caps_framerate_numerator ||
        parse->framerate_denominator != parse->caps_framerate_denominator) {
      GstCaps *caps;

      /* framerate == 0/1 is a still frame */
      if (parse->framerate_denominator == 0) {
        parse->framerate_numerator = 0;
        parse->framerate_denominator = 1;
      }

      caps = gst_caps_new_simple ("image/jpeg",
          "format", GST_TYPE_FOURCC, parse->fourcc,
          "width", G_TYPE_INT, parse->width,
          "height", G_TYPE_INT, parse->height,
          "framerate", GST_TYPE_FRACTION, parse->framerate_numerator,
          parse->framerate_denominator,
          "interlaced", G_TYPE_BOOLEAN, parse->interlaced, NULL);

      if (!gst_pad_set_caps (parse->srcpad, caps)) {
        GST_ELEMENT_ERROR (parse, CORE, NEGOTIATION,
            ("Can't set caps to the src pad"),
            ("Can't set caps to the src pad"));
        return GST_FLOW_ERROR;
      }

      gst_caps_unref (caps);

      parse->caps_width = parse->width;
      parse->caps_height = parse->height;
      parse->caps_framerate_numerator = parse->framerate_numerator;
      parse->caps_framerate_denominator = parse->framerate_denominator;
    }
  } else {
    GST_ELEMENT_ERROR (parse, STREAM, DECODE,
        ("Failed to read the image header"),
        ("Failed to read the image header"));
    return GST_FLOW_ERROR;
  }

  GST_BUFFER_TIMESTAMP (outbuf) = parse->next_ts;

  if (parse->packetized && GST_CLOCK_TIME_IS_VALID (parse->next_ts)) {
    if (GST_CLOCK_TIME_IS_VALID (parse->duration)) {
      parse->next_ts += parse->duration;
    } else if (parse->framerate_numerator != 0) {
      parse->duration = gst_util_uint64_scale (GST_SECOND,
          parse->framerate_denominator, parse->framerate_numerator);
      parse->next_ts += parse->duration;
    } else {
      parse->duration = GST_CLOCK_TIME_NONE;
      parse->next_ts = GST_CLOCK_TIME_NONE;
    }
  } else {
    parse->duration = GST_CLOCK_TIME_NONE;
    parse->next_ts = GST_CLOCK_TIME_NONE;
  }

  GST_BUFFER_DURATION (outbuf) = parse->duration;

  gst_buffer_set_caps (outbuf, GST_PAD_CAPS (parse->srcpad));

  GST_LOG_OBJECT (parse, "pushing buffer (ts=%" GST_TIME_FORMAT ", len=%u)",
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (outbuf)), len);

  ret = gst_pad_push (parse->srcpad, outbuf);

  return ret;
}

static GstFlowReturn
gst_jpeg_parse_chain (GstPad * pad, GstBuffer * buf)
{
  GstJpegParse *parse;
  guint len, thumbnail_ffd8_offset, thumbnail_buf_length;
  GstClockTime timestamp, duration;
  GstFlowReturn ret = GST_FLOW_OK;
  GstJpegParsePrivate* priv;
  GstBuffer * thumbnail_buf;

  parse = GST_JPEG_PARSE (GST_PAD_PARENT (pad));
  priv = GST_JPEGPARSE_GET_PRIVATE (parse);

  timestamp = GST_BUFFER_TIMESTAMP (buf);
  duration = GST_BUFFER_DURATION (buf);

  gst_adapter_push (parse->adapter, buf);
  thumbnail_ffd8_offset=0;


  while (ret == GST_FLOW_OK && gst_jpeg_parse_skip_to_jpeg_header (parse)) {
    if (GST_CLOCK_TIME_IS_VALID (timestamp))
      parse->next_ts = timestamp;

    if (GST_CLOCK_TIME_IS_VALID (duration))
      parse->duration = duration;

    if (!parse->packetized) {
      len = gst_jpeg_parse_get_image_length (parse);
      if (len == 0)
        return GST_FLOW_OK;
    } else {
      len = GST_BUFFER_SIZE (buf);
    }

    if ((priv->thumbnail_only) && (priv->thumbnail_length) ) {
      GST_DEBUG_OBJECT (parse, "thumbnail_offset: %d & thumbnail_length: %d",
              priv->thumbnail_offset, priv->thumbnail_length);

      /* Search for SOI (0xffd8) header within the EXIF (0xffe1) tag       */
      /* the value corresponds to the absolute position within the adapter */
      /* starting search point  = offset + marker length + initial header  */
      thumbnail_ffd8_offset = gst_adapter_masked_scan_uint32 (parse->adapter,
          0xffffff00, 0xffd8ff00, (priv->thumbnail_offset+2+2),
          (priv->thumbnail_length) );

      GST_DEBUG_OBJECT (parse, "thumbnail_ffd8_offset: %d ",
                thumbnail_ffd8_offset);

      /* Thumbnail buffer length calculation                         */
      /* size = marker length - SOI offset + initial header +        */
      /* previous marker offset + 1 (offsets start counting from 0 ) */
      thumbnail_buf_length = ( priv->thumbnail_length - thumbnail_ffd8_offset
                + 2 + priv->thumbnail_offset + 1 );
      /* Thumbnail buffer allocation                            */
      thumbnail_buf = gst_buffer_try_new_and_alloc (thumbnail_buf_length );
      /* If succeeded  */
      if (thumbnail_buf) {
          gst_adapter_copy (parse->adapter, GST_BUFFER_DATA(thumbnail_buf),
                  thumbnail_ffd8_offset, thumbnail_buf_length );
          gst_adapter_clear(parse->adapter);
          gst_adapter_push (parse->adapter, thumbnail_buf);

          len = gst_jpeg_parse_get_image_length (parse);
          GST_DEBUG_OBJECT (parse, "thumbnail_buf len : %d", len);
      } else {
          GST_ELEMENT_ERROR (parse, STREAM, DECODE,
                  ("Failed to allocate thumbnail buffer of size %u", len),
                  ("Failed to allocate thumbnail buffer of size %u", len));
      }

    }

    ret = gst_jpeg_parse_push_buffer (parse, len);
  }

  GST_DEBUG_OBJECT (parse, "No further start marker found.");
  return ret;
}

static gboolean
gst_jpeg_parse_src_event (GstPad * pad, GstEvent * event)
{
  GstJpegParse *parse;
  gboolean res;

  parse = GST_JPEG_PARSE (gst_pad_get_parent (pad));

  GST_DEBUG_OBJECT (parse, "event : %s", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:{
      /* Push the remaining data, even though it's incomplete */
      guint available = gst_adapter_available (parse->adapter);

      if (available > 0)
        gst_jpeg_parse_push_buffer (parse, available);
      break;
    }
    case GST_EVENT_NEWSEGMENT:
      /* Discard any data in the adapter.  There should have been an EOS before
       * to flush it. */
      gst_adapter_clear (parse->adapter);
      break;
    default:
      break;
  }

  res = gst_pad_push_event (parse->sinkpad, event);

  gst_object_unref (parse);
  return res;
}

static gboolean
gst_jpeg_parse_sink_event (GstPad * pad, GstEvent * event)
{
  gboolean ret = TRUE;
  GstJpegParse *parse = GST_JPEG_PARSE (GST_OBJECT_PARENT (pad));

  ret = gst_pad_push_event (parse->srcpad, event);

  return ret;
}

static GstStateChangeReturn
gst_jpeg_parse_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstJpegParse *parse;

  parse = GST_JPEG_PARSE (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      parse->next_ts = GST_CLOCK_TIME_NONE;
      parse->width = parse->height = 0;
      parse->framerate_numerator = 0;
      parse->framerate_denominator = 1;
      parse->caps_framerate_numerator = parse->caps_framerate_denominator = 0;
      parse->caps_width = parse->caps_height = -1;
      parse->interlaced = FALSE;
      parse->packetized = FALSE;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret != GST_STATE_CHANGE_SUCCESS)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_adapter_clear (parse->adapter);
      break;
    default:
      break;
  }

  return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{

  if (!gst_element_register (plugin, "jpegparse", GST_RANK_PRIMARY + 1,
          GST_TYPE_JPEG_PARSE))
    return FALSE;

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "jpegparse",
    "JPEG parser",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)