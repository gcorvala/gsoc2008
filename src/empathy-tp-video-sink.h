#ifndef __EMPATHY_TP_VIDEO_SINK_H__
#define __EMPATHY_TP_VIDEO_SINK_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define EMPATHY_TP_TYPE_VIDEO_SINK empathy_tp_video_sink_get_type()

#define EMPATHY_TP_VIDEO_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  EMPATHY_TP_TYPE_VIDEO_SINK, EmpathyTpVideoSink))

#define EMPATHY_TP_VIDEO_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
  EMPATHY_TP_TYPE_VIDEO_SINK, EmpathyTpVideoSinkClass))

#define EMPATHY_TP_IS_VIDEO_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
  EMPATHY_TP_TYPE_VIDEO_SINK))

#define EMPATHY_TP_IS_VIDEO_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
  EMPATHY_TP_TYPE_VIDEO_SINK))

#define EMPATHY_TP_VIDEO_SINK_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
  EMPATHY_TP_TYPE_VIDEO_SINK, EmpathyTpVideoSinkClass))

typedef struct _EmpathyTpVideoSinkPrivate
          EmpathyTpVideoSinkPrivate;

typedef struct {
  GObject parent;

  EmpathyTpVideoSinkPrivate *priv;
} EmpathyTpVideoSink;

typedef struct {
  GObjectClass parent_class;
} EmpathyTpVideoSinkClass;

GType empathy_tp_video_sink_get_type (void);

gboolean
empathy_tp_video_sink_bus_sync_message (
    EmpathyTpVideoSink *self,
    GstMessage *message);


G_END_DECLS

#endif /* __EMPATHY_TP_VIDEO_SINK_H__ */
