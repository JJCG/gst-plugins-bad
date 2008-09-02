/*
 * Copyright 2007 Ole André Vadla Ravnås <ole.andre.ravnas@tandberg.com>
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
 * SECTION:element-pcapparse
 *
 * Extracts payloads from Ethernet-encapsulated IP packets, currently limited
 * to UDP. Use #GstPcapParse:src-ip, #GstPcapParse:dst-ip,
 * #GstPcapParse:src-port and #GstPcapParse:dst-port to restrict which packets
 * should be included.
 *
 * <refsect2>
 * <title>Example pipelines</title>
 * |[
 * gst-launch-0.10 filesrc location=h264crasher.pcap ! pcapparse ! rtph264depay
 * ! ffdec_h264 ! fakesink
 * ]| Read from a pcap dump file using filesrc, extract the raw UDP packets,
 * depayload and decode them.
 * </refsect2>
 */

/* TODO:
 * - React on state-change and update state accordingly.
 * - Implement support for timestamping the buffers.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "gstpcapparse.h"

#ifndef _MSC_VER
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#else
#include <winsock2.h>
#endif

enum
{
  PROP_0,
  PROP_SRC_IP,
  PROP_DST_IP,
  PROP_SRC_PORT,
  PROP_DST_PORT,
};

GST_DEBUG_CATEGORY_STATIC (gst_pcap_parse_debug);
#define GST_CAT_DEFAULT gst_pcap_parse_debug

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("raw/x-pcap"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static void gst_pcap_parse_dispose (GObject * object);
static void gst_pcap_parse_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_pcap_parse_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);

static void gst_pcap_parse_reset (GstPcapParse * self);

static GstFlowReturn gst_pcap_parse_chain (GstPad * pad, GstBuffer * buffer);

GST_BOILERPLATE (GstPcapParse, gst_pcap_parse, GstElement, GST_TYPE_ELEMENT);

static void
gst_pcap_parse_base_init (gpointer gclass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (gclass);
  static GstElementDetails element_details = {
    "PCapParse",
    "Raw/Parser",
    "Parses a raw pcap stream",
    "Ole André Vadla Ravnås <ole.andre.ravnas@tandberg.com>"
  };

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_template));

  gst_element_class_set_details (element_class, &element_details);
}

static void
gst_pcap_parse_class_init (GstPcapParseClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = gst_pcap_parse_dispose;
  gobject_class->get_property = gst_pcap_parse_get_property;
  gobject_class->set_property = gst_pcap_parse_set_property;

  g_object_class_install_property (gobject_class,
      PROP_SRC_IP, g_param_spec_string ("src-ip", "Source IP",
          "Source IP to restrict to", "", G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
      PROP_DST_IP, g_param_spec_string ("dst-ip", "Destination IP",
          "Destination IP to restrict to", "", G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
      PROP_SRC_PORT, g_param_spec_int ("src-port", "Source port",
          "Source port to restrict to", -1, G_MAXUINT16, -1,
          G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
      PROP_DST_PORT, g_param_spec_int ("dst-port", "Destination port",
          "Destination port to restrict to", -1, G_MAXUINT16, -1,
          G_PARAM_READWRITE));

  GST_DEBUG_CATEGORY_INIT (gst_pcap_parse_debug, "pcapparse", 0, "pcap parser");
}

static void
gst_pcap_parse_init (GstPcapParse * self, GstPcapParseClass * gclass)
{
  self->sink_pad = gst_pad_new_from_static_template (&sink_template, "sink");
  gst_pad_set_chain_function (self->sink_pad,
      GST_DEBUG_FUNCPTR (gst_pcap_parse_chain));
  gst_pad_use_fixed_caps (self->sink_pad);
  gst_element_add_pad (GST_ELEMENT (self), self->sink_pad);

  self->src_pad = gst_pad_new_from_static_template (&src_template, "src");
  gst_pad_use_fixed_caps (self->src_pad);
  gst_element_add_pad (GST_ELEMENT (self), self->src_pad);

  self->src_ip = -1;
  self->dst_ip = -1;
  self->src_port = -1;
  self->dst_port = -1;

  self->adapter = gst_adapter_new ();

  gst_pcap_parse_reset (self);
}

static void
gst_pcap_parse_dispose (GObject * object)
{
  GstPcapParse *self = GST_PCAP_PARSE (object);

  g_object_unref (self->adapter);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static const gchar *
get_ip_address_as_string (gint64 ip_addr)
{
  if (ip_addr >= 0) {
    struct in_addr addr;
    addr.s_addr = ip_addr;
    return inet_ntoa (addr);
  } else {
    return "";
  }
}

static void
set_ip_address_from_string (gint64 * ip_addr, const gchar * ip_str)
{
  if (ip_str[0] != '\0') {
    gulong addr = inet_addr (ip_str);
    if (addr != INADDR_NONE)
      *ip_addr = addr;
  } else {
    *ip_addr = -1;
  }
}

static void
gst_pcap_parse_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPcapParse *self = GST_PCAP_PARSE (object);

  switch (prop_id) {
    case PROP_SRC_IP:
      g_value_set_string (value, get_ip_address_as_string (self->src_ip));
      break;

    case PROP_DST_IP:
      g_value_set_string (value, get_ip_address_as_string (self->dst_ip));
      break;

    case PROP_SRC_PORT:
      g_value_set_int (value, self->src_port);
      break;

    case PROP_DST_PORT:
      g_value_set_int (value, self->dst_port);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pcap_parse_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPcapParse *self = GST_PCAP_PARSE (object);

  switch (prop_id) {
    case PROP_SRC_IP:
      set_ip_address_from_string (&self->src_ip, g_value_get_string (value));
      break;

    case PROP_DST_IP:
      set_ip_address_from_string (&self->dst_ip, g_value_get_string (value));
      break;

    case PROP_SRC_PORT:
      self->src_port = g_value_get_int (value);
      break;

    case PROP_DST_PORT:
      self->dst_port = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pcap_parse_reset (GstPcapParse * self)
{
  self->initialized = FALSE;
  self->swap_endian = FALSE;
  self->cur_packet_size = -1;
  self->buffer_offset = 0;

  gst_adapter_clear (self->adapter);
}

static guint32
gst_pcap_parse_read_uint32 (GstPcapParse * self, const guint8 * p)
{
  guint32 val = *((guint32 *) p);

  if (self->swap_endian) {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    return GUINT32_FROM_BE (val);
#else
    return GUINT32_FROM_LE (val);
#endif
  } else {
    return val;
  }
}

#define ETH_HEADER_LEN    14
#define IP_HEADER_MIN_LEN 20
#define UDP_HEADER_LEN     8

#define IP_PROTO_UDP      17

static gboolean
gst_pcap_parse_scan_frame (GstPcapParse * self,
    const guint8 * buf,
    gint buf_size, const guint8 ** payload, gint * payload_size)
{
  const guint8 *buf_ip;
  const guint8 *buf_udp;
  guint16 eth_type;
  guint8 b;
  guint8 ip_header_size;
  guint8 ip_protocol;
  guint32 ip_src_addr;
  guint32 ip_dst_addr;
  guint16 udp_src_port;
  guint16 udp_dst_port;
  guint16 udp_len;

  if (buf_size < ETH_HEADER_LEN + IP_HEADER_MIN_LEN + UDP_HEADER_LEN)
    return FALSE;

  eth_type = GUINT16_FROM_BE (*((guint16 *) (buf + 12)));
  if (eth_type != 0x800)
    return FALSE;

  buf_ip = buf + ETH_HEADER_LEN;

  b = *buf_ip;
  if (((b >> 4) & 0x0f) != 4)
    return FALSE;

  ip_header_size = (b & 0x0f) * 4;
  if (buf_ip + ip_header_size > buf + buf_size)
    return FALSE;

  ip_protocol = *(buf_ip + 9);
  if (ip_protocol != IP_PROTO_UDP)
    return FALSE;

  ip_src_addr = *((guint32 *) (buf_ip + 12));
  if (self->src_ip >= 0 && ip_src_addr != self->src_ip)
    return FALSE;

  ip_dst_addr = *((guint32 *) (buf_ip + 16));
  if (self->dst_ip >= 0 && ip_dst_addr != self->dst_ip)
    return FALSE;

  buf_udp = buf_ip + ip_header_size;

  udp_src_port = GUINT16_FROM_BE (*((guint16 *) (buf_udp + 0)));
  if (self->src_port >= 0 && udp_src_port != self->src_port)
    return FALSE;

  udp_dst_port = GUINT16_FROM_BE (*((guint16 *) (buf_udp + 2)));
  if (self->dst_port >= 0 && udp_dst_port != self->dst_port)
    return FALSE;

  udp_len = GUINT16_FROM_BE (*((guint16 *) (buf_udp + 4)));
  if (udp_len < UDP_HEADER_LEN || buf_udp + udp_len > buf + buf_size)
    return FALSE;

  *payload = buf_udp + UDP_HEADER_LEN;
  *payload_size = udp_len - UDP_HEADER_LEN;

  return TRUE;
}

static GstFlowReturn
gst_pcap_parse_chain (GstPad * pad, GstBuffer * buffer)
{
  GstPcapParse *self = GST_PCAP_PARSE (GST_PAD_PARENT (pad));
  GstFlowReturn ret = GST_FLOW_OK;

  if (GST_PAD_CAPS (self->src_pad) == NULL) {
    GstCaps *caps;

    caps = gst_pad_peer_get_caps (self->src_pad);
    if (caps == NULL)
      return GST_FLOW_NOT_NEGOTIATED;

    if (!gst_caps_is_fixed (caps) || !gst_pad_set_caps (self->src_pad, caps)) {
      gst_caps_unref (caps);
      return GST_FLOW_NOT_NEGOTIATED;
    }

    gst_caps_unref (caps);
  }

  gst_adapter_push (self->adapter, buffer);

  while (ret == GST_FLOW_OK) {
    gint avail;
    const guint8 *data;

    avail = gst_adapter_available (self->adapter);

    if (self->initialized) {
      if (self->cur_packet_size >= 0) {
        if (avail < self->cur_packet_size)
          break;

        if (self->cur_packet_size > 0) {
          const guint8 *payload_data;
          gint payload_size;

          data = gst_adapter_peek (self->adapter, self->cur_packet_size);

          if (gst_pcap_parse_scan_frame (self, data, self->cur_packet_size,
                  &payload_data, &payload_size)) {
            GstBuffer *out_buf;

            ret = gst_pad_alloc_buffer_and_set_caps (self->src_pad,
                self->buffer_offset, payload_size,
                GST_PAD_CAPS (self->src_pad), &out_buf);

            if (ret == GST_FLOW_OK) {
              memcpy (GST_BUFFER_DATA (out_buf), payload_data, payload_size);

              ret = gst_pad_push (self->src_pad, out_buf);

              self->buffer_offset += payload_size;
            }
          }

          gst_adapter_flush (self->adapter, self->cur_packet_size);
        }

        self->cur_packet_size = -1;
      } else {
        guint32 ts_sec;
        guint32 ts_usec;
        guint32 incl_len;
        guint32 orig_len;

        if (avail < 16)
          break;

        data = gst_adapter_peek (self->adapter, 16);

        ts_sec = gst_pcap_parse_read_uint32 (self, data + 0);
        ts_usec = gst_pcap_parse_read_uint32 (self, data + 4);
        incl_len = gst_pcap_parse_read_uint32 (self, data + 8);
        orig_len = gst_pcap_parse_read_uint32 (self, data + 12);

        gst_adapter_flush (self->adapter, 16);

        self->cur_packet_size = incl_len;
      }
    } else {
      guint magic;

      if (avail < 24)
        break;

      data = gst_adapter_peek (self->adapter, 24);

      magic = *((guint32 *) data);

      gst_adapter_flush (self->adapter, 24);

      if (magic == 0xa1b2c3d4)
        self->swap_endian = FALSE;
      else if (magic == 0xd4c3b2a1)
        self->swap_endian = TRUE;
      else
        ret = GST_FLOW_ERROR;

      if (ret == GST_FLOW_OK)
        self->initialized = TRUE;
    }
  }

  if (ret != GST_FLOW_OK)
    gst_pcap_parse_reset (self);

  return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "pcapparse",
      GST_RANK_NONE, GST_TYPE_PCAP_PARSE);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "pcapparse",
    "Element parsing raw pcap streams",
    plugin_init, VERSION, "LGPL", "GStreamer", "http://gstreamer.net/")