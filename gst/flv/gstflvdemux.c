/* GStreamer
 * Copyright (C) <2007> Julien Moutte <julien@moutte.net>
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

#include "gstflvdemux.h"
#include "gstflvparse.h"

#include <string.h>

static GstStaticPadTemplate flv_sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-flv")
    );

static GstStaticPadTemplate audio_src_template =
GST_STATIC_PAD_TEMPLATE ("audio",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate video_src_template =
GST_STATIC_PAD_TEMPLATE ("video",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstElementDetails flv_demux_details = {
  "FLV Demuxer",
  "Codec/Demuxer",
  "Demux FLV feeds into digital streams",
  "Julien Moutte <julien@moutte.net>"
};

GST_DEBUG_CATEGORY (flvdemux_debug);
#define GST_CAT_DEFAULT flvdemux_debug

GST_BOILERPLATE (GstFLVDemux, gst_flv_demux, GstElement, GST_TYPE_ELEMENT);

#define FLV_HEADER_SIZE 13
#define FLV_TAG_TYPE_SIZE 4

static void
gst_flv_demux_flush (GstFLVDemux * demux, gboolean discont)
{
  GST_DEBUG_OBJECT (demux, "flushing queued data in the FLV demuxer");

  gst_adapter_clear (demux->adapter);

  demux->audio_need_discont = TRUE;
  demux->video_need_discont = TRUE;
}

static void
gst_flv_demux_cleanup (GstFLVDemux * demux)
{
  GST_DEBUG_OBJECT (demux, "cleaning up FLV demuxer");

  demux->state = FLV_STATE_HEADER;

  demux->need_header = TRUE;
  demux->audio_need_segment = TRUE;
  demux->video_need_segment = TRUE;
  demux->audio_need_discont = TRUE;
  demux->video_need_discont = TRUE;

  /* By default we consider them as linked */
  demux->audio_linked = TRUE;
  demux->video_linked = TRUE;

  demux->has_audio = FALSE;
  demux->has_video = FALSE;
  demux->push_tags = FALSE;

  demux->video_offset = 0;
  demux->audio_offset = 0;
  demux->offset = demux->tag_size = demux->tag_data_size = 0;
  demux->duration = GST_CLOCK_TIME_NONE;

  if (demux->new_seg_event) {
    gst_event_unref (demux->new_seg_event);
    demux->new_seg_event = NULL;
  }

  gst_adapter_clear (demux->adapter);

  if (demux->audio_pad) {
    gst_element_remove_pad (GST_ELEMENT (demux), demux->audio_pad);
    gst_object_unref (demux->audio_pad);
    demux->audio_pad = NULL;
  }

  if (demux->video_pad) {
    gst_element_remove_pad (GST_ELEMENT (demux), demux->video_pad);
    gst_object_unref (demux->video_pad);
    demux->video_pad = NULL;
  }
}

static GstFlowReturn
gst_flv_demux_chain (GstPad * pad, GstBuffer * buffer)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstFLVDemux *demux = NULL;

  demux = GST_FLV_DEMUX (gst_pad_get_parent (pad));

  gst_adapter_push (demux->adapter, buffer);

parse:
  switch (demux->state) {
    case FLV_STATE_HEADER:
    {
      if (gst_adapter_available (demux->adapter) >= FLV_HEADER_SIZE) {
        const guint8 *data;

        data = gst_adapter_peek (demux->adapter, FLV_HEADER_SIZE);

        ret = gst_flv_parse_header (demux, data, FLV_HEADER_SIZE);

        gst_adapter_flush (demux->adapter, FLV_HEADER_SIZE);

        demux->state = FLV_STATE_TAG_TYPE;
        goto parse;
      } else {
        goto beach;
      }
    }
    case FLV_STATE_TAG_TYPE:
    {
      if (gst_adapter_available (demux->adapter) >= FLV_TAG_TYPE_SIZE) {
        const guint8 *data;

        data = gst_adapter_peek (demux->adapter, FLV_TAG_TYPE_SIZE);

        ret = gst_flv_parse_tag_type (demux, data, FLV_TAG_TYPE_SIZE);

        gst_adapter_flush (demux->adapter, FLV_TAG_TYPE_SIZE);

        goto parse;
      } else {
        goto beach;
      }
    }
    case FLV_STATE_TAG_VIDEO:
    {
      if (gst_adapter_available (demux->adapter) >= demux->tag_size) {
        const guint8 *data;

        data = gst_adapter_peek (demux->adapter, demux->tag_size);

        ret = gst_flv_parse_tag_video (demux, data, demux->tag_size);

        gst_adapter_flush (demux->adapter, demux->tag_size);

        demux->state = FLV_STATE_TAG_TYPE;
        goto parse;
      } else {
        goto beach;
      }
    }
    case FLV_STATE_TAG_AUDIO:
    {
      if (gst_adapter_available (demux->adapter) >= demux->tag_size) {
        const guint8 *data;

        data = gst_adapter_peek (demux->adapter, demux->tag_size);

        ret = gst_flv_parse_tag_audio (demux, data, demux->tag_size);

        gst_adapter_flush (demux->adapter, demux->tag_size);

        demux->state = FLV_STATE_TAG_TYPE;
        goto parse;
      } else {
        goto beach;
      }
    }
    case FLV_STATE_TAG_SCRIPT:
    {
      if (gst_adapter_available (demux->adapter) >= demux->tag_size) {
        const guint8 *data;

        data = gst_adapter_peek (demux->adapter, demux->tag_size);

        ret = gst_flv_parse_tag_script (demux, data, demux->tag_size);

        gst_adapter_flush (demux->adapter, demux->tag_size);

        demux->state = FLV_STATE_TAG_TYPE;
        goto parse;
      } else {
        goto beach;
      }
    }
    default:
      GST_DEBUG_OBJECT (demux, "unexpected demuxer state");
  }

beach:
  if (G_UNLIKELY (ret == GST_FLOW_NOT_LINKED)) {
    /* If either audio or video is linked we return GST_FLOW_OK */
    if (demux->audio_linked || demux->video_linked) {
      ret = GST_FLOW_OK;
    }
  }

  gst_object_unref (demux);

  return ret;
}

static GstFlowReturn
gst_flv_demux_pull_tag (GstPad * pad, GstFLVDemux * demux)
{
  GstBuffer *buffer = NULL;
  GstFlowReturn ret = GST_FLOW_OK;

  /* Get the first 4 bytes to identify tag type and size */
  ret = gst_pad_pull_range (pad, demux->offset, FLV_TAG_TYPE_SIZE, &buffer);
  if (G_UNLIKELY (ret != GST_FLOW_OK)) {
    GST_WARNING_OBJECT (demux, "failed when pulling %d bytes",
        FLV_TAG_TYPE_SIZE);
    goto beach;
  }

  if (G_UNLIKELY (buffer && GST_BUFFER_SIZE (buffer) != FLV_TAG_TYPE_SIZE)) {
    GST_WARNING_OBJECT (demux, "partial pull got %d when expecting %d",
        GST_BUFFER_SIZE (buffer), FLV_TAG_TYPE_SIZE);
    gst_buffer_unref (buffer);
    ret = GST_FLOW_UNEXPECTED;
    goto beach;
  }

  /* Identify tag type */
  ret = gst_flv_parse_tag_type (demux, GST_BUFFER_DATA (buffer),
      GST_BUFFER_SIZE (buffer));

  gst_buffer_unref (buffer);

  /* Jump over tag type + size */
  demux->offset += FLV_TAG_TYPE_SIZE;

  /* Pull the whole tag */
  ret = gst_pad_pull_range (pad, demux->offset, demux->tag_size, &buffer);
  if (G_UNLIKELY (ret != GST_FLOW_OK)) {
    GST_WARNING_OBJECT (demux, "failed when pulling %d bytes", demux->tag_size);
    goto beach;
  }

  if (G_UNLIKELY (buffer && GST_BUFFER_SIZE (buffer) != demux->tag_size)) {
    GST_WARNING_OBJECT (demux, "partial pull got %d when expecting %d",
        GST_BUFFER_SIZE (buffer), demux->tag_size);
    gst_buffer_unref (buffer);
    ret = GST_FLOW_UNEXPECTED;
    goto beach;
  }

  switch (demux->state) {
    case FLV_STATE_TAG_VIDEO:
      ret = gst_flv_parse_tag_video (demux, GST_BUFFER_DATA (buffer),
          GST_BUFFER_SIZE (buffer));
      break;
    case FLV_STATE_TAG_AUDIO:
      ret = gst_flv_parse_tag_audio (demux, GST_BUFFER_DATA (buffer),
          GST_BUFFER_SIZE (buffer));
      break;
    case FLV_STATE_TAG_SCRIPT:
      ret = gst_flv_parse_tag_script (demux, GST_BUFFER_DATA (buffer),
          GST_BUFFER_SIZE (buffer));
      break;
    default:
      GST_WARNING_OBJECT (demux, "unexpected state %d", demux->state);
  }

  gst_buffer_unref (buffer);

  /* Jump over that part we've just parsed */
  demux->offset += demux->tag_size;

  /* Make sure we reinitialize the tag size */
  demux->tag_size = 0;

  /* Ready for the next tag */
  demux->state = FLV_STATE_TAG_TYPE;

  if (G_UNLIKELY (ret == GST_FLOW_NOT_LINKED)) {
    /* If either audio or video is linked we return GST_FLOW_OK */
    if (demux->audio_linked || demux->video_linked) {
      ret = GST_FLOW_OK;
    }
  }

beach:
  return ret;
}

static GstFlowReturn
gst_flv_demux_pull_header (GstPad * pad, GstFLVDemux * demux)
{
  GstBuffer *buffer = NULL;
  GstFlowReturn ret = GST_FLOW_OK;

  /* Get the first 9 bytes */
  ret = gst_pad_pull_range (pad, demux->offset, FLV_HEADER_SIZE, &buffer);
  if (G_UNLIKELY (ret != GST_FLOW_OK)) {
    GST_WARNING_OBJECT (demux, "failed when pulling %d bytes", FLV_HEADER_SIZE);
    goto beach;
  }

  if (G_UNLIKELY (buffer && GST_BUFFER_SIZE (buffer) != FLV_HEADER_SIZE)) {
    GST_WARNING_OBJECT (demux, "partial pull got %d when expecting %d",
        GST_BUFFER_SIZE (buffer), FLV_HEADER_SIZE);
    gst_buffer_unref (buffer);
    ret = GST_FLOW_UNEXPECTED;
    goto beach;
  }

  ret = gst_flv_parse_header (demux, GST_BUFFER_DATA (buffer),
      GST_BUFFER_SIZE (buffer));

  /* Jump over the header now */
  demux->offset += FLV_HEADER_SIZE;
  demux->state = FLV_STATE_TAG_TYPE;

beach:
  return ret;
}

static GstFlowReturn
gst_flv_demux_seek_to_prev_keyframe (GstFLVDemux * demux)
{
  return GST_FLOW_OK;
}

static void
gst_flv_demux_loop (GstPad * pad)
{
  GstFLVDemux *demux = NULL;
  GstFlowReturn ret = GST_FLOW_OK;

  demux = GST_FLV_DEMUX (gst_pad_get_parent (pad));

  if (demux->segment->rate >= 0) {
    /* pull in data */
    switch (demux->state) {
      case FLV_STATE_TAG_TYPE:
        ret = gst_flv_demux_pull_tag (pad, demux);
        break;
      case FLV_STATE_DONE:
        ret = GST_FLOW_UNEXPECTED;
        break;
      default:
        ret = gst_flv_demux_pull_header (pad, demux);
    }

    /* pause if something went wrong */
    if (G_UNLIKELY (ret != GST_FLOW_OK))
      goto pause;

    /* check EOS condition */
    if ((demux->segment->flags & GST_SEEK_FLAG_SEGMENT) &&
        (demux->segment->stop != -1) &&
        (demux->segment->last_stop >= demux->segment->stop)) {
      ret = GST_FLOW_UNEXPECTED;
      goto pause;
    }
  } else {                      /* Reverse playback */
    /* pull in data */
    switch (demux->state) {
      case FLV_STATE_TAG_TYPE:
        ret = gst_flv_demux_pull_tag (pad, demux);
        /* When packet parsing returns UNEXPECTED that means we ve reached the
           point where we want to go to the previous keyframe. This is either
           the last FLV tag or the keyframe we used last time */
        if (ret == GST_FLOW_UNEXPECTED) {
          ret = gst_flv_demux_seek_to_prev_keyframe (demux);
          demux->state = FLV_STATE_TAG_TYPE;
        }
        break;
      default:
        ret = gst_flv_demux_pull_header (pad, demux);
    }

    /* pause if something went wrong */
    if (G_UNLIKELY (ret != GST_FLOW_OK))
      goto pause;

    /* check EOS condition */
    if (demux->segment->last_stop <= demux->segment->start) {
      ret = GST_FLOW_UNEXPECTED;
      goto pause;
    }
  }

  gst_object_unref (demux);

  return;

pause:
  {
    const gchar *reason = gst_flow_get_name (ret);

    GST_LOG_OBJECT (demux, "pausing task, reason %s", reason);
    gst_pad_pause_task (pad);

    if (GST_FLOW_IS_FATAL (ret) || ret == GST_FLOW_NOT_LINKED) {
      if (ret == GST_FLOW_UNEXPECTED) {
        /* perform EOS logic */
        gst_element_no_more_pads (GST_ELEMENT_CAST (demux));
        if (demux->segment->flags & GST_SEEK_FLAG_SEGMENT) {
          gint64 stop;

          /* for segment playback we need to post when (in stream time)
           * we stopped, this is either stop (when set) or the duration. */
          if ((stop = demux->segment->stop) == -1)
            stop = demux->segment->duration;

          if (demux->segment->rate >= 0) {
            GST_LOG_OBJECT (demux, "Sending segment done, at end of segment");
            gst_element_post_message (GST_ELEMENT_CAST (demux),
                gst_message_new_segment_done (GST_OBJECT_CAST (demux),
                    GST_FORMAT_TIME, stop));
          } else {              /* Reverse playback */
            GST_LOG_OBJECT (demux, "Sending segment done, at beginning of "
                "segment");
            gst_element_post_message (GST_ELEMENT_CAST (demux),
                gst_message_new_segment_done (GST_OBJECT_CAST (demux),
                    GST_FORMAT_TIME, demux->segment->start));
          }
        } else {
          /* normal playback, send EOS to all linked pads */
          gst_element_no_more_pads (GST_ELEMENT (demux));
          GST_LOG_OBJECT (demux, "Sending EOS, at end of stream");
          if (!gst_pad_event_default (demux->sinkpad, gst_event_new_eos ())) {
            GST_WARNING_OBJECT (demux, "failed pushing EOS on streams");
            GST_ELEMENT_ERROR (demux, STREAM, FAILED,
                ("Internal data stream error."),
                ("Can't push EOS downstream (empty/invalid file "
                    "with no streams/tags ?)"));
          }
        }
      } else {
        GST_ELEMENT_ERROR (demux, STREAM, FAILED,
            ("Internal data stream error."),
            ("stream stopped, reason %s", reason));
        gst_pad_event_default (demux->sinkpad, gst_event_new_eos ());
      }
    }
    gst_object_unref (demux);
    return;
  }
}

/* If we can pull that's prefered */
static gboolean
gst_flv_demux_sink_activate (GstPad * sinkpad)
{
  if (gst_pad_check_pull_range (sinkpad)) {
    return gst_pad_activate_pull (sinkpad, TRUE);
  } else {
    return gst_pad_activate_push (sinkpad, TRUE);
  }
}

/* This function gets called when we activate ourselves in push mode.
 * We cannot seek (ourselves) in the stream */
static gboolean
gst_flv_demux_sink_activate_push (GstPad * sinkpad, gboolean active)
{
  GstFLVDemux *demux;

  demux = GST_FLV_DEMUX (gst_pad_get_parent (sinkpad));

  demux->random_access = FALSE;

  gst_object_unref (demux);

  return TRUE;
}

/* this function gets called when we activate ourselves in pull mode.
 * We can perform  random access to the resource and we start a task
 * to start reading */
static gboolean
gst_flv_demux_sink_activate_pull (GstPad * sinkpad, gboolean active)
{
  GstFLVDemux *demux;

  demux = GST_FLV_DEMUX (gst_pad_get_parent (sinkpad));

  if (active) {
    demux->random_access = TRUE;
    gst_object_unref (demux);
    return gst_pad_start_task (sinkpad, (GstTaskFunction) gst_flv_demux_loop,
        sinkpad);
  } else {
    demux->random_access = FALSE;
    gst_object_unref (demux);
    return gst_pad_stop_task (sinkpad);
  }
}

static gboolean
gst_flv_demux_sink_event (GstPad * pad, GstEvent * event)
{
  GstFLVDemux *demux;
  gboolean ret = FALSE;

  demux = GST_FLV_DEMUX (gst_pad_get_parent (pad));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_STOP:
      GST_DEBUG_OBJECT (demux, "flushing FLV demuxer");
      gst_flv_demux_flush (demux, TRUE);
      gst_adapter_clear (demux->adapter);
      gst_segment_init (demux->segment, GST_FORMAT_TIME);
      ret = gst_pad_event_default (demux->sinkpad, event);
      break;
    case GST_EVENT_EOS:
      GST_DEBUG_OBJECT (demux, "received EOS");
      gst_element_no_more_pads (GST_ELEMENT (demux));
      if (!gst_pad_event_default (demux->sinkpad, event)) {
        GST_WARNING_OBJECT (demux, "failed pushing EOS on streams");
        GST_ELEMENT_ERROR (demux, STREAM, FAILED,
            ("Internal data stream error."),
            ("Can't push EOS downstream (empty/invalid file "
                "with no streams/tags ?)"));
      }
      ret = TRUE;
      break;
    case GST_EVENT_NEWSEGMENT:
    {
      GstFormat format;
      gdouble rate;
      gint64 start, stop, time;
      gboolean update;

      GST_DEBUG_OBJECT (demux, "received new segment");

      gst_event_parse_new_segment (event, &update, &rate, &format, &start,
          &stop, &time);

      if (format == GST_FORMAT_TIME) {
        /* time segment, this is perfect, copy over the values. */
        gst_segment_set_newsegment (demux->segment, update, rate, format, start,
            stop, time);

#ifdef POST_10_10
        GST_DEBUG_OBJECT (demux, "NEWSEGMENT: %" GST_SEGMENT_FORMAT,
            demux->segment);
#endif

        /* and forward */
        ret = gst_pad_event_default (demux->sinkpad, event);
      } else {
        /* non-time format */
        demux->audio_need_segment = TRUE;
        demux->video_need_segment = TRUE;
        ret = TRUE;
        gst_event_unref (event);
      }
      break;
    }
    default:
      ret = gst_pad_event_default (demux->sinkpad, event);
      break;
  }

  gst_object_unref (demux);

  return ret;
}

static GstStateChangeReturn
gst_flv_demux_change_state (GstElement * element, GstStateChange transition)
{
  GstFLVDemux *demux;
  GstStateChangeReturn ret;

  demux = GST_FLV_DEMUX (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_flv_demux_cleanup (demux);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_flv_demux_cleanup (demux);
      break;
    default:
      break;
  }

  return ret;
}

static void
gst_flv_demux_dispose (GObject * object)
{
  GstFLVDemux *demux = GST_FLV_DEMUX (object);

  GST_DEBUG_OBJECT (demux, "disposing FLV demuxer");

  if (demux->adapter) {
    gst_adapter_clear (demux->adapter);
    g_object_unref (demux->adapter);
    demux->adapter = NULL;
  }

  if (demux->segment) {
    gst_segment_free (demux->segment);
    demux->segment = NULL;
  }

  if (demux->taglist) {
    gst_tag_list_free (demux->taglist);
    demux->taglist = NULL;
  }

  if (demux->new_seg_event) {
    gst_event_unref (demux->new_seg_event);
    demux->new_seg_event = NULL;
  }

  if (demux->audio_pad) {
    gst_object_unref (demux->audio_pad);
    demux->audio_pad = NULL;
  }

  if (demux->video_pad) {
    gst_object_unref (demux->video_pad);
    demux->video_pad = NULL;
  }

  GST_CALL_PARENT (G_OBJECT_CLASS, dispose, (object));
}

static void
gst_flv_demux_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&flv_sink_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&audio_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&video_src_template));
  gst_element_class_set_details (element_class, &flv_demux_details);
}

static void
gst_flv_demux_class_init (GstFLVDemuxClass * klass)
{
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_flv_demux_dispose);

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_flv_demux_change_state);
}

static void
gst_flv_demux_init (GstFLVDemux * demux, GstFLVDemuxClass * g_class)
{
  demux->sinkpad =
      gst_pad_new_from_static_template (&flv_sink_template, "sink");

  gst_pad_set_event_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_flv_demux_sink_event));
  gst_pad_set_chain_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_flv_demux_chain));
  gst_pad_set_activate_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_flv_demux_sink_activate));
  gst_pad_set_activatepull_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_flv_demux_sink_activate_pull));
  gst_pad_set_activatepush_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_flv_demux_sink_activate_push));

  gst_element_add_pad (GST_ELEMENT (demux), demux->sinkpad);

  demux->adapter = gst_adapter_new ();
  demux->segment = gst_segment_new ();
  demux->taglist = gst_tag_list_new ();
  gst_segment_init (demux->segment, GST_FORMAT_TIME);

  gst_flv_demux_cleanup (demux);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (flvdemux_debug, "flvdemux", 0, "FLV demuxer");

  if (!gst_element_register (plugin, "flvdemux", GST_RANK_PRIMARY,
          gst_flv_demux_get_type ()))
    return FALSE;

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR,
    "flvdemux", "Element demuxing FLV stream",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)