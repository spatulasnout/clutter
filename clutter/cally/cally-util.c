/* CALLY - The Clutter Accessibility Implementation Library
 *
 * Copyright (C) 2008 Igalia, S.L.
 *
 * Author: Alejandro Piñeiro Iglesias <apinheiro@igalia.com>
 *
 * Based on GailUtil from GAIL
 * Copyright 2001, 2002, 2003 Sun Microsystems Inc.
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:cally-util
 * @Title: CallyUtil
 * @short_description: #AtkUtil implementation
 * @see_also: #ClutterActor
 *
 * #CallyUtil implements #AtkUtil abstract methods. Although it
 * includes the name "Util" it is in fact one of the most important
 * interfaces to be implemented in any ATK toolkit implementation.

 * For instance, it defines atk_get_root(), the method that returns
 * the root object in the hierarchy. Without it, you don't have
 * available any accessible object.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <clutter/clutter.h>

#include "cally-util.h"
#include "cally-root.h"
#include "cally-stage.h"

static void cally_util_class_init (CallyUtilClass	*klass);
static void cally_util_init       (CallyUtil *cally_util);

/* atkutil.h */

static guint                 cally_util_add_global_event_listener    (GSignalEmissionHook listener,
                                                                      const gchar*        event_type);
static void                  cally_util_remove_global_event_listener (guint remove_listener);
static guint                 cally_util_add_key_event_listener	    (AtkKeySnoopFunc listener,
                                                                     gpointer        data);
static void                  cally_util_remove_key_event_listener    (guint remove_listener);
static AtkObject*            cally_util_get_root			    (void);
static const gchar *         cally_util_get_toolkit_name		    (void);
static const gchar *         cally_util_get_toolkit_version          (void);

/* private */
static void		     _listener_info_destroy                  (gpointer data);
static guint                 add_listener	                     (GSignalEmissionHook  listener,
                                                                      const gchar         *object_type,
                                                                      const gchar         *signal,
                                                                      const gchar         *hook_data);
static void                  cally_util_simulate_snooper_install     (void);
static void                  cally_util_simulate_snooper_remove      (void);
static gboolean              cally_key_snooper                       (ClutterActor *actor,
                                                                      ClutterEvent *event,
                                                                      gpointer      user_data);
static void                  cally_util_stage_added_cb               (ClutterStageManager *stage_manager,
                                                                      ClutterStage *stage,
                                                                      gpointer data);
static void                  cally_util_stage_removed_cb             (ClutterStageManager *stage_manager,
                                                                      ClutterStage *stage,
                                                                      gpointer data);
static gboolean              notify_hf                               (gpointer key,
                                                                      gpointer value,
                                                                      gpointer data);
static void                  insert_hf                               (gpointer key,
                                                                      gpointer value,
                                                                      gpointer data);
static AtkKeyEventStruct *  atk_key_event_from_clutter_event_key     (ClutterKeyEvent *event);
static void                 do_window_event_initialization           (void);


/* This is just a copy of the Gail one, a shared library or place to
   define it could be a good idea. */
typedef struct _CallyUtilListenerInfo CallyUtilListenerInfo;
typedef struct _CallyKeyEventInfo CallyKeyEventInfo;

struct _CallyUtilListenerInfo
{
  gint key;
  guint signal_id;
  gulong hook_id;
};

struct _CallyKeyEventInfo
{
  AtkKeySnoopFunc listener;
  gpointer func_data;
};

static AtkObject* root = NULL;
static GHashTable *listener_list = NULL;
static GHashTable *key_listener_list = NULL;
static gint listener_idx = 1;


G_DEFINE_TYPE (CallyUtil, cally_util, ATK_TYPE_UTIL);

static void
cally_util_class_init (CallyUtilClass *klass)
{
  AtkUtilClass *atk_class;
  gpointer data;

  data = g_type_class_peek (ATK_TYPE_UTIL);
  atk_class = ATK_UTIL_CLASS (data);

  atk_class->add_global_event_listener    = cally_util_add_global_event_listener;
  atk_class->remove_global_event_listener = cally_util_remove_global_event_listener;
  atk_class->add_key_event_listener       = cally_util_add_key_event_listener;
  atk_class->remove_key_event_listener    = cally_util_remove_key_event_listener;
  atk_class->get_root                     = cally_util_get_root;
  atk_class->get_toolkit_name             = cally_util_get_toolkit_name;
  atk_class->get_toolkit_version          = cally_util_get_toolkit_version;

  /* FIXME: Instead of create this on the class, I think that would
     worth to implement CallyUtil as a singleton instance, so the
     class methods will access this instance. This will be a good
     future enhancement, meanwhile, just using the same *working*
     implementation used on GailUtil */

  listener_list = g_hash_table_new_full (g_int_hash, g_int_equal, NULL,
                                         _listener_info_destroy);
}

static void
cally_util_init (CallyUtil *cally_util)
{
  /* instance init: usually not required */
}

/* ------------------------------ ATK UTIL METHODS -------------------------- */

static AtkObject*
cally_util_get_root (void)
{
  if (!root)
    root = cally_root_new ();

  return root;
}

static const gchar *
cally_util_get_toolkit_name (void)
{
  return "clutter";
}

static const gchar *
cally_util_get_toolkit_version (void)
{
  return CLUTTER_VERSION_S;
}


static guint
cally_util_add_global_event_listener (GSignalEmissionHook listener,
                                      const gchar *event_type)
{
  guint rc = 0;
  gchar **split_string;

  split_string = g_strsplit (event_type, ":", 3);

  if (split_string)
    {
      if (!strcmp ("window", split_string[0]))
        {
          /* Using ClutterStage as the window equivalent, although
             several methods (move, etc) are missing. This would be
             probably defined for other window-related classes (MxWindow)

             FIXME: for this reason, this process should be extendable
             on the future.*/
          static gboolean initialized = FALSE;

          if (initialized == FALSE)
            {
              do_window_event_initialization ();
              initialized = TRUE;
            }

          rc = add_listener (listener, "CallyStage", split_string[1], event_type);
        }
      else
        {
          rc = add_listener (listener, split_string[1], split_string[2], event_type);
        }

      g_strfreev (split_string);
    }

  return rc;
}

static void
cally_util_remove_global_event_listener (guint remove_listener)
{
  if (remove_listener > 0)
    {
      CallyUtilListenerInfo *listener_info;
      gint tmp_idx = remove_listener;

      listener_info = (CallyUtilListenerInfo *)
        g_hash_table_lookup(listener_list, &tmp_idx);

      if (listener_info != NULL)
        {
          /* Hook id of 0 and signal id of 0 are invalid */
          if (listener_info->hook_id != 0 && listener_info->signal_id != 0)
            {
              /* Remove the emission hook */
              g_signal_remove_emission_hook(listener_info->signal_id,
                                            listener_info->hook_id);

              /* Remove the element from the hash */
              g_hash_table_remove(listener_list, &tmp_idx);
            }
          else
            {
              g_warning("Invalid listener hook_id %ld or signal_id %d\n",
                        listener_info->hook_id, listener_info->signal_id);
            }
        }
      else
        {
          g_warning("No listener with the specified listener id %d", 
                    remove_listener);
        }
    }
  else
    {
      g_warning("Invalid listener_id %d", remove_listener);
    }
}

static guint
cally_util_add_key_event_listener (AtkKeySnoopFunc  listener,
                                   gpointer         data)
{
  static guint key=0;
  CallyKeyEventInfo *event_info = NULL;

  if (!key_listener_list)
  {
    key_listener_list = g_hash_table_new_full (NULL, NULL, NULL, g_free);

    cally_util_simulate_snooper_install ();
  }

  event_info = g_new (CallyKeyEventInfo, 1);
  event_info->listener = listener;
  event_info->func_data = data;

  g_hash_table_insert (key_listener_list, GUINT_TO_POINTER (key++), event_info);
  /* XXX: we don't check to see if n_listeners > MAXUINT */
  return key - 1;
}

static void
cally_util_remove_key_event_listener (guint remove_listener)
{
  if (!g_hash_table_remove (key_listener_list, GUINT_TO_POINTER (remove_listener))) {
    g_warning ("Not able to remove listener with id %i", remove_listener);
  }

  if (g_hash_table_size (key_listener_list) == 0)
    {
      cally_util_simulate_snooper_remove ();
    }
}

/* ------------------------------ PRIVATE FUNCTIONS ------------------------- */

static void
_listener_info_destroy (gpointer data)
{
  g_free(data);
}

static guint
add_listener (GSignalEmissionHook listener,
              const gchar         *object_type,
              const gchar         *signal_name,
              const gchar         *hook_data)
{
  GType type;
  guint signal_id;
  gint  rc = 0;

  type = g_type_from_name (object_type);
  if (type)
    {
      signal_id  = g_signal_lookup (signal_name, type);
      if (signal_id > 0)
        {
          CallyUtilListenerInfo *listener_info;

          rc = listener_idx;

          listener_info = g_new (CallyUtilListenerInfo, 1);
          listener_info->key = listener_idx;
          listener_info->hook_id =
            g_signal_add_emission_hook (signal_id, 0, listener,
                                        g_strdup (hook_data),
                                        (GDestroyNotify) g_free);
          listener_info->signal_id = signal_id;

	  g_hash_table_insert(listener_list, &(listener_info->key), listener_info);
          listener_idx++;
        }
      else
        {
          /* This is mainly because some "window:xxx" methods not
             implemented on CallyStage */
          g_debug ("Signal type %s not supported\n", signal_name);
        }
    }
  else
    {
      g_warning("Invalid object type %s\n", object_type);
    }
  return rc;
}

/* Trying to emulate gtk_key_snooper install (a kind of wrapper). This
   could be implemented without it, but I will maintain it in this
   way, so if in the future clutter implements it natively it would be
   easier the transition */
static void
cally_util_simulate_snooper_install (void)
{
  ClutterStageManager *stage_manager = NULL;
  ClutterStage *stage = NULL;
  GSList *stage_list = NULL;
  GSList *iter = NULL;

  stage_manager = clutter_stage_manager_get_default ();
  stage_list = clutter_stage_manager_list_stages (stage_manager);

  for (iter = stage_list; iter != NULL; iter = g_slist_next (iter))
    {
      stage = CLUTTER_STAGE (iter->data);

      g_signal_connect (G_OBJECT (stage), "captured-event",
                        G_CALLBACK (cally_key_snooper), NULL);
    }

  g_signal_connect (G_OBJECT (stage_manager), "stage-added",
                    G_CALLBACK (cally_util_stage_added_cb), cally_key_snooper);
  g_signal_connect (G_OBJECT (stage_manager), "stage-removed",
                    G_CALLBACK (cally_util_stage_removed_cb), cally_key_snooper);
}

static void
cally_util_simulate_snooper_remove (void)
{
  ClutterStageManager *stage_manager = NULL;
  ClutterStage *stage = NULL;
  GSList *stage_list = NULL;
  GSList *iter = NULL;
  gint num = 0;

  stage_manager = clutter_stage_manager_get_default ();
  stage_list = clutter_stage_manager_list_stages (stage_manager);

  for (iter = stage_list; iter != NULL; iter = g_slist_next (iter))
    {
      stage = CLUTTER_STAGE (iter->data);

      num += g_signal_handlers_disconnect_by_func (stage, cally_key_snooper, NULL);
    }

  g_signal_handlers_disconnect_by_func (G_OBJECT (stage_manager),
                                        G_CALLBACK (cally_util_stage_added_cb),
                                        cally_key_snooper);

  g_signal_handlers_disconnect_by_func (G_OBJECT (stage_manager),
                                        G_CALLBACK (cally_util_stage_removed_cb),
                                        cally_key_snooper);

#ifdef CALLY_DEBUG
  g_print ("Number of snooper callbacks disconnected: %i\n", num);
#endif
}

static AtkKeyEventStruct *
atk_key_event_from_clutter_event_key (ClutterKeyEvent *clutter_event)
{
  AtkKeyEventStruct *atk_event = g_new0 (AtkKeyEventStruct, 1);
  gunichar key_unichar;

  switch (clutter_event->type)
    {
    case CLUTTER_KEY_PRESS:
      atk_event->type = ATK_KEY_EVENT_PRESS;
      break;
    case CLUTTER_KEY_RELEASE:
      atk_event->type = ATK_KEY_EVENT_RELEASE;
      break;
    default:
      g_assert_not_reached ();
      return NULL;
    }

  atk_event->state = clutter_event->modifier_state;

  /* We emit the clutter keyval. This is not exactly the one expected
     by AtkKeyEventStruct, as it expects a Gdk-like event, with the
     modifiers applied. But to avoid a dependency to gdk, we delegate
     that on the AT application.
     More information: Bug 1952 and bug 2072
  */
  atk_event->keyval = clutter_event->keyval;

  /* It is expected to store a key defining string here (ie "Space" in
     case you press a space). Anyway, there are no function on clutter
     to obtain that, and we want to avoid a gdk dependency here, so we
     delegate on the AT application to obtain that string using the
     rest of the data on the ATK event struct.

     More information: Bug 1952 and 2072
  */

  key_unichar = clutter_event_get_key_unicode ((ClutterEvent *) clutter_event);

  if (g_unichar_validate (key_unichar) && !g_unichar_iscntrl (key_unichar))
    {
      GString *new = NULL;

      new = g_string_new ("");
      new = g_string_insert_unichar (new, 0, key_unichar);
      atk_event->string = new->str;
      g_string_free (new, FALSE);
    }
  else
    atk_event->string = NULL;

  atk_event->length = 0;

  atk_event->keycode = clutter_event->hardware_keycode;
  atk_event->timestamp = clutter_event->time;

#ifdef CALLY_DEBUG

  g_debug ("CallyKeyEvent:\tsym 0x%x\n\t\tmods %x\n\t\tcode %u\n\t\ttime %lx \n\t\tstring %s\n",
	   (unsigned int) atk_event->keyval,
	   (unsigned int) atk_event->state,
	   (unsigned int) atk_event->keycode,
	   (unsigned long int) atk_event->timestamp,
           atk_event->string);
#endif

  return atk_event;
}


static gboolean
notify_hf (gpointer key, gpointer value, gpointer data)
{
  CallyKeyEventInfo *info = (CallyKeyEventInfo *) value;
  AtkKeyEventStruct *key_event = (AtkKeyEventStruct *)data;

  return (*(AtkKeySnoopFunc) info->listener) (key_event, info->func_data) ? TRUE : FALSE;
}

static void
insert_hf (gpointer key, gpointer value, gpointer data)
{
  GHashTable *new_table = (GHashTable *) data;
  g_hash_table_insert (new_table, key, value);
}

static gboolean
cally_key_snooper (ClutterActor *actor,
                   ClutterEvent *event,
                   gpointer      user_data)
{
  AtkKeyEventStruct *key_event = NULL;
  gint consumed = 0;

  /* filter key events */
  if ((event->type != CLUTTER_KEY_PRESS) && (event->type != CLUTTER_KEY_RELEASE))
    {
      return FALSE;
    }

  if (key_listener_list)
    {
      GHashTable *new_hash = g_hash_table_new (NULL, NULL);

      g_hash_table_foreach (key_listener_list, insert_hf, new_hash);
      key_event = atk_key_event_from_clutter_event_key ((ClutterKeyEvent *)event);
      /* func data is inside the hash table */
      consumed = g_hash_table_foreach_steal (new_hash, notify_hf, key_event);
      g_hash_table_destroy (new_hash);
    }

  g_free (key_event->string);
  g_free (key_event);

  return (consumed ? 1 : 0);
}

static void
cally_util_stage_added_cb (ClutterStageManager *stage_manager,
                           ClutterStage *stage,
                           gpointer data)
{
  GCallback cally_key_snooper_cb = G_CALLBACK (data);

  g_signal_connect (G_OBJECT (stage), "captured-event", cally_key_snooper_cb, NULL);
}

static void
cally_util_stage_removed_cb (ClutterStageManager *stage_manager,
                             ClutterStage *stage,
                             gpointer data)
{
  GCallback cally_key_snooper_cb = G_CALLBACK (data);

  g_signal_handlers_disconnect_by_func (stage, cally_key_snooper_cb, NULL);
}

static void
do_window_event_initialization (void)
{
  /*
   * Ensure that CallyStageClass exists.
   */
  g_type_class_unref (g_type_class_ref (CALLY_TYPE_STAGE));
}
