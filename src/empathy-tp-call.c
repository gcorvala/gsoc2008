/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2007 Elliot Fairweather
 * Copyright (C) 2007-2008 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied w//arranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Authors: Elliot Fairweather <elliot.fairweather@collabora.co.uk>
 *          Xavier Claessens <xclaesse@gmail.com>
 */

#include <string.h>

#include <telepathy-glib/proxy-subclass.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/interfaces.h>

#include <extensions/extensions.h>

#include <telepathy-farsight/channel.h>

#include <libempathy/empathy-contact-factory.h>
#include <libempathy/empathy-tp-group.h>
#include <libempathy/empathy-utils.h>

#include <gst/farsight/fs-element-added-notifier.h>

#include "empathy-tp-call.h"
#include "empathy-tp-audio-stream.h"
#include "empathy-tp-video-stream.h"
#include "empathy-tp-video-preview.h"
#include "empathy-tp-util.h"

#define DEBUG_FLAG EMPATHY_DEBUG_CALL
#include <libempathy/empathy-debug.h>

#define STREAM_ENGINE_BUS_NAME "org.freedesktop.Telepathy.StreamEngine"
#define STREAM_ENGINE_OBJECT_PATH "/org/freedesktop/Telepathy/StreamEngine"

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyTpCall)
typedef struct
{
  TfChannel *channel;
  TpProxy *stream_engine;
  TpDBusDaemon *dbus_daemon;
  EmpathyTpGroup *group;
  EmpathyContact *contact;
  gboolean is_incoming;
  guint status;
  gboolean stream_engine_running;

  GstElement *pipeline;
  GstElement *audiotee;
  
  GMutex *mutex;

  GstElement *audiosink;
  GstElement *audioadder;
  GstElement *audiosrc;

  gint audio_sink_use_count;
  gint audio_source_use_count;
  gint video_source_use_count;

  GstElement *videotee;
  GstElement *videosrc;

  gboolean force_fakesrc;

  GList *output_sinks;
  GList *preview_sinks;

  FsElementAddedNotifier *notifier;
  
  guint bus_async_source_id;

  EmpathyTpCallStream *audio;
  EmpathyTpCallStream *video;
} EmpathyTpCallPriv;

enum
{
  PROP_0,
  PROP_CHANNEL,
  PROP_CONTACT,
  PROP_IS_INCOMING,
  PROP_STATUS,
  PROP_AUDIO_STREAM,
  PROP_VIDEO_STREAM
};

G_DEFINE_TYPE (EmpathyTpCall, empathy_tp_call, G_TYPE_OBJECT)

static void
tp_call_add_stream (EmpathyTpCall *call,
                    guint stream_id,
                    guint contact_handle,
                    guint stream_type,
                    guint stream_state,
                    guint stream_direction)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  switch (stream_type)
    {
      case TP_MEDIA_STREAM_TYPE_AUDIO:
        DEBUG ("Audio stream - id: %d, state: %d, direction: %d",
            stream_id, stream_state, stream_direction);
        priv->audio->exists = TRUE;
        priv->audio->id = stream_id;
        priv->audio->state = stream_state;
        priv->audio->direction = stream_direction;
        g_object_notify (G_OBJECT (call), "audio-stream");
        break;
      case TP_MEDIA_STREAM_TYPE_VIDEO:
        DEBUG ("Video stream - id: %d, state: %d, direction: %d",
            stream_id, stream_state, stream_direction);
        priv->video->exists = TRUE;
        priv->video->id = stream_id;
        priv->video->state = stream_state;
        priv->video->direction = stream_direction;
        g_object_notify (G_OBJECT (call), "video-stream");
        break;
      default:
        DEBUG ("Unknown stream type: %d", stream_type);
    }
}

static void
tp_call_stream_added_cb (TpChannel *channel,
                         guint stream_id,
                         guint contact_handle,
                         guint stream_type,
                         gpointer user_data,
                         GObject *call)
{
  DEBUG ("Stream added - stream id: %d, contact handle: %d, stream type: %d",
      stream_id, contact_handle, stream_type);

  tp_call_add_stream (EMPATHY_TP_CALL (call), stream_id, contact_handle,
      stream_type, TP_MEDIA_STREAM_STATE_DISCONNECTED,
      TP_MEDIA_STREAM_DIRECTION_NONE);
}

static void
tp_call_stream_removed_cb (TpChannel *channel,
                           guint stream_id,
                           gpointer user_data,
                           GObject *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  DEBUG ("Stream removed - stream id: %d", stream_id);

  if (stream_id == priv->audio->id)
    {
      priv->audio->exists = FALSE;
      g_object_notify (call, "audio-stream");
    }
  else if (stream_id == priv->video->id)
    {
      priv->video->exists = FALSE;
      g_object_notify (call, "video-stream");
    }
}

static void
tp_call_stream_state_changed_cb (TpChannel *proxy,
                                 guint stream_id,
                                 guint stream_state,
                                 gpointer user_data,
                                 GObject *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  DEBUG ("Stream state changed - stream id: %d, state state: %d",
      stream_id, stream_state);

  if (stream_id == priv->audio->id)
    {
      priv->audio->state = stream_state;
      g_object_notify (call, "audio-stream");
    }
  else if (stream_id == priv->video->id)
    {
      priv->video->state = stream_state;
      g_object_notify (call, "video-stream");
    }
}

static void
tp_call_stream_direction_changed_cb (TpChannel *channel,
                                     guint stream_id,
                                     guint stream_direction,
                                     guint pending_flags,
                                     gpointer user_data,
                                     GObject *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  DEBUG ("Stream direction changed - stream: %d, direction: %d",
      stream_id, stream_direction);

  if (stream_id == priv->audio->id)
    {
      priv->audio->direction = stream_direction;
      g_object_notify (call, "audio-stream");
    }
  else if (stream_id == priv->video->id)
    {
      priv->video->direction = stream_direction;
      g_object_notify (call, "video-stream");
    }
}

static void
tp_call_request_streams_cb (TpChannel *channel,
                            const GPtrArray *streams,
                            const GError *error,
                            gpointer user_data,
                            GObject *call)
{
  guint i;

  if (error)
    {
      DEBUG ("Error requesting streams: %s", error->message);
      return;
    }

  for (i = 0; i < streams->len; i++)
    {
      GValueArray *values;
      guint stream_id;
      guint contact_handle;
      guint stream_type;
      guint stream_state;
      guint stream_direction;

      values = g_ptr_array_index (streams, i);
      stream_id = g_value_get_uint (g_value_array_get_nth (values, 0));
      contact_handle = g_value_get_uint (g_value_array_get_nth (values, 1));
      stream_type = g_value_get_uint (g_value_array_get_nth (values, 2));
      stream_state = g_value_get_uint (g_value_array_get_nth (values, 3));
      stream_direction = g_value_get_uint (g_value_array_get_nth (values, 4));

      tp_call_add_stream (EMPATHY_TP_CALL (call), stream_id, contact_handle,
          stream_type, stream_state, stream_direction);
  }
}

static void
tp_call_request_streams_for_capabilities (EmpathyTpCall *call,
                                          EmpathyCapabilities capabilities)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  TpChannel *chan;
  GArray *stream_types;
  guint handle;
  guint stream_type;

  g_object_get (priv->channel,
      "channel", &chan,
      NULL);

  if (capabilities == EMPATHY_CAPABILITIES_UNKNOWN)
      capabilities = EMPATHY_CAPABILITIES_AUDIO | EMPATHY_CAPABILITIES_VIDEO;

  DEBUG ("Requesting new stream for capabilities %d",
      capabilities);

  stream_types = g_array_new (FALSE, FALSE, sizeof (guint));
  handle = empathy_contact_get_handle (priv->contact);

  if (capabilities & EMPATHY_CAPABILITIES_AUDIO)
    {
      stream_type = TP_MEDIA_STREAM_TYPE_AUDIO;
      g_array_append_val (stream_types, stream_type);
    }
  if (capabilities & EMPATHY_CAPABILITIES_VIDEO)
    {
      stream_type = TP_MEDIA_STREAM_TYPE_VIDEO;
      g_array_append_val (stream_types, stream_type);
    }

  tp_cli_channel_type_streamed_media_call_request_streams (chan, -1,
      handle, stream_types, tp_call_request_streams_cb, NULL, NULL,
      G_OBJECT (call));

  g_array_free (stream_types, TRUE);
}

static void
tp_call_member_added_cb (EmpathyTpGroup *group,
                         EmpathyContact *contact,
                         EmpathyContact *actor,
                         guint reason,
                         const gchar *message,
                         EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  g_object_ref (call);
  if (!priv->contact && !empathy_contact_is_user (contact))
    {
      priv->contact = g_object_ref (contact);
      priv->is_incoming = TRUE;
      priv->status = EMPATHY_TP_CALL_STATUS_PENDING;
      g_object_notify (G_OBJECT (call), "is-incoming");
      g_object_notify (G_OBJECT (call), "contact");
      g_object_notify (G_OBJECT (call), "status");
      tp_call_request_streams_for_capabilities (call,
          EMPATHY_CAPABILITIES_AUDIO);

    }

  if (priv->status == EMPATHY_TP_CALL_STATUS_PENDING &&
      ((priv->is_incoming && contact != priv->contact) ||
       (!priv->is_incoming && contact == priv->contact)))
    {
      priv->status = EMPATHY_TP_CALL_STATUS_ACCEPTED;
      g_object_notify (G_OBJECT (call), "status");
    }
  g_object_unref (call);
}

static void
tp_call_remote_pending_cb (EmpathyTpGroup *group,
                           EmpathyContact *contact,
                           EmpathyContact *actor,
                           guint reason,
                           const gchar *message,
                           EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  g_object_ref (call);
  if (!priv->contact && !empathy_contact_is_user (contact))
    {
      priv->contact = g_object_ref (contact);
      priv->is_incoming = FALSE;
      priv->status = EMPATHY_TP_CALL_STATUS_PENDING;
      g_object_notify (G_OBJECT (call), "is-incoming");
      g_object_notify (G_OBJECT (call), "contact"); 
      g_object_notify (G_OBJECT (call), "status");	
      tp_call_request_streams_for_capabilities (call,
          EMPATHY_CAPABILITIES_AUDIO);
    }
  g_object_unref (call);
}

static void
tp_call_channel_invalidated_cb (TpChannel     *channel,
                                GQuark         domain,
                                gint           code,
                                gchar         *message,
                                EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  DEBUG ("Channel invalidated: %s", message);
  priv->status = EMPATHY_TP_CALL_STATUS_CLOSED;
  g_object_notify (G_OBJECT (call), "status");
}

static void
tp_call_async_cb (TpProxy *proxy,
                  const GError *error,
                  gpointer user_data,
                  GObject *call)
{
  if (error)
      DEBUG ("Error %s: %s", (gchar*) user_data, error->message);
}

static void
tp_call_close_channel (EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  TpChannel *chan;

  if (priv->status == EMPATHY_TP_CALL_STATUS_CLOSED)
      return;

  g_object_get (priv->channel,
      "channel", &chan,
      NULL);

  DEBUG ("Closing channel");

  tp_cli_channel_call_close (chan, -1,
      NULL, NULL, NULL, NULL);

  priv->status = EMPATHY_TP_CALL_STATUS_CLOSED;
  g_object_notify (G_OBJECT (call), "status");
}

static void
tp_call_stream_engine_invalidated_cb (TpProxy       *stream_engine,
				      GQuark         domain,
				      gint           code,
				      gchar         *message,
				      EmpathyTpCall *call)
{
  DEBUG ("Stream engine proxy invalidated: %s", message);
  tp_call_close_channel (call);
}

static void
tp_call_stream_engine_watch_name_owner_cb (TpDBusDaemon *daemon,
					   const gchar *name,
					   const gchar *new_owner,
					   gpointer call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  /* G_STR_EMPTY(new_owner) means either stream-engine has not started yet or
   * has crashed. We want to close the channel if stream-engine has crashed.
   * */
  DEBUG ("Watch SE: name='%s' SE running='%s' new_owner='%s'",
      name, priv->stream_engine_running ? "yes" : "no",
      new_owner ? new_owner : "none");
  if (priv->stream_engine_running && G_STR_EMPTY (new_owner))
    {
      DEBUG ("Stream engine falled off the bus");
      tp_call_close_channel (call);
      return;
    }

  priv->stream_engine_running = !G_STR_EMPTY (new_owner);
}

static void
tp_call_handler_result (TfChannel *chan G_GNUC_UNUSED,
                GError *error,
                EmpathyTpCall *call)
{
  g_debug ("HandlerResult called");
  if (error)
    {
      DEBUG ("Error handler_result domain: %s", g_quark_to_string(error->domain));
      DEBUG ("Error handler_result code: %d", error->code);
      DEBUG ("Error handler_result message: %s", error->message);
    }
}

static void
tp_call_channel_closed (TfChannel *chan, EmpathyTpCall *call)
{
  gchar *object_path;

  g_object_get (chan, "object-path", &object_path, NULL);

  g_debug ("channel %s (%p) closed", object_path, chan);

  g_object_unref (chan);

  g_free (object_path);
}

static void
tp_call_session_invalidated (TfChannel *chan G_GNUC_UNUSED,
    FsConference  *conference,
    FsParticipant *participant,
    EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  
  gst_element_set_locked_state ((GstElement *) conference, TRUE);
  gst_element_set_state ((GstElement *) conference, GST_STATE_NULL);
  gst_element_get_state ((GstElement *) conference, NULL, NULL, GST_CLOCK_TIME_NONE);

  gst_bin_remove (GST_BIN (priv->pipeline), (GstElement *) conference);
}

static void
tp_call_channel_session_created (TfChannel *chan G_GNUC_UNUSED,
    FsConference  *conference,
    FsParticipant *participant,
    EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  
  g_debug ("SessionCreated called");
  
  g_object_set (conference, "latency", 100, NULL);

  if (!gst_bin_add (GST_BIN (priv->pipeline), (GstElement *) conference))
    g_error ("Could not add conference to pipeline");

  gst_element_set_state ((GstElement *) conference, GST_STATE_PLAYING);

  g_signal_connect (priv->channel, "session-invalidated", G_CALLBACK (tp_call_session_invalidated),
      call);
}

static GstElement *
_make_audio_sink (void)
{
  const gchar *elem;
  GstElement *bin = NULL;
  GstElement *sink = NULL;
  GstElement *audioconvert = NULL;
  GstElement *audioresample = NULL;
  GstPad *pad;

  if ((elem = getenv ("FS_AUDIO_SINK")) || (elem = getenv("FS_AUDIOSINK")))
    {
      g_debug ("making audio sink with pipeline \"%s\"", elem);
      sink = gst_parse_bin_from_description (elem, TRUE, NULL);
      g_assert (sink);
    }
  else
    {
#ifdef MAEMO_OSSO_SUPPORT
      sink = gst_element_factory_make ("dsppcmsink", NULL);
#else
      sink = gst_element_factory_make ("gconfaudiosink", NULL);

      if (sink == NULL)
        sink = gst_element_factory_make ("autoaudiosink", NULL);

      if (sink == NULL)
        sink = gst_element_factory_make ("alsasink", NULL);
#endif
    }

  if (sink == NULL)
    {
      g_warning ("failed to make audio sink element!");
      return NULL;
    }

  g_debug ("made audio sink element %s", GST_ELEMENT_NAME (sink));

  bin = gst_bin_new ("audiosinkbin");

  if (!gst_bin_add (GST_BIN (bin), sink))
    {
      gst_object_unref (bin);
      gst_object_unref (sink);
      g_warning ("Could not add sink to bin");
      return NULL;
    }

  audioresample = gst_element_factory_make ("audioresample", NULL);
  if (!gst_bin_add (GST_BIN (bin), audioresample))
    {
      gst_object_unref (audioresample);
      gst_object_unref (bin);
      g_warning ("Could not add audioresample to the bin");
      return NULL;
    }

  audioconvert = gst_element_factory_make ("audioconvert", NULL);
  if (!gst_bin_add (GST_BIN (bin), audioconvert))
    {
      gst_object_unref (audioconvert);
      gst_object_unref (bin);
      g_warning ("Could not add audioconvert to the bin");
      return NULL;
    }

  if (!gst_element_link_many (audioresample, audioconvert, sink, NULL))
    {
      gst_object_unref (bin);
      g_warning ("Could not link sink elements");
      return NULL;
    }

  pad = gst_bin_find_unconnected_pad (GST_BIN (bin), GST_PAD_SINK);

  if (!pad)
    {
      gst_object_unref (bin);
      g_warning ("Could not find unconnected sink pad in src bin");
      return NULL;
    }

  if (!gst_element_add_pad (bin, gst_ghost_pad_new ("sink", pad)))
    {
      gst_object_unref (bin);
      g_warning ("Could not add pad to bin");
      return NULL;
    }

  gst_object_unref (pad);


  return bin;
}

static gboolean
tp_call_add_audio_sink_locked (EmpathyTpCall *call)
{
  GstElement *sink = NULL;
  GstElement *liveadder = NULL;
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  sink = _make_audio_sink ();

  if (!sink)
    {
      g_warning ("Could not make audio sink");
      goto error;
    }

  if (!gst_bin_add (GST_BIN (priv->pipeline), sink))
    {
      g_warning ("Could not add audio sink to pipeline");
      gst_object_unref (sink);
      sink = NULL;
      goto error;
    }

  liveadder = gst_element_factory_make ("liveadder", NULL);
  if (!liveadder)
    {
      g_warning ("Could not create liveadder");
      goto error;
    }

  if (!gst_bin_add (GST_BIN (priv->pipeline), liveadder))
    {
      gst_object_unref (liveadder);
      liveadder = NULL;
      g_warning ("Could not add liveadder to the bin");
      goto error;
    }

  if (!gst_element_link_pads (liveadder, "src", sink, "sink"))
    {
      g_warning ("Could not linnk the liveadder to the sink");
      goto error;
    }

  if (gst_element_set_state (sink, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE ||
      gst_element_set_state (liveadder, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE)
    {
      g_warning ("Could not start sink or liveadder");
      goto error;
    }

  priv->audiosink = sink;
  priv->audioadder = liveadder;

  return TRUE;

 error:
  if (sink)
    {
      gst_element_set_locked_state (sink, TRUE);
      gst_element_set_state (sink, GST_STATE_NULL);
      gst_bin_remove (GST_BIN (priv->pipeline), sink);
    }
  if (liveadder)
    {
      gst_element_set_locked_state (liveadder, TRUE);
      gst_element_set_state (liveadder, GST_STATE_NULL);
      gst_bin_remove (GST_BIN (priv->pipeline), liveadder);
    }

  return FALSE;
}

static GstPad *
tp_call_audio_stream_request_pad (EmpathyTpAudioStream *audiostream,
    EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  GstPad *pad = NULL;

  g_mutex_lock (priv->mutex);

  if (priv->audiosink == NULL)
    if (!tp_call_add_audio_sink_locked (call))
      goto out;

  g_assert (priv->audioadder);

  pad = gst_element_get_request_pad (priv->audioadder, "sink%d");

  priv->audio_sink_use_count++;

 out:
  g_mutex_unlock (priv->mutex);

  return pad;
}

static void
tp_call_remove_audio_sink_locked (EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  
  gst_element_set_locked_state (priv->audiosink, TRUE);
  gst_element_set_locked_state (priv->audioadder, TRUE);
  gst_element_set_state (priv->audiosink, GST_STATE_NULL);
  gst_element_set_state (priv->audioadder, GST_STATE_NULL);

  gst_bin_remove (GST_BIN (priv->pipeline), priv->audioadder);
  gst_bin_remove (GST_BIN (priv->pipeline), priv->audiosink);

  priv->audiosink = NULL;
  priv->audioadder = NULL;
}

static void
tp_call_audio_stream_release_pad (EmpathyTpAudioStream *audiostream,
    GstPad *pad,
    EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  g_mutex_lock (priv->mutex);

  gst_element_release_request_pad (priv->audioadder, pad);

  priv->audio_sink_use_count--;

  if (priv->audio_sink_use_count <= 0)
    {
      priv->audio_sink_use_count = 0;

      tp_call_remove_audio_sink_locked (call);
    }

  g_mutex_unlock (priv->mutex);
}

static void
tp_call_stream_closed (TfStream *stream, EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  GObject *sestream = NULL;

  sestream = g_object_get_data (G_OBJECT (stream), "se-stream");
  if (sestream)
    {
      if (EMPATHY_TP_IS_VIDEO_STREAM (sestream))
        {
          EmpathyTpVideoStream *videostream =
              (EmpathyTpVideoStream *) sestream;

          g_mutex_lock (priv->mutex);
          priv->output_sinks = g_list_remove (priv->output_sinks,
              videostream);
          g_mutex_unlock (priv->mutex);

        }

      if (EMPATHY_TP_IS_VIDEO_STREAM (sestream) ||
          EMPATHY_TP_IS_AUDIO_STREAM (sestream))
        {
          GstPad *pad, *peer;

          g_object_get (sestream, "pad", &pad, NULL);


          /* Take the stream lock to make sure nothing is flowing through the
           * pad
           * We can only do that because we have no blocking elements before
           * a queue in our pipeline after the pads.
           */
          peer = gst_pad_get_peer (pad);
          GST_PAD_STREAM_LOCK(pad);
          if (peer)
            {
              gst_pad_unlink (pad, peer);
              gst_object_unref (peer);
            }
          /*
           * Releasing request pads currently fail cause stuff like
           * alloc_buffer() does take any lock and there is no way to prevent it
           * ??? or something like that

          if (TP_STREAM_ENGINE_IS_VIDEO_STREAM (sestream))
            gst_element_release_request_pad (self->priv->videotee, pad);
          else if (TP_STREAM_ENGINE_IS_AUDIO_STREAM (sestream))
            gst_element_release_request_pad (self->priv->audiotee, pad);
          */
          GST_PAD_STREAM_UNLOCK(pad);

          gst_object_unref (pad);
        }
      g_object_unref (sestream);
    }
}

static void
tp_call_start_video_source (EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  GstStateChangeReturn state_ret;


  if (priv->force_fakesrc)
    {
      g_debug ("Don't have a video source, not starting it");
      return;
    }

  g_debug ("Starting video source");

  priv->video_source_use_count++;

  state_ret = gst_element_set_state (priv->videosrc, GST_STATE_PLAYING);

  if (state_ret == GST_STATE_CHANGE_FAILURE)
    g_error ("Error starting the video source");
}

static void
tp_call_start_audio_source (EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  GstStateChangeReturn state_ret;

  g_debug ("Starting audio source");

  priv->audio_source_use_count++;

  state_ret = gst_element_set_state (priv->audiosrc, GST_STATE_PLAYING);

  if (state_ret == GST_STATE_CHANGE_FAILURE)
    g_error ("Error starting the audio source");
}

static gboolean
tp_call_stream_request_resource (TfStream *stream,
    TpMediaStreamDirection dir,
    EmpathyTpCall *call)
{
  TpMediaStreamType media_type;

  if (!(dir & TP_MEDIA_STREAM_DIRECTION_SEND))
    return TRUE;

  g_object_get (stream, "media-type", &media_type, NULL);

  if (media_type == TP_MEDIA_STREAM_TYPE_VIDEO)
    tp_call_start_video_source (call);
  else if (media_type == TP_MEDIA_STREAM_TYPE_AUDIO)
    tp_call_start_audio_source (call);

  return TRUE;
}

static void
tp_call_stop_video_source (EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  GstStateChangeReturn state_ret;

  priv->video_source_use_count--;

  if (priv->video_source_use_count > 0)
    return;

  g_debug ("Stopping video source");

  state_ret = gst_element_set_state (priv->videosrc, GST_STATE_NULL);

  if (state_ret == GST_STATE_CHANGE_FAILURE)
    g_error ("Error stopping the video source");
  else if (state_ret == GST_STATE_CHANGE_ASYNC)
    g_debug ("Stopping video src async??");
}

static void
tp_call_stop_audio_source (EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  GstStateChangeReturn state_ret;

  priv->audio_source_use_count--;

  if (priv->audio_source_use_count > 0)
    return;

  g_debug ("Stopping audio source");

  state_ret = gst_element_set_state (priv->audiosrc, GST_STATE_NULL);

  if (state_ret == GST_STATE_CHANGE_FAILURE)
    g_error ("Error stopping the audio source");
  else if (state_ret == GST_STATE_CHANGE_ASYNC)
    g_debug ("Stopping audio src async??");
}

static void
tp_call_stream_free_resource (TfStream *stream,
    TpMediaStreamDirection dir,
    EmpathyTpCall *call)
{
  TpMediaStreamType media_type;

  if (!(dir & TP_MEDIA_STREAM_DIRECTION_SEND))
    return;

  g_object_get (stream,
      "media-type", &media_type,
      NULL);

  if (media_type == TP_MEDIA_STREAM_TYPE_VIDEO)
    tp_call_stop_video_source (call);
  else if (media_type == TP_MEDIA_STREAM_TYPE_AUDIO)
    tp_call_stop_audio_source (call);
}

static void
tp_call_channel_stream_created (TfChannel *chan G_GNUC_UNUSED,
    TfStream *stream, EmpathyTpCall *call)
{
  guint media_type;
  GError *error = NULL;
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  g_debug ("StreamCreated called");

  g_object_get (G_OBJECT (stream), "media-type", &media_type, NULL);

  if (media_type == TP_MEDIA_STREAM_TYPE_AUDIO)
    {
      EmpathyTpAudioStream *audiostream;
      GstPad *pad;

      pad = gst_element_get_request_pad (priv->audiotee, "src%d");

      audiostream = empathy_tp_audio_stream_new (stream,
          GST_BIN (priv->pipeline), pad, &error);

      if (!audiostream)
        {
          g_warning ("Could not create audio stream: %s", error->message);
          gst_element_release_request_pad (priv->audiotee, pad);
          return;
        }
      g_clear_error (&error);
      g_object_set_data ((GObject*) stream, "se-stream", audiostream);

      g_signal_connect (audiostream,
          "request-pad",
          G_CALLBACK (tp_call_audio_stream_request_pad),
          call);
      g_signal_connect (audiostream,
          "release-pad",
          G_CALLBACK (tp_call_audio_stream_release_pad),
          call);

    }
  else if (media_type == TP_MEDIA_STREAM_TYPE_VIDEO)
    {
      EmpathyTpVideoStream *videostream = NULL;
      GstPad *pad;

      pad = gst_element_get_request_pad (priv->videotee, "src%d");

      videostream = empathy_tp_video_stream_new (stream,
          GST_BIN (priv->pipeline), pad, &error);

      if (!videostream)
        {
          g_warning ("Could not create video stream: %s", error->message);
          gst_element_release_request_pad (priv->videotee, pad);
          return;
        }
      g_clear_error (&error);
      g_object_set_data ((GObject*) stream, "se-stream", videostream);

      g_mutex_lock (priv->mutex);
      priv->output_sinks = g_list_prepend (priv->output_sinks,
          videostream);
      g_mutex_unlock (priv->mutex);
   }

  g_signal_connect (stream,
      "closed",
      G_CALLBACK (tp_call_stream_closed),
      call);

  g_signal_connect (stream,
      "request-resource",
      G_CALLBACK (tp_call_stream_request_resource),
      call);

  g_signal_connect (stream,
      "free-resource",
      G_CALLBACK (tp_call_stream_free_resource),
      call);
}

static GList *
tp_call_stream_get_codec_config (TfChannel *chan,
    guint stream_id,
    TpMediaStreamType media_type,
    TpMediaStreamDirection direction,
    EmpathyTpCall *call)
{
  GList *codec_config = NULL;
  gchar *filename;
  gint i;
  GError *error = NULL;
  const gchar * const *system_config_dirs = g_get_system_config_dirs ();

  filename = g_build_filename (g_get_user_config_dir (), "stream-engine",
      "gstcodecs.conf", NULL);

  codec_config = fs_codec_list_from_keyfile (filename, &error);

  if (!codec_config)
    {
      g_debug ("Could not read local codecs config at %s: %s", filename,
          error ? error->message : "");
      g_free (filename);
    }
  g_clear_error (&error);

  for (i = 0; system_config_dirs[i] && codec_config == NULL; i++)
    {
      filename = g_build_filename (system_config_dirs[i],
          "stream-engine",
          "gstcodecs.conf", NULL);

      codec_config = fs_codec_list_from_keyfile (filename, &error);

      if (!codec_config)
        {
          g_debug ("Could not read global codecs config at %s: %s", filename,
              error ? error->message : "");
          g_free (filename);
        }
      g_clear_error (&error);
    }

  if (codec_config)
    {
      g_debug ("Loaded codec config from %s", filename);
      g_free (filename);
    }

  return codec_config;
}

static void
tp_call_stream_engine_handle_channel (EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  TpChannel *chan;
  gchar *channel_type;
  gchar *object_path;
  guint handle_type;
  guint handle;
  TpProxy *connection;
  GError *error = NULL;

  g_object_get (priv->channel,
      "channel", &chan,
      NULL);

  g_object_get (chan,
      "connection", &connection,
      "channel-type", &channel_type,
      "object-path", &object_path,
      "handle_type", &handle_type,
      "handle", &handle,
      NULL);

  //-------------------------------------------------------------------
  g_debug("HandleChannel called");

  if (strcmp (channel_type, TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA) != 0)
    {
      GError e = { TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
        "TpCall was passed a channel that was not a "
        TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA };

      g_message ("%s", e.message);
      return;
     }

  if (priv->channel == NULL)
    {
      g_error_free (error);
      g_debug ("TfChannel == NULL");
      return;
    }

  g_signal_connect (priv->channel,
      "handler-result",
      G_CALLBACK (tp_call_handler_result),
      call);
  g_signal_connect (priv->channel,
      "closed",
      G_CALLBACK (tp_call_channel_closed),
      call);
  g_signal_connect (priv->channel,
      "session-created",
      G_CALLBACK (tp_call_channel_session_created),
      call);
  g_signal_connect (priv->channel,
      "stream-created",
      G_CALLBACK (tp_call_channel_stream_created),
      call);
  g_signal_connect (priv->channel,
      "stream-get-codec-config",
      G_CALLBACK (tp_call_stream_get_codec_config),
      call);
  //-------------------------------------------------------------------

  g_object_unref (connection);
  g_free (channel_type);
  g_free (object_path);
}

static GObject *
tp_call_constructor (GType type,
                     guint n_construct_params,
                     GObjectConstructParam *construct_params)
{
  GObject *object;
  EmpathyTpCall *call;
  EmpathyTpCallPriv *priv;
  TpChannel *chan;

  object = G_OBJECT_CLASS (empathy_tp_call_parent_class)->constructor (type,
      n_construct_params, construct_params);

  call = EMPATHY_TP_CALL (object);
  priv = GET_PRIV (call);

  g_object_get (priv->channel,
      "channel", &chan,
      NULL);

  /* Setup streamed media channel */
  g_signal_connect (chan,
      "invalidated",
      G_CALLBACK (tp_call_channel_invalidated_cb),
      call);
  tp_cli_channel_type_streamed_media_connect_to_stream_added (chan,
      tp_call_stream_added_cb, NULL, NULL, G_OBJECT (call), NULL);
  tp_cli_channel_type_streamed_media_connect_to_stream_removed (chan,
      tp_call_stream_removed_cb, NULL, NULL, G_OBJECT (call), NULL);
  tp_cli_channel_type_streamed_media_connect_to_stream_state_changed (chan,
      tp_call_stream_state_changed_cb, NULL, NULL, G_OBJECT (call), NULL);
  tp_cli_channel_type_streamed_media_connect_to_stream_direction_changed (chan,
      tp_call_stream_direction_changed_cb, NULL, NULL, G_OBJECT (call), NULL);
  tp_cli_channel_type_streamed_media_call_list_streams (chan, -1,
      tp_call_request_streams_cb, NULL, NULL, G_OBJECT (call));

  /* Setup group interface */
  priv->group = empathy_tp_group_new (chan);

  g_signal_connect (priv->group, "member-added",
      G_CALLBACK (tp_call_member_added_cb), call);
  g_signal_connect (priv->group, "remote-pending",
      G_CALLBACK (tp_call_remote_pending_cb), call);

  /* Start stream engine */
  tp_call_stream_engine_handle_channel (call);

  return object;
}

static void 
tp_call_finalize (GObject *object)
{
  EmpathyTpCallPriv *priv = GET_PRIV (object);
  TpChannel *chan;

  DEBUG ("Finalizing: %p", object);

  g_slice_free (EmpathyTpCallStream, priv->audio);
  g_slice_free (EmpathyTpCallStream, priv->video);
  g_object_unref (priv->group);

  g_object_get (priv->channel,
      "channel", &chan,
      NULL);

  if (chan != NULL)
    {
      g_signal_handlers_disconnect_by_func (chan,
          tp_call_channel_invalidated_cb, object);
      tp_call_close_channel (EMPATHY_TP_CALL (object));
      g_object_unref (chan);
    }

  if (priv->stream_engine != NULL)
    {
      g_signal_handlers_disconnect_by_func (priv->stream_engine,
          tp_call_stream_engine_invalidated_cb, object);
      g_object_unref (priv->stream_engine);
    }

  if (priv->contact != NULL)
      g_object_unref (priv->contact);

  if (priv->dbus_daemon != NULL)
    {
      tp_dbus_daemon_cancel_name_owner_watch (priv->dbus_daemon,
          STREAM_ENGINE_BUS_NAME,
          tp_call_stream_engine_watch_name_owner_cb,
          object);
      g_object_unref (priv->dbus_daemon);
    }

  (G_OBJECT_CLASS (empathy_tp_call_parent_class)->finalize) (object);
}

static void 
tp_call_set_property (GObject *object,
                      guint prop_id,
                      const GValue *value,
                      GParamSpec *pspec)
{
  EmpathyTpCallPriv *priv = GET_PRIV (object);

  switch (prop_id)
    {
    case PROP_CHANNEL:
      priv->channel = g_value_dup_object (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
tp_call_get_property (GObject *object,
                      guint prop_id,
                      GValue *value,
                      GParamSpec *pspec)
{
  EmpathyTpCallPriv *priv = GET_PRIV (object);

  switch (prop_id)
    {
    case PROP_CHANNEL:
      g_value_set_object (value, priv->channel);
      break;
    case PROP_CONTACT:
      g_value_set_object (value, priv->contact);
      break;
    case PROP_IS_INCOMING:
      g_value_set_boolean (value, priv->is_incoming);
      break;
    case PROP_STATUS:
      g_value_set_uint (value, priv->status);
      break;
    case PROP_AUDIO_STREAM:
      g_value_set_pointer (value, priv->audio);
      break;
    case PROP_VIDEO_STREAM:
      g_value_set_pointer (value, priv->video);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
empathy_tp_call_class_init (EmpathyTpCallClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  emp_cli_init ();

  object_class->constructor = tp_call_constructor;
  object_class->finalize = tp_call_finalize;
  object_class->set_property = tp_call_set_property;
  object_class->get_property = tp_call_get_property;

  g_type_class_add_private (klass, sizeof (EmpathyTpCallPriv));

  g_object_class_install_property (object_class, PROP_CHANNEL,
      g_param_spec_object ("channel", "channel", "channel",
      TF_TYPE_CHANNEL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (object_class, PROP_CONTACT,
      g_param_spec_object ("contact", "Call contact", "Call contact",
      EMPATHY_TYPE_CONTACT,
      G_PARAM_READABLE | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (object_class, PROP_IS_INCOMING,
      g_param_spec_boolean ("is-incoming", "Is media stream incoming",
      "Is media stream incoming", FALSE, G_PARAM_READABLE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (object_class, PROP_STATUS,
      g_param_spec_uint ("status", "Call status",
      "Call status", 0, 255, 0, G_PARAM_READABLE | G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB));
  g_object_class_install_property (object_class, PROP_AUDIO_STREAM,
      g_param_spec_pointer ("audio-stream", "Audio stream data",
      "Audio stream data",
      G_PARAM_READABLE | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (object_class, PROP_VIDEO_STREAM,
      g_param_spec_pointer ("video-stream", "Video stream data",
      "Video stream data",
      G_PARAM_READABLE | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
}

static void
_build_base_video_elements (EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  GstElement *videosrc = NULL;
  GstElement *tee;
  GstElement *capsfilter;
#ifndef MAEMO_OSSO_SUPPORT
  GstElement *tmp;
#endif
  const gchar *elem;
  const gchar *caps_str;
  gboolean ret;
  GstElement *fakesink;
  GstCaps *filter = NULL;
  GstStateChangeReturn state_ret;

 try_again:

  if ((elem = getenv ("FS_VIDEO_SRC")) || (elem = getenv ("FS_VIDEOSRC")))
    {
      if (priv->force_fakesrc)
        g_error ("Invalid video source passed in FS_VIDEOSRC");
      g_debug ("making video src with pipeline \"%s\"", elem);
      videosrc = gst_parse_bin_from_description (elem, TRUE, NULL);
      g_assert (videosrc);
      gst_element_set_name (videosrc, "videosrc");
    }
  else
    {
#ifdef MAEMO_OSSO_SUPPORT
      videosrc = gst_element_factory_make ("gconfv4l2src", NULL);
      if (!videosrc)
        g_error ("Could make video source");
#else
      if (priv->force_fakesrc)
        {
          videosrc = gst_element_factory_make ("fakesrc", NULL);
          g_object_set (videosrc, "is-live", TRUE, NULL);
        }

      if (videosrc == NULL)
        videosrc = gst_element_factory_make ("gconfvideosrc", NULL);

      if (videosrc == NULL)
        videosrc = gst_element_factory_make ("v4l2src", NULL);

      if (videosrc == NULL)
        videosrc = gst_element_factory_make ("v4lsrc", NULL);

      if (videosrc != NULL)
        {
          g_debug ("using %s as video source", GST_ELEMENT_NAME (videosrc));
        }
      else
        {
          videosrc = gst_element_factory_make ("videotestsrc", NULL);
          if (videosrc == NULL)
            g_error ("failed to create any video source aaa");
          g_object_set (videosrc, "is-live", TRUE, NULL);
        }
#endif
    }

  if ((caps_str = getenv ("FS_VIDEO_SRC_CAPS")) || (caps_str = getenv ("FS_VIDEOSRC_CAPS")))
    {
      filter = gst_caps_from_string (caps_str);
    }

  if (!filter)
    {
      filter = gst_caps_new_simple ("video/x-raw-yuv",
#ifdef MAEMO_OSSO_SUPPORT
          "width", G_TYPE_INT, 176,
          "height", G_TYPE_INT, 144,
#else
          "width", G_TYPE_INT, 352,
          "height", G_TYPE_INT, 288,
          "format", GST_TYPE_FOURCC, GST_MAKE_FOURCC ('I', '4', '2', '0'),
#endif
          NULL);
    }
  else
    {
      g_debug ("applying custom caps '%s' on the video source\n", caps_str);
    }

  fakesink = gst_element_factory_make ("fakesink", NULL);

  gst_bin_add_many (GST_BIN (priv->pipeline), videosrc, fakesink, NULL);

  if (!gst_element_link (videosrc, fakesink))
    g_error ("Could not link fakesink to videosrc");

  state_ret = gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
  if (state_ret == GST_STATE_CHANGE_FAILURE)
    {
      if (priv->force_fakesrc)
        {
          g_error ("Could not even start fakesrc");
        }
      else
        {
          g_debug ("Video source failed, falling back to fakesrc");
          state_ret = gst_element_set_state (priv->pipeline, GST_STATE_NULL);
          g_assert (state_ret == GST_STATE_CHANGE_SUCCESS);
          if (!gst_bin_remove (GST_BIN (priv->pipeline), fakesink))
            g_error ("Could not remove fakesink");

          priv->force_fakesrc = TRUE;
          gst_bin_remove (GST_BIN (priv->pipeline), videosrc);
          goto try_again;
        }
    }
  else
    {
      state_ret = gst_element_set_state (priv->pipeline, GST_STATE_NULL);
      g_assert (state_ret == GST_STATE_CHANGE_SUCCESS);
      gst_bin_remove (GST_BIN (priv->pipeline), fakesink);
    }

  gst_element_set_locked_state (videosrc, TRUE);

  tee = gst_element_factory_make ("tee", "videotee");
  g_assert (tee);
  if (!gst_bin_add (GST_BIN (priv->pipeline), tee))
    g_error ("Could not add tee to pipeline");

  priv->videosrc = videosrc;
  priv->videotee = tee;

#ifndef MAEMO_OSSO_SUPPORT
#if 0
  /* <wtay> Tester_, yes, videorate does not play nice with live pipelines */
  tmp = gst_element_factory_make ("videorate", NULL);
  if (tmp != NULL)
    {
      g_debug ("linking videorate");
      gst_bin_add (GST_BIN (priv->pipeline), tmp);
      gst_element_link (videosrc, tmp);
      videosrc = tmp;
    }
#endif

  tmp = gst_element_factory_make ("ffmpegcolorspace", NULL);
  if (tmp != NULL)
    {
      g_debug ("linking ffmpegcolorspace");
      gst_bin_add (GST_BIN (priv->pipeline), tmp);
      gst_element_link (videosrc, tmp);
      videosrc = tmp;
    }

  tmp = gst_element_factory_make ("videoscale", NULL);
  if (tmp != NULL)
    {
      g_debug ("linking videoscale");
      gst_bin_add (GST_BIN (priv->pipeline), tmp);
      gst_element_link (videosrc, tmp);
      videosrc = tmp;
    }
#endif

  capsfilter = gst_element_factory_make ("capsfilter", NULL);
  g_assert (capsfilter);
  ret = gst_bin_add (GST_BIN (priv->pipeline), capsfilter);
  g_assert (ret);

  g_object_set (capsfilter, "caps", filter, NULL);
  gst_caps_unref (filter);

  ret = gst_element_link (videosrc, capsfilter);
  g_assert (ret);
  ret = gst_element_link (capsfilter, tee);
  g_assert (ret);
}

static GstElement *
_make_audio_src (void)
{
  const gchar *elem;
  GstElement *bin = NULL;
  GstElement *audioconvert = NULL;
  GstElement *src = NULL;
  GstPad *pad;

  bin = gst_bin_new ("audiosrcbin");

  if ((elem = getenv ("FS_AUDIO_SRC")) || (elem = getenv ("FS_AUDIOSRC")))
    {
      g_debug ("making audio src with pipeline \"%s\"", elem);
      src = gst_parse_bin_from_description (elem, TRUE, NULL);
      g_assert (src);
    }
  else
    {
#ifdef MAEMO_OSSO_SUPPORT
      src = gst_element_factory_make ("dsppcmsrc", NULL);
#else
      src = gst_element_factory_make ("gconfaudiosrc", NULL);

      if (src == NULL)
        src = gst_element_factory_make ("alsasrc", NULL);
#endif
    }

  if (src == NULL)
    {
      g_debug ("failed to make audio src element!");
      return NULL;
    }

  g_debug ("made audio src element %s", GST_ELEMENT_NAME (src));

  audioconvert = gst_element_factory_make ("audioconvert", NULL);


  if (!gst_bin_add (GST_BIN (bin), audioconvert) ||
      !gst_bin_add (GST_BIN (bin), src))
    {
      gst_object_unref (bin);
      gst_object_unref (src);
      gst_object_unref (audioconvert);

      g_warning ("Could not add audioconvert or src to the bin");

      return NULL;
    }

  if (!gst_element_link_pads (src, "src", audioconvert, "sink"))
    {
      gst_object_unref (bin);
      g_warning ("Could not link src and audioconvert elements");
      return NULL;
    }

  pad = gst_bin_find_unconnected_pad (GST_BIN (bin), GST_PAD_SRC);

  if (!pad)
    {
      gst_object_unref (bin);
      g_warning ("Could not find unconnected sink pad in src bin");
      return NULL;
    }

  if (!gst_element_add_pad (bin, gst_ghost_pad_new ("src", pad)))
    {
      gst_object_unref (bin);
      g_warning ("Could not add pad to bin");
      return NULL;
    }

  gst_object_unref (pad);

  return bin;

}

static void
_build_base_audio_elements (EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  GstElement *src = NULL;
  GstElement *tee = NULL;

  src = _make_audio_src ();

  if (!src)
    {
      g_warning ("Could not make audio src");
      return;
    }

  if (!gst_bin_add (GST_BIN (priv->pipeline), src))
    {
      g_warning ("Could not add audiosrc to pipeline");
      gst_object_unref (src);
      return;
    }

  gst_element_set_locked_state (src, TRUE);

  tee = gst_element_factory_make ("tee", "audiotee");

  if (!gst_bin_add (GST_BIN (priv->pipeline), tee))
    {
      g_warning ("Could not add audiosrc to pipeline");
      gst_object_unref (tee);
      return;
    }

  if (!gst_element_link_pads (src, "src", tee, "sink"))
    {
      g_warning ("Could not link audio src and tee");
      return;
    }


  priv->audiosrc = src;
  priv->audiotee = tee;
}

static GstBusSyncReply
tp_call_bus_sync_handler (GstBus *bus G_GNUC_UNUSED,
    GstMessage *message,
    gpointer data)
{
  EmpathyTpCallPriv *priv = GET_PRIV (data);
  GList *item;
  gboolean handled = FALSE;

  if (GST_MESSAGE_TYPE (message) != GST_MESSAGE_ELEMENT)
    return GST_BUS_PASS;

  if (!gst_structure_has_name (message->structure, "prepare-xwindow-id"))
    return GST_BUS_PASS;


  g_mutex_lock (priv->mutex);
  for (item = g_list_first (priv->preview_sinks);
       item && !handled;
       item = g_list_next (item))
    {
      EmpathyTpVideoSink *preview = item->data;

      handled = empathy_tp_video_sink_bus_sync_message (preview, message);
      if (handled)
        break;
    }

  if (!handled)
    {
      for (item = g_list_first (priv->output_sinks);
           item && !handled;
           item = g_list_next (item))
        {
          EmpathyTpVideoSink *output = item->data;

          handled = empathy_tp_video_sink_bus_sync_message (output,
              message);
          if (handled)
            break;
        }
    }
  g_mutex_unlock (priv->mutex);

  if (handled)
    {
      gst_message_unref (message);
      return GST_BUS_DROP;
    }
  else
    return GST_BUS_PASS;
}

static gboolean
tp_call_bus_async_handler (GstBus *bus G_GNUC_UNUSED,
                   GstMessage *message,
                   gpointer data)
{
  EmpathyTpCallPriv *priv = GET_PRIV (data);
  GError *error = NULL;
  gchar *error_string;
  GstElement *source = GST_ELEMENT (GST_MESSAGE_SRC (message));
  gchar *name = gst_element_get_name (source);


  //for (i = 0; i < priv->channels->len; i++)
    //if (tf_channel_bus_message (
            //g_ptr_array_index (priv->channels, i), message))
      //return TRUE;
  if (tf_channel_bus_message (priv->channel, message))
    return TRUE;

  switch (GST_MESSAGE_TYPE (message))
    {
      case GST_MESSAGE_ERROR:
        gst_message_parse_error (message, &error, &error_string);

        g_debug ("%s: got error from %s: %s: %s (%d %d), stopping pipeline",
            G_STRFUNC, name, error->message, error_string,
            error->domain, error->code);

        //close_all_streams (engine, error->message);

        gst_element_set_state (priv->pipeline, GST_STATE_NULL);
        gst_element_set_state (priv->videosrc, GST_STATE_NULL);
        gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);

        g_free (error_string);
        g_error_free (error);
        break;
      case GST_MESSAGE_WARNING:
        {
          gchar *debug_msg = NULL;
          gst_message_parse_warning (message, &error, &debug_msg);
          g_warning ("%s: got warning: %s (%s)", G_STRFUNC, error->message,
              debug_msg);
          g_free (debug_msg);
          g_error_free (error);
          break;
        }

      default:
        break;
    }

  g_free (name);
  return TRUE;
}

static void
_create_pipeline (EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  GstBus *bus;
  GstStateChangeReturn state_ret;

  priv->pipeline = gst_pipeline_new (NULL);

  fs_element_added_notifier_add (priv->notifier, GST_BIN (priv->pipeline));

  _build_base_video_elements (call);
  _build_base_audio_elements (call);


  /* connect a callback to the stream bus so that we can set X window IDs
   * at the right time, and detect when sinks have gone away */
  bus = gst_element_get_bus (priv->pipeline);
  gst_bus_set_sync_handler (bus, tp_call_bus_sync_handler, call);
  priv->bus_async_source_id =
    gst_bus_add_watch (bus, tp_call_bus_async_handler, call);
  gst_object_unref (bus);

  state_ret = gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
  g_assert (state_ret != GST_STATE_CHANGE_FAILURE);
}

static void
tp_call_set_element_props (FsElementAddedNotifier *notifier,
    GstBin *bin,
    GstElement *element,
    EmpathyTpCall *call)
{
  if (g_object_has_property ((GObject *) element, "min-ptime"))
    g_object_set ((GObject *) element, "min-ptime", GST_MSECOND * 20, NULL);

  if (g_object_has_property ((GObject *) element, "max-ptime"))
    g_object_set ((GObject *) element, "max-ptime", GST_MSECOND * 50, NULL);
}

static void
tp_call_add_gstelements_conf_to_notifier (FsElementAddedNotifier *notifier)
{
  GKeyFile *keyfile = NULL;
  gchar *filename;
  gboolean loaded = FALSE;
  GError *error = NULL;

  keyfile = g_key_file_new ();

  filename = g_build_filename (g_get_user_config_dir (), "stream-engine",
      "gstelements.conf", NULL);

  if (!g_key_file_load_from_file (keyfile, filename, G_KEY_FILE_NONE, &error))
    {
      g_debug ("Could not read element properties config at %s: %s", filename,
          error ? error->message : "");
    }
  else
    {
      loaded = TRUE;
    }
  g_free (filename);
  g_clear_error (&error);

  if (!loaded)
    {
      gchar *path = NULL;
      filename = g_build_filename ("stream-engine", "gstelements.conf", NULL);

      if (!g_key_file_load_from_dirs (keyfile, filename,
              (const char **) g_get_system_config_dirs (),
              &path, G_KEY_FILE_NONE, &error))
        {
          g_debug ("Could not read element properties config file %s from any"
              "of the XDG system config directories: %s", filename,
              error ? error->message : "");
        }
      else
        {
          g_debug ("Loaded element properties from %s", path);
          g_free (path);
          loaded = TRUE;
        }
      g_free (filename);
    }

  g_clear_error (&error);

  if (loaded)
    {
      fs_element_added_notifier_set_properties_from_keyfile (notifier,
          keyfile);
    }
  else
    {
      g_key_file_free (keyfile);
    }
}

static void
empathy_tp_call_init (EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = G_TYPE_INSTANCE_GET_PRIVATE (call,
		EMPATHY_TYPE_TP_CALL, EmpathyTpCallPriv);

  call->priv = priv;
  priv->status = EMPATHY_TP_CALL_STATUS_READYING;
  priv->contact = NULL;
  priv->stream_engine_running = FALSE;
  priv->audio = g_slice_new0 (EmpathyTpCallStream);
  priv->video = g_slice_new0 (EmpathyTpCallStream);
  priv->audio->exists = FALSE;
  priv->video->exists = FALSE;


  priv->mutex = g_mutex_new ();

  priv->notifier = fs_element_added_notifier_new ();
  g_signal_connect (priv->notifier, "element-added",
      G_CALLBACK (tp_call_set_element_props), call);
  tp_call_add_gstelements_conf_to_notifier (priv->notifier);
  
  _create_pipeline (call);
}

EmpathyTpCall *
empathy_tp_call_new (TpChannel *channel)
{
  TfChannel *chan;
  
  g_return_val_if_fail (TP_IS_CHANNEL (channel), NULL);
  
  chan = tf_channel_new_from_proxy (channel);

  return g_object_new (EMPATHY_TYPE_TP_CALL,
      "channel", chan,
      NULL);
}

void
empathy_tp_call_accept_incoming_call (EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  EmpathyContact *self_contact;

  g_return_if_fail (EMPATHY_IS_TP_CALL (call));
  g_return_if_fail (priv->status == EMPATHY_TP_CALL_STATUS_PENDING);

  DEBUG ("Accepting incoming call");

  self_contact = empathy_tp_group_get_self_contact (priv->group);
  empathy_tp_group_add_member (priv->group, self_contact, NULL);
  g_object_unref (self_contact);
}

void
empathy_tp_call_request_video_stream_direction (EmpathyTpCall *call,
                                                gboolean is_sending)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  TpChannel *chan;
  guint new_direction;

  g_return_if_fail (EMPATHY_IS_TP_CALL (call));
  g_return_if_fail (priv->status == EMPATHY_TP_CALL_STATUS_ACCEPTED);

  DEBUG ("Requesting video stream direction - is_sending: %d", is_sending);

  if (!priv->video->exists)
    {
      if (is_sending)
          tp_call_request_streams_for_capabilities (call,
              EMPATHY_CAPABILITIES_VIDEO);
      return;
    }

  if (is_sending)
      new_direction = priv->video->direction | TP_MEDIA_STREAM_DIRECTION_SEND;
  else
      new_direction = priv->video->direction & ~TP_MEDIA_STREAM_DIRECTION_SEND;

  g_object_get (priv->channel,
      "channel", &chan,
      NULL);

  tp_cli_channel_type_streamed_media_call_request_stream_direction (chan,
      -1, priv->video->id, new_direction,
      (tp_cli_channel_type_streamed_media_callback_for_request_stream_direction)
      tp_call_async_cb, NULL, NULL, G_OBJECT (call));
}

void
empathy_tp_call_add_preview_video (EmpathyTpCall *call,
                                   guint preview_video_socket_id)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  g_return_if_fail (EMPATHY_IS_TP_CALL (call));

  DEBUG ("Adding preview video");

  //-------------------------------------------------------------------
  GError *error = NULL;
  //GstPad *pad;
  EmpathyTpVideoPreview *preview;
  //guint window_id;

  if (priv->force_fakesrc)
    {
      GError *error = g_error_new (TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "Could not get a video source");
      g_debug ("%s", error->message);

      g_error_free (error);
      return;
    }

  preview = empathy_tp_video_preview_new (GST_BIN (priv->pipeline),
      &error);

  if (!preview)
    {
      g_warning ("Could not create video preview: %s", error->message);
      g_clear_error (&error);
      return;
    }

  g_debug ("We are Here");
  //g_mutex_lock (priv->mutex);
  //priv->preview_sinks = g_list_prepend (priv->preview_sinks,
      //preview);
  //g_mutex_unlock (priv->mutex);

  //pad = gst_element_get_request_pad (priv->videotee, "src%d");

  //g_object_set (preview, "pad", pad, NULL);

  ////g_signal_connect (preview, "plug-deleted",
      ////G_CALLBACK (_preview_window_plug_deleted), self);

  //g_object_get (preview, "window-id", &window_id, NULL);

  //tp_call_start_video_source (call);

  //g_signal_emit (self, signals[HANDLING_CHANNEL], 0);
  //-------------------------------------------------------------------
  //emp_cli_stream_engine_call_add_preview_window (priv->stream_engine, -1,
      //preview_video_socket_id,
      //tp_call_async_cb,
      //"adding preview window", NULL,
      //G_OBJECT (call));
}

void
empathy_tp_call_remove_preview_video (EmpathyTpCall *call,
                                      guint preview_video_socket_id)
{
  //EmpathyTpCallPriv *priv = GET_PRIV (call);

  g_return_if_fail (EMPATHY_IS_TP_CALL (call));

  DEBUG ("Removing preview video");

  //emp_cli_stream_engine_call_remove_preview_window (priv->stream_engine, -1,
      //preview_video_socket_id,
      //tp_call_async_cb,
      //"removing preview window", NULL,
      //G_OBJECT (call));
}

void
empathy_tp_call_add_output_video (EmpathyTpCall *call,
                                  guint output_video_socket_id)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  TpChannel *chan;

  g_return_if_fail (EMPATHY_IS_TP_CALL (call));

  DEBUG ("Adding output video - socket: %d", output_video_socket_id);

  g_object_get (priv->channel,
      "channel", &chan,
      NULL);
  //-------------------------------------------------------------------
  TfStream *stream;
  TpMediaStreamType media_type;
  GError *error = NULL;

  g_debug ("%s: channel_path=%s, stream_id=%u", G_STRFUNC, TP_PROXY (chan)->object_path,
      priv->video->id);

  stream = tf_channel_lookup_stream (priv->channel, priv->audio->id);

  if (stream == NULL)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Stream does not exist");
      g_error_free (error);
      return;
    }

  g_object_get (stream, "media-type", &media_type, NULL);

  if (media_type != TP_MEDIA_STREAM_TYPE_VIDEO)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "GetOutputWindow can only be called on video streams");
      g_error_free (error);
      return;
    }

  if (stream)
    {
      guint window_id;
      EmpathyTpVideoSink *videosink;

      videosink = EMPATHY_TP_VIDEO_SINK (
          g_object_get_data ((GObject*) stream, "se-stream"));

      g_object_get (videosink, "window-id", &window_id, NULL);

      g_debug ("Returning window id %u", window_id);
    }
  //-------------------------------------------------------------------
  //emp_cli_stream_engine_call_set_output_window (priv->stream_engine, -1,
      //TP_PROXY (chan)->object_path,
      //priv->video->id, output_video_socket_id,
      //tp_call_async_cb,
      //"setting output window", NULL,
      //G_OBJECT (call));
}

void
empathy_tp_call_set_output_volume (EmpathyTpCall *call,
                                   guint volume)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  g_return_if_fail (EMPATHY_IS_TP_CALL (call));
  g_return_if_fail (priv->status != EMPATHY_TP_CALL_STATUS_CLOSED);

  DEBUG ("Setting output volume: %d", volume);

  //-------------------------------------------------------------------
  TfStream *stream;
  GError *error = NULL;
  TpMediaStreamType media_type;
  EmpathyTpAudioStream *audiostream;
  gdouble doublevolume = volume / 100.0;

  stream = tf_channel_lookup_stream (priv->channel, priv->audio->id);
  
  if (stream == NULL)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Stream does not exist");
      g_error_free (error);
      return;
    }

  g_object_get (stream, "media-type", &media_type, NULL);

  if (media_type != TP_MEDIA_STREAM_TYPE_AUDIO)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "MuteOutput can only be called on audio streams");
      g_error_free (error);
      return;
    }

  audiostream = g_object_get_data ((GObject*) stream, "se-stream");

  g_object_set (audiostream, "output-volume", doublevolume, NULL);
  //-------------------------------------------------------------------
}

void
empathy_tp_call_mute_output (EmpathyTpCall *call,
                             gboolean is_muted)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  g_return_if_fail (EMPATHY_IS_TP_CALL (call));

  DEBUG ("Setting output mute: %d", is_muted);

  //-------------------------------------------------------------------
  TfStream *stream;
  GError *error = NULL;
  TpMediaStreamType media_type;
  EmpathyTpAudioStream *audiostream;

  stream = tf_channel_lookup_stream (priv->channel, priv->audio->id);

  if (stream == NULL)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Stream does not exist");
      g_error_free (error);
      return;
    }

  g_object_get (stream, "media-type", &media_type, NULL);

  if (media_type != TP_MEDIA_STREAM_TYPE_AUDIO)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "MuteOutput can only be called on audio streams");
      g_error_free (error);
      return;
    }

  audiostream = g_object_get_data ((GObject*) stream, "se-stream");

  g_object_set (audiostream, "output-mute", is_muted, NULL);
  //-------------------------------------------------------------------
}

void
empathy_tp_call_mute_input (EmpathyTpCall *call,
                            gboolean is_muted)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);

  g_return_if_fail (EMPATHY_IS_TP_CALL (call));

  DEBUG ("Setting input mute: %d", is_muted);

  //-------------------------------------------------------------------
  TfStream *stream;
  GError *error = NULL;
  TpMediaStreamType media_type;
  EmpathyTpAudioStream *audiostream;

  stream = tf_channel_lookup_stream (priv->channel, priv->audio->id);

  if (stream == NULL)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Stream does not exist");
      g_error_free (error);
      return;
    }

  g_object_get (stream, "media-type", &media_type, NULL);

  if (media_type != TP_MEDIA_STREAM_TYPE_AUDIO)
    {
      error = g_error_new (TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "MuteInput can only be called on audio streams");
      g_error_free (error);
      return;
    }

  audiostream = g_object_get_data ((GObject*) stream, "se-stream");

  g_object_set (audiostream, "input-mute", is_muted, NULL);

  //-------------------------------------------------------------------
}

void
empathy_tp_call_start_tone (EmpathyTpCall *call, TpDTMFEvent event)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  TpChannel *chan;

  g_return_if_fail (EMPATHY_IS_TP_CALL (call));
  g_return_if_fail (priv->status == EMPATHY_TP_CALL_STATUS_ACCEPTED);

  if (!priv->audio->exists)
      return;

  g_object_get (priv->channel,
      "channel", &chan,
      NULL);

  tp_cli_channel_interface_dtmf_call_start_tone (chan, -1,
      priv->audio->id, event,
      (tp_cli_channel_interface_dtmf_callback_for_start_tone) tp_call_async_cb,
      "starting tone", NULL, G_OBJECT (call));
}

void
empathy_tp_call_stop_tone (EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  TpChannel *chan;

  g_return_if_fail (EMPATHY_IS_TP_CALL (call));
  g_return_if_fail (priv->status == EMPATHY_TP_CALL_STATUS_ACCEPTED);

  if (!priv->audio->exists)
      return;

  g_object_get (priv->channel,
      "channel", &chan,
      NULL);

  tp_cli_channel_interface_dtmf_call_stop_tone (chan, -1,
      priv->audio->id,
      (tp_cli_channel_interface_dtmf_callback_for_stop_tone) tp_call_async_cb,
      "stoping tone", NULL, G_OBJECT (call));
}

gboolean
empathy_tp_call_has_dtmf (EmpathyTpCall *call)
{
  EmpathyTpCallPriv *priv = GET_PRIV (call);
  TpChannel *chan;

  g_return_val_if_fail (EMPATHY_IS_TP_CALL (call), FALSE);

  g_object_get (priv->channel,
      "channel", &chan,
      NULL);

  return tp_proxy_has_interface_by_id (chan,
      TP_IFACE_QUARK_CHANNEL_INTERFACE_DTMF);
}

