#ifndef __EMPATHY_TP_VIDEO_STREAM_H__
#define __EMPATHY_TP_VIDEO_STREAM_H__

#include <glib-object.h>

#include <telepathy-farsight/stream.h>

#include "empathy-tp-video-sink.h"

G_BEGIN_DECLS

#define EMPATHY_TP_TYPE_VIDEO_STREAM empathy_tp_video_stream_get_type()

#define EMPATHY_TP_VIDEO_STREAM(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  EMPATHY_TP_TYPE_VIDEO_STREAM, EmpathyTpVideoStream))

#define EMPATHY_TP_VIDEO_STREAM_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
  EMPATHY_TP_TYPE_VIDEO_STREAM, EmpathyTpVideoStreamClass))

#define EMPATHY_TP_IS_VIDEO_STREAM(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
  EMPATHY_TP_TYPE_VIDEO_STREAM))

#define EMPATHY_TP_IS_VIDEO_STREAM_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
  EMPATHY_TP_TYPE_VIDEO_STREAM))

#define EMPATHY_TP_VIDEO_STREAM_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
  EMPATHY_TP_TYPE_VIDEO_STREAM, EmpathyTpVideoStreamClass))

typedef struct _EmpathyTpVideoStreamPrivate
          EmpathyTpVideoStreamPrivate;

typedef struct {
  EmpathyTpVideoSink parent;

  EmpathyTpVideoStreamPrivate *priv;
} EmpathyTpVideoStream;

typedef struct {
  EmpathyTpVideoSinkClass parent_class;
} EmpathyTpVideoStreamClass;

GType empathy_tp_video_stream_get_type (void);


EmpathyTpVideoStream *
empathy_tp_video_stream_new (
  TfStream *stream,
  GstBin *bin,
  GstPad *pad,
  GError **error);

G_END_DECLS

#endif /* __EMPATHY_TP_VIDEO_STREAM_H__ */
