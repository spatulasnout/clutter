/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Authored By Matthew Allum  <mallum@openedhand.com>
 *
 * Copyright (C) 2006 OpenedHand
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

#ifndef __CLUTTER_PRIVATE_H__
#define __CLUTTER_PRIVATE_H__

#include <glib.h>

#include <glib/gi18n-lib.h>

#include <cogl-pango/cogl-pango.h>

#include "clutter-backend.h"
#include "clutter-effect.h"
#include "clutter-event.h"
#include "clutter-feature.h"
#include "clutter-id-pool.h"
#include "clutter-layout-manager.h"
#include "clutter-master-clock.h"
#include "clutter-settings.h"
#include "clutter-stage.h"

G_BEGIN_DECLS

typedef struct _ClutterMainContext      ClutterMainContext;

#define CLUTTER_REGISTER_VALUE_TRANSFORM_TO(TYPE_TO,func)             { \
  g_value_register_transform_func (g_define_type_id, TYPE_TO, func);    \
}

#define CLUTTER_REGISTER_VALUE_TRANSFORM_FROM(TYPE_FROM,func)         { \
  g_value_register_transform_func (TYPE_FROM, g_define_type_id, func);  \
}

#define CLUTTER_REGISTER_INTERVAL_PROGRESS(func)                      { \
  clutter_interval_register_progress_func (g_define_type_id, func);     \
}

#define CLUTTER_PRIVATE_FLAGS(a)	 (((ClutterActor *) (a))->private_flags)
#define CLUTTER_SET_PRIVATE_FLAGS(a,f)	 (CLUTTER_PRIVATE_FLAGS (a) |= (f))
#define CLUTTER_UNSET_PRIVATE_FLAGS(a,f) (CLUTTER_PRIVATE_FLAGS (a) &= ~(f))

#define CLUTTER_ACTOR_IS_TOPLEVEL(a)            ((CLUTTER_PRIVATE_FLAGS (a) & CLUTTER_IS_TOPLEVEL) != FALSE)
#define CLUTTER_ACTOR_IS_INTERNAL_CHILD(a)      ((CLUTTER_PRIVATE_FLAGS (a) & CLUTTER_INTERNAL_CHILD) != FALSE)
#define CLUTTER_ACTOR_IN_DESTRUCTION(a)         ((CLUTTER_PRIVATE_FLAGS (a) & CLUTTER_IN_DESTRUCTION) != FALSE)
#define CLUTTER_ACTOR_IN_REPARENT(a)            ((CLUTTER_PRIVATE_FLAGS (a) & CLUTTER_IN_REPARENT) != FALSE)
#define CLUTTER_ACTOR_IN_PAINT(a)               ((CLUTTER_PRIVATE_FLAGS (a) & CLUTTER_IN_PAINT) != FALSE)
#define CLUTTER_ACTOR_IN_RELAYOUT(a)            ((CLUTTER_PRIVATE_FLAGS (a) & CLUTTER_IN_RELAYOUT) != FALSE)
#define CLUTTER_STAGE_IN_RESIZE(a)              ((CLUTTER_PRIVATE_FLAGS (a) & CLUTTER_IN_RESIZE) != FALSE)

#define CLUTTER_PARAM_READABLE  (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS)
#define CLUTTER_PARAM_WRITABLE  (G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS)
#define CLUTTER_PARAM_READWRITE (G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS)

/* automagic interning of a static string */
#define I_(str)  (g_intern_static_string ((str)))

/* mark all properties under the "Property" context */
#ifdef ENABLE_NLS
#define P_(String) (_clutter_gettext ((String)))
#else
#define P_(String) (String)
#endif

/* This is a replacement for the nearbyint function which always rounds to the
 * nearest integer. nearbyint is apparently a C99 function so it might not
 * always be available but also it seems in glibc it is defined as a function
 * call so this macro could end up faster anyway. We can't just add 0.5f
 * because it will break for negative numbers. */
#define CLUTTER_NEARBYINT(x) ((int) ((x) < 0.0f ? (x) - 0.5f : (x) + 0.5f))

typedef enum {
  CLUTTER_ACTOR_UNUSED_FLAG = 0,

  CLUTTER_IN_DESTRUCTION = 1 << 0,
  CLUTTER_IS_TOPLEVEL    = 1 << 1,
  CLUTTER_IN_REPARENT    = 1 << 2,

  /* Used to avoid recursion */
  CLUTTER_IN_PAINT       = 1 << 3,

  /* Used to avoid recursion */
  CLUTTER_IN_RELAYOUT    = 1 << 4,

  /* Used by the stage if resizing is an asynchronous operation (like on
   * X11) to delay queueing relayouts until we got a notification from the
   * event handling
   */
  CLUTTER_IN_RESIZE      = 1 << 5,

  /* a flag for internal children of Containers */
  CLUTTER_INTERNAL_CHILD = 1 << 6
} ClutterPrivateFlags;

/*
 * ClutterMainContext:
 *
 * The shared state of Clutter
 */
struct _ClutterMainContext
{
  /* the main windowing system backend */
  ClutterBackend *backend;

  /* the main event queue */
  GQueue *events_queue;

  /* timer used to print the FPS count */
  GTimer *timer;

  ClutterPickMode  pick_mode;

  /* mapping between reused integer ids and actors */
  ClutterIDPool *id_pool;

  /* default FPS; this is only used if we cannot sync to vblank */
  guint frame_rate;

  /* actors with a grab on all devices */
  ClutterActor *pointer_grab_actor;
  ClutterActor *keyboard_grab_actor;

  /* stack of actors with shaders during paint */
  GSList *shaders;

  /* fb bit masks for col<->id mapping in picking */
  gint fb_r_mask;
  gint fb_g_mask;
  gint fb_b_mask;
  gint fb_r_mask_used;
  gint fb_g_mask_used;
  gint fb_b_mask_used;

  PangoContext *pango_context;  /* Global Pango context */
  CoglPangoFontMap *font_map;   /* Global font map */

  ClutterEvent *current_event;
  guint32 last_event_time;

  /* list of repaint functions installed through
   * clutter_threads_add_repaint_func()
   */
  GList *repaint_funcs;

  /* main settings singleton */
  ClutterSettings *settings;

  /* boolean flags */
  guint is_initialized          : 1;
  guint motion_events_per_actor : 1;
  guint defer_display_setup     : 1;
  guint options_parsed          : 1;
};

/* shared between clutter-main.c and clutter-frame-source.c */
typedef struct
{
  GSourceFunc func;
  gpointer data;
  GDestroyNotify notify;
} ClutterThreadsDispatch;

gboolean _clutter_threads_dispatch      (gpointer data);
void     _clutter_threads_dispatch_free (gpointer data);

ClutterMainContext *    _clutter_context_get_default                    (void);
gboolean                _clutter_context_is_initialized                 (void);
PangoContext *          _clutter_context_create_pango_context           (void);
PangoContext *          _clutter_context_get_pango_context              (void);
ClutterPickMode         _clutter_context_get_pick_mode                  (void);
void                    _clutter_context_push_shader_stack              (ClutterActor *actor);
ClutterActor *          _clutter_context_pop_shader_stack               (ClutterActor *actor);
ClutterActor *          _clutter_context_peek_shader_stack              (void);
guint32                 _clutter_context_acquire_id                     (gpointer      key);
void                    _clutter_context_release_id                     (guint32       id_);
gboolean                _clutter_context_get_motion_events_enabled      (void);

const gchar *_clutter_gettext (const gchar *str);

gboolean      _clutter_feature_init (GError **error);

/* Picking code */
guint           _clutter_pixel_to_id            (guchar        pixel[4]);
void            _clutter_id_to_color            (guint         id,
                                                 ClutterColor *col);
ClutterActor *  _clutter_get_actor_by_id        (ClutterStage *stage,
                                                 guint32       actor_id);

/* use this function as the accumulator if you have a signal with
 * a G_TYPE_BOOLEAN return value; this will stop the emission as
 * soon as one handler returns TRUE
 */
gboolean _clutter_boolean_handled_accumulator (GSignalInvocationHint *ihint,
                                               GValue                *return_accu,
                                               const GValue          *handler_return,
                                               gpointer               dummy);

void _clutter_run_repaint_functions (void);

void _clutter_constraint_update_allocation (ClutterConstraint *constraint,
                                            ClutterActor      *actor,
                                            ClutterActorBox   *allocation);

GType _clutter_layout_manager_get_child_meta_type (ClutterLayoutManager *manager);

void  _clutter_util_fully_transform_vertices (const CoglMatrix    *modelview,
                                              const CoglMatrix    *projection,
                                              const float         *viewport,
                                              const ClutterVertex *vertices_in,
                                              ClutterVertex       *vertices_out,
                                              int                  n_vertices);

typedef struct _ClutterPlane
{
  CoglVector3 v0;
  CoglVector3 n;
} ClutterPlane;

typedef enum _ClutterCullResult
{
  CLUTTER_CULL_RESULT_UNKNOWN,
  CLUTTER_CULL_RESULT_IN,
  CLUTTER_CULL_RESULT_OUT,
  CLUTTER_CULL_RESULT_PARTIAL
} ClutterCullResult;

G_END_DECLS

#endif /* __CLUTTER_PRIVATE_H__ */
