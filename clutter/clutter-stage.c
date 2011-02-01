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

/**
 * SECTION:clutter-stage
 * @short_description: Top level visual element to which actors are placed.
 *
 * #ClutterStage is a top level 'window' on which child actors are placed
 * and manipulated.
 *
 * Clutter creates a default stage upon initialization, which can be retrieved
 * using clutter_stage_get_default(). Clutter always provides the default
 * stage, unless the backend is unable to create one. The stage returned
 * by clutter_stage_get_default() is guaranteed to always be the same.
 *
 * Backends might provide support for multiple stages. The support for this
 * feature can be checked at run-time using the clutter_feature_available()
 * function and the %CLUTTER_FEATURE_STAGE_MULTIPLE flag. If the backend used
 * supports multiple stages, new #ClutterStage instances can be created
 * using clutter_stage_new(). These stages must be managed by the developer
 * using clutter_actor_destroy(), which will take care of destroying all the
 * actors contained inside them.
 *
 * #ClutterStage is a proxy actor, wrapping the backend-specific
 * implementation of the windowing system. It is possible to subclass
 * #ClutterStage, as long as every overridden virtual function chains up to
 * the parent class corresponding function.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "clutter-stage.h"

#include "clutter-actor-private.h"
#include "clutter-backend-private.h"
#include "clutter-color.h"
#include "clutter-container.h"
#include "clutter-debug.h"
#include "clutter-device-manager-private.h"
#include "clutter-enum-types.h"
#include "clutter-event-private.h"
#include "clutter-main.h"
#include "clutter-marshal.h"
#include "clutter-master-clock.h"
#include "clutter-paint-volume-private.h"
#include "clutter-private.h"
#include "clutter-profile.h"
#include "clutter-stage-manager-private.h"
#include "clutter-stage-private.h"
#include "clutter-util.h"
#include "clutter-version.h" 	/* For flavour */

#include "cogl/cogl.h"

G_DEFINE_TYPE (ClutterStage, clutter_stage, CLUTTER_TYPE_GROUP);

#define CLUTTER_STAGE_GET_PRIVATE(obj) \
(G_TYPE_INSTANCE_GET_PRIVATE ((obj), CLUTTER_TYPE_STAGE, ClutterStagePrivate))

/* <private>
 * ClutterStageHint:
 * @CLUTTER_STAGE_NONE: No hint set
 * @CLUTTER_STAGE_NO_CLEAR_ON_PAINT: When this hint is set, the stage
 *   should not clear the viewport; this flag is useful when painting
 *   fully opaque actors covering the whole visible area of the stage,
 *   i.e. when no blending with the stage color happens over the whole
 *   stage viewport
 *
 * A series of hints that enable or disable behaviours on the stage
 */
typedef enum { /*< prefix=CLUTTER_STAGE >*/
  CLUTTER_STAGE_HINT_NONE = 0,

  CLUTTER_STAGE_NO_CLEAR_ON_PAINT = 1 << 0
} ClutterStageHint;

#define STAGE_NO_CLEAR_ON_PAINT(s)      ((((ClutterStage *) (s))->priv->stage_hints & CLUTTER_STAGE_NO_CLEAR_ON_PAINT) != 0)

struct _ClutterStageQueueRedrawEntry
{
  ClutterActor *actor;
  gboolean has_clip;
  ClutterPaintVolume clip;
};

struct _ClutterStagePrivate
{
  /* the stage implementation */
  ClutterStageWindow *impl;

  ClutterColor        color;
  ClutterPerspective  perspective;
  CoglMatrix          projection;
  float               viewport[4];
  ClutterFog          fog;

  gchar              *title;
  ClutterActor       *key_focused_actor;

  GQueue             *event_queue;

  ClutterStageHint    stage_hints;

  gint                picks_per_frame;

  GArray             *paint_volume_stack;

  const ClutterGeometry *current_paint_clip;

  GList              *pending_queue_redraws;

  ClutterPickMode     pick_buffer_mode;

  GHashTable *devices;

  GTimer *fps_timer;
  gint32 timer_n_frames;

#ifdef CLUTTER_ENABLE_DEBUG
  gulong redraw_count;
#endif /* CLUTTER_ENABLE_DEBUG */

  guint relayout_pending       : 1;
  guint redraw_pending         : 1;
  guint is_fullscreen          : 1;
  guint is_cursor_visible      : 1;
  guint is_user_resizable      : 1;
  guint use_fog                : 1;
  guint throttle_motion_events : 1;
  guint use_alpha              : 1;
  guint min_size_changed       : 1;
  guint dirty_viewport         : 1;
  guint dirty_projection       : 1;
  guint have_valid_pick_buffer : 1;
  guint accept_focus           : 1;
  guint motion_events_enabled  : 1;
};

enum
{
  PROP_0,

  PROP_COLOR,
  PROP_FULLSCREEN_SET,
  PROP_OFFSCREEN,
  PROP_CURSOR_VISIBLE,
  PROP_PERSPECTIVE,
  PROP_TITLE,
  PROP_USER_RESIZABLE,
  PROP_USE_FOG,
  PROP_FOG,
  PROP_USE_ALPHA,
  PROP_KEY_FOCUS,
  PROP_NO_CLEAR_HINT,
  PROP_ACCEPT_FOCUS
};

enum
{
  FULLSCREEN,
  UNFULLSCREEN,
  ACTIVATE,
  DEACTIVATE,
  DELETE_EVENT,

  LAST_SIGNAL
};

static guint stage_signals[LAST_SIGNAL] = { 0, };

static const ClutterColor default_stage_color = { 255, 255, 255, 255 };

static void _clutter_stage_maybe_finish_queue_redraws (ClutterStage *stage);

static void
_clutter_stage_maybe_finish_queue_redraws (ClutterStage *stage);

static void
clutter_stage_get_preferred_width (ClutterActor *self,
                                   gfloat        for_height,
                                   gfloat       *min_width_p,
                                   gfloat       *natural_width_p)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;
  ClutterGeometry geom = { 0, };

  if (priv->impl == NULL)
    return;

  _clutter_stage_window_get_geometry (priv->impl, &geom);

  if (min_width_p)
    *min_width_p = geom.width;

  if (natural_width_p)
    *natural_width_p = geom.width;
}

static void
clutter_stage_get_preferred_height (ClutterActor *self,
                                    gfloat        for_width,
                                    gfloat       *min_height_p,
                                    gfloat       *natural_height_p)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;
  ClutterGeometry geom = { 0, };

  if (priv->impl == NULL)
    return;

  _clutter_stage_window_get_geometry (priv->impl, &geom);

  if (min_height_p)
    *min_height_p = geom.height;

  if (natural_height_p)
    *natural_height_p = geom.height;
}

static inline void
queue_full_redraw (ClutterStage *stage)
{
  ClutterStageWindow *stage_window;

  if (CLUTTER_ACTOR_IN_DESTRUCTION (stage))
    return;

  clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));

  /* Just calling clutter_actor_queue_redraw will typically only
   * redraw the bounding box of the children parented on the stage but
   * in this case we really need to ensure that the full stage is
   * redrawn so we add a NULL redraw clip to the stage window. */
  stage_window = _clutter_stage_get_window (stage);
  if (stage_window == NULL)
    return;

  _clutter_stage_window_add_redraw_clip (stage_window, NULL);
}

static void
clutter_stage_allocate (ClutterActor           *self,
                        const ClutterActorBox  *box,
                        ClutterAllocationFlags  flags)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;
  ClutterGeometry prev_geom;
  ClutterGeometry geom = { 0, };
  gboolean origin_changed;
  gint width, height;

  origin_changed = (flags & CLUTTER_ABSOLUTE_ORIGIN_CHANGED) ? TRUE : FALSE;

  if (priv->impl == NULL)
    return;

  clutter_actor_get_allocation_geometry (self, &prev_geom);

  width = clutter_actor_box_get_width (box);
  height = clutter_actor_box_get_height (box);
  _clutter_stage_window_get_geometry (priv->impl, &geom);

  /* if the stage is fixed size (for instance, it's using a frame-buffer)
   * then we simply ignore any allocation request and override the
   * allocation chain.
   */
  if ((!clutter_feature_available (CLUTTER_FEATURE_STAGE_STATIC)))
    {
      ClutterActorClass *klass;

      CLUTTER_NOTE (LAYOUT,
                    "Following allocation to %dx%d (origin %s)",
                    width, height,
                    origin_changed ? "changed" : "not changed");

      klass = CLUTTER_ACTOR_CLASS (clutter_stage_parent_class);
      klass->allocate (self, box, flags);

      /* Ensure the window is sized correctly */
      if (!priv->is_fullscreen)
        {
          if (priv->min_size_changed)
            {
              gfloat min_width, min_height;
              gboolean min_width_set, min_height_set;

              g_object_get (G_OBJECT (self),
                            "min-width", &min_width,
                            "min-width-set", &min_width_set,
                            "min-height", &min_height,
                            "min-height-set", &min_height_set,
                            NULL);

              if (!min_width_set)
                min_width = 1;
              if (!min_height_set)
                min_height = 1;

              if (width < min_width)
                width = min_width;
              if (height < min_height)
                height = min_height;

              priv->min_size_changed = FALSE;
            }

          if ((geom.width != width) || (geom.height != height))
            _clutter_stage_window_resize (priv->impl, width, height);
        }
    }
  else
    {
      ClutterActorBox override = { 0, };
      ClutterActorClass *klass;

      override.x1 = 0;
      override.y1 = 0;
      override.x2 = geom.width;
      override.y2 = geom.height;

      CLUTTER_NOTE (LAYOUT,
                    "Overrigin original allocation of %dx%d "
                    "with %dx%d (origin %s)",
                    width, height,
                    (int) (override.x2),
                    (int) (override.y2),
                    origin_changed ? "changed" : "not changed");

      /* and store the overridden allocation */
      klass = CLUTTER_ACTOR_CLASS (clutter_stage_parent_class);
      klass->allocate (self, &override, flags);
    }

  /* XXX: Until Cogl becomes fully responsible for backend windows
   * Clutter need to manually keep it informed of the current window
   * size. We do this after the allocation above so that the stage
   * window has a chance to update the window size based on the
   * allocation. */
  _clutter_stage_window_get_geometry (priv->impl, &geom);
  _cogl_onscreen_clutter_backend_set_size (geom.width, geom.height);

  clutter_actor_get_allocation_geometry (self, &geom);
  if (geom.width != prev_geom.width || geom.height != prev_geom.height)
    {
      _clutter_stage_set_viewport (CLUTTER_STAGE (self),
                                   0, 0, geom.width, geom.height);

      /* Note: we don't assume that set_viewport will queue a full redraw
       * since it may bail-out early if something preemptively set the
       * viewport before the stage was really allocated its new size. */
      queue_full_redraw (CLUTTER_STAGE (self));
    }
}

/* This provides a common point of entry for painting the scenegraph
 * for picking or painting...
 *
 * XXX: Instead of having a toplevel 2D clip region, it might be
 * better to have a clip volume within the view frustum. This could
 * allow us to avoid projecting actors into window coordinates to
 * be able to cull them.
 */
void
_clutter_stage_do_paint (ClutterStage *stage, const ClutterGeometry *clip)
{
  ClutterStagePrivate *priv = stage->priv;
  priv->current_paint_clip = clip;
  _clutter_stage_paint_volume_stack_free_all (stage);
  clutter_actor_paint (CLUTTER_ACTOR (stage));
  priv->current_paint_clip = NULL;
}

static void
clutter_stage_paint (ClutterActor *self)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;
  CoglBufferBit clear_flags;
  CoglColor stage_color;
  guint8 real_alpha;
  CLUTTER_STATIC_TIMER (stage_clear_timer,
                        "Painting actors", /* parent */
                        "Stage clear",
                        "The time spent clearing the stage",
                        0 /* no application private data */);

  CLUTTER_NOTE (PAINT, "Initializing stage paint");

  /* composite the opacity to the stage color */
  real_alpha = clutter_actor_get_opacity (self)
             * priv->color.alpha
             / 255;

  /* we use the real alpha to clear the stage if :use-alpha is
   * set; the effect depends entirely on how the Clutter backend
   */
  cogl_color_init_from_4ub (&stage_color,
                            priv->color.red,
                            priv->color.green,
                            priv->color.blue,
                            priv->use_alpha ? real_alpha
                                           : 255);
  cogl_color_premultiply (&stage_color);

  clear_flags = COGL_BUFFER_BIT_DEPTH;
  if (!STAGE_NO_CLEAR_ON_PAINT (self))
    clear_flags |= COGL_BUFFER_BIT_COLOR;

  CLUTTER_TIMER_START (_clutter_uprof_context, stage_clear_timer);

  cogl_clear (&stage_color, clear_flags);

  CLUTTER_TIMER_STOP (_clutter_uprof_context, stage_clear_timer);

  if (priv->use_fog)
    {
      /* we only expose the linear progression of the fog in
       * the ClutterStage API, and that ignores the fog density.
       * thus, we pass 1.0 as the density parameter
       */
      cogl_set_fog (&stage_color,
                    COGL_FOG_MODE_LINEAR,
                    1.0,
                    priv->fog.z_near,
                    priv->fog.z_far);
    }
  else
    cogl_disable_fog ();

  /* this will take care of painting every child */
  CLUTTER_ACTOR_CLASS (clutter_stage_parent_class)->paint (self);
}

static void
clutter_stage_pick (ClutterActor       *self,
		    const ClutterColor *color)
{
  /* Note: we don't chain up to our parent as we don't want any geometry
   * emitted for the stage itself. The stage's pick id is effectively handled
   * by the call to cogl_clear done in clutter-main.c:_clutter_do_pick_async()
   */
  clutter_container_foreach (CLUTTER_CONTAINER (self),
                             CLUTTER_CALLBACK (clutter_actor_paint),
                             NULL);
}

static gboolean
clutter_stage_get_paint_volume (ClutterActor *self,
                                ClutterPaintVolume *volume)
{
  /* Returning False effectively means Clutter has to assume it covers
   * everything... */
  return FALSE;
}

static void
clutter_stage_realize (ClutterActor *self)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;
  gboolean is_realized;

  /* Make sure the viewport and projection matrix are valid for the
   * first paint (which will likely occur before the ConfigureNotify
   * is received)
   */
  priv->dirty_viewport = TRUE;
  priv->dirty_projection = TRUE;

  g_assert (priv->impl != NULL);
  is_realized = _clutter_stage_window_realize (priv->impl);

  /* ensure that the stage is using the context if the
   * realization sequence was successful
   */
  if (is_realized)
    {
      ClutterBackend *backend = clutter_get_default_backend ();
      GError *error = NULL;

      /* We want to select the context without calling
         clutter_backend_ensure_context so that it doesn't call any
         Cogl functions. Otherwise it would create the Cogl context
         before we get a chance to check whether the GL version is
         valid */
      _clutter_backend_ensure_context_internal (backend, CLUTTER_STAGE (self));

      /* Make sure Cogl can support the driver */
      if (!_cogl_check_driver_valid (&error))
        {
          g_critical ("The GL driver is not supported: %s",
                      error->message);
          g_clear_error (&error);
          CLUTTER_ACTOR_UNSET_FLAGS (self, CLUTTER_ACTOR_REALIZED);
        }
      else
        CLUTTER_ACTOR_SET_FLAGS (self, CLUTTER_ACTOR_REALIZED);
    }
  else
    CLUTTER_ACTOR_UNSET_FLAGS (self, CLUTTER_ACTOR_REALIZED);
}

static void
clutter_stage_unrealize (ClutterActor *self)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;

  /* and then unrealize the implementation */
  g_assert (priv->impl != NULL);
  _clutter_stage_window_unrealize (priv->impl);

  CLUTTER_ACTOR_UNSET_FLAGS (self, CLUTTER_ACTOR_REALIZED);

  clutter_stage_ensure_current (CLUTTER_STAGE (self));
}

static void
clutter_stage_show (ClutterActor *self)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;

  CLUTTER_ACTOR_CLASS (clutter_stage_parent_class)->show (self);

  /* Possibly do an allocation run so that the stage will have the
     right size before we map it */
  _clutter_stage_maybe_relayout (self);

  g_assert (priv->impl != NULL);
  _clutter_stage_window_show (priv->impl, TRUE);
}

static void
clutter_stage_hide (ClutterActor *self)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (self)->priv;

  g_assert (priv->impl != NULL);
  _clutter_stage_window_hide (priv->impl);

  CLUTTER_ACTOR_CLASS (clutter_stage_parent_class)->hide (self);
}

static void
clutter_stage_emit_key_focus_event (ClutterStage *stage,
                                    gboolean      focus_in)
{
  ClutterStagePrivate *priv = stage->priv;

  if (priv->key_focused_actor == NULL)
    return;

  if (focus_in)
    g_signal_emit_by_name (priv->key_focused_actor, "key-focus-in");
  else
    g_signal_emit_by_name (priv->key_focused_actor, "key-focus-out");
}

static void
clutter_stage_real_activate (ClutterStage *stage)
{
  clutter_stage_emit_key_focus_event (stage, TRUE);
}

static void
clutter_stage_real_deactivate (ClutterStage *stage)
{
  clutter_stage_emit_key_focus_event (stage, FALSE);
}

static void
clutter_stage_real_fullscreen (ClutterStage *stage)
{
  ClutterStagePrivate *priv = stage->priv;
  ClutterGeometry geom;
  ClutterActorBox box;

  /* we need to force an allocation here because the size
   * of the stage might have been changed by the backend
   *
   * this is a really bad solution to the issues caused by
   * the fact that fullscreening the stage on the X11 backends
   * is really an asynchronous operation
   */
  _clutter_stage_window_get_geometry (priv->impl, &geom);

  box.x1 = 0;
  box.y1 = 0;
  box.x2 = geom.width;
  box.y2 = geom.height;

  clutter_actor_allocate (CLUTTER_ACTOR (stage),
                          &box,
                          CLUTTER_ALLOCATION_NONE);
}

void
_clutter_stage_queue_event (ClutterStage *stage,
			    ClutterEvent *event)
{
  ClutterStagePrivate *priv;
  gboolean first_event;
  ClutterInputDevice *device;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  first_event = priv->event_queue->length == 0;

  g_queue_push_tail (priv->event_queue, clutter_event_copy (event));

  if (first_event)
    {
      ClutterMasterClock *master_clock = _clutter_master_clock_get_default ();
      _clutter_master_clock_start_running (master_clock);
    }

  /* if needed, update the state of the input device of the event.
   * we do it here to avoid calling the same code from every backend
   * event processing function
   */
  device = clutter_event_get_device (event);
  if (device != NULL)
    {
      ClutterModifierType event_state = clutter_event_get_state (event);
      guint32 event_time = clutter_event_get_time (event);
      gfloat event_x, event_y;

      clutter_event_get_coords (event, &event_x, &event_y);

      _clutter_input_device_set_coords (device, event_x, event_y);
      _clutter_input_device_set_state (device, event_state);
      _clutter_input_device_set_time (device, event_time);
    }
}

gboolean
_clutter_stage_has_queued_events (ClutterStage *stage)
{
  ClutterStagePrivate *priv;

  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);

  priv = stage->priv;

  return priv->event_queue->length > 0;
}

void
_clutter_stage_process_queued_events (ClutterStage *stage)
{
  ClutterStagePrivate *priv;
  GList *events, *l;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  if (priv->event_queue->length == 0)
    return;

  /* In case the stage gets destroyed during event processing */
  g_object_ref (stage);

  /* Steal events before starting processing to avoid reentrancy
   * issues */
  events = priv->event_queue->head;
  priv->event_queue->head =  NULL;
  priv->event_queue->tail = NULL;
  priv->event_queue->length = 0;

  for (l = events; l != NULL; l = l->next)
    {
      ClutterEvent *event;
      ClutterEvent *next_event;
      ClutterInputDevice *device;
      ClutterInputDevice *next_device;
      gboolean check_device = FALSE;

      event = l->data;
      next_event = l->next ? l->next->data : NULL;

      device = clutter_event_get_device (event);

      if (next_event != NULL)
        next_device = clutter_event_get_device (next_event);
      else
        next_device = NULL;

      if (device != NULL && next_device != NULL)
        check_device = TRUE;

      /* Skip consecutive motion events coming from the same device */
      if (priv->throttle_motion_events &&
          next_event != NULL &&
	  event->type == CLUTTER_MOTION &&
	  (next_event->type == CLUTTER_MOTION ||
	   next_event->type == CLUTTER_LEAVE) &&
          (!check_device || (device == next_device)))
	{
          CLUTTER_NOTE (EVENT,
                        "Omitting motion event at %d, %d",
                        (int) event->motion.x,
                        (int) event->motion.y);
          goto next_event;
	}

      _clutter_process_event (event);

    next_event:
      clutter_event_free (event);
    }

  g_list_free (events);

  g_object_unref (stage);
}

/**
 * _clutter_stage_needs_update:
 * @stage: A #ClutterStage
 *
 * Determines if _clutter_stage_do_update() needs to be called.
 *
 * Return value: %TRUE if the stage need layout or painting
 */
gboolean
_clutter_stage_needs_update (ClutterStage *stage)
{
  ClutterStagePrivate *priv;

  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);

  priv = stage->priv;

  return priv->relayout_pending || priv->redraw_pending;
}

void
_clutter_stage_maybe_relayout (ClutterActor *actor)
{
  ClutterStage *stage = CLUTTER_STAGE (actor);
  ClutterStagePrivate *priv = stage->priv;
  gfloat natural_width, natural_height;
  ClutterActorBox box = { 0, };
  CLUTTER_STATIC_TIMER (relayout_timer,
                        "Mainloop", /* no parent */
                        "Layouting",
                        "The time spent reallocating the stage",
                        0 /* no application private data */);

  if (!priv->relayout_pending)
    return;

  /* avoid reentrancy */
  if (!CLUTTER_ACTOR_IN_RELAYOUT (stage))
    {
      priv->relayout_pending = FALSE;

      CLUTTER_TIMER_START (_clutter_uprof_context, relayout_timer);
      CLUTTER_NOTE (ACTOR, "Recomputing layout");

      CLUTTER_SET_PRIVATE_FLAGS (stage, CLUTTER_IN_RELAYOUT);

      natural_width = natural_height = 0;
      clutter_actor_get_preferred_size (CLUTTER_ACTOR (stage),
                                        NULL, NULL,
                                        &natural_width, &natural_height);

      box.x1 = 0;
      box.y1 = 0;
      box.x2 = natural_width;
      box.y2 = natural_height;

      CLUTTER_NOTE (ACTOR, "Allocating (0, 0 - %d, %d) for the stage",
                    (int) natural_width,
                    (int) natural_height);

      clutter_actor_allocate (CLUTTER_ACTOR (stage),
                              &box, CLUTTER_ALLOCATION_NONE);

      CLUTTER_UNSET_PRIVATE_FLAGS (stage, CLUTTER_IN_RELAYOUT);
      CLUTTER_TIMER_STOP (_clutter_uprof_context, relayout_timer);
    }
}

static void
clutter_stage_do_redraw (ClutterStage *stage)
{
  ClutterBackend *backend = clutter_get_default_backend ();
  ClutterActor *actor = CLUTTER_ACTOR (stage);
  ClutterStagePrivate *priv = stage->priv;

  CLUTTER_TIMESTAMP (SCHEDULER, "Redraw started for %s[%p]",
                     _clutter_actor_get_debug_name (actor),
                     stage);

  _clutter_stage_set_pick_buffer_valid (stage, FALSE, -1);
  _clutter_stage_reset_picks_per_frame_counter (stage);

  _clutter_backend_ensure_context (backend, stage);

  if (clutter_get_show_fps ())
    {
      if (priv->fps_timer == NULL)
        priv->fps_timer = g_timer_new ();
    }

  _clutter_stage_maybe_setup_viewport (stage);

  _clutter_backend_redraw (backend, stage);

  if (clutter_get_show_fps ())
    {
      priv->timer_n_frames += 1;

      if (g_timer_elapsed (priv->fps_timer, NULL) >= 1.0)
        {
          g_print ("*** FPS for %s: %i ***\n",
                   _clutter_actor_get_debug_name (actor),
                   priv->timer_n_frames);

          priv->timer_n_frames = 0;
          g_timer_start (priv->fps_timer);
        }
    }

  CLUTTER_TIMESTAMP (SCHEDULER, "Redraw finished for %s[%p]",
                     _clutter_actor_get_debug_name (actor),
                     stage);
}

/**
 * _clutter_stage_do_update:
 * @stage: A #ClutterStage
 *
 * Handles per-frame layout and repaint for the stage.
 *
 * Return value: %TRUE if the stage was updated
 */
gboolean
_clutter_stage_do_update (ClutterStage *stage)
{
  ClutterStagePrivate *priv;

  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);

  priv = stage->priv;

  if (CLUTTER_ACTOR_IN_DESTRUCTION (stage))
    return FALSE;

  /* NB: We need to ensure we have an up to date layout *before* we
   * check or clear the pending redraws flag since a relayout may
   * queue a redraw.
   */
  _clutter_stage_maybe_relayout (CLUTTER_ACTOR (stage));

  if (!priv->redraw_pending)
    return FALSE;

  _clutter_stage_maybe_finish_queue_redraws (stage);

  clutter_stage_do_redraw (stage);

  /* reset the guard, so that new redraws are possible */
  priv->redraw_pending = FALSE;

#ifdef CLUTTER_ENABLE_DEBUG
  if (priv->redraw_count > 0)
    {
      CLUTTER_NOTE (SCHEDULER, "Queued %lu redraws during the last cycle",
                    priv->redraw_count);

      priv->redraw_count = 0;
    }
#endif /* CLUTTER_ENABLE_DEBUG */

  return TRUE;
}

static void
clutter_stage_real_queue_relayout (ClutterActor *self)
{
  ClutterStage *stage = CLUTTER_STAGE (self);
  ClutterStagePrivate *priv = stage->priv;
  ClutterActorClass *parent_class;

  priv->relayout_pending = TRUE;

  /* chain up */
  parent_class = CLUTTER_ACTOR_CLASS (clutter_stage_parent_class);
  parent_class->queue_relayout (self);
}

static void
clutter_stage_real_queue_redraw (ClutterActor *actor,
                                 ClutterActor *leaf)
{
  ClutterStage *stage = CLUTTER_STAGE (actor);
  ClutterStagePrivate *priv = stage->priv;
  ClutterStageWindow *stage_window;
  ClutterGeometry stage_clip;
  const ClutterPaintVolume *redraw_clip;
  ClutterPaintVolume projected_clip;
  CoglMatrix modelview;
  ClutterActorBox bounding_box;

  if (CLUTTER_ACTOR_IN_DESTRUCTION (actor))
    return;

  /* If the backend can't do anything with redraw clips (e.g. it already knows
   * it needs to redraw everything anyway) then don't spend time transforming
   * any clip volume into stage coordinates... */
  stage_window = _clutter_stage_get_window (stage);
  if (stage_window == NULL)
    return;

  if (_clutter_stage_window_ignoring_redraw_clips (stage_window))
    {
      _clutter_stage_window_add_redraw_clip (stage_window, NULL);
      return;
    }

  /* Convert the clip volume (which is in leaf actor coordinates) into stage
   * coordinates and then into an axis aligned stage coordinates bounding
   * box...
   */

  if (!_clutter_actor_get_queue_redraw_clip (leaf))
    {
      _clutter_stage_window_add_redraw_clip (stage_window, NULL);
      return;
    }

  redraw_clip = _clutter_actor_get_queue_redraw_clip (leaf);

  _clutter_paint_volume_copy_static (redraw_clip, &projected_clip);

  /* NB: _clutter_actor_apply_modelview_transform_recursive will never
   * include the transformation between stage coordinates and OpenGL
   * window coordinates, we have to explicitly use the
   * stage->apply_transform to get that... */
  cogl_matrix_init_identity (&modelview);
  _clutter_actor_apply_modelview_transform (CLUTTER_ACTOR (stage), &modelview);
  _clutter_actor_apply_modelview_transform_recursive (leaf, NULL, &modelview);

  _clutter_paint_volume_project (&projected_clip,
                                 &modelview,
                                 &priv->projection,
                                 priv->viewport);

  _clutter_paint_volume_get_bounding_box (&projected_clip, &bounding_box);
  clutter_paint_volume_free (&projected_clip);

  clutter_actor_box_clamp_to_pixel (&bounding_box);

  /* when converting to integer coordinates make sure we round the edges of the
   * clip rectangle outwards... */
  stage_clip.x = bounding_box.x1;
  stage_clip.y = bounding_box.y1;
  stage_clip.width = bounding_box.x2 - stage_clip.x;
  stage_clip.height = bounding_box.y2 - stage_clip.y;

  _clutter_stage_window_add_redraw_clip (stage_window, &stage_clip);
}

gboolean
_clutter_stage_has_full_redraw_queued (ClutterStage *stage)
{
  ClutterStageWindow *stage_window = _clutter_stage_get_window (stage);

  if (CLUTTER_ACTOR_IN_DESTRUCTION (stage) || stage_window == NULL)
    return FALSE;

  if (stage->priv->redraw_pending &&
      !_clutter_stage_window_has_redraw_clips (stage_window))
    return TRUE;
  else
    return FALSE;
}

static gboolean
clutter_stage_real_delete_event (ClutterStage *stage,
                                 ClutterEvent *event)
{
  if (clutter_stage_is_default (stage))
    clutter_main_quit ();
  else
    clutter_actor_destroy (CLUTTER_ACTOR (stage));

  return TRUE;
}

static void
clutter_stage_real_apply_transform (ClutterActor *stage,
                                    CoglMatrix   *matrix)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (stage)->priv;
  CoglMatrix perspective;
  gfloat z_camera;
  gfloat width, height;

  /*
   * In theory, we can compute the camera distance from screen as:
   *
   *   0.5 * tan (FOV)
   *
   * However, it's better to compute the z_camera from our projection
   * matrix so that we get a 1:1 mapping at the screen distance. Consider
   * the upper-left corner of the screen. It has object coordinates
   * (0,0,0), so by the transform below, ends up with eye coordinate
   *
   *   x_eye = x_object / width - 0.5 = - 0.5
   *   y_eye = (height - y_object) / width - 0.5 = 0.5
   *   z_eye = z_object / width - z_camera = - z_camera
   *
   * From cogl_perspective(), we know that the projection matrix has
   * the form:
   *
   *  (x, 0,  0, 0)
   *  (0, y,  0, 0)
   *  (0, 0,  c, d)
   *  (0, 0, -1, 0)
   *
   * Applied to the above, we get clip coordinates of
   *
   *  x_clip = x * (- 0.5)
   *  y_clip = y * 0.5
   *  w_clip = - 1 * (- z_camera) = z_camera
   *
   * Dividing through by w to get normalized device coordinates, we
   * have, x_nd = x * 0.5 / z_camera, y_nd = - y * 0.5 / z_camera.
   * The upper left corner of the screen has normalized device coordinates,
   * (-1, 1), so to have the correct 1:1 mapping, we have to have:
   *
   *   z_camera = 0.5 * x = 0.5 * y
   *
   * If x != y, then we have a non-uniform aspect ration, and a 1:1 mapping
   * doesn't make sense.
   */

  cogl_matrix_init_identity (&perspective);
  cogl_matrix_perspective (&perspective,
                           priv->perspective.fovy,
                           priv->perspective.aspect,
                           priv->perspective.z_near,
                           priv->perspective.z_far);

  z_camera = 0.5f * perspective.xx;

  clutter_actor_get_size (stage, &width, &height);

  cogl_matrix_init_identity (matrix);
  cogl_matrix_translate (matrix, -0.5f, -0.5f, -z_camera);
  cogl_matrix_scale (matrix,
                     1.0f / width, -1.0f / height, 1.0f / width);
  cogl_matrix_translate (matrix, 0.0f, -1.0f * height, 0.0f);
}

static void
clutter_stage_set_property (GObject      *object,
			    guint         prop_id,
			    const GValue *value,
			    GParamSpec   *pspec)
{
  ClutterStage *stage = CLUTTER_STAGE (object);

  switch (prop_id)
    {
    case PROP_COLOR:
      clutter_stage_set_color (stage, clutter_value_get_color (value));
      break;

    case PROP_OFFSCREEN:
      if (g_value_get_boolean (value))
        g_warning ("Offscreen stages are currently not supported\n");
      break;

    case PROP_CURSOR_VISIBLE:
      if (g_value_get_boolean (value))
        clutter_stage_show_cursor (stage);
      else
        clutter_stage_hide_cursor (stage);
      break;

    case PROP_PERSPECTIVE:
      clutter_stage_set_perspective (stage, g_value_get_boxed (value));
      break;

    case PROP_TITLE:
      clutter_stage_set_title (stage, g_value_get_string (value));
      break;

    case PROP_USER_RESIZABLE:
      clutter_stage_set_user_resizable (stage, g_value_get_boolean (value));
      break;

    case PROP_USE_FOG:
      clutter_stage_set_use_fog (stage, g_value_get_boolean (value));
      break;

    case PROP_FOG:
      clutter_stage_set_fog (stage, g_value_get_boxed (value));
      break;

    case PROP_USE_ALPHA:
      clutter_stage_set_use_alpha (stage, g_value_get_boolean (value));
      break;

    case PROP_KEY_FOCUS:
      clutter_stage_set_key_focus (stage, g_value_get_object (value));
      break;

    case PROP_NO_CLEAR_HINT:
      clutter_stage_set_no_clear_hint (stage, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
clutter_stage_get_property (GObject    *gobject,
			    guint       prop_id,
			    GValue     *value,
			    GParamSpec *pspec)
{
  ClutterStagePrivate *priv = CLUTTER_STAGE (gobject)->priv;

  switch (prop_id)
    {
    case PROP_COLOR:
      clutter_value_set_color (value, &priv->color);
      break;

    case PROP_OFFSCREEN:
      g_value_set_boolean (value, FALSE);
      break;

    case PROP_FULLSCREEN_SET:
      g_value_set_boolean (value, priv->is_fullscreen);
      break;

    case PROP_CURSOR_VISIBLE:
      g_value_set_boolean (value, priv->is_cursor_visible);
      break;

    case PROP_PERSPECTIVE:
      g_value_set_boxed (value, &priv->perspective);
      break;

    case PROP_TITLE:
      g_value_set_string (value, priv->title);
      break;

    case PROP_USER_RESIZABLE:
      g_value_set_boolean (value, priv->is_user_resizable);
      break;

    case PROP_USE_FOG:
      g_value_set_boolean (value, priv->use_fog);
      break;

    case PROP_FOG:
      g_value_set_boxed (value, &priv->fog);
      break;

    case PROP_USE_ALPHA:
      g_value_set_boolean (value, priv->use_alpha);
      break;

    case PROP_KEY_FOCUS:
      g_value_set_object (value, priv->key_focused_actor);
      break;

    case PROP_NO_CLEAR_HINT:
      {
        gboolean hint =
          (priv->stage_hints & CLUTTER_STAGE_NO_CLEAR_ON_PAINT) != 0;

        g_value_set_boolean (value, hint);
      }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
clutter_stage_dispose (GObject *object)
{
  ClutterStage        *stage = CLUTTER_STAGE (object);
  ClutterStagePrivate *priv = stage->priv;
  ClutterStageManager *stage_manager;

  clutter_actor_hide (CLUTTER_ACTOR (object));

  stage_manager = clutter_stage_manager_get_default ();
  _clutter_stage_manager_remove_stage (stage_manager, stage);

  _clutter_clear_events_queue_for_stage (stage);

  if (priv->impl != NULL)
    {
      CLUTTER_NOTE (BACKEND, "Disposing of the stage implementation");

      _clutter_stage_window_unrealize (priv->impl);
      g_object_unref (priv->impl);
      priv->impl = NULL;
    }

  G_OBJECT_CLASS (clutter_stage_parent_class)->dispose (object);
}

static void
clutter_stage_finalize (GObject *object)
{
  ClutterStage *stage = CLUTTER_STAGE (object);
  ClutterStagePrivate *priv = stage->priv;

  g_queue_foreach (priv->event_queue, (GFunc) clutter_event_free, NULL);
  g_queue_free (priv->event_queue);

  g_free (priv->title);

  g_array_free (priv->paint_volume_stack, TRUE);

  g_hash_table_destroy (priv->devices);

  if (priv->fps_timer != NULL)
    g_timer_destroy (priv->fps_timer);

  G_OBJECT_CLASS (clutter_stage_parent_class)->finalize (object);
}

static void
clutter_stage_class_init (ClutterStageClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  GParamSpec *pspec;

  gobject_class->set_property = clutter_stage_set_property;
  gobject_class->get_property = clutter_stage_get_property;
  gobject_class->dispose = clutter_stage_dispose;
  gobject_class->finalize = clutter_stage_finalize;

  actor_class->allocate = clutter_stage_allocate;
  actor_class->get_preferred_width = clutter_stage_get_preferred_width;
  actor_class->get_preferred_height = clutter_stage_get_preferred_height;
  actor_class->paint = clutter_stage_paint;
  actor_class->pick = clutter_stage_pick;
  actor_class->get_paint_volume = clutter_stage_get_paint_volume;
  actor_class->realize = clutter_stage_realize;
  actor_class->unrealize = clutter_stage_unrealize;
  actor_class->show = clutter_stage_show;
  actor_class->hide = clutter_stage_hide;
  actor_class->queue_relayout = clutter_stage_real_queue_relayout;
  actor_class->queue_redraw = clutter_stage_real_queue_redraw;
  actor_class->apply_transform = clutter_stage_real_apply_transform;

  /**
   * ClutterStage:fullscreen:
   *
   * Whether the stage should be fullscreen or not.
   *
   * This property is set by calling clutter_stage_set_fullscreen()
   * but since the actual implementation is delegated to the backend
   * you should connect to the notify::fullscreen-set signal in order
   * to get notification if the fullscreen state has been successfully
   * achieved.
   *
   * Since: 1.0
   */
  pspec = g_param_spec_boolean ("fullscreen-set",
                                P_("Fullscreen Set"),
                                P_("Whether the main stage is fullscreen"),
                                FALSE,
                                CLUTTER_PARAM_READABLE);
  g_object_class_install_property (gobject_class,
                                   PROP_FULLSCREEN_SET,
                                   pspec);
  /**
   * ClutterStage:offscreen:
   *
   * Whether the stage should be rendered in an offscreen buffer.
   *
   * <warning><para>Not every backend supports redirecting the
   * stage to an offscreen buffer. This property might not work
   * and it might be deprecated at any later date.</para></warning>
   */
  pspec = g_param_spec_boolean ("offscreen",
                                P_("Offscreen"),
                                P_("Whether the main stage should be rendered offscreen"),
                                FALSE,
                                CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class,
                                   PROP_OFFSCREEN,
                                   pspec);
  /**
   * ClutterStage:cursor-visible:
   *
   * Whether the mouse pointer should be visible
   */
  pspec = g_param_spec_boolean ("cursor-visible",
                                P_("Cursor Visible"),
                                P_("Whether the mouse pointer is visible on the main stage"),
                                TRUE,
                                CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class,
                                   PROP_CURSOR_VISIBLE,
                                   pspec);
  /**
   * ClutterStage:user-resizable:
   *
   * Whether the stage is resizable via user interaction.
   *
   * Since: 0.4
   */
  pspec = g_param_spec_boolean ("user-resizable",
                                P_("User Resizable"),
                                P_("Whether the stage is able to be resized via user interaction"),
                                FALSE,
                                CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class,
                                   PROP_USER_RESIZABLE,
                                   pspec);
  /**
   * ClutterStage:color:
   *
   * The color of the main stage.
   */
  pspec = clutter_param_spec_color ("color",
                                    P_("Color"),
                                    P_("The color of the stage"),
                                    &default_stage_color,
                                    CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_COLOR, pspec);

  /**
   * ClutterStage:perspective:
   *
   * The parameters used for the perspective projection from 3D
   * coordinates to 2D
   *
   * Since: 0.8.2
   */
  pspec = g_param_spec_boxed ("perspective",
                              P_("Perspective"),
                              P_("Perspective projection parameters"),
                              CLUTTER_TYPE_PERSPECTIVE,
                              CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class,
                                   PROP_PERSPECTIVE,
                                   pspec);

  /**
   * ClutterStage:title:
   *
   * The stage's title - usually displayed in stage windows title decorations.
   *
   * Since: 0.4
   */
  pspec = g_param_spec_string ("title",
                               P_("Title"),
                               P_("Stage Title"),
                               NULL,
                               CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_TITLE, pspec);

  /**
   * ClutterStage:use-fog:
   *
   * Whether the stage should use a linear GL "fog" in creating the
   * depth-cueing effect, to enhance the perception of depth by fading
   * actors farther from the viewpoint.
   *
   * Since: 0.6
   */
  pspec = g_param_spec_boolean ("use-fog",
                                P_("Use Fog"),
                                P_("Whether to enable depth cueing"),
                                FALSE,
                                CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_USE_FOG, pspec);

  /**
   * ClutterStage:fog:
   *
   * The settings for the GL "fog", used only if #ClutterStage:use-fog
   * is set to %TRUE
   *
   * Since: 1.0
   */
  pspec = g_param_spec_boxed ("fog",
                              P_("Fog"),
                              P_("Settings for the depth cueing"),
                              CLUTTER_TYPE_FOG,
                              CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_FOG, pspec);

  /**
   * ClutterStage:use-alpha:
   *
   * Whether the #ClutterStage should honour the alpha component of the
   * #ClutterStage:color property when painting. If Clutter is run under
   * a compositing manager this will result in the stage being blended
   * with the underlying window(s)
   *
   * Since: 1.2
   */
  pspec = g_param_spec_boolean ("use-alpha",
                                P_("Use Alpha"),
                                P_("Whether to honour the alpha component of the stage color"),
                                FALSE,
                                CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_USE_ALPHA, pspec);

  /**
   * ClutterStage:key-focus:
   *
   * The #ClutterActor that will receive key events from the underlying
   * windowing system.
   *
   * If %NULL, the #ClutterStage will receive the events.
   *
   * Since: 1.2
   */
  pspec = g_param_spec_object ("key-focus",
                               P_("Key Focus"),
                               P_("The currently key focused actor"),
                               CLUTTER_TYPE_ACTOR,
                               CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_KEY_FOCUS, pspec);

  /**
   * ClutterStage:no-clear-hint:
   *
   * Whether or not the #ClutterStage should clear its contents
   * before each paint cycle.
   *
   * See clutter_stage_set_no_clear_hint() for further information.
   *
   * Since: 1.4
   */
  pspec = g_param_spec_boolean ("no-clear-hint",
                                P_("No Clear Hint"),
                                P_("Whether the stage should clear its contents"),
                                FALSE,
                                CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_NO_CLEAR_HINT, pspec);

  /**
   * ClutterStage:accept-focus:
   *
   * Whether the #ClutterStage should accept key focus when shown.
   *
   * Since: 1.6
   */
  pspec = g_param_spec_boolean ("accept-focus",
                                P_("Accept Focus"),
                                P_("Whether the stage should accept focus on show"),
                                TRUE,
                                CLUTTER_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_ACCEPT_FOCUS, pspec);

  /**
   * ClutterStage::fullscreen
   * @stage: the stage which was fullscreened
   *
   * The ::fullscreen signal is emitted when the stage is made fullscreen.
   *
   * Since: 0.6
   */
  stage_signals[FULLSCREEN] =
    g_signal_new (I_("fullscreen"),
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_FIRST,
		  G_STRUCT_OFFSET (ClutterStageClass, fullscreen),
		  NULL, NULL,
		  _clutter_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
  /**
   * ClutterStage::unfullscreen
   * @stage: the stage which has left a fullscreen state.
   *
   * The ::unfullscreen signal is emitted when the stage leaves a fullscreen
   * state.
   *
   * Since: 0.6
   */
  stage_signals[UNFULLSCREEN] =
    g_signal_new (I_("unfullscreen"),
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (ClutterStageClass, unfullscreen),
		  NULL, NULL,
		  _clutter_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
  /**
   * ClutterStage::activate
   * @stage: the stage which was activated
   *
   * The ::activate signal is emitted when the stage receives key focus
   * from the underlying window system.
   *
   * Since: 0.6
   */
  stage_signals[ACTIVATE] =
    g_signal_new (I_("activate"),
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (ClutterStageClass, activate),
		  NULL, NULL,
		  _clutter_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
  /**
   * ClutterStage::deactivate
   * @stage: the stage which was deactivated
   *
   * The ::activate signal is emitted when the stage loses key focus
   * from the underlying window system.
   *
   * Since: 0.6
   */
  stage_signals[DEACTIVATE] =
    g_signal_new (I_("deactivate"),
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (ClutterStageClass, deactivate),
		  NULL, NULL,
		  _clutter_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);

  /**
   * ClutterStage::delete-event:
   * @stage: the stage that received the event
   * @event: a #ClutterEvent of type %CLUTTER_DELETE
   *
   * The ::delete-event signal is emitted when the user closes a
   * #ClutterStage window using the window controls.
   *
   * Clutter by default will call clutter_main_quit() if @stage is
   * the default stage, and clutter_actor_destroy() for any other
   * stage.
   *
   * It is possible to override the default behaviour by connecting
   * a new handler and returning %TRUE there.
   *
   * <note>This signal is emitted only on Clutter backends that
   * embed #ClutterStage in native windows. It is not emitted for
   * backends that use a static frame buffer.</note>
   *
   * Since: 1.2
   */
  stage_signals[DELETE_EVENT] =
    g_signal_new (I_("delete-event"),
                  G_TYPE_FROM_CLASS (gobject_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (ClutterStageClass, delete_event),
                  _clutter_boolean_handled_accumulator, NULL,
                  _clutter_marshal_BOOLEAN__BOXED,
                  G_TYPE_BOOLEAN, 1,
                  CLUTTER_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);

  klass->fullscreen = clutter_stage_real_fullscreen;
  klass->activate = clutter_stage_real_activate;
  klass->deactivate = clutter_stage_real_deactivate;
  klass->delete_event = clutter_stage_real_delete_event;

  g_type_class_add_private (gobject_class, sizeof (ClutterStagePrivate));
}

static void
clutter_stage_notify_min_size (ClutterStage *self)
{
  self->priv->min_size_changed = TRUE;
}

static void
clutter_stage_init (ClutterStage *self)
{
  ClutterStagePrivate *priv;
  ClutterBackend *backend;
  ClutterGeometry geom;

  /* a stage is a top-level object */
  CLUTTER_SET_PRIVATE_FLAGS (self, CLUTTER_IS_TOPLEVEL);

  self->priv = priv = CLUTTER_STAGE_GET_PRIVATE (self);

  CLUTTER_NOTE (BACKEND, "Creating stage from the default backend");
  backend = clutter_get_default_backend ();
  priv->impl = _clutter_backend_create_stage (backend, self, NULL);
  if (!priv->impl)
    {
      g_warning ("Unable to create a new stage, falling back to the "
                 "default stage.");
      priv->impl = _clutter_stage_get_default_window ();

      /* at this point we must have a default stage, or we're screwed */
      g_assert (priv->impl != NULL);
    }

  priv->event_queue = g_queue_new ();

  priv->is_fullscreen          = FALSE;
  priv->is_user_resizable      = FALSE;
  priv->is_cursor_visible      = TRUE;
  priv->use_fog                = FALSE;
  priv->throttle_motion_events = TRUE;
  priv->min_size_changed       = FALSE;
  priv->motion_events_enabled  = clutter_get_motion_events_enabled ();

  priv->color = default_stage_color;

  priv->perspective.fovy   = 60.0; /* 60 Degrees */
  priv->perspective.aspect = 1.0;
  priv->perspective.z_near = 0.1;
  priv->perspective.z_far  = 100.0;

  cogl_matrix_init_identity (&priv->projection);
  cogl_matrix_perspective (&priv->projection,
                           priv->perspective.fovy,
                           priv->perspective.aspect,
                           priv->perspective.z_near,
                           priv->perspective.z_far);

  /* depth cueing */
  priv->fog.z_near = 1.0;
  priv->fog.z_far  = 2.0;

  priv->relayout_pending = TRUE;

  clutter_actor_set_reactive (CLUTTER_ACTOR (self), TRUE);
  clutter_stage_set_title (self, g_get_prgname ());
  clutter_stage_set_key_focus (self, NULL);

  g_signal_connect (self, "notify::min-width",
                    G_CALLBACK (clutter_stage_notify_min_size), NULL);
  g_signal_connect (self, "notify::min-height",
                    G_CALLBACK (clutter_stage_notify_min_size), NULL);

  _clutter_stage_window_get_geometry (priv->impl, &geom);
  _clutter_stage_set_viewport (self, 0, 0, geom.width, geom.height);

  _clutter_stage_set_pick_buffer_valid (self, FALSE, CLUTTER_PICK_ALL);
  _clutter_stage_reset_picks_per_frame_counter (self);

  priv->paint_volume_stack =
    g_array_new (FALSE, FALSE, sizeof (ClutterPaintVolume));

  priv->devices = g_hash_table_new (NULL, NULL);
}

/**
 * clutter_stage_get_default:
 *
 * Returns the main stage. The default #ClutterStage is a singleton,
 * so the stage will be created the first time this function is
 * called (typically, inside clutter_init()); all the subsequent
 * calls to clutter_stage_get_default() will return the same instance.
 *
 * Clutter guarantess the existence of the default stage.
 *
 * Return value: (transfer none): the main #ClutterStage.  You should never
 *   destroy or unref the returned actor.
 */
ClutterActor *
clutter_stage_get_default (void)
{
  ClutterStageManager *stage_manager = clutter_stage_manager_get_default ();
  ClutterStage *stage;

  stage = clutter_stage_manager_get_default_stage (stage_manager);
  if (G_UNLIKELY (stage == NULL))
    {
      /* This will take care of automatically adding the stage to the
       * stage manager and setting it as the default. Its floating
       * reference will be claimed by the stage manager.
       */
      stage = g_object_new (CLUTTER_TYPE_STAGE, NULL);
      _clutter_stage_manager_set_default_stage (stage_manager, stage);

      /* the default stage is realized by default */
      clutter_actor_realize (CLUTTER_ACTOR (stage));
    }

  return CLUTTER_ACTOR (stage);
}

/**
 * clutter_stage_set_color:
 * @stage: A #ClutterStage
 * @color: A #ClutterColor
 *
 * Sets the stage color.
 */
void
clutter_stage_set_color (ClutterStage       *stage,
			 const ClutterColor *color)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (color != NULL);

  priv = stage->priv;

  priv->color = *color;

  clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));

  g_object_notify (G_OBJECT (stage), "color");
}

/**
 * clutter_stage_get_color:
 * @stage: A #ClutterStage
 * @color: (out caller-allocates): return location for a #ClutterColor
 *
 * Retrieves the stage color.
 */
void
clutter_stage_get_color (ClutterStage *stage,
			 ClutterColor *color)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (color != NULL);

  priv = stage->priv;

  *color = priv->color;
}

/**
 * clutter_stage_set_perspective:
 * @stage: A #ClutterStage
 * @perspective: A #ClutterPerspective
 *
 * Sets the stage perspective.
 */
void
clutter_stage_set_perspective (ClutterStage       *stage,
                               ClutterPerspective *perspective)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (perspective != NULL);
  g_return_if_fail (perspective->z_far - perspective->z_near != 0);

  priv = stage->priv;

  if (priv->perspective.fovy == perspective->fovy &&
      priv->perspective.aspect == perspective->aspect &&
      priv->perspective.z_near == perspective->z_near &&
      priv->perspective.z_far == perspective->z_far)
    return;

  priv->perspective = *perspective;

  cogl_matrix_init_identity (&priv->projection);
  cogl_matrix_perspective (&priv->projection,
                           priv->perspective.fovy,
                           priv->perspective.aspect,
                           priv->perspective.z_near,
                           priv->perspective.z_far);

  priv->dirty_projection = TRUE;
  clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
}

/**
 * clutter_stage_get_perspective:
 * @stage: A #ClutterStage
 * @perspective: (out caller-allocates) (allow-none): return location for a
 *   #ClutterPerspective
 *
 * Retrieves the stage perspective.
 */
void
clutter_stage_get_perspective (ClutterStage       *stage,
                               ClutterPerspective *perspective)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (perspective != NULL);

  *perspective = stage->priv->perspective;
}

/*
 * clutter_stage_get_projection_matrix:
 * @stage: A #ClutterStage
 * @projection: return location for a #CoglMatrix representing the
 *              perspective projection applied to actors on the given
 *              @stage.
 *
 * Retrieves the @stage's projection matrix. This is derived from the
 * current perspective set using clutter_stage_set_perspective().
 *
 * Since: 1.6
 */
void
_clutter_stage_get_projection_matrix (ClutterStage *stage,
                                      CoglMatrix *projection)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (projection != NULL);

  *projection = stage->priv->projection;
}

/* This simply provides a simple mechanism for us to ensure that
 * the projection matrix gets re-asserted before painting.
 *
 * This is used when switching between multiple stages */
void
_clutter_stage_dirty_projection (ClutterStage *stage)
{
  stage->priv->dirty_projection = TRUE;
}

/*
 * clutter_stage_set_viewport:
 * @stage: A #ClutterStage
 * @x: The X postition to render the stage at, in window coordinates
 * @y: The Y position to render the stage at, in window coordinates
 * @width: The width to render the stage at, in window coordinates
 * @height: The height to render the stage at, in window coordinates
 *
 * Sets the stage viewport. The viewport defines a final scale and
 * translation of your rendered stage and actors. This lets you render
 * your stage into a subregion of the stage window or you could use it to
 * pan a subregion of the stage if your stage window is smaller then
 * the stage. (XXX: currently this isn't possible)
 *
 * Unlike a scale and translation done using the modelview matrix this
 * is done after everything has had perspective projection applied, so
 * for example if you were to pan across a subregion of the stage using
 * the viewport then you would not see a change in perspective for the
 * actors on the stage.
 *
 * Normally the stage viewport will automatically track the size of the
 * stage window with no offset so the stage will fill your window. This
 * behaviour can be changed with the "viewport-mimics-window" property
 * which will automatically be set to FALSE if you use this API. If
 * you want to revert to the original behaviour then you should set
 * this property back to %TRUE using
 * clutter_stage_set_viewport_mimics_window().
 * (XXX: If we were to make this API public then we might want to do
 *  add that property.)
 *
 * Note: currently this interface only support integer precision
 * offsets and sizes for viewports but the interface takes floats because
 * OpenGL 4.0 has introduced floating point viewports which we might
 * want to expose via this API eventually.
 *
 * Since: 1.6
 */
void
_clutter_stage_set_viewport (ClutterStage *stage,
                             float         x,
                             float         y,
                             float         width,
                             float         height)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;


  if (x == priv->viewport[0] &&
      y == priv->viewport[1] &&
      width == priv->viewport[2] &&
      height == priv->viewport[3])
    return;

  priv->viewport[0] = x;
  priv->viewport[1] = y;
  priv->viewport[2] = width;
  priv->viewport[3] = height;

  priv->dirty_viewport = TRUE;

  queue_full_redraw (stage);
}

/* This simply provides a simple mechanism for us to ensure that
 * the viewport gets re-asserted before next painting.
 *
 * This is used when switching between multiple stages */
void
_clutter_stage_dirty_viewport (ClutterStage *stage)
{
  stage->priv->dirty_viewport = TRUE;
}

/*
 * clutter_stage_get_viewport:
 * @stage: A #ClutterStage
 * @x: A location for the X position where the stage is rendered,
 *     in window coordinates.
 * @y: A location for the Y position where the stage is rendered,
 *     in window coordinates.
 * @width: A location for the width the stage is rendered at,
 *         in window coordinates.
 * @height: A location for the height the stage is rendered at,
 *          in window coordinates.
 *
 * Returns the viewport offset and size set using
 * clutter_stage_set_viewport() or if the "viewport-mimics-window" property
 * is TRUE then @x and @y will be set to 0 and @width and @height will equal
 * the width if the stage window.
 *
 * Since: 1.6
 */
void
_clutter_stage_get_viewport (ClutterStage *stage,
                             float        *x,
                             float        *y,
                             float        *width,
                             float        *height)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  *x = priv->viewport[0];
  *y = priv->viewport[1];
  *width = priv->viewport[2];
  *height = priv->viewport[3];
}

/**
 * clutter_stage_set_fullscreen:
 * @stage: a #ClutterStage
 * @fullscreen: %TRUE to to set the stage fullscreen
 *
 * Asks to place the stage window in the fullscreen or unfullscreen
 * states.
 *
 ( Note that you shouldn't assume the window is definitely full screen
 * afterward, because other entities (e.g. the user or window manager)
 * could unfullscreen it again, and not all window managers honor
 * requests to fullscreen windows.
 *
 * If you want to receive notification of the fullscreen state you
 * should either use the #ClutterStage::fullscreen and
 * #ClutterStage::unfullscreen signals, or use the notify signal
 * for the #ClutterStage:fullscreen-set property
 *
 * Since: 1.0
 */
void
clutter_stage_set_fullscreen (ClutterStage *stage,
                              gboolean      fullscreen)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  if (priv->is_fullscreen != fullscreen)
    {
      ClutterStageWindow *impl = CLUTTER_STAGE_WINDOW (priv->impl);
      ClutterStageWindowIface *iface;

      iface = CLUTTER_STAGE_WINDOW_GET_IFACE (impl);

      /* Only set if backend implements.
       *
       * Also see clutter_stage_event() for setting priv->is_fullscreen
       * on state change event.
       */
      if (iface->set_fullscreen)
	iface->set_fullscreen (impl, fullscreen);
    }

  /* If the backend did fullscreen the stage window then we need to resize
   * the stage and update its viewport so we queue a relayout.  Note: if the
   * fullscreen request is handled asynchronously we can't rely on this
   * queue_relayout to update the viewport, but for example the X backend
   * will recieve a ConfigureNotify after a successful resize which is how
   * we ensure the viewport is updated on X.
   */
  clutter_actor_queue_relayout (CLUTTER_ACTOR (stage));
}

/**
 * clutter_stage_get_fullscreen:
 * @stage: a #ClutterStage
 *
 * Retrieves whether the stage is full screen or not
 *
 * Return value: %TRUE if the stage is full screen
 *
 * Since: 1.0
 */
gboolean
clutter_stage_get_fullscreen (ClutterStage *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);

  return stage->priv->is_fullscreen;
}

/**
 * clutter_stage_set_user_resizable:
 * @stage: a #ClutterStage
 * @resizable: whether the stage should be user resizable.
 *
 * Sets if the stage is resizable by user interaction (e.g. via
 * window manager controls)
 *
 * Since: 0.4
 */
void
clutter_stage_set_user_resizable (ClutterStage *stage,
                                  gboolean      resizable)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  if (clutter_feature_available (CLUTTER_FEATURE_STAGE_USER_RESIZE)
      && priv->is_user_resizable != resizable)
    {
      ClutterStageWindow *impl = CLUTTER_STAGE_WINDOW (priv->impl);
      ClutterStageWindowIface *iface;

      iface = CLUTTER_STAGE_WINDOW_GET_IFACE (impl);
      if (iface->set_user_resizable)
        {
          priv->is_user_resizable = resizable;

          iface->set_user_resizable (impl, resizable);

          g_object_notify (G_OBJECT (stage), "user-resizable");
        }
    }
}

/**
 * clutter_stage_get_user_resizable:
 * @stage: a #ClutterStage
 *
 * Retrieves the value set with clutter_stage_set_user_resizable().
 *
 * Return value: %TRUE if the stage is resizable by the user.
 *
 * Since: 0.4
 */
gboolean
clutter_stage_get_user_resizable (ClutterStage *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);

  return stage->priv->is_user_resizable;
}

/**
 * clutter_stage_show_cursor:
 * @stage: a #ClutterStage
 *
 * Shows the cursor on the stage window
 */
void
clutter_stage_show_cursor (ClutterStage *stage)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;
  if (!priv->is_cursor_visible)
    {
      ClutterStageWindow *impl = CLUTTER_STAGE_WINDOW (priv->impl);
      ClutterStageWindowIface *iface;

      iface = CLUTTER_STAGE_WINDOW_GET_IFACE (impl);
      if (iface->set_cursor_visible)
        {
          priv->is_cursor_visible = TRUE;

          iface->set_cursor_visible (impl, TRUE);

          g_object_notify (G_OBJECT (stage), "cursor-visible");
        }
    }
}

/**
 * clutter_stage_hide_cursor:
 * @stage: a #ClutterStage
 *
 * Makes the cursor invisible on the stage window
 *
 * Since: 0.4
 */
void
clutter_stage_hide_cursor (ClutterStage *stage)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;
  if (priv->is_cursor_visible)
    {
      ClutterStageWindow *impl = CLUTTER_STAGE_WINDOW (priv->impl);
      ClutterStageWindowIface *iface;

      iface = CLUTTER_STAGE_WINDOW_GET_IFACE (impl);
      if (iface->set_cursor_visible)
        {
          priv->is_cursor_visible = FALSE;

          iface->set_cursor_visible (impl, FALSE);

          g_object_notify (G_OBJECT (stage), "cursor-visible");
        }
    }
}

/**
 * clutter_stage_read_pixels:
 * @stage: A #ClutterStage
 * @x: x coordinate of the first pixel that is read from stage
 * @y: y coordinate of the first pixel that is read from stage
 * @width: Width dimention of pixels to be read, or -1 for the
 *   entire stage width
 * @height: Height dimention of pixels to be read, or -1 for the
 *   entire stage height
 *
 * Makes a screenshot of the stage in RGBA 8bit data, returns a
 * linear buffer with @width * 4 as rowstride.
 *
 * The alpha data contained in the returned buffer is driver-dependent,
 * and not guaranteed to hold any sensible value.
 *
 * Return value: a pointer to newly allocated memory with the buffer
 *   or %NULL if the read failed. Use g_free() on the returned data
 *   to release the resources it has allocated.
 */
guchar *
clutter_stage_read_pixels (ClutterStage *stage,
                           gint          x,
                           gint          y,
                           gint          width,
                           gint          height)
{
  ClutterGeometry geom;
  guchar *pixels;

  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), NULL);

  /* Force a redraw of the stage before reading back pixels */
  clutter_stage_ensure_current (stage);
  clutter_actor_paint (CLUTTER_ACTOR (stage));

  clutter_actor_get_allocation_geometry (CLUTTER_ACTOR (stage), &geom);

  if (width < 0)
    width = geom.width;

  if (height < 0)
    height = geom.height;

  pixels = g_malloc (height * width * 4);

  cogl_read_pixels (x, y, width, height,
                    COGL_READ_PIXELS_COLOR_BUFFER,
                    COGL_PIXEL_FORMAT_RGBA_8888,
                    pixels);

  return pixels;
}

/**
 * clutter_stage_get_actor_at_pos:
 * @stage: a #ClutterStage
 * @pick_mode: how the scene graph should be painted
 * @x: X coordinate to check
 * @y: Y coordinate to check
 *
 * Checks the scene at the coordinates @x and @y and returns a pointer
 * to the #ClutterActor at those coordinates.
 *
 * By using @pick_mode it is possible to control which actors will be
 * painted and thus available.
 *
 * Return value: (transfer none): the actor at the specified coordinates,
 *   if any
 */
ClutterActor *
clutter_stage_get_actor_at_pos (ClutterStage    *stage,
                                ClutterPickMode  pick_mode,
                                gint             x,
                                gint             y)
{
  return _clutter_do_pick (stage, x, y, pick_mode);
}

/**
 * clutter_stage_event:
 * @stage: a #ClutterStage
 * @event: a #ClutterEvent
 *
 * This function is used to emit an event on the main stage.
 *
 * You should rarely need to use this function, except for
 * synthetised events.
 *
 * Return value: the return value from the signal emission
 *
 * Since: 0.4
 */
gboolean
clutter_stage_event (ClutterStage *stage,
                     ClutterEvent *event)
{
  ClutterStagePrivate *priv;

  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);
  g_return_val_if_fail (event != NULL, FALSE);

  priv = stage->priv;

  if (event->type == CLUTTER_DELETE)
    {
      gboolean retval = FALSE;

      g_signal_emit_by_name (stage, "event", event, &retval);

      if (!retval)
        g_signal_emit_by_name (stage, "delete-event", event, &retval);

      return retval;
    }

  if (event->type != CLUTTER_STAGE_STATE)
    return FALSE;

  /* emit raw event */
  if (clutter_actor_event (CLUTTER_ACTOR (stage), event, FALSE))
    return TRUE;

  if (event->stage_state.changed_mask & CLUTTER_STAGE_STATE_FULLSCREEN)
    {
      if (event->stage_state.new_state & CLUTTER_STAGE_STATE_FULLSCREEN)
	{
	  priv->is_fullscreen = TRUE;
	  g_signal_emit (stage, stage_signals[FULLSCREEN], 0);

          g_object_notify (G_OBJECT (stage), "fullscreen-set");
	}
      else
	{
	  priv->is_fullscreen = FALSE;
	  g_signal_emit (stage, stage_signals[UNFULLSCREEN], 0);

          g_object_notify (G_OBJECT (stage), "fullscreen-set");
	}
    }

  if (event->stage_state.changed_mask & CLUTTER_STAGE_STATE_ACTIVATED)
    {
      if (event->stage_state.new_state & CLUTTER_STAGE_STATE_ACTIVATED)
	g_signal_emit (stage, stage_signals[ACTIVATE], 0);
      else
	g_signal_emit (stage, stage_signals[DEACTIVATE], 0);
    }

  return TRUE;
}

/**
 * clutter_stage_set_title
 * @stage: A #ClutterStage
 * @title: A utf8 string for the stage windows title.
 *
 * Sets the stage title.
 *
 * Since: 0.4
 **/
void
clutter_stage_set_title (ClutterStage       *stage,
			 const gchar        *title)
{
  ClutterStagePrivate *priv;
  ClutterStageWindow *impl;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  g_free (priv->title);
  priv->title = g_strdup (title);

  impl = CLUTTER_STAGE_WINDOW (priv->impl);
  if (CLUTTER_STAGE_WINDOW_GET_IFACE(impl)->set_title != NULL)
    CLUTTER_STAGE_WINDOW_GET_IFACE (impl)->set_title (impl, priv->title);

  g_object_notify (G_OBJECT (stage), "title");
}

/**
 * clutter_stage_get_title
 * @stage: A #ClutterStage
 *
 * Gets the stage title.
 *
 * Return value: pointer to the title string for the stage. The
 * returned string is owned by the actor and should not
 * be modified or freed.
 *
 * Since: 0.4
 **/
G_CONST_RETURN gchar *
clutter_stage_get_title (ClutterStage       *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), NULL);

  return stage->priv->title;
}

static void
on_key_focused_weak_notify (gpointer data,
			    GObject *where_the_object_was)
{
  ClutterStagePrivate *priv;
  ClutterStage        *stage = CLUTTER_STAGE (data);

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;
  priv->key_focused_actor = NULL;

  /* focused actor has dissapeared - fall back to stage
   * FIXME: need some kind of signal dance/block here.
  */
  clutter_stage_set_key_focus (stage, NULL);
}

/**
 * clutter_stage_set_key_focus:
 * @stage: the #ClutterStage
 * @actor: (allow-none): the actor to set key focus to, or %NULL
 *
 * Sets the key focus on @actor. An actor with key focus will receive
 * all the key events. If @actor is %NULL, the stage will receive
 * focus.
 *
 * Since: 0.6
 */
void
clutter_stage_set_key_focus (ClutterStage *stage,
			     ClutterActor *actor)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (actor == NULL || CLUTTER_IS_ACTOR (actor));

  priv = stage->priv;

  if (priv->key_focused_actor == actor)
    return;

  if (priv->key_focused_actor)
    {
      ClutterActor *old_focused_actor;

      old_focused_actor = priv->key_focused_actor;

      /* set key_focused_actor to NULL before emitting the signal or someone
       * might hide the previously focused actor in the signal handler and we'd
       * get re-entrant call and get glib critical from g_object_weak_unref
       */

      g_object_weak_unref (G_OBJECT (priv->key_focused_actor),
			   on_key_focused_weak_notify,
			   stage);

      priv->key_focused_actor = NULL;

      g_signal_emit_by_name (old_focused_actor, "key-focus-out");
    }
  else
    g_signal_emit_by_name (stage, "key-focus-out");

  /* Note, if someone changes key focus in focus-out signal handler we'd be
   * overriding the latter call below moving the focus where it was originally
   * intended. The order of events would be:
   *   1st focus-out, 2nd focus-out (on stage), 2nd focus-in, 1st focus-in
   */

  if (actor)
    {
      priv->key_focused_actor = actor;

      g_object_weak_ref (G_OBJECT (actor),
			 on_key_focused_weak_notify,
			 stage);
      g_signal_emit_by_name (priv->key_focused_actor, "key-focus-in");
    }
  else
    g_signal_emit_by_name (stage, "key-focus-in");

  g_object_notify (G_OBJECT (stage), "key-focus");
}

/**
 * clutter_stage_get_key_focus:
 * @stage: the #ClutterStage
 *
 * Retrieves the actor that is currently under key focus.
 *
 * Return value: (transfer none): the actor with key focus, or the stage
 *
 * Since: 0.6
 */
ClutterActor *
clutter_stage_get_key_focus (ClutterStage *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), NULL);

  if (stage->priv->key_focused_actor)
    return stage->priv->key_focused_actor;

  return CLUTTER_ACTOR (stage);
}

/**
 * clutter_stage_get_use_fog:
 * @stage: the #ClutterStage
 *
 * Gets whether the depth cueing effect is enabled on @stage.
 *
 * Return value: %TRUE if the depth cueing effect is enabled
 *
 * Since: 0.6
 */
gboolean
clutter_stage_get_use_fog (ClutterStage *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);

  return stage->priv->use_fog;
}

/**
 * clutter_stage_set_use_fog:
 * @stage: the #ClutterStage
 * @fog: %TRUE for enabling the depth cueing effect
 *
 * Sets whether the depth cueing effect on the stage should be enabled
 * or not.
 *
 * Depth cueing is a 3D effect that makes actors farther away from the
 * viewing point less opaque, by fading them with the stage color.

 * The parameters of the GL fog used can be changed using the
 * clutter_stage_set_fog() function.
 *
 * Since: 0.6
 */
void
clutter_stage_set_use_fog (ClutterStage *stage,
                           gboolean      fog)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  if (priv->use_fog != fog)
    {
      priv->use_fog = fog;

      CLUTTER_NOTE (MISC, "%s depth-cueing inside stage",
                    priv->use_fog ? "enabling" : "disabling");

      clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));

      g_object_notify (G_OBJECT (stage), "use-fog");
    }
}

/**
 * clutter_stage_set_fog:
 * @stage: the #ClutterStage
 * @fog: a #ClutterFog structure
 *
 * Sets the fog (also known as "depth cueing") settings for the @stage.
 *
 * A #ClutterStage will only use a linear fog progression, which
 * depends solely on the distance from the viewer. The cogl_set_fog()
 * function in COGL exposes more of the underlying implementation,
 * and allows changing the for progression function. It can be directly
 * used by disabling the #ClutterStage:use-fog property and connecting
 * a signal handler to the #ClutterActor::paint signal on the @stage,
 * like:
 *
 * |[
 *   clutter_stage_set_use_fog (stage, FALSE);
 *   g_signal_connect (stage, "paint", G_CALLBACK (on_stage_paint), NULL);
 * ]|
 *
 * The paint signal handler will call cogl_set_fog() with the
 * desired settings:
 *
 * |[
 *   static void
 *   on_stage_paint (ClutterActor *actor)
 *   {
 *     ClutterColor stage_color = { 0, };
 *     CoglColor fog_color = { 0, };
 *
 *     /&ast; set the fog color to the stage background color &ast;/
 *     clutter_stage_get_color (CLUTTER_STAGE (actor), &amp;stage_color);
 *     cogl_color_init_from_4ub (&amp;fog_color,
 *                               stage_color.red,
 *                               stage_color.green,
 *                               stage_color.blue,
 *                               stage_color.alpha);
 *
 *     /&ast; enable fog &ast;/
 *     cogl_set_fog (&amp;fog_color,
 *                   COGL_FOG_MODE_EXPONENTIAL, /&ast; mode &ast;/
 *                   0.5,                       /&ast; density &ast;/
 *                   5.0, 30.0);                /&ast; z_near and z_far &ast;/
 *   }
 * ]|
 *
 * <note>The fogging functions only work correctly when the visible actors use
 * unmultiplied alpha colors. By default Cogl will premultiply textures and
 * cogl_set_source_color() will premultiply colors, so unless you explicitly
 * load your textures requesting an unmultiplied internal format and use
 * cogl_material_set_color() you can only use fogging with fully opaque actors.
 * Support for premultiplied colors will improve in the future when we can
 * depend on fragment shaders.</note>
 *
 * Since: 0.6
 */
void
clutter_stage_set_fog (ClutterStage *stage,
                       ClutterFog   *fog)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (fog != NULL);

  priv = stage->priv;

  priv->fog = *fog;

  if (priv->use_fog)
    clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
}

/**
 * clutter_stage_get_fog:
 * @stage: the #ClutterStage
 * @fog: return location for a #ClutterFog structure
 *
 * Retrieves the current depth cueing settings from the stage.
 *
 * Since: 0.6
 */
void
clutter_stage_get_fog (ClutterStage *stage,
                        ClutterFog   *fog)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (fog != NULL);

  *fog = stage->priv->fog;
}

/*** Perspective boxed type ******/

static gpointer
clutter_perspective_copy (gpointer data)
{
  if (G_LIKELY (data))
    return g_slice_dup (ClutterPerspective, data);

  return NULL;
}

static void
clutter_perspective_free (gpointer data)
{
  if (G_LIKELY (data))
    g_slice_free (ClutterPerspective, data);
}

G_DEFINE_BOXED_TYPE (ClutterPerspective, clutter_perspective,
                     clutter_perspective_copy,
                     clutter_perspective_free);

static gpointer
clutter_fog_copy (gpointer data)
{
  if (G_LIKELY (data))
    return g_slice_dup (ClutterFog, data);

  return NULL;
}

static void
clutter_fog_free (gpointer data)
{
  if (G_LIKELY (data))
    g_slice_free (ClutterFog, data);
}

G_DEFINE_BOXED_TYPE (ClutterFog, clutter_fog, clutter_fog_copy, clutter_fog_free);

/**
 * clutter_stage_new:
 *
 * Creates a new, non-default stage. A non-default stage is a new
 * top-level actor which can be used as another container. It works
 * exactly like the default stage, but while clutter_stage_get_default()
 * will always return the same instance, you will have to keep a pointer
 * to any #ClutterStage returned by clutter_stage_new().
 *
 * The ability to support multiple stages depends on the current
 * backend. Use clutter_feature_available() and
 * %CLUTTER_FEATURE_STAGE_MULTIPLE to check at runtime whether a
 * backend supports multiple stages.
 *
 * Return value: a new stage, or %NULL if the default backend does
 *   not support multiple stages. Use clutter_actor_destroy() to
 *   programmatically close the returned stage.
 *
 * Since: 0.8
 */
ClutterActor *
clutter_stage_new (void)
{
  if (!clutter_feature_available (CLUTTER_FEATURE_STAGE_MULTIPLE))
    {
      g_warning ("Unable to create a new stage: the %s backend does not "
                 "support multiple stages.",
                 CLUTTER_FLAVOUR);
      return NULL;
    }

  /* The stage manager will grab the floating reference when the stage
     is added to it in the constructor */
  return g_object_new (CLUTTER_TYPE_STAGE, NULL);
}

/**
 * clutter_stage_ensure_current:
 * @stage: the #ClutterStage
 *
 * This function essentially makes sure the right GL context is
 * current for the passed stage. It is not intended to
 * be used by applications.
 *
 * Since: 0.8
 */
void
clutter_stage_ensure_current (ClutterStage *stage)
{
  ClutterBackend *backend;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  backend = clutter_get_default_backend ();
  _clutter_backend_ensure_context (backend, stage);
}

/**
 * clutter_stage_ensure_viewport:
 * @stage: a #ClutterStage
 *
 * Ensures that the GL viewport is updated with the current
 * stage window size.
 *
 * This function will queue a redraw of @stage.
 *
 * This function should not be called by applications; it is used
 * when embedding a #ClutterStage into a toolkit with another
 * windowing system, like GTK+.
 *
 * Since: 1.0
 */
void
clutter_stage_ensure_viewport (ClutterStage *stage)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  _clutter_stage_dirty_viewport (stage);

  clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
}

void
_clutter_stage_maybe_setup_viewport (ClutterStage *stage)
{
  ClutterStagePrivate *priv = stage->priv;

  if (priv->dirty_viewport)
    {
      CLUTTER_NOTE (PAINT,
                    "Setting up the viewport { w:%f, h:%f }",
                    priv->viewport[2], priv->viewport[3]);

      cogl_set_viewport (priv->viewport[0],
                         priv->viewport[1],
                         priv->viewport[2],
                         priv->viewport[3]);


      priv->dirty_viewport = FALSE;
    }

  if (priv->dirty_projection)
    {
      cogl_set_projection_matrix (&priv->projection);

      priv->dirty_projection = FALSE;
    }
}

/**
 * clutter_stage_ensure_redraw:
 * @stage: a #ClutterStage
 *
 * Ensures that @stage is redrawn
 *
 * This function should not be called by applications: it is
 * used when embedding a #ClutterStage into a toolkit with
 * another windowing system, like GTK+.
 *
 * Since: 1.0
 */
void
clutter_stage_ensure_redraw (ClutterStage *stage)
{
  ClutterMasterClock *master_clock;
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;
  priv->relayout_pending = TRUE;
  priv->redraw_pending = TRUE;

  master_clock = _clutter_master_clock_get_default ();
  _clutter_master_clock_start_running (master_clock);
}

/**
 * clutter_stage_queue_redraw:
 * @stage: the #ClutterStage
 *
 * Queues a redraw for the passed stage.
 *
 * <note>Applications should call clutter_actor_queue_redraw() and not
 * this function.</note>
 *
 * <note>This function is just a wrapper for clutter_actor_queue_redraw()
 * and should probably go away.</note>
 *
 * Since: 0.8
 */
void
clutter_stage_queue_redraw (ClutterStage *stage)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
}

/**
 * clutter_stage_is_default:
 * @stage: a #ClutterStage
 *
 * Checks if @stage is the default stage, or an instance created using
 * clutter_stage_new() but internally using the same implementation.
 *
 * Return value: %TRUE if the passed stage is the default one
 *
 * Since: 0.8
 */
gboolean
clutter_stage_is_default (ClutterStage *stage)
{
  ClutterStageManager *stage_manager;
  ClutterStageWindow *impl;

  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);

  stage_manager = clutter_stage_manager_get_default ();
  if (stage != clutter_stage_manager_get_default_stage (stage_manager))
    return FALSE;

  impl = _clutter_stage_get_window (stage);
  if (impl != _clutter_stage_get_default_window ())
    return FALSE;

  return TRUE;
}

void
_clutter_stage_set_window (ClutterStage       *stage,
                           ClutterStageWindow *stage_window)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail (CLUTTER_IS_STAGE_WINDOW (stage_window));

  if (stage->priv->impl)
    g_object_unref (stage->priv->impl);

  stage->priv->impl = stage_window;
}

ClutterStageWindow *
_clutter_stage_get_window (ClutterStage *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), NULL);

  return CLUTTER_STAGE_WINDOW (stage->priv->impl);
}

ClutterStageWindow *
_clutter_stage_get_default_window (void)
{
  ClutterActor *stage = clutter_stage_get_default ();

  return _clutter_stage_get_window (CLUTTER_STAGE (stage));
}

/**
 * clutter_stage_set_throttle_motion_events:
 * @stage: a #ClutterStage
 * @throttle: %TRUE to throttle motion events
 *
 * Sets whether motion events received between redraws should
 * be throttled or not. If motion events are throttled, those
 * events received by the windowing system between redraws will
 * be compressed so that only the last event will be propagated
 * to the @stage and its actors.
 *
 * This function should only be used if you want to have all
 * the motion events delivered to your application code.
 *
 * Since: 1.0
 */
void
clutter_stage_set_throttle_motion_events (ClutterStage *stage,
                                          gboolean      throttle)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  if (priv->throttle_motion_events != throttle)
    priv->throttle_motion_events = throttle;
}

/**
 * clutter_stage_get_throttle_motion_events:
 * @stage: a #ClutterStage
 *
 * Retrieves the value set with clutter_stage_set_throttle_motion_events()
 *
 * Return value: %TRUE if the motion events are being throttled,
 *   and %FALSE otherwise
 *
 * Since: 1.0
 */
gboolean
clutter_stage_get_throttle_motion_events (ClutterStage *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);

  return stage->priv->throttle_motion_events;
}

/**
 * clutter_stage_set_use_alpha:
 * @stage: a #ClutterStage
 * @use_alpha: whether the stage should honour the opacity or the
 *   alpha channel of the stage color
 *
 * Sets whether the @stage should honour the #ClutterActor:opacity and
 * the alpha channel of the #ClutterStage:color
 *
 * Since: 1.2
 */
void
clutter_stage_set_use_alpha (ClutterStage *stage,
                             gboolean      use_alpha)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;

  if (priv->use_alpha != use_alpha)
    {
      priv->use_alpha = use_alpha;

      clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));

      g_object_notify (G_OBJECT (stage), "use-alpha");
    }
}

/**
 * clutter_stage_get_use_alpha:
 * @stage: a #ClutterStage
 *
 * Retrieves the value set using clutter_stage_set_use_alpha()
 *
 * Return value: %TRUE if the stage should honour the opacity and the
 *   alpha channel of the stage color
 *
 * Since: 1.2
 */
gboolean
clutter_stage_get_use_alpha (ClutterStage *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);

  return stage->priv->use_alpha;
}

/**
 * clutter_stage_set_minimum_size:
 * @stage: a #ClutterStage
 * @width: width, in pixels
 * @height: height, in pixels
 *
 * Sets the minimum size for a stage window, if the default backend
 * uses #ClutterStage inside a window
 *
 * This is a convenience function, and it is equivalent to setting the
 * #ClutterActor:min-width and #ClutterActor:min-height on @stage
 *
 * If the current size of @stage is smaller than the minimum size, the
 * @stage will be resized to the new @width and @height
 *
 * This function has no effect if @stage is fullscreen
 *
 * Since: 1.2
 */
void
clutter_stage_set_minimum_size (ClutterStage *stage,
                                guint         width,
                                guint         height)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));
  g_return_if_fail ((width > 0) && (height > 0));

  g_object_set (G_OBJECT (stage),
                "min-width", (gfloat) width,
                "min-height", (gfloat )height,
                NULL);
}

/**
 * clutter_stage_get_minimum_size:
 * @stage: a #ClutterStage
 * @width: (out): return location for the minimum width, in pixels,
 *   or %NULL
 * @height: (out): return location for the minimum height, in pixels,
 *   or %NULL
 *
 * Retrieves the minimum size for a stage window as set using
 * clutter_stage_set_minimum_size().
 *
 * The returned size may not correspond to the actual minimum size and
 * it is specific to the #ClutterStage implementation inside the
 * Clutter backend
 *
 * Since: 1.2
 */
void
clutter_stage_get_minimum_size (ClutterStage *stage,
                                guint        *width_p,
                                guint        *height_p)
{
  gfloat width, height;
  gboolean width_set, height_set;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  g_object_get (G_OBJECT (stage),
                "min-width", &width,
                "min-width-set", &width_set,
                "min-height", &height,
                "min-height-set", &height_set,
                NULL);

  /* if not width or height have been set, then the Stage
   * minimum size is defined to be 1x1
   */
  if (!width_set)
    width = 1;

  if (!height_set)
    height = 1;

  if (width_p)
    *width_p = (guint) width;

  if (height_p)
    *height_p = (guint) height;
}

/* Returns the number of swap buffers pending completion for the stage */
int
_clutter_stage_get_pending_swaps (ClutterStage *stage)
{
  ClutterStageWindow *stage_window;

  if (CLUTTER_ACTOR_IN_DESTRUCTION (stage))
    return 0;

  stage_window = _clutter_stage_get_window (stage);
  if (stage_window == NULL)
    return 0;

  return _clutter_stage_window_get_pending_swaps (stage_window);
}

/**
 * clutter_stage_set_no_clear_hint:
 * @stage: a #ClutterStage
 * @no_clear: %TRUE if the @stage should not clear itself on every
 *   repaint cycle
 *
 * Sets whether the @stage should clear itself at the beginning
 * of each paint cycle or not.
 *
 * Clearing the #ClutterStage can be a costly operation, especially
 * if the stage is always covered - for instance, in a full-screen
 * video player or in a game with a background texture.
 *
 * <note><para>This setting is a hint; Clutter might discard this
 * hint depending on its internal state.</para></note>
 *
 * <warning><para>If parts of the stage are visible and you disable
 * clearing you might end up with visual artifacts while painting the
 * contents of the stage.</para></warning>
 *
 * Since: 1.4
 */
void
clutter_stage_set_no_clear_hint (ClutterStage *stage,
                                 gboolean      no_clear)
{
  ClutterStagePrivate *priv;
  ClutterStageHint new_hints;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  priv = stage->priv;
  new_hints = priv->stage_hints;

  if (no_clear)
    new_hints |= CLUTTER_STAGE_NO_CLEAR_ON_PAINT;
  else
    new_hints &= ~CLUTTER_STAGE_NO_CLEAR_ON_PAINT;

  if (priv->stage_hints == new_hints)
    return;

  priv->stage_hints = new_hints;

  g_object_notify (G_OBJECT (stage), "no-clear-hint");
}

/**
 * clutter_stage_get_no_clear_hint:
 * @stage: a #ClutterStage
 *
 * Retrieves the hint set with clutter_stage_set_no_clear_hint()
 *
 * Return value: %TRUE if the stage should not clear itself on every paint
 *   cycle, and %FALSE otherwise
 *
 * Since: 1.4
 */
gboolean
clutter_stage_get_no_clear_hint (ClutterStage *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);

  return (stage->priv->stage_hints & CLUTTER_STAGE_NO_CLEAR_ON_PAINT) != 0;
}

gboolean
_clutter_stage_get_pick_buffer_valid (ClutterStage *stage, ClutterPickMode mode)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), FALSE);

  if (stage->priv->pick_buffer_mode != mode)
    return FALSE;

  return stage->priv->have_valid_pick_buffer;
}

void
_clutter_stage_set_pick_buffer_valid (ClutterStage   *stage,
                                      gboolean        valid,
                                      ClutterPickMode mode)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  stage->priv->have_valid_pick_buffer = !!valid;
  stage->priv->pick_buffer_mode = mode;
}

void
_clutter_stage_increment_picks_per_frame_counter (ClutterStage *stage)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  stage->priv->picks_per_frame++;
}

void
_clutter_stage_reset_picks_per_frame_counter (ClutterStage *stage)
{
  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  stage->priv->picks_per_frame = 0;
}

guint
_clutter_stage_get_picks_per_frame_counter (ClutterStage *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), 0);

  return stage->priv->picks_per_frame;
}

ClutterPaintVolume *
_clutter_stage_paint_volume_stack_allocate (ClutterStage *stage)
{
  GArray *paint_volume_stack = stage->priv->paint_volume_stack;

  g_array_set_size (paint_volume_stack,
                    paint_volume_stack->len+1);

  return &g_array_index (paint_volume_stack,
                         ClutterPaintVolume,
                         paint_volume_stack->len - 1);
}

void
_clutter_stage_paint_volume_stack_free_all (ClutterStage *stage)
{
  GArray *paint_volume_stack = stage->priv->paint_volume_stack;
  int i;

  for (i = 0; i < paint_volume_stack->len; i++)
    {
      ClutterPaintVolume *pv =
        &g_array_index (paint_volume_stack, ClutterPaintVolume, i);
      clutter_paint_volume_free (pv);
    }

  g_array_set_size (paint_volume_stack, 0);
}

/* The is an out-of-band paramater available while painting that
 * can be used to cull actors. */
const ClutterGeometry *
_clutter_stage_get_clip (ClutterStage *stage)
{
  return stage->priv->current_paint_clip;
}

/* When an actor queues a redraw we add it to a list on the stage that
 * gets processed once all updates to the stage have been finished.
 *
 * This deferred approach to processing queue_redraw requests means
 * that we can avoid redundant transformations of clip volumes if
 * something later triggers a full stage redraw anyway. It also means
 * we can be more sure that all the referenced actors will have valid
 * allocations improving the chance that we can determine the actors
 * paint volume so we can clip the redraw request even if the user
 * didn't explicitly do so.
 */
ClutterStageQueueRedrawEntry *
_clutter_stage_queue_actor_redraw (ClutterStage *stage,
                                   ClutterStageQueueRedrawEntry *entry,
                                   ClutterActor *actor,
                                   ClutterPaintVolume *clip)
{
  ClutterStagePrivate *priv = stage->priv;

  if (!priv->redraw_pending)
    {
      ClutterMasterClock *master_clock;

      CLUTTER_NOTE (PAINT, "First redraw request");

      priv->redraw_pending = TRUE;

      master_clock = _clutter_master_clock_get_default ();
      _clutter_master_clock_start_running (master_clock);
    }
#ifdef CLUTTER_ENABLE_DEBUG
  else
    {
      CLUTTER_NOTE (PAINT, "Redraw request number %lu",
                    priv->redraw_count + 1);

      priv->redraw_count += 1;
    }
#endif /* CLUTTER_ENABLE_DEBUG */

  /* We have an optimization in _clutter_do_pick to detect when the
   * scene is static so we can cache a full, un-clipped pick buffer to
   * avoid continuous pick renders.
   *
   * Currently the assumption is that actors queue a redraw when some
   * state changes that affects painting *or* picking so we can use
   * this point to invalidate any currently cached pick buffer.
   */
  _clutter_stage_set_pick_buffer_valid (stage, FALSE, -1);

  if (entry)
    {
      /* Ignore all requests to queue a redraw for an actor if a full
       * (non-clipped) redraw of the actor has already been queued. */
      if (!entry->has_clip)
        return entry;

      /* If queuing a clipped redraw and a clipped redraw has
       * previously been queued for this actor then combine the latest
       * clip together with the existing clip */
      if (clip)
        clutter_paint_volume_union (&entry->clip, clip);
      else
        {
          clutter_paint_volume_free (&entry->clip);
          entry->has_clip = FALSE;
        }
      return entry;
    }
  else
    {
      entry = g_slice_new (ClutterStageQueueRedrawEntry);
      entry->actor = g_object_ref (actor);

      if (clip)
        {
          entry->has_clip = TRUE;
          _clutter_paint_volume_init_static (actor, &entry->clip);
          _clutter_paint_volume_set_from_volume (&entry->clip, clip);
        }
      else
        entry->has_clip = FALSE;

      stage->priv->pending_queue_redraws =
        g_list_prepend (stage->priv->pending_queue_redraws, entry);

      return entry;
    }
}

static void
free_queue_redraw_entry (ClutterStageQueueRedrawEntry *entry)
{
  if (entry->actor)
    g_object_unref (entry->actor);
  if (entry->has_clip)
    clutter_paint_volume_free (&entry->clip);
  g_slice_free (ClutterStageQueueRedrawEntry, entry);
}

void
_clutter_stage_queue_redraw_entry_invalidate (ClutterStageQueueRedrawEntry *entry)
{
  if (entry == NULL)
    return;

  if (entry->actor != NULL)
    {
      g_object_unref (entry->actor);
      entry->actor = NULL;
    }

  if (entry->has_clip)
    {
      clutter_paint_volume_free (&entry->clip);
      entry->has_clip = FALSE;
    }
}

static void
_clutter_stage_maybe_finish_queue_redraws (ClutterStage *stage)
{
  /* Note: we have to repeat until the pending_queue_redraws list is
   * empty because actors are allowed to queue redraws in response to
   * the queue-redraw signal. For example Clone actors or
   * texture_new_from_actor actors will have to queue a redraw if
   * their source queues a redraw.
   */
  while (stage->priv->pending_queue_redraws)
    {
      GList *l;
      /* XXX: we need to allow stage->priv->pending_queue_redraws to
       * be updated while we process the current entries in the list
       * so we steal the list pointer and then reset it to an empty
       * list before processing... */
      GList *stolen_list = stage->priv->pending_queue_redraws;
      stage->priv->pending_queue_redraws = NULL;

      for (l = stolen_list; l; l = l->next)
        {
          ClutterStageQueueRedrawEntry *entry = l->data;
          ClutterPaintVolume *clip;

          /* NB: Entries may be invalidated if the actor gets destroyed */
          if (G_LIKELY (entry->actor != NULL))
	    {
	      clip = entry->has_clip ? &entry->clip : NULL;

	      _clutter_actor_finish_queue_redraw (entry->actor, clip);
	    }

          free_queue_redraw_entry (entry);
        }
      g_list_free (stolen_list);
    }
}

/**
 * clutter_stage_set_accept_focus:
 * @stage: a #ClutterStage
 * @accept_focus: %TRUE to accept focus on show
 *
 * Sets whether the @stage should accept the key focus when shown.
 *
 * This function should be called before showing @stage using
 * clutter_actor_show().
 *
 * Since: 1.6
 */
void
clutter_stage_set_accept_focus (ClutterStage *stage,
                                gboolean      accept_focus)
{
  ClutterStagePrivate *priv;

  g_return_if_fail (CLUTTER_IS_STAGE (stage));

  accept_focus = !!accept_focus;

  priv = stage->priv;

  if (priv->accept_focus != accept_focus)
    {
      _clutter_stage_window_set_accept_focus (priv->impl, accept_focus);
      g_object_notify (G_OBJECT (stage), "accept-focus");
    }
}

/**
 * clutter_stage_get_accept_focus:
 * @stage: a #ClutterStage
 *
 * Retrieves the value set with clutter_stage_set_accept_focus().
 *
 * Return value: %TRUE if the #ClutterStage should accept focus, and %FALSE
 *   otherwise
 *
 * Since: 1.6
 */
gboolean
clutter_stage_get_accept_focus (ClutterStage *stage)
{
  g_return_val_if_fail (CLUTTER_IS_STAGE (stage), TRUE);

  return stage->priv->accept_focus;
}

void
_clutter_stage_add_device (ClutterStage       *stage,
                           ClutterInputDevice *device)
{
  ClutterStagePrivate *priv = stage->priv;

  if (g_hash_table_lookup (priv->devices, device) != NULL)
    return;

  g_hash_table_insert (priv->devices, device, GINT_TO_POINTER (1));
  _clutter_input_device_set_stage (device, stage);
}

void
_clutter_stage_remove_device (ClutterStage       *stage,
                              ClutterInputDevice *device)
{
  ClutterStagePrivate *priv = stage->priv;

  _clutter_input_device_set_stage (device, NULL);
  g_hash_table_remove (priv->devices, device);
}

gboolean
_clutter_stage_has_device (ClutterStage       *stage,
                           ClutterInputDevice *device)
{
  ClutterStagePrivate *priv = stage->priv;

  return g_hash_table_lookup (priv->devices, device) != NULL;
}

void
_clutter_stage_set_motion_events_enabled (ClutterStage *stage,
                                          gboolean      enabled)
{
  ClutterStagePrivate *priv = stage->priv;

  enabled = !!enabled;

  if (priv->motion_events_enabled != enabled)
    {
      priv->motion_events_enabled = enabled;
    }
}

gboolean
_clutter_stage_get_motion_events_enabled (ClutterStage *stage)
{
  return stage->priv->motion_events_enabled;
}
