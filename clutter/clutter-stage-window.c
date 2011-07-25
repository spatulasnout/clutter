#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib-object.h>

#include "clutter-actor.h"
#include "clutter-stage-window.h"
#include "clutter-private.h"

typedef ClutterStageWindowIface ClutterStageWindowInterface;

G_DEFINE_INTERFACE (ClutterStageWindow, clutter_stage_window, G_TYPE_OBJECT);

static void
clutter_stage_window_default_init (ClutterStageWindowInterface *iface)
{
}

ClutterActor *
_clutter_stage_window_get_wrapper (ClutterStageWindow *window)
{
  return CLUTTER_STAGE_WINDOW_GET_IFACE (window)->get_wrapper (window);
}

void
_clutter_stage_window_set_title (ClutterStageWindow *window,
                                 const gchar        *title)
{
  ClutterStageWindowIface *iface = CLUTTER_STAGE_WINDOW_GET_IFACE (window);

  if (iface->set_title)
    iface->set_title (window, title);
}

void
_clutter_stage_window_set_fullscreen (ClutterStageWindow *window,
                                      gboolean            is_fullscreen)
{
  ClutterStageWindowIface *iface = CLUTTER_STAGE_WINDOW_GET_IFACE (window);

  if (iface->set_fullscreen)
    iface->set_fullscreen (window, is_fullscreen);
}

void
_clutter_stage_window_set_cursor_visible (ClutterStageWindow *window,
                                          gboolean            is_visible)
{
  ClutterStageWindowIface *iface = CLUTTER_STAGE_WINDOW_GET_IFACE (window);

  if (iface->set_cursor_visible)
    iface->set_cursor_visible (window, is_visible);
}

void
_clutter_stage_window_set_user_resizable (ClutterStageWindow *window,
                                          gboolean            is_resizable)
{
  CLUTTER_STAGE_WINDOW_GET_IFACE (window)->set_user_resizable (window,
                                                               is_resizable);
}

gboolean
_clutter_stage_window_realize (ClutterStageWindow *window)
{
  return CLUTTER_STAGE_WINDOW_GET_IFACE (window)->realize (window);
}

void
_clutter_stage_window_unrealize (ClutterStageWindow *window)
{
  CLUTTER_STAGE_WINDOW_GET_IFACE (window)->unrealize (window);
}

void
_clutter_stage_window_show (ClutterStageWindow *window,
                            gboolean            do_raise)
{
  CLUTTER_STAGE_WINDOW_GET_IFACE (window)->show (window, do_raise);
}

void
_clutter_stage_window_hide (ClutterStageWindow *window)
{
  CLUTTER_STAGE_WINDOW_GET_IFACE (window)->hide (window);
}

void
_clutter_stage_window_resize (ClutterStageWindow *window,
                              gint                width,
                              gint                height)
{
  CLUTTER_STAGE_WINDOW_GET_IFACE (window)->resize (window, width, height);
}

void
_clutter_stage_window_get_geometry (ClutterStageWindow *window,
                                    ClutterGeometry    *geometry)
{
  CLUTTER_STAGE_WINDOW_GET_IFACE (window)->get_geometry (window, geometry);
}

int
_clutter_stage_window_get_pending_swaps (ClutterStageWindow *window)
{
  ClutterStageWindowIface *iface;

  g_return_val_if_fail (CLUTTER_IS_STAGE_WINDOW (window), 0);

  iface = CLUTTER_STAGE_WINDOW_GET_IFACE (window);
  if (iface->get_pending_swaps == NULL)
    {
      g_assert (!clutter_feature_available (CLUTTER_FEATURE_SWAP_EVENTS));
      return 0;
    }

  return iface->get_pending_swaps (window);
}

void
_clutter_stage_window_add_redraw_clip (ClutterStageWindow *window,
                                       ClutterGeometry    *stage_clip)
{
  ClutterStageWindowIface *iface;

  g_return_if_fail (CLUTTER_IS_STAGE_WINDOW (window));

  iface = CLUTTER_STAGE_WINDOW_GET_IFACE (window);
  if (iface->add_redraw_clip)
    iface->add_redraw_clip (window, stage_clip);
}

/* Determines if the backend will clip the rendering of the next
 * frame.
 *
 * Note: at the start of each new frame there is an implied clip that
 * clips everything (i.e. nothing would be drawn) so this function
 * will return True at the start of a new frame if the backend
 * supports clipped redraws.
 */
gboolean
_clutter_stage_window_has_redraw_clips (ClutterStageWindow *window)
{
  ClutterStageWindowIface *iface;

  g_return_val_if_fail (CLUTTER_IS_STAGE_WINDOW (window), FALSE);

  iface = CLUTTER_STAGE_WINDOW_GET_IFACE (window);
  if (iface->has_redraw_clips)
    return iface->has_redraw_clips (window);

  return FALSE;
}

/* Determines if the backend will discard any additional redraw clips
 * and instead promote them to a full stage redraw.
 *
 * The ideas is that backend may have some heuristics that cause it to
 * give up tracking redraw clips so this can be used to avoid the cost
 * of calculating a redraw clip when we know it's going to be ignored
 * anyway.
 */
gboolean
_clutter_stage_window_ignoring_redraw_clips (ClutterStageWindow *window)
{
  ClutterStageWindowIface *iface;

  g_return_val_if_fail (CLUTTER_IS_STAGE_WINDOW (window), FALSE);

  iface = CLUTTER_STAGE_WINDOW_GET_IFACE (window);
  if (iface->ignoring_redraw_clips)
    return iface->ignoring_redraw_clips (window);

  return TRUE;
}

gboolean
_clutter_stage_window_get_redraw_clip_bounds (ClutterStageWindow    *window,
                                              cairo_rectangle_int_t *stage_clip)
{
  ClutterStageWindowIface *iface;

  g_return_val_if_fail (CLUTTER_IS_STAGE_WINDOW (window), FALSE);

  iface = CLUTTER_STAGE_WINDOW_GET_IFACE (window);
  if (iface->get_redraw_clip_bounds)
    return iface->get_redraw_clip_bounds (window, stage_clip);

  return FALSE;
}

void
_clutter_stage_window_set_accept_focus (ClutterStageWindow *window,
                                        gboolean            accept_focus)
{
  ClutterStageWindowIface *iface;

  g_return_if_fail (CLUTTER_IS_STAGE_WINDOW (window));

  iface = CLUTTER_STAGE_WINDOW_GET_IFACE (window);
  if (iface->set_accept_focus)
    iface->set_accept_focus (window, accept_focus);
}

void
_clutter_stage_window_redraw (ClutterStageWindow *window)
{
  ClutterStageWindowIface *iface;

  g_return_if_fail (CLUTTER_IS_STAGE_WINDOW (window));

  iface = CLUTTER_STAGE_WINDOW_GET_IFACE (window);
  if (iface->redraw)
    iface->redraw (window);
}

/* NB: The presumption shouldn't be that a stage can't be comprised of
 * multiple internal framebuffers, so instead of simply naming this
 * function _clutter_stage_window_get_framebuffer(), the "active"
 * infix is intended to clarify that it gets the framebuffer that is
 * currently in use/being painted.
 */
CoglFramebuffer *
_clutter_stage_window_get_active_framebuffer (ClutterStageWindow *window)
{
  ClutterStageWindowIface *iface;

  g_return_val_if_fail (CLUTTER_IS_STAGE_WINDOW (window), NULL);

  iface = CLUTTER_STAGE_WINDOW_GET_IFACE (window);
  if (iface->get_active_framebuffer)
    return iface->get_active_framebuffer (window);
  else
    return NULL;
}
