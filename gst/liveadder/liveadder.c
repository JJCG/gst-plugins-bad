/*
 * Farsight Voice+Video library
 *
 *  Copyright 2008 Collabora Ltd
 *  Copyright 2008 Nokia Corporation
 *   @author: Olivier Crete <olivier.crete@collabora.co.uk>
 *
 * With parts copied from the adder plugin which is
 * Copyright (C) 1999,2000 Erik Walthinsen <omega@cse.ogi.edu>
 *                    2001 Thomas <thomas@apestaart.org>
 *               2005,2006 Wim Taymans <wim@fluendo.com>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "liveadder.h"

#include <gst/audio/audio.h>

#include <string.h>

#define DEFAULT_LATENCY_MS 60

GST_DEBUG_CATEGORY_STATIC (live_adder_debug);
#define GST_CAT_DEFAULT (live_adder_debug)

/* elementfactory information */
static const GstElementDetails gst_live_adder_details =
GST_ELEMENT_DETAILS (
  "Live Adder element",
  "Generic/Audio",
  "Mixes live/discontinuous audio streams",
  "Olivier Crete <olivier.crete@collabora.co.uk>");


static GstStaticPadTemplate gst_live_adder_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink%d",
        GST_PAD_SINK,
        GST_PAD_REQUEST,
        GST_STATIC_CAPS (GST_AUDIO_INT_PAD_TEMPLATE_CAPS "; "
            GST_AUDIO_FLOAT_PAD_TEMPLATE_CAPS)
        );

static GstStaticPadTemplate gst_live_adder_src_template =
    GST_STATIC_PAD_TEMPLATE ("src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS (GST_AUDIO_INT_PAD_TEMPLATE_CAPS "; "
            GST_AUDIO_FLOAT_PAD_TEMPLATE_CAPS)
        );

/* Valve signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_LATENCY,
};

typedef struct _GstLiveAdderPadPrivate
{
  GstSegment segment;
  gboolean eos;

  GstClockTime next_timestamp;

} GstLiveAdderPadPrivate;


GST_BOILERPLATE(GstLiveAdder, gst_live_adder, GstElement, GST_TYPE_ELEMENT);


static void
gst_live_adder_finalize (GObject * object);
static void
gst_live_adder_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void
gst_live_adder_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static GstPad *
gst_live_adder_request_new_pad (GstElement * element, GstPadTemplate * templ,
    const gchar * unused);
static void
gst_live_adder_release_pad (GstElement * element, GstPad * pad);
static GstStateChangeReturn
gst_live_adder_change_state (GstElement * element, GstStateChange transition);

static gboolean
gst_live_adder_setcaps (GstPad * pad, GstCaps * caps);
static GstCaps *
gst_live_adder_sink_getcaps (GstPad * pad);
static gboolean
gst_live_adder_src_activate_push (GstPad * pad, gboolean active);
static gboolean
gst_live_adder_src_event (GstPad * pad, GstEvent * event);

static void
gst_live_adder_loop (gpointer data);
static gboolean
gst_live_adder_query (GstPad * pad, GstQuery * query);
static gboolean
gst_live_adder_sink_event (GstPad * pad, GstEvent * event);


/* highest positive/lowest negative x-bit value we can use for clamping */
#define MAX_INT_32  ((gint32) (0x7fffffff))
#define MAX_INT_16  ((gint16) (0x7fff))
#define MAX_INT_8   ((gint8)  (0x7f))
#define MAX_UINT_32 ((guint32)(0xffffffff))
#define MAX_UINT_16 ((guint16)(0xffff))
#define MAX_UINT_8  ((guint8) (0xff))

#define MIN_INT_32  ((gint32) (0x80000000))
#define MIN_INT_16  ((gint16) (0x8000))
#define MIN_INT_8   ((gint8)  (0x80))
#define MIN_UINT_32 ((guint32)(0x00000000))
#define MIN_UINT_16 ((guint16)(0x0000))
#define MIN_UINT_8  ((guint8) (0x00))

/* clipping versions */
#define MAKE_FUNC(name,type,ttype,min,max)                      \
static void name (type *out, type *in, gint bytes) {            \
  gint i;                                                       \
  for (i = 0; i < bytes / sizeof (type); i++)                   \
    out[i] = CLAMP ((ttype)out[i] + (ttype)in[i], min, max);    \
}

/* non-clipping versions (for float) */
#define MAKE_FUNC_NC(name,type,ttype)                           \
static void name (type *out, type *in, gint bytes) {            \
  gint i;                                                       \
  for (i = 0; i < bytes / sizeof (type); i++)                   \
    out[i] = (ttype)out[i] + (ttype)in[i];                      \
}

/* *INDENT-OFF* */
MAKE_FUNC (add_int32, gint32, gint64, MIN_INT_32, MAX_INT_32)
MAKE_FUNC (add_int16, gint16, gint32, MIN_INT_16, MAX_INT_16)
MAKE_FUNC (add_int8, gint8, gint16, MIN_INT_8, MAX_INT_8)
MAKE_FUNC (add_uint32, guint32, guint64, MIN_UINT_32, MAX_UINT_32)
MAKE_FUNC (add_uint16, guint16, guint32, MIN_UINT_16, MAX_UINT_16)
MAKE_FUNC (add_uint8, guint8, guint16, MIN_UINT_8, MAX_UINT_8)
MAKE_FUNC_NC (add_float64, gdouble, gdouble)
MAKE_FUNC_NC (add_float32, gfloat, gfloat)
/* *INDENT-ON* */


static void
gst_live_adder_base_init (gpointer klass)
{
}

static void
gst_live_adder_class_init (GstLiveAdderClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = gst_live_adder_finalize;
  gobject_class->set_property = gst_live_adder_set_property;
  gobject_class->get_property = gst_live_adder_get_property;

  gstelement_class = (GstElementClass *) klass;

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_live_adder_src_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_live_adder_sink_template));
  gst_element_class_set_details (gstelement_class, &gst_live_adder_details);

  parent_class = g_type_class_peek_parent (klass);

  gstelement_class->request_new_pad = gst_live_adder_request_new_pad;
  gstelement_class->release_pad = gst_live_adder_release_pad;
  gstelement_class->change_state = gst_live_adder_change_state;

  g_object_class_install_property (gobject_class, PROP_LATENCY,
      g_param_spec_uint ("latency", "Buffer latency in ms",
          "Amount of data to buffer", 0, G_MAXUINT, DEFAULT_LATENCY_MS,
          G_PARAM_READWRITE));

  GST_DEBUG_CATEGORY_INIT
      (live_adder_debug, "liveadder", 0, "Live Adder");

}

static void
gst_live_adder_init (GstLiveAdder * adder, GstLiveAdderClass *klass)
{
  GstPadTemplate *template;

  template = gst_static_pad_template_get (&gst_live_adder_src_template);
  adder->srcpad = gst_pad_new_from_template (template, "src");
  gst_object_unref (template);
  gst_pad_set_getcaps_function (adder->srcpad,
      GST_DEBUG_FUNCPTR (gst_pad_proxy_getcaps));
  gst_pad_set_setcaps_function (adder->srcpad,
      GST_DEBUG_FUNCPTR (gst_live_adder_setcaps));
  gst_pad_set_query_function (adder->srcpad,
      GST_DEBUG_FUNCPTR (gst_live_adder_query));
  gst_pad_set_event_function (adder->srcpad,
      GST_DEBUG_FUNCPTR (gst_live_adder_src_event));
  gst_pad_set_activatepush_function (adder->srcpad,
      GST_DEBUG_FUNCPTR (gst_live_adder_src_activate_push));
  gst_element_add_pad (GST_ELEMENT (adder), adder->srcpad);

  adder->format = GST_LIVE_ADDER_FORMAT_UNSET;
  adder->padcount = 0;
  adder->func = NULL;
  adder->not_empty_cond = g_cond_new ();

  adder->next_timestamp = GST_CLOCK_TIME_NONE;

  adder->latency_ms = DEFAULT_LATENCY_MS;

  adder->buffers = g_queue_new ();
}



static void
gst_live_adder_finalize (GObject * object)
{
  GstLiveAdder *adder = GST_LIVE_ADDER (object);

  g_cond_free (adder->not_empty_cond);

  g_queue_foreach (adder->buffers, (GFunc) gst_mini_object_unref, NULL);
  g_queue_clear (adder->buffers);
  g_queue_free (adder->buffers);

  g_list_free (adder->sinkpads);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}


static void
gst_live_adder_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstLiveAdder *adder = GST_LIVE_ADDER (object);

  switch (prop_id) {
    case PROP_LATENCY:
    {
      guint64 new_latency, old_latency;

      new_latency = g_value_get_uint (value);

      GST_OBJECT_LOCK (adder);
      old_latency = adder->latency_ms;
      adder->latency_ms = new_latency;
      GST_OBJECT_UNLOCK (adder);

      /* post message if latency changed, this will inform the parent pipeline
       * that a latency reconfiguration is possible/needed. */
      if (new_latency != old_latency) {
        GST_DEBUG_OBJECT (adder, "latency changed to: %" GST_TIME_FORMAT,
            GST_TIME_ARGS (new_latency));

        gst_element_post_message (GST_ELEMENT_CAST (adder),
            gst_message_new_latency (GST_OBJECT_CAST (adder)));
      }
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}


static void
gst_live_adder_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstLiveAdder *adder = GST_LIVE_ADDER (object);

  switch (prop_id) {
    case PROP_LATENCY:
      GST_OBJECT_LOCK (adder);
      g_value_set_uint (value, adder->latency_ms);
      GST_OBJECT_UNLOCK (adder);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}


/* we can only accept caps that we and downstream can handle. */
static GstCaps *
gst_live_adder_sink_getcaps (GstPad * pad)
{
  GstLiveAdder *adder;
  GstCaps *result, *peercaps, *sinkcaps;

  adder = GST_LIVE_ADDER (GST_PAD_PARENT (pad));

  /* get the downstream possible caps */
  peercaps = gst_pad_peer_get_caps (adder->srcpad);
  /* get the allowed caps on this sinkpad, we use the fixed caps function so
   * that it does not call recursively in this function. */
  sinkcaps = gst_pad_get_fixed_caps_func (pad);
  if (peercaps) {
    /* if the peer has caps, intersect */
    GST_DEBUG_OBJECT (adder, "intersecting peer and template caps");
    result = gst_caps_intersect (peercaps, sinkcaps);
    gst_caps_unref (peercaps);
    gst_caps_unref (sinkcaps);
  } else {
    /* the peer has no caps (or there is no peer), just use the allowed caps
     * of this sinkpad. */
    GST_DEBUG_OBJECT (adder, "no peer caps, using sinkcaps");
    result = sinkcaps;
  }

  return result;
}

/* the first caps we receive on any of the sinkpads will define the caps for all
 * the other sinkpads because we can only mix streams with the same caps.
 * */
static gboolean
gst_live_adder_setcaps (GstPad * pad, GstCaps * caps)
{
  GstLiveAdder *adder;
  GList *pads;
  GstStructure *structure;
  const char *media_type;

  adder = GST_LIVE_ADDER (GST_PAD_PARENT (pad));

  GST_LOG_OBJECT (adder, "setting caps on pad %p,%s to %" GST_PTR_FORMAT, pad,
      GST_PAD_NAME (pad), caps);

  /* FIXME, see if the other pads can accept the format. Also lock the
   * format on the other pads to this new format. */
  GST_OBJECT_LOCK (adder);
  pads = GST_ELEMENT (adder)->pads;
  while (pads) {
    GstPad *otherpad = GST_PAD (pads->data);

    if (otherpad != pad)
      gst_caps_replace (&GST_PAD_CAPS (otherpad), caps);

    pads = g_list_next (pads);
  }
  GST_OBJECT_UNLOCK (adder);

  /* parse caps now */
  structure = gst_caps_get_structure (caps, 0);
  media_type = gst_structure_get_name (structure);
  if (strcmp (media_type, "audio/x-raw-int") == 0) {
    GST_DEBUG_OBJECT (adder, "parse_caps sets adder to format int");
    adder->format = GST_LIVE_ADDER_FORMAT_INT;
    gst_structure_get_int (structure, "width", &adder->width);
    gst_structure_get_int (structure, "depth", &adder->depth);
    gst_structure_get_int (structure, "endianness", &adder->endianness);
    gst_structure_get_boolean (structure, "signed", &adder->is_signed);

    if (adder->endianness != G_BYTE_ORDER)
      goto not_supported;

    switch (adder->width) {
      case 8:
        adder->func = (adder->is_signed ?
            (GstLiveAdderFunction) add_int8 : (GstLiveAdderFunction) add_uint8);
        break;
      case 16:
        adder->func = (adder->is_signed ?
            (GstLiveAdderFunction) add_int16 : (GstLiveAdderFunction) add_uint16);
        break;
      case 32:
        adder->func = (adder->is_signed ?
            (GstLiveAdderFunction) add_int32 : (GstLiveAdderFunction) add_uint32);
        break;
      default:
        goto not_supported;
    }
  } else if (strcmp (media_type, "audio/x-raw-float") == 0) {
    GST_DEBUG_OBJECT (adder, "parse_caps sets adder to format float");
    adder->format = GST_LIVE_ADDER_FORMAT_FLOAT;
    gst_structure_get_int (structure, "width", &adder->width);

    switch (adder->width) {
      case 32:
        adder->func = (GstLiveAdderFunction) add_float32;
        break;
      case 64:
        adder->func = (GstLiveAdderFunction) add_float64;
        break;
      default:
        goto not_supported;
    }
  } else {
    goto not_supported;
  }

  gst_structure_get_int (structure, "channels", &adder->channels);
  gst_structure_get_int (structure, "rate", &adder->rate);
  /* precalc bps */
  adder->bps = (adder->width / 8) * adder->channels;

  return TRUE;

  /* ERRORS */
not_supported:
  {
    GST_DEBUG_OBJECT (adder, "unsupported format set as caps");
    return FALSE;
  }
}

static void
gst_live_adder_flush_start (GstLiveAdder * adder)
{
  GST_DEBUG_OBJECT (adder, "Disabling pop on queue");

  GST_OBJECT_LOCK (adder);
  /* mark ourselves as flushing */
  adder->srcresult = GST_FLOW_WRONG_STATE;
  /* unlock clock, we just unschedule, the entry will be released by the
   * locking streaming thread. */
  if (adder->clock_id)
    gst_clock_id_unschedule (adder->clock_id);
  GST_OBJECT_UNLOCK (adder);
}

static void
gst_live_adder_flush_stop (GstLiveAdder * adder)
{
  GST_DEBUG_OBJECT (adder, "Enabling pop on queue");

  /* Mark as non flushing */
  GST_OBJECT_LOCK (adder);
  adder->srcresult = GST_FLOW_OK;
  GST_OBJECT_UNLOCK (adder);
}

static gboolean
gst_live_adder_src_activate_push (GstPad * pad, gboolean active)
{
  gboolean result = TRUE;
  GstLiveAdder *adder = NULL;

  adder = GST_LIVE_ADDER (gst_pad_get_parent (pad));

  if (active) {
    /* allow data processing */
    gst_live_adder_flush_stop (adder);

    /* start pushing out buffers */
    GST_DEBUG_OBJECT (adder, "Starting task on srcpad");
    gst_pad_start_task (adder->srcpad,
        (GstTaskFunction) gst_live_adder_loop, adder);
  } else {
    /* make sure all data processing stops ASAP */
    gst_live_adder_flush_start (adder);

    /* NOTE this will hardlock if the state change is called from the src pad
     * task thread because we will _join() the thread. */
    GST_DEBUG_OBJECT (adder, "Stopping task on srcpad");
    result = gst_pad_stop_task (pad);
  }

  gst_object_unref (adder);

  return result;
}

static gboolean
gst_live_adder_sink_event (GstPad * pad, GstEvent * event)
{
  gboolean ret = TRUE;
  GstLiveAdder *adder = NULL;
  GstLiveAdderPadPrivate *padprivate = NULL;

  adder = GST_LIVE_ADDER (gst_pad_get_parent (pad));

  padprivate = gst_pad_get_element_private (pad);

  if (!padprivate)
    return FALSE;

  GST_LOG_OBJECT (adder, "received %s", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_NEWSEGMENT:
    {
      GstFormat format;
      gdouble rate, arate;
      gint64 start, stop, time;
      gboolean update;

      gst_event_parse_new_segment_full (event, &update, &rate, &arate, &format,
          &start, &stop, &time);

      /* we need time for now */
      if (format != GST_FORMAT_TIME)
        goto newseg_wrong_format;

      GST_DEBUG_OBJECT (adder,
          "newsegment: update %d, rate %g, arate %g, start %" GST_TIME_FORMAT
          ", stop %" GST_TIME_FORMAT ", time %" GST_TIME_FORMAT,
          update, rate, arate, GST_TIME_ARGS (start), GST_TIME_ARGS (stop),
          GST_TIME_ARGS (time));

      /* now configure the values, we need these to time the release of the
       * buffers on the srcpad. */
      GST_OBJECT_LOCK (adder);
      gst_segment_set_newsegment_full (&padprivate->segment, update,
          rate, arate, format, start, stop, time);
      GST_OBJECT_UNLOCK (adder);

      gst_event_unref (event);
      break;
    }
    case GST_EVENT_FLUSH_START:
      gst_live_adder_flush_start (adder);
      ret = gst_pad_push_event (adder->srcpad, event);
      break;
    case GST_EVENT_FLUSH_STOP:
      ret = gst_pad_push_event (adder->srcpad, event);
      ret = gst_live_adder_src_activate_push (adder->srcpad, TRUE);
      GST_OBJECT_LOCK (adder);
      adder->segment_pending = TRUE;
      GST_OBJECT_UNLOCK (adder);
      break;
    case GST_EVENT_EOS:
    {
      GST_OBJECT_LOCK (adder);

      ret = adder->srcresult == GST_FLOW_OK;
      if (ret && !padprivate->eos) {
        GST_DEBUG_OBJECT (adder, "queuing EOS");
        padprivate->eos = TRUE;
        g_cond_signal (adder->not_empty_cond);
      } else if (padprivate->eos) {
        GST_DEBUG_OBJECT (adder, "dropping EOS, we are already EOS");
      } else {
        GST_DEBUG_OBJECT (adder, "dropping EOS, reason %s",
            gst_flow_get_name (adder->srcresult));
      }

      GST_OBJECT_UNLOCK (adder);

      gst_event_unref (event);
      break;
    }
    default:
      ret = gst_pad_push_event (adder->srcpad, event);
      break;
  }

done:
  gst_object_unref (adder);

  return ret;

  /* ERRORS */
newseg_wrong_format:
  {
    GST_DEBUG_OBJECT (adder, "received non TIME newsegment");
    ret = FALSE;
    goto done;
  }
}

static gboolean
gst_live_adder_query (GstPad * pad, GstQuery * query)
{
  GstLiveAdder *adder;
  gboolean res = FALSE;

  adder = GST_LIVE_ADDER (gst_pad_get_parent (pad));

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
    {
      /* We need to send the query upstream and add the returned latency to our
       * own */
      GstClockTime min_latency = G_MAXUINT64, max_latency = 0;
      gpointer item;
      GstIterator *iter = NULL;
      gboolean done = FALSE;

      iter = gst_element_iterate_sink_pads (GST_ELEMENT (adder));

      while (!done) {
        switch (gst_iterator_next (iter, &item)) {
          case GST_ITERATOR_OK:
            {
              GstPad *sinkpad = item;
              GstClockTime pad_min_latency, pad_max_latency;
              gboolean pad_us_live;

              if (gst_pad_peer_query (sinkpad, query)) {
                gst_query_parse_latency (query, &pad_us_live, &pad_min_latency,
                    &pad_max_latency);

                res = TRUE;

                GST_DEBUG_OBJECT (adder, "Peer latency for pad %s: min %"
                    GST_TIME_FORMAT " max %" GST_TIME_FORMAT,
                    GST_PAD_NAME (sinkpad),
                    GST_TIME_ARGS (pad_min_latency), GST_TIME_ARGS (pad_max_latency));

                min_latency = MIN (pad_min_latency, min_latency);
                max_latency = MAX (pad_max_latency, max_latency);
              }
              gst_object_unref (item);
            }
            break;
          case GST_ITERATOR_RESYNC:
            min_latency = G_MAXUINT64;
            max_latency = 0;

            gst_iterator_resync (iter);
            break;
          case GST_ITERATOR_ERROR:
            GST_ERROR_OBJECT (adder, "Error looping sink pads");
            done = TRUE;
            break;
          case GST_ITERATOR_DONE:
            done = TRUE;
            break;
        }
      }
      gst_iterator_free (iter);

      if (res) {
        if (min_latency == G_MAXUINT64)
          min_latency = 0;

        GST_OBJECT_LOCK (adder);
        adder->peer_latency = min_latency;
        min_latency += adder->latency_ms * GST_MSECOND;
        GST_OBJECT_UNLOCK (adder);

        max_latency = MAX (max_latency, min_latency);
        gst_query_set_latency (query, TRUE, min_latency, max_latency);
        GST_DEBUG_OBJECT (adder, "Calculated total latency : min %"
            GST_TIME_FORMAT " max %" GST_TIME_FORMAT,
            GST_TIME_ARGS (min_latency), GST_TIME_ARGS (max_latency));
      }
      break;
    }
    default:
      res = gst_pad_query_default (pad, query);
      break;
  }

  gst_object_unref (adder);

  return res;
}

static gboolean
forward_event_func (GstPad * pad, GValue * ret, GstEvent * event)
{
  gst_event_ref (event);
  GST_LOG_OBJECT (pad, "About to send event %s", GST_EVENT_TYPE_NAME (event));
  if (!gst_pad_push_event (pad, event)) {
    g_value_set_boolean (ret, FALSE);
    GST_WARNING_OBJECT (pad, "Sending event  %p (%s) failed.",
        event, GST_EVENT_TYPE_NAME (event));
  } else {
    GST_LOG_OBJECT (pad, "Sent event  %p (%s).",
        event, GST_EVENT_TYPE_NAME (event));
  }
  gst_object_unref (pad);
  return TRUE;
}

/* forwards the event to all sinkpads, takes ownership of the
 * event
 *
 * Returns: TRUE if the event could be forwarded on all
 * sinkpads.
 */
static gboolean
forward_event (GstLiveAdder * adder, GstEvent * event)
{
  gboolean ret;
  GstIterator *it;
  GValue vret = { 0 };

  GST_LOG_OBJECT (adder, "Forwarding event %p (%s)", event,
      GST_EVENT_TYPE_NAME (event));

  ret = TRUE;

  g_value_init (&vret, G_TYPE_BOOLEAN);
  g_value_set_boolean (&vret, TRUE);
  it = gst_element_iterate_sink_pads (GST_ELEMENT_CAST (adder));
  gst_iterator_fold (it, (GstIteratorFoldFunction) forward_event_func, &vret,
      event);
  gst_iterator_free (it);
  gst_event_unref (event);

  ret = g_value_get_boolean (&vret);

  return ret;
}


static gboolean
gst_live_adder_src_event (GstPad * pad, GstEvent * event)
{
  GstLiveAdder *adder;
  gboolean result;

  adder = GST_LIVE_ADDER (gst_pad_get_parent (pad));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_QOS:
      /* QoS might be tricky */
      result = FALSE;
      break;
    case GST_EVENT_SEEK:
      /* I'm not certain how to handle seeks yet */
      result = FALSE;
      break;
    case GST_EVENT_NAVIGATION:
      /* navigation is rather pointless. */
      result = FALSE;
      break;
    default:
      /* just forward the rest for now */
      result = forward_event (adder, event);
      break;

  }
  gst_object_unref (adder);

  return result;
}

static guint
gst_live_adder_length_from_duration (GstLiveAdder *adder, GstClockTime duration)
{
  guint64 ret = (duration * adder->rate / GST_SECOND) * adder->bps;

  return (guint) ret;
}

static GstFlowReturn
gst_live_live_adder_chain (GstPad *pad, GstBuffer *buffer)
{
  GstLiveAdder *adder = GST_LIVE_ADDER (gst_pad_get_parent_element (pad));
  GstLiveAdderPadPrivate *padprivate = NULL;
  GstFlowReturn ret = GST_FLOW_OK;
  GList *item = NULL;
  GstClockTime skip = 0;

  GST_OBJECT_LOCK (adder);

  ret = adder->srcresult;

  if (ret != GST_FLOW_OK)
  {
    gst_buffer_unref (buffer);
    goto out;
  }

  padprivate = gst_pad_get_element_private (pad);

  if (!padprivate)
  {
    ret = GST_FLOW_NOT_LINKED;
    gst_buffer_unref (buffer);
    goto out;
  }

  if (padprivate->eos)
  {
    GST_DEBUG_OBJECT (adder, "Received buffer after EOS");
    ret = GST_FLOW_UNEXPECTED;
    gst_buffer_unref (buffer);
    goto out;
  }

  if (!GST_BUFFER_TIMESTAMP_IS_VALID(buffer)) {
    GST_ELEMENT_ERROR (adder, STREAM, FAILED,
        ("Buffer without a valid timestamp received"), (""));
    ret = GST_FLOW_ERROR;
    gst_buffer_unref (buffer);
    goto out;
  }

  /* Just see if we receive invalid timestamp/durations */
  if (GST_CLOCK_TIME_IS_VALID (padprivate->next_timestamp) &&
      !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT) &&
      GST_BUFFER_TIMESTAMP(buffer) != padprivate->next_timestamp)
    GST_ERROR_OBJECT (adder,
        "Timestamp discontinuity without the DISCONT flag set"
        " (expected %" GST_TIME_FORMAT ", got %" GST_TIME_FORMAT")",
        GST_TIME_ARGS (padprivate->next_timestamp),
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buffer)));

  padprivate->next_timestamp = GST_BUFFER_TIMESTAMP (buffer) +
      GST_BUFFER_DURATION (buffer);

  buffer = gst_buffer_make_metadata_writable (buffer);

  /*
   * Lets clip the buffer to the segment (so we don't have to worry about
   * cliping afterwards).
   * This should also guarantee us that we'll have valid timestamps and
   * durations afterwards
   */

  buffer = gst_audio_buffer_clip (buffer, &padprivate->segment, adder->rate,
      adder->bps);

  /*
   * Make sure all incoming buffers share the same timestamping
   */
  GST_BUFFER_TIMESTAMP (buffer) = gst_segment_to_running_time (
      &padprivate->segment, padprivate->segment.format,
      GST_BUFFER_TIMESTAMP (buffer));


  if (GST_CLOCK_TIME_IS_VALID (adder->next_timestamp) &&
      GST_BUFFER_TIMESTAMP (buffer) < adder->next_timestamp) {
    if (GST_BUFFER_TIMESTAMP (buffer) + GST_BUFFER_DURATION (buffer) <
        adder->next_timestamp) {
      GST_DEBUG_OBJECT (adder, "Buffer is late, dropping (ts: %" GST_TIME_FORMAT
          " duration: %" GST_TIME_FORMAT ")",
          GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buffer)),
          GST_TIME_ARGS (GST_BUFFER_DURATION (buffer)));
      gst_buffer_unref (buffer);
      goto out;
    } else {
      skip = adder->next_timestamp - GST_BUFFER_TIMESTAMP (buffer);
      GST_DEBUG_OBJECT (adder, "Buffer is partially late, skipping %"
          GST_TIME_FORMAT,
          GST_TIME_ARGS (skip));
    }
  }

  /* If our new buffer's head is higher than the queue's head, lets wake up,
   * we may not have to wait for as long
   */
  if (adder->clock_id &&
      GST_BUFFER_TIMESTAMP (buffer) + skip <
      GST_BUFFER_TIMESTAMP (g_queue_peek_head (adder->buffers)))
    gst_clock_id_unschedule (adder->clock_id);

  for (item = g_queue_peek_head_link (adder->buffers);
       item;
       item = g_list_next (item)) {
    GstBuffer *oldbuffer = item->data;
    GstClockTime old_skip = 0;
    GstClockTime mix_duration = 0;
    GstClockTime mix_start = 0;
    GstClockTime mix_end = 0;

    /* We haven't reached our place yet */
    if (GST_BUFFER_TIMESTAMP (buffer) + skip >=
        GST_BUFFER_TIMESTAMP (oldbuffer) + GST_BUFFER_DURATION (oldbuffer))
      continue;

    /* We're past our place, lets insert ouselves here */
    if (GST_BUFFER_TIMESTAMP (buffer) + GST_BUFFER_DURATION (buffer) <=
        GST_BUFFER_TIMESTAMP (oldbuffer))
      break;

    /* if we reach this spot, we have overlap, so we must mix */

    /* First make a subbuffer with the non-overlapping part */
    if (GST_BUFFER_TIMESTAMP (buffer) + skip <
        GST_BUFFER_TIMESTAMP (oldbuffer)) {
      GstBuffer *subbuffer = NULL;
      GstClockTime subbuffer_duration = GST_BUFFER_TIMESTAMP (oldbuffer) -
          (GST_BUFFER_TIMESTAMP (buffer) + skip);

      subbuffer = gst_buffer_create_sub (buffer,
          gst_live_adder_length_from_duration (adder, skip),
          gst_live_adder_length_from_duration (adder, subbuffer_duration));

      GST_BUFFER_TIMESTAMP (subbuffer) = GST_BUFFER_TIMESTAMP (buffer) + skip;
      GST_BUFFER_DURATION (subbuffer) = subbuffer_duration;

      skip += subbuffer_duration;

      g_queue_insert_before (adder->buffers, item, subbuffer);
    }

    /* Now we are on the overlapping part */
    oldbuffer = gst_buffer_make_writable (oldbuffer);
    item->data = oldbuffer;

    old_skip = GST_BUFFER_TIMESTAMP (buffer) + skip -
        GST_BUFFER_TIMESTAMP (oldbuffer);

    if (GST_BUFFER_TIMESTAMP (buffer) + skip >
        GST_BUFFER_TIMESTAMP (oldbuffer) + old_skip)
      mix_start = GST_BUFFER_TIMESTAMP (buffer) + skip;
    else
      mix_start = GST_BUFFER_TIMESTAMP (oldbuffer) + old_skip;

    if (GST_BUFFER_TIMESTAMP (buffer) + GST_BUFFER_DURATION (buffer) <
        GST_BUFFER_TIMESTAMP (oldbuffer) + GST_BUFFER_DURATION (oldbuffer))
      mix_end = GST_BUFFER_TIMESTAMP (buffer) + GST_BUFFER_DURATION (buffer);
    else
      mix_end = GST_BUFFER_TIMESTAMP (oldbuffer) +
          GST_BUFFER_DURATION (oldbuffer);

    mix_duration = mix_end - mix_start;

    adder->func (GST_BUFFER_DATA (oldbuffer) +
        gst_live_adder_length_from_duration (adder, old_skip),
        GST_BUFFER_DATA (buffer) +
        gst_live_adder_length_from_duration (adder, skip),
        gst_live_adder_length_from_duration (adder, mix_duration));

    skip += mix_duration;
  }

  g_cond_signal (adder->not_empty_cond);

  if (skip == GST_BUFFER_DURATION (buffer)) {
    gst_buffer_unref (buffer);
  } else {
    if (skip) {
      GstClockTime subbuffer_duration = GST_BUFFER_DURATION (buffer) - skip;
      GstClockTime subbuffer_ts = GST_BUFFER_TIMESTAMP (buffer) + skip;

      buffer = gst_buffer_create_sub (buffer,
          gst_live_adder_length_from_duration (adder, skip),
          gst_live_adder_length_from_duration (adder, subbuffer_duration));
      GST_BUFFER_TIMESTAMP (buffer) = subbuffer_ts;
      GST_BUFFER_DURATION (buffer) = subbuffer_duration;
    }

    if (item)
      g_queue_insert_before (adder->buffers, item, buffer);
    else
      g_queue_push_tail (adder->buffers, buffer);
  }

 out:

  GST_OBJECT_UNLOCK (adder);
  gst_object_unref (adder);

  return ret;
}

/*
 * This only works because the GstObject lock is taken
 *
 * It checks if all sink pads are EOS
 */
static gboolean
check_eos_locked (GstLiveAdder *adder)
{
  GList *item;

  for (item = adder->sinkpads;
       item;
       item = g_list_next (item))
  {
    GstPad *pad = item->data;
    GstLiveAdderPadPrivate *padprivate = gst_pad_get_element_private (pad);

    if (padprivate && padprivate->eos != TRUE)
      return FALSE;
  }

  if (item)
    return TRUE;
  else
    return FALSE;
}

static void
gst_live_adder_loop (gpointer data)
{
  GstLiveAdder *adder = GST_LIVE_ADDER (data);
  GstClockTime buffer_timestamp = 0;
  GstClockTime sync_time = 0;
  GstClock *clock = NULL;
  GstClockID id = NULL;
  GstClockReturn ret;
  GstBuffer *buffer = NULL;
  GstFlowReturn result;
  GstEvent *newseg_event = NULL;

  GST_OBJECT_LOCK (adder);

 again:

  for (;;)
  {
    if (!g_queue_is_empty (adder->buffers))
      break;
    if (check_eos_locked (adder))
      goto eos;
    if (adder->srcresult != GST_FLOW_OK)
      goto flushing;
    g_cond_wait (adder->not_empty_cond, GST_OBJECT_GET_LOCK(adder));
  }

  buffer_timestamp = GST_BUFFER_TIMESTAMP (g_queue_peek_head (adder->buffers));

  clock = GST_ELEMENT_CLOCK (adder);
  if (!clock)
    /* let's just push if there is no clock */
    goto push_buffer;

  GST_DEBUG_OBJECT (adder, "sync to timestamp %" GST_TIME_FORMAT,
      GST_TIME_ARGS (buffer_timestamp));

  sync_time = buffer_timestamp + GST_ELEMENT_CAST (adder)->base_time;
  /* add latency, this includes our own latency and the peer latency. */
  sync_time += adder->latency_ms * GST_MSECOND;
  sync_time += adder->peer_latency;

  /* create an entry for the clock */
  id = adder->clock_id = gst_clock_new_single_shot_id (clock, sync_time);
  GST_OBJECT_UNLOCK (adder);

  ret = gst_clock_id_wait (id, NULL);

  GST_OBJECT_LOCK (adder);

  /* and free the entry */
  gst_clock_id_unref (id);
  adder->clock_id = NULL;

  /* at this point, the clock could have been unlocked by a timeout, a new
   * head element was added to the queue or because we are shutting down. Check
   * for shutdown first. */

  if (adder->srcresult != GST_FLOW_OK)
    goto flushing;

  if (ret == GST_CLOCK_UNSCHEDULED) {
    GST_DEBUG_OBJECT (adder,
        "Wait got unscheduled, will retry to push with new buffer");
    goto again;
  }

  if (ret != GST_CLOCK_OK && ret != GST_CLOCK_EARLY)
    goto clock_error;

 push_buffer:

  buffer = g_queue_pop_head (adder->buffers);

  if (!buffer)
    goto again;

  if (GST_CLOCK_TIME_IS_VALID (adder->next_timestamp) &&
      GST_BUFFER_TIMESTAMP (buffer) != adder->next_timestamp)
  {
    if (llabs (GST_BUFFER_TIMESTAMP (buffer) - adder->next_timestamp) <
        GST_SECOND / adder->rate) {
      GST_BUFFER_TIMESTAMP (buffer) = adder->next_timestamp;
      GST_DEBUG_OBJECT (adder, "Correcting slight skew");
      GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_DISCONT);
    } else {
      GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
      GST_DEBUG_OBJECT (adder, "Expected buffer at %" GST_TIME_FORMAT
          ", but is at %" GST_TIME_FORMAT", setting discont",
          GST_TIME_ARGS (adder->next_timestamp),
          GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buffer)));
    }
  } else {
    GST_DEBUG_OBJECT (adder, "Continuous buffer");
    GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_DISCONT);
  }

  adder->next_timestamp = GST_BUFFER_TIMESTAMP (buffer) +
      GST_BUFFER_DURATION (buffer);

  if (adder->segment_pending)
  {
    /*
     * We should probably loop through all of the sinks that have a segment
     * and take the min of the starts and the max of the stops
     * and convert them to running times and use these are start/stop.
     * And so something smart about the positions with seeks that I dont
     * understand yet.
     */
     newseg_event = gst_event_new_new_segment_full (FALSE, 1.0,
        1.0, GST_FORMAT_TIME, GST_BUFFER_TIMESTAMP (buffer), -1,
        0);

    adder->segment_pending = FALSE;
  }

  GST_OBJECT_UNLOCK (adder);

  if (newseg_event)
    gst_pad_push_event (adder->srcpad, newseg_event);

  result = gst_pad_push (adder->srcpad, buffer);
  if (result != GST_FLOW_OK)
    goto pause;

  return;

 flushing:
  {
    GST_DEBUG_OBJECT (adder, "we are flushing");
    gst_pad_pause_task (adder->srcpad);
    GST_OBJECT_UNLOCK (adder);
    return;
  }

 clock_error:
 {
   gst_pad_pause_task (adder->srcpad);
   GST_OBJECT_UNLOCK (adder);
   GST_ELEMENT_ERROR (adder, STREAM, MUX, ("Error with the clock"),
       ("Error with the clock: %d", ret));
   GST_ERROR_OBJECT (adder, "Error with the clock: %d", ret);
   return;
 }

 pause:
  {
    const gchar *reason = gst_flow_get_name (result);

    GST_DEBUG_OBJECT (adder, "pausing task, reason %s", reason);

    GST_OBJECT_LOCK (adder);

    /* store result */
    adder->srcresult = result;
    /* we don't post errors or anything because upstream will do that for us
     * when we pass the return value upstream. */
    gst_pad_pause_task (adder->srcpad);
    GST_OBJECT_UNLOCK (adder);
    return;
  }

 eos:
  {
    /* store result, we are flushing now */
    GST_DEBUG_OBJECT (adder, "We are EOS, pushing EOS downstream");
    adder->srcresult = GST_FLOW_UNEXPECTED;
    gst_pad_pause_task (adder->srcpad);
    gst_pad_push_event (adder->srcpad, gst_event_new_eos ());
    GST_OBJECT_UNLOCK (adder);
    return;
  }
}

static GstPad *
gst_live_adder_request_new_pad (GstElement * element, GstPadTemplate * templ,
    const gchar * unused)
{
  gchar *name;
  GstLiveAdder *adder;
  GstPad *newpad;
  gint padcount;
  GstLiveAdderPadPrivate *padprivate = NULL;

  if (templ->direction != GST_PAD_SINK)
    goto not_sink;

  adder = GST_LIVE_ADDER (element);

  /* increment pad counter */
  padcount = g_atomic_int_exchange_and_add (&adder->padcount, 1);

  name = g_strdup_printf ("sink%d", padcount);
  newpad = gst_pad_new_from_template (templ, name);
  GST_DEBUG_OBJECT (adder, "request new pad %s", name);
  g_free (name);

  gst_pad_set_getcaps_function (newpad,
      GST_DEBUG_FUNCPTR (gst_live_adder_sink_getcaps));
  gst_pad_set_setcaps_function (newpad,
      GST_DEBUG_FUNCPTR (gst_live_adder_setcaps));
  gst_pad_set_event_function (newpad,
      GST_DEBUG_FUNCPTR (gst_live_adder_sink_event));

  padprivate = g_new0 (GstLiveAdderPadPrivate, 1);

  gst_segment_init (&padprivate->segment, GST_FORMAT_UNDEFINED);
  padprivate->eos = FALSE;
  padprivate->next_timestamp = GST_CLOCK_TIME_NONE;

  gst_pad_set_element_private (newpad, padprivate);

  gst_pad_set_chain_function (newpad, gst_live_live_adder_chain);

  /* takes ownership of the pad */
  if (!gst_element_add_pad (GST_ELEMENT (adder), newpad))
    goto could_not_add;

  GST_OBJECT_LOCK (adder);
  adder->sinkpads = g_list_prepend (adder->sinkpads, newpad);
  GST_OBJECT_UNLOCK (adder);

  return newpad;

  /* errors */
not_sink:
  {
    g_warning ("gstadder: request new pad that is not a SINK pad\n");
    return NULL;
  }
could_not_add:
  {
    GST_DEBUG_OBJECT (adder, "could not add pad");
    gst_object_unref (newpad);
    return NULL;
  }
}

static void
gst_live_adder_release_pad (GstElement * element, GstPad * pad)
{
  GstLiveAdder *adder;
  GstLiveAdderPadPrivate *padprivate;

  adder = GST_LIVE_ADDER (element);

  GST_DEBUG_OBJECT (adder, "release pad %s:%s", GST_DEBUG_PAD_NAME (pad));

  GST_OBJECT_LOCK (element);
  padprivate = gst_pad_get_element_private (pad);
  gst_pad_set_element_private (pad, NULL);
    adder->sinkpads = g_list_remove_all (adder->sinkpads, pad);
  GST_OBJECT_UNLOCK (element);

  g_free (padprivate);

  gst_element_remove_pad (element, pad);
}

static void
reset_pad_private (gpointer data, gpointer user_data)
{
  GstPad *pad = data;
  //GstLiveAdder *adder = user_data;
  GstLiveAdderPadPrivate *padprivate;

  padprivate = gst_pad_get_element_private (pad);

  if (!padprivate)
    return;

  gst_segment_init (&padprivate->segment, GST_FORMAT_UNDEFINED);

  padprivate->next_timestamp = GST_CLOCK_TIME_NONE;
  padprivate->eos = FALSE;
}

static GstStateChangeReturn
gst_live_adder_change_state (GstElement * element, GstStateChange transition)
{
  GstLiveAdder *adder;
  GstStateChangeReturn ret;

  adder = GST_LIVE_ADDER (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      {
        GST_OBJECT_LOCK (adder);
        adder->segment_pending = TRUE;
        adder->peer_latency = 0;
        adder->next_timestamp = GST_CLOCK_TIME_NONE;
        g_list_foreach (adder->sinkpads, reset_pad_private, adder);
        GST_OBJECT_UNLOCK (adder);
        break;
      }
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    default:
      break;
  }

  return ret;
}


static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, "liveadder", GST_RANK_NONE,
          GST_TYPE_LIVE_ADDER)) {
    return FALSE;
  }

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "liveadder",
    "Adds multiple live discontinuous streams",
    plugin_init, VERSION, "LGPL", "Farsight", "http://farsight.sf.net")