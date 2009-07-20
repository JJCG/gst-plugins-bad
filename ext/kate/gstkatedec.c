/*
 * GStreamer
 * Copyright 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright 2008 Vincent Penquerc'h <ogg.k.ogg.k@googlemail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
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

/**
 * SECTION:element-katedec
 * @see_also: oggdemux
 *
 * <refsect2>
 * <para>
 * This element decodes Kate streams
 * <ulink url="http://libkate.googlecode.com/">Kate</ulink> is a free codec
 * for text based data, such as subtitles. Any number of kate streams can be
 * embedded in an Ogg stream.
 * </para>
 * <para>
 * libkate (see above url) is needed to build this plugin.
 * </para>
 * <title>Example pipeline</title>
 * <para>
 * This explicitely decodes a Kate stream:
 * <programlisting>
 * gst-launch filesrc location=test.ogg ! oggdemux ! katedec ! fakesink silent=TRUE
 * </programlisting>
 * </para>
 * <para>
 * This will automatically detect and use any Kate streams multiplexed
 * in an Ogg stream:
 * <programlisting>
 * gst-launch playbin uri=file:///tmp/test.ogg
 * </programlisting>
 * </para>
 * </refsect2>
 */

/* FIXME: post appropriate GST_ELEMENT_ERROR when returning FLOW_ERROR */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <gst/gst.h>

#include "gstkate.h"
#include "gstkatedec.h"

GST_DEBUG_CATEGORY_EXTERN (gst_katedec_debug);
#define GST_CAT_DEFAULT gst_katedec_debug

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  ARG_REMOVE_MARKUP = DECODER_BASE_ARG_COUNT
};

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("subtitle/x-kate")
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("text/plain; text/x-pango-markup")
    );

GST_BOILERPLATE (GstKateDec, gst_kate_dec, GstElement, GST_TYPE_ELEMENT);

static void gst_kate_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_kate_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstFlowReturn gst_kate_dec_chain (GstPad * pad, GstBuffer * buf);
static GstStateChangeReturn gst_kate_dec_change_state (GstElement * element,
    GstStateChange transition);
static gboolean gst_kate_dec_sink_query (GstPad * pad, GstQuery * query);

static void
gst_kate_dec_base_init (gpointer gclass)
{
  static GstElementDetails element_details =
      GST_ELEMENT_DETAILS ("Kate stream text decoder",
      "Codec/Decoder/Subtitle",
      "Decodes Kate text streams",
      "Vincent Penquerc'h <ogg.k.ogg.k@googlemail.com>");
  GstElementClass *element_class = GST_ELEMENT_CLASS (gclass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_factory));
  gst_element_class_set_details (element_class, &element_details);
}

/* initialize the plugin's class */
static void
gst_kate_dec_class_init (GstKateDecClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_kate_dec_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_kate_dec_get_property);

  gst_kate_util_install_decoder_base_properties (gobject_class);

  g_object_class_install_property (gobject_class, ARG_REMOVE_MARKUP,
      g_param_spec_boolean ("remove-markup", "Remove markup",
          "Remove markup from decoded text ?", FALSE, G_PARAM_READWRITE));

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_kate_dec_change_state);
}

/* initialize the new element
 * instantiate pads and add them to element
 * set functions
 * initialize structure
 */
static void
gst_kate_dec_init (GstKateDec * dec, GstKateDecClass * gclass)
{
  GST_DEBUG_OBJECT (dec, "gst_kate_dec_init");

  dec->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_chain_function (dec->sinkpad,
      GST_DEBUG_FUNCPTR (gst_kate_dec_chain));
  gst_pad_set_query_function (dec->sinkpad,
      GST_DEBUG_FUNCPTR (gst_kate_dec_sink_query));
  gst_pad_use_fixed_caps (dec->sinkpad);
  gst_pad_set_caps (dec->sinkpad,
      gst_static_pad_template_get_caps (&sink_factory));
  gst_element_add_pad (GST_ELEMENT (dec), dec->sinkpad);

  dec->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_element_add_pad (GST_ELEMENT (dec), dec->srcpad);

  gst_kate_util_decode_base_init (&dec->decoder);

  dec->remove_markup = FALSE;
}

static void
gst_kate_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_kate_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstKateDec *kd = GST_KATE_DEC (object);

  switch (prop_id) {
    case ARG_REMOVE_MARKUP:
      g_value_set_boolean (value, kd->remove_markup);
      break;
    default:
      if (!gst_kate_util_decoder_base_get_property (&kd->decoder, object,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
}

/* GstElement vmethod implementations */

/* chain function
 * this function does the actual processing
 */

static GstFlowReturn
gst_kate_dec_chain (GstPad * pad, GstBuffer * buf)
{
  GstKateDec *kd = GST_KATE_DEC (gst_pad_get_parent (pad));
  const kate_event *ev = NULL;
  GstFlowReturn rflow = GST_FLOW_OK;

  rflow =
      gst_kate_util_decoder_base_chain_kate_packet (&kd->decoder,
      GST_ELEMENT_CAST (kd), pad, buf, kd->srcpad, &ev);
  if (G_UNLIKELY (rflow != GST_FLOW_OK)) {
    gst_object_unref (kd);
    gst_buffer_unref (buf);
    return rflow;
  }

  if (ev) {
    gchar *escaped;
    GstBuffer *buffer;
    size_t len;

    if (kd->remove_markup && ev->text_markup_type != kate_markup_none) {
      size_t len0 = ev->len + 1;
      escaped = g_strdup (ev->text);
      if (escaped) {
        kate_text_remove_markup (ev->text_encoding, escaped, &len0);
      }
    } else if (ev->text_markup_type == kate_markup_none) {
      /* no pango markup yet, escape text */
      /* TODO: actually do the pango thing */
      escaped = g_markup_printf_escaped ("%s", ev->text);
    } else {
      escaped = g_strdup (ev->text);
    }

    if (G_LIKELY (escaped)) {
      len = strlen (escaped);
      GST_DEBUG_OBJECT (kd, "kate event: %s, escaped %s", ev->text, escaped);
      buffer = gst_buffer_new_and_alloc (len + 1);
      if (G_LIKELY (buffer)) {
        /* allocate and copy the NULs, but don't include them in passed size */
        memcpy (GST_BUFFER_DATA (buffer), escaped, len + 1);
        GST_BUFFER_SIZE (buffer) = len;
        GST_BUFFER_TIMESTAMP (buffer) = ev->start_time * GST_SECOND;
        GST_BUFFER_DURATION (buffer) =
            (ev->end_time - ev->start_time) * GST_SECOND;
        gst_buffer_set_caps (buffer, GST_PAD_CAPS (kd->srcpad));
        rflow = gst_pad_push (kd->srcpad, buffer);
        if (rflow == GST_FLOW_NOT_LINKED) {
          GST_DEBUG_OBJECT (kd, "source pad not linked, ignored");
        } else if (rflow != GST_FLOW_OK) {
          GST_WARNING_OBJECT (kd, "failed to push buffer: %s",
              gst_flow_get_name (rflow));
        }
      } else {
        GST_WARNING_OBJECT (kd, "failed to create buffer");
        rflow = GST_FLOW_ERROR;
      }
      g_free (escaped);
    } else {
      GST_WARNING_OBJECT (kd, "failed to allocate string");
      rflow = GST_FLOW_ERROR;
    }
  }

  gst_object_unref (kd);
  gst_buffer_unref (buf);
  return rflow;
}

static GstStateChangeReturn
gst_kate_dec_change_state (GstElement * element, GstStateChange transition)
{
  GstKateDec *kd = GST_KATE_DEC (element);
  return gst_kate_decoder_base_change_state (&kd->decoder, element,
      parent_class, transition);
}

gboolean
gst_kate_dec_sink_query (GstPad * pad, GstQuery * query)
{
  GstKateDec *kd = GST_KATE_DEC (gst_pad_get_parent (pad));
  gboolean res =
      gst_kate_decoder_base_sink_query (&kd->decoder, GST_ELEMENT_CAST (kd),
      pad, query);
  gst_object_unref (kd);
  return res;
}