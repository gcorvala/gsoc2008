#ifndef __EMPATHY_TP_VIDEO_PREVIEW_H__
#define __EMPATHY_TP_VIDEO_PREVIEW_H__

#include <glib-object.h>

#include "empathy-tp-video-sink.h"

G_BEGIN_DECLS

#define EMPATHY_TP_TYPE_VIDEO_PREVIEW empathy_tp_video_preview_get_type()

#define EMPATHY_TP_VIDEO_PREVIEW(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  EMPATHY_TP_TYPE_VIDEO_PREVIEW, EmpathyTpVideoPreview))

#define EMPATHY_TP_VIDEO_PREVIEW_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
  EMPATHY_TP_TYPE_VIDEO_PREVIEW, EmpathyTpVideoPreviewClass))

#define EMPATHY_TP_IS_VIDEO_PREVIEW(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
  EMPATHY_TP_TYPE_VIDEO_PREVIEW))

#define EMPATHY_TP_IS_VIDEO_PREVIEW_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
  EMPATHY_TP_TYPE_VIDEO_PREVIEW))

#define EMPATHY_TP_VIDEO_PREVIEW_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
  EMPATHY_TP_TYPE_VIDEO_PREVIEW, EmpathyTpVideoPreviewClass))

typedef struct _EmpathyTpVideoPreviewPrivate
          EmpathyTpVideoPreviewPrivate;

typedef struct {
  EmpathyTpVideoSink parent;

  EmpathyTpVideoPreviewPrivate *priv;
} EmpathyTpVideoPreview;

typedef struct {
  EmpathyTpVideoSinkClass parent_class;
} EmpathyTpVideoPreviewClass;

GType empathy_tp_video_preview_get_type (void);

EmpathyTpVideoPreview *
empathy_tp_video_preview_new (GstBin *bin,
    GError **error);

G_END_DECLS

#endif /* __EMPATHY_TP_VIDEO_PREVIEW_H__ */
