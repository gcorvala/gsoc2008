#ifndef __EMPATHY_TP_AUDIO_STREAM_H__
#define __EMPATHY_TP_AUDIO_STREAM_H__

#include <glib-object.h>
#include <telepathy-glib/enums.h>
#include <telepathy-glib/media-interfaces.h>

#include <telepathy-farsight/stream.h>

G_BEGIN_DECLS

#define EMPATHY_TP_TYPE_AUDIO_STREAM empathy_tp_audio_stream_get_type()

#define EMPATHY_TP_AUDIO_STREAM(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  EMPATHY_TP_TYPE_AUDIO_STREAM, EmpathyTpAudioStream))

#define EMPATHY_TP_AUDIO_STREAM_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
  EMPATHY_TP_TYPE_AUDIO_STREAM, EmpathyTpAudioStreamClass))

#define EMPATHY_TP_IS_AUDIO_STREAM(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
  EMPATHY_TP_TYPE_AUDIO_STREAM))

#define EMPATHY_TP_IS_AUDIO_STREAM_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
  EMPATHY_TP_TYPE_AUDIO_STREAM))

#define EMPATHY_TP_AUDIO_STREAM_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
  EMPATHY_TP_TYPE_AUDIO_STREAM, EmpathyTpAudioStreamClass))

typedef struct _EmpathyTpAudioStreamPrivate
          EmpathyTpAudioStreamPrivate;

typedef struct {
  GObject parent;

  EmpathyTpAudioStreamPrivate *priv;
} EmpathyTpAudioStream;

typedef struct {
  GObjectClass parent_class;
} EmpathyTpAudioStreamClass;

GType empathy_tp_audio_stream_get_type (void);

EmpathyTpAudioStream *
empathy_tp_audio_stream_new (TfStream *stream,
    GstBin *bin,
    GstPad *pad,
    GError **error);

G_END_DECLS

#endif /* __EMPATHY_TP_AUDIO_STREAM_H__ */
