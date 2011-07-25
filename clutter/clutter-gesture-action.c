/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Copyright (C) 2010  Intel Corporation.
 * Copyright (C) 2011  Robert Bosch Car Multimedia GmbH.
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
 * Author:
 *   Tomeu Vizoso <tomeu.vizoso@collabora.co.uk>
 */

/**
 * SECTION:clutter-gesture-action
 * @Title: ClutterGestureAction
 * @Short_Description: Action for gesture gestures
 *
 * #ClutterGestureAction is a sub-class of #ClutterAction that implements
 * the logic for recognizing gesture gestures. It listens for low level events
 * such as #ClutterButtonEvent and #ClutterMotionEvent on the stage to raise
 * the signals #ClutterGestureAction::gesture-begin, #ClutterGestureAction::gesture-motion and
 * #ClutterGestureAction::gesture-end.
 *
 * To use #ClutterGestureAction you just need to apply it to a #ClutterActor
 * using clutter_actor_add_action() and connect to the signals:
 *
 * |[
 *   ClutterAction *action = clutter_gesture_action_new ();
 *
 *   clutter_actor_add_action (actor, action);
 *
 *   g_signal_connect (action, "gesture-begin", G_CALLBACK (on_gesture_begin), NULL);
 *   g_signal_connect (action, "gesture-motion", G_CALLBACK (on_gesture_motion), NULL);
 *   g_signal_connect (action, "gesture-end", G_CALLBACK (on_gesture_end), NULL);
 * ]|
 *
 * Since: 1.8
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "clutter-gesture-action.h"

#include "clutter-debug.h"
#include "clutter-enum-types.h"
#include "clutter-marshal.h"
#include "clutter-private.h"

struct _ClutterGestureActionPrivate
{
  ClutterActor *stage;

  guint actor_capture_id;
  gulong stage_capture_id;

  gfloat press_x, press_y;
  gfloat last_motion_x, last_motion_y;
  gfloat release_x, release_y;

  guint in_drag : 1;
};

enum
{
  GESTURE_BEGIN,
  GESTURE_PROGRESS,
  GESTURE_END,
  GESTURE_CANCEL,

  LAST_SIGNAL
};

static guint gesture_signals[LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE (ClutterGestureAction, clutter_gesture_action, CLUTTER_TYPE_ACTION);

static gboolean
signal_accumulator (GSignalInvocationHint *ihint,
                    GValue                *return_accu,
                    const GValue          *handler_return,
                    gpointer               user_data)
{
  gboolean continue_emission;

  continue_emission = g_value_get_boolean (handler_return);
  g_value_set_boolean (return_accu, continue_emission);

  return continue_emission;
}

static void
cancel_gesture (ClutterGestureAction *action)
{
  ClutterGestureActionPrivate *priv = action->priv;
  ClutterActor *actor;

  priv->in_drag = FALSE;

  g_signal_handler_disconnect (priv->stage, priv->stage_capture_id);
  priv->stage_capture_id = 0;

  actor = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (action));
  g_signal_emit (action, gesture_signals[GESTURE_CANCEL], 0, actor);
}

static gboolean
stage_captured_event_cb (ClutterActor       *stage,
                         ClutterEvent       *event,
                         ClutterGestureAction *action)
{
  ClutterGestureActionPrivate *priv = action->priv;
  ClutterActor *actor;
  gboolean return_value;

  actor = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (action));

  switch (clutter_event_type (event))
    {
    case CLUTTER_MOTION:
      {
        ClutterModifierType mods = clutter_event_get_state (event);

        /* we might miss a button-release event in case of grabs,
         * so we need to check whether the button is still down
         * during a motion event
         */
        if (!(mods & CLUTTER_BUTTON1_MASK))
          {
            cancel_gesture (action);
            return FALSE;
          }

        clutter_event_get_coords (event, &priv->last_motion_x,
                                         &priv->last_motion_y);

        if (!clutter_actor_transform_stage_point (actor,
                                                  priv->last_motion_x,
                                                  priv->last_motion_y,
                                                  NULL, NULL))
          return FALSE;

        if (!priv->in_drag)
          {
            gint drag_threshold;
            ClutterSettings *settings = clutter_settings_get_default ();

            g_object_get (settings,
                          "dnd-drag-threshold", &drag_threshold,
                          NULL);

            if ((ABS (priv->press_y - priv->last_motion_y) >= drag_threshold) ||
                (ABS (priv->press_x - priv->last_motion_x) >= drag_threshold))
              {
                priv->in_drag = TRUE;

                g_signal_emit (action, gesture_signals[GESTURE_BEGIN], 0, actor,
                               &return_value);
                if (!return_value)
                  {
                    cancel_gesture (action);
                    return FALSE;
                  }
              }
            else
              return FALSE;
          }

          g_signal_emit (action, gesture_signals[GESTURE_PROGRESS], 0, actor,
                         &return_value);
          if (!return_value)
            {
              cancel_gesture (action);
              return FALSE;
            }
      }
      break;

    case CLUTTER_BUTTON_RELEASE:
      {
        clutter_event_get_coords (event, &priv->release_x, &priv->release_y);

        g_signal_handler_disconnect (priv->stage, priv->stage_capture_id);
        priv->stage_capture_id = 0;

        if (priv->in_drag)
          {
            priv->in_drag = FALSE;
            g_signal_emit (action, gesture_signals[GESTURE_END], 0, actor);
          }
      }
      break;

    default:
      break;
    }

  return FALSE;
}

static gboolean
actor_captured_event_cb (ClutterActor *actor,
                         ClutterEvent *event,
                         ClutterGestureAction *action)
{
  ClutterGestureActionPrivate *priv = action->priv;

  if (clutter_event_type (event) != CLUTTER_BUTTON_PRESS)
    return FALSE;

  if (!clutter_actor_meta_get_enabled (CLUTTER_ACTOR_META (action)))
    return FALSE;

  clutter_event_get_coords (event, &priv->press_x, &priv->press_y);

  if (priv->stage == NULL)
    priv->stage = clutter_actor_get_stage (actor);

  priv->stage_capture_id =
    g_signal_connect_after (priv->stage, "captured-event",
                            G_CALLBACK (stage_captured_event_cb),
                            action);

  return FALSE;
}

static void
clutter_gesture_action_set_actor (ClutterActorMeta *meta,
                                  ClutterActor     *actor)
{
  ClutterGestureActionPrivate *priv = CLUTTER_GESTURE_ACTION (meta)->priv;
  ClutterActorMetaClass *meta_class =
    CLUTTER_ACTOR_META_CLASS (clutter_gesture_action_parent_class);

  if (priv->actor_capture_id != 0)
    {
      ClutterActor *old_actor = clutter_actor_meta_get_actor (meta);

      g_signal_handler_disconnect (old_actor, priv->actor_capture_id);
      priv->actor_capture_id = 0;
    }

  if (priv->stage_capture_id != 0)
    {
      g_signal_handler_disconnect (priv->stage, priv->stage_capture_id);
      priv->stage_capture_id = 0;
      priv->stage = NULL;
    }

  if (actor != NULL)
    {
      priv->actor_capture_id =
        g_signal_connect (actor, "captured-event",
                          G_CALLBACK (actor_captured_event_cb),
                          meta);
    }

  meta_class->set_actor (meta, actor);
}

static gboolean
default_event_handler (ClutterGestureAction *action,
                       ClutterActor *actor)
{
  return TRUE;
}

static void
clutter_gesture_action_class_init (ClutterGestureActionClass *klass)
{
  ClutterActorMetaClass *meta_class = CLUTTER_ACTOR_META_CLASS (klass);

  g_type_class_add_private (klass, sizeof (ClutterGestureActionPrivate));

  meta_class->set_actor = clutter_gesture_action_set_actor;

  klass->gesture_begin = default_event_handler;
  klass->gesture_progress = default_event_handler;

  /**
   * ClutterGestureAction::gesture-begin:
   * @action: the #ClutterGestureAction that emitted the signal
   * @actor: the #ClutterActor attached to the @action
   *
   * The ::gesture_begin signal is emitted when the #ClutterActor to which
   * a #ClutterGestureAction has been applied starts receiving a gesture.
   *
   * Return value: %TRUE if the gesture should start, and %FALSE if
   *   the gesture should be ignored.
   *
   * Since: 1.8
   */
  gesture_signals[GESTURE_BEGIN] =
    g_signal_new (I_("gesture-begin"),
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (ClutterGestureActionClass, gesture_begin),
                  signal_accumulator, NULL,
                  _clutter_marshal_BOOLEAN__OBJECT,
                  G_TYPE_BOOLEAN, 1,
                  CLUTTER_TYPE_ACTOR);

  /**
   * ClutterGestureAction::gesture-progress:
   * @action: the #ClutterGestureAction that emitted the signal
   * @actor: the #ClutterActor attached to the @action
   *
   * The ::gesture-progress signal is emitted for each motion event after
   * the #ClutterGestureAction::gesture-begin signal has been emitted.
   *
   * Return value: %TRUE if the gesture should continue, and %FALSE if
   *   the gesture should be cancelled.
   *
   * Since: 1.8
   */
  gesture_signals[GESTURE_PROGRESS] =
    g_signal_new (I_("gesture-progress"),
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (ClutterGestureActionClass, gesture_progress),
                  signal_accumulator, NULL,
                  _clutter_marshal_BOOLEAN__OBJECT,
                  G_TYPE_BOOLEAN, 1,
                  CLUTTER_TYPE_ACTOR);

  /**
   * ClutterGestureAction::gesture-end:
   * @action: the #ClutterGestureAction that emitted the signal
   * @actor: the #ClutterActor attached to the @action
   *
   * The ::gesture-end signal is emitted at the end of the gesture gesture,
   * when the pointer's button is released
   *
   * This signal is emitted if and only if the #ClutterGestureAction::gesture-begin
   * signal has been emitted first.
   *
   * Since: 1.8
   */
  gesture_signals[GESTURE_END] =
    g_signal_new (I_("gesture-end"),
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (ClutterGestureActionClass, gesture_end),
                  NULL, NULL,
                  _clutter_marshal_VOID__OBJECT,
                  G_TYPE_NONE, 1,
                  CLUTTER_TYPE_ACTOR);

  /**
   * ClutterGestureAction::gesture-cancel:
   * @action: the #ClutterGestureAction that emitted the signal
   * @actor: the #ClutterActor attached to the @action
   *
   * The ::gesture-cancel signal is emitted when the ongoing gesture gets
   * cancelled from the #ClutterGestureAction::gesture-progress signal handler.
   *
   * This signal is emitted if and only if the #ClutterGestureAction::gesture-begin
   * signal has been emitted first.
   *
   * Since: 1.8
   */
  gesture_signals[GESTURE_CANCEL] =
    g_signal_new (I_("gesture-cancel"),
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (ClutterGestureActionClass, gesture_cancel),
                  NULL, NULL,
                  _clutter_marshal_VOID__OBJECT,
                  G_TYPE_NONE, 1,
                  CLUTTER_TYPE_ACTOR);
}

static void
clutter_gesture_action_init (ClutterGestureAction *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, CLUTTER_TYPE_GESTURE_ACTION,
                                            ClutterGestureActionPrivate);

  self->priv->press_x = self->priv->press_y = 0.f;
  self->priv->last_motion_x = self->priv->last_motion_y = 0.f;
  self->priv->release_x = self->priv->release_y = 0.f;
}

/**
 * clutter_gesture_action_new:
 *
 * Creates a new #ClutterGestureAction instance.
 *
 * Return value: the newly created #ClutterGestureAction
 *
 * Since: 1.8
 */
ClutterAction *
clutter_gesture_action_new (void)
{
  return g_object_new (CLUTTER_TYPE_GESTURE_ACTION, NULL);
}

/**
 * clutter_gesture_action_get_press_coords:
 * @action: a #ClutterGestureAction
 * @device: currently unused, set to 0
 * @press_x: (out): return location for the press event's X coordinate
 * @press_y: (out): return location for the press event's Y coordinate
 *
 * Retrieves the coordinates, in stage space, of the press event
 * that started the dragging for an specific pointer device
 *
 * Since: 1.8
 */
void
clutter_gesture_action_get_press_coords (ClutterGestureAction *action,
                                         guint                 device,
                                         gfloat               *press_x,
                                         gfloat               *press_y)
{
  g_return_if_fail (CLUTTER_IS_GESTURE_ACTION (action));

  if (device != 0)
    g_warning ("Multi-device support not yet implemented");

  if (press_x)
    *press_x = action->priv->press_x;

  if (press_y)
    *press_y = action->priv->press_y;
}

/**
 * clutter_gesture_action_get_motion_coords:
 * @action: a #ClutterGestureAction
 * @device: currently unused, set to 0
 * @motion_x: (out): return location for the latest motion
 *   event's X coordinate
 * @motion_y: (out): return location for the latest motion
 *   event's Y coordinate
 *
 * Retrieves the coordinates, in stage space, of the latest motion
 * event during the dragging
 *
 * Since: 1.8
 */
void
clutter_gesture_action_get_motion_coords (ClutterGestureAction *action,
                                          guint                 device,
                                          gfloat               *motion_x,
                                          gfloat               *motion_y)
{
  g_return_if_fail (CLUTTER_IS_GESTURE_ACTION (action));

  if (device != 0)
    g_warning ("Multi-device support not yet implemented");

  if (motion_x)
    *motion_x = action->priv->last_motion_x;

  if (motion_y)
    *motion_y = action->priv->last_motion_y;
}

/**
 * clutter_gesture_action_get_release_coords:
 * @action: a #ClutterGestureAction
 * @device: currently unused, set to 0
 * @release_x: (out): return location for the X coordinate of the last release
 * @release_y: (out): return location for the Y coordinate of the last release
 *
 * Retrieves the coordinates, in stage space, of the point where the pointer
 * device was last released.
 *
 * Since: 1.8
 */
void
clutter_gesture_action_get_release_coords (ClutterGestureAction *action,
                                           guint                 device,
                                           gfloat               *release_x,
                                           gfloat               *release_y)
{
  g_return_if_fail (CLUTTER_IS_GESTURE_ACTION (action));

  if (device != 0)
    g_warning ("Multi-device support not yet implemented");

  if (release_x)
    *release_x = action->priv->release_x;

  if (release_y)
    *release_y = action->priv->release_y;
}
