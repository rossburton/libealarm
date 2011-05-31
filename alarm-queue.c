/*
 * Alarm queueing engine
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 *
 * Authors:
 *		Federico Mena-Quintero <federico@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <glib.h>

#include <libecal/e-cal-time-util.h>
#include <libecal/e-cal-component.h>

#include "alarm.h"
#include "alarm-notify.h"
#include "alarm-queue.h"
#include "config-data.h"

#define d(x) x

static AlarmNotify *an;

/* Whether the queueing system has been initialized */
static gboolean alarm_queue_inited = FALSE;

/* Clients we are monitoring for alarms */
static GHashTable *client_alarms_hash = NULL;

/* Structure that stores a client we are monitoring */
typedef struct {
	/* Monitored client */
	ECal *client;

	/* The live query to the calendar */
	ECalView *query;

	/* Hash table of component UID -> CompQueuedAlarms.  If an element is
	 * present here, then it means its cqa->queued_alarms contains at least
	 * one queued alarm.  When all the alarms for a component have been
	 * dequeued, the CompQueuedAlarms structure is removed from the hash
	 * table.  Thus a CQA exists <=> it has queued alarms.
	 */
	GHashTable *uid_alarms_hash;
} ClientAlarms;

/* Pair of a ECalComponentAlarms and the mapping from queued alarm IDs to the
 * actual alarm instance structures.
 */
typedef struct {
	/* The parent client alarms structure */
	ClientAlarms *parent_client;

	/* The component's UID */
	ECalComponentId *id;

	/* The actual component and its alarm instances */
	ECalComponentAlarms *alarms;

	/* List of QueuedAlarm structures */
	GSList *queued_alarms;

	/* Flags */
	gboolean expecting_update;
} CompQueuedAlarms;

/* Pair of a queued alarm ID and the alarm trigger instance it refers to */
typedef struct {
	/* Alarm ID from alarm.h */
	gpointer alarm_id;

	/* Instance from our parent CompQueuedAlarms->alarms->alarms list */
	ECalComponentAlarmInstance *instance;

	/* original trigger of the instance from component */
	time_t orig_trigger;

	/* Whether this is a snoozed queued alarm or a normal one */
	guint snooze : 1;
} QueuedAlarm;

/* Alarm ID for the midnight refresh function */
static gpointer midnight_refresh_id = NULL;
static time_t midnight = 0;

static void remove_client_alarms (ClientAlarms *ca);
static void query_objects_changed_cb (ECal *client, GList *objects, gpointer data);
static void query_objects_removed_cb (ECal *client, GList *objects, gpointer data);

static void update_cqa (CompQueuedAlarms *cqa, ECalComponent *comp);
static void update_qa (ECalComponentAlarms *alarms, QueuedAlarm *qa);

/* Alarm queue engine */

static void load_alarms_for_today (ClientAlarms *ca);
static void midnight_refresh_cb (gpointer alarm_id, time_t trigger, gpointer data);

/* Simple asynchronous message dispatcher */

typedef struct _Message Message;
typedef void (*MessageFunc) (Message *msg);

struct _Message {
	MessageFunc func;
};

/*
static void
message_proxy (Message *msg)
{
	g_return_if_fail (msg->func != NULL);

	msg->func (msg);
}

static gpointer
create_thread_pool (void)
{
	return g_thread_pool_new ((GFunc) message_proxy, NULL, 1, FALSE, NULL);
}*/

static void
message_push (Message *msg)
{
	/* This used be pushed through the thread pool. This fix is made to work-around
	the crashers in dbus due to threading. The threading is not completely removed as
	its better to have alarm daemon running in a thread rather than blocking main thread.
	 This is the reason the creation of thread pool is commented out */
	msg->func (msg);
}

/*
 * use a static ring-buffer so we can call this twice
 * in a printf without getting nonsense results.
 */
G_GNUC_UNUSED static const gchar *
e_ctime (const time_t *timep)
{
  static gchar *buffer[4] = { 0, };
  static gint next = 0;
  const gchar *ret;

  g_free (buffer[next]);
  ret = buffer[next++] = g_strdup (ctime (timep));
  if (next >= G_N_ELEMENTS (buffer))
	next = 0;

  return ret;
}

/* Queues an alarm trigger for midnight so that we can load the next day's worth
 * of alarms.
 */
static void
queue_midnight_refresh (void)
{
	icaltimezone *zone;

	if (midnight_refresh_id != NULL) {
		e_alarm_remove (midnight_refresh_id);
		midnight_refresh_id = NULL;
	}

	zone = config_data_get_timezone ();
	midnight = time_day_end_with_zone (time (NULL), zone);

	d(printf("%s:%d (queue_midnight_refresh) - Refresh at %s \n",__FILE__, __LINE__, e_ctime(&midnight)));

	midnight_refresh_id = e_alarm_add (midnight, midnight_refresh_cb, NULL, NULL);
	if (!midnight_refresh_id) {
		 d(printf("%s:%d (queue_midnight_refresh) - Could not setup the midnight refresh alarm\n",__FILE__, __LINE__));
		/* FIXME: what to do? */
	}
}

/* Loads a client's alarms; called from g_hash_table_foreach() */
static void
add_client_alarms_cb (gpointer key, gpointer value, gpointer data)
{
	ClientAlarms *ca = (ClientAlarms *) value;

	d(printf("%s:%d (add_client_alarms_cb) - Adding %p\n",__FILE__, __LINE__, ca));

	load_alarms_for_today (ca);
}

struct _midnight_refresh_msg {
	Message header;
	gboolean remove;
};

/* Loads the alarms for the new day every midnight */
static void
midnight_refresh_async (struct _midnight_refresh_msg *msg)
{
	d(printf("%s:%d (midnight_refresh_async) \n",__FILE__, __LINE__));

	/* Re-load the alarms for all clients */
	g_hash_table_foreach (client_alarms_hash, add_client_alarms_cb, NULL);

	/* Re-schedule the midnight update */
	if (msg->remove && midnight_refresh_id != NULL) {
		d(printf("%s:%d (midnight_refresh_async) - Reschedule the midnight update \n",__FILE__, __LINE__));
		e_alarm_remove (midnight_refresh_id);
		midnight_refresh_id = NULL;
	}

	queue_midnight_refresh ();

	g_slice_free (struct _midnight_refresh_msg, msg);
}

static void
midnight_refresh_cb (gpointer alarm_id, time_t trigger, gpointer data)
{
	struct _midnight_refresh_msg *msg;

	msg = g_slice_new0 (struct _midnight_refresh_msg);
	msg->header.func = (MessageFunc) midnight_refresh_async;
	msg->remove = TRUE;

	message_push ((Message *) msg);
}

/* Looks up a client in the client alarms hash table */
static ClientAlarms *
lookup_client (ECal *client)
{
	return g_hash_table_lookup (client_alarms_hash, client);
}

#if 0
/* Looks up a queued alarm based on its alarm ID */
static QueuedAlarm *
lookup_queued_alarm (CompQueuedAlarms *cqa, gpointer alarm_id)
{
	GSList *l;
	QueuedAlarm *qa;

	qa = NULL;

	for (l = cqa->queued_alarms; l; l = l->next) {
		qa = l->data;
		if (qa->alarm_id == alarm_id)
			return qa;
	}

	/* not found, might have been updated/removed */
	return NULL;
}
#endif

/* Removes an alarm from the list of alarms of a component.  If the alarm was
 * the last one listed for the component, it removes the component itself.
 */
static gboolean
remove_queued_alarm (CompQueuedAlarms *cqa, gpointer alarm_id,
		     gboolean free_object, gboolean remove_alarm)
{
	QueuedAlarm *qa=NULL;
	GSList *l;

	d(printf("%s:%d (remove_queued_alarm) \n",__FILE__, __LINE__));

	for (l = cqa->queued_alarms; l; l = l->next) {
		qa = l->data;
		if (qa->alarm_id == alarm_id)
			break;
	}

	if (!l)
		return FALSE;

	cqa->queued_alarms = g_slist_delete_link (cqa->queued_alarms, l);

	if (remove_alarm) {
		cqa->expecting_update = TRUE;
		e_cal_discard_alarm (cqa->parent_client->client, cqa->alarms->comp,
				     qa->instance->auid, NULL);
		cqa->expecting_update = FALSE;
	}

	g_free (qa);

	/* If this was the last queued alarm for this component, remove the
	 * component itself.
	 */

	if (cqa->queued_alarms != NULL)
		return FALSE;

	d(printf("%s:%d (remove_queued_alarm) - Last Component. Removing CQA- Free=%d\n",__FILE__, __LINE__, free_object));
	if (free_object) {
		cqa->id = NULL;
		cqa->parent_client = NULL;
		e_cal_component_alarms_free (cqa->alarms);
		g_free (cqa);
	} else {
		e_cal_component_alarms_free (cqa->alarms);
		cqa->alarms = NULL;
	}

	return TRUE;
}

/**
 * has_known_notification:
 * Test for notification method and returns if it knows it or not.
 * @param comp Component with an alarm.
 * @param alarm_uid ID of the alarm in the comp to test.
 * @return TRUE when we know the notification type, FALSE otherwise.
 */
static gboolean
has_known_notification (ECalComponent *comp, const gchar *alarm_uid)
{
	ECalComponentAlarm *alarm;
	ECalComponentAlarmAction action;

	g_return_val_if_fail (comp != NULL, FALSE);
	g_return_val_if_fail (alarm_uid != NULL, FALSE);

	alarm = e_cal_component_get_alarm (comp, alarm_uid);
	if (!alarm)
		 return FALSE;

	e_cal_component_alarm_get_action (alarm, &action);
	e_cal_component_alarm_free (alarm);

	switch (action) {
	case E_CAL_COMPONENT_ALARM_AUDIO:
	case E_CAL_COMPONENT_ALARM_DISPLAY:
	case E_CAL_COMPONENT_ALARM_EMAIL:
	case E_CAL_COMPONENT_ALARM_PROCEDURE:
		return TRUE;
	default:
		break;
	}
	return FALSE;
}

/* Callback used when an alarm triggers */
static void
alarm_trigger_cb (gpointer alarm_id, time_t trigger, gpointer data)
{
	CompQueuedAlarms *cqa;
	ECalComponent *comp;

	cqa = data;
	comp = cqa->alarms->comp;

	config_data_set_last_notification_time (cqa->parent_client->client, trigger);
	d(printf("%s:%d (alarm_trigger_cb) - Setting Last notification time to %s\n",__FILE__, __LINE__, ctime (&trigger)));

#if 0
WHY WOULD SNOOZING MAKE A DIFFERENCE TO THE ACTION?
	qa = lookup_queued_alarm (cqa, alarm_id);
	if (!qa)
		return;

	/* Decide what to do based on the alarm action.  We use the trigger that
	 * is passed to us instead of the one from the instance structure
	 * because this may be a snoozed alarm instead of an original
	 * occurrence.
	 */

	alarm = e_cal_component_get_alarm (comp, qa->instance->auid);
	if (!alarm)
		 return;

	e_cal_component_alarm_get_action (alarm, &action);
	e_cal_component_alarm_free (alarm);
#endif

        /* Emit the signal */
        alarm_notify_emit_alarm (an, trigger, comp);

	d(printf("%s:%d (alarm_trigger_cb) - Notification sent\n",__FILE__, __LINE__));
}

/* Adds the alarms in a ECalComponentAlarms structure to the alarms queued for a
 * particular client.  Also puts the triggers in the alarm timer queue.
 */
static void
add_component_alarms (ClientAlarms *ca, ECalComponentAlarms *alarms)
{
	ECalComponentId *id;
	CompQueuedAlarms *cqa;
	GSList *l;

	/* No alarms? */
	if (alarms == NULL || alarms->alarms == NULL) {
		d(printf("%s:%d (add_component_alarms) - No alarms to add\n",__FILE__, __LINE__));
		if (alarms)
			e_cal_component_alarms_free (alarms);
		return;
	}

	cqa = g_new (CompQueuedAlarms, 1);
	cqa->parent_client = ca;
	cqa->alarms = alarms;
	cqa->expecting_update = FALSE;

	cqa->queued_alarms = NULL;
	d(printf("%s:%d (add_component_alarms) - Creating CQA %p\n",__FILE__, __LINE__, cqa));

	for (l = alarms->alarms; l; l = l->next) {
		ECalComponentAlarmInstance *instance;
		gpointer alarm_id;
		QueuedAlarm *qa;
		d(time_t tnow = time(NULL));

		instance = l->data;

		if (!has_known_notification (cqa->alarms->comp, instance->auid)) {
			g_debug ("Could not recognize alarm's notification type, discarding.");
			continue;
		}

		alarm_id = e_alarm_add (instance->trigger, alarm_trigger_cb, cqa, NULL);
		if (!alarm_id) {
			d(printf("%s:%d (add_component_alarms) - Could not schedule a trigger for %s. Discarding \n",__FILE__, __LINE__, e_ctime(&(instance->trigger))));
			continue;
		}

		qa = g_new (QueuedAlarm, 1);
		qa->alarm_id = alarm_id;
		qa->instance = instance;
		qa->orig_trigger = instance->trigger;
		qa->snooze = FALSE;

		cqa->queued_alarms = g_slist_prepend (cqa->queued_alarms, qa);
		d(printf("%s:%d (add_component_alarms) - Adding alarm %p %p at %s %s\n",__FILE__, __LINE__, qa, alarm_id, ctime (&(instance->trigger)), e_ctime(&tnow)));
	}

	id = e_cal_component_get_id (alarms->comp);

	/* If we failed to add all the alarms, then we should get rid of the cqa */
	if (cqa->queued_alarms == NULL) {
		e_cal_component_alarms_free (cqa->alarms);
		cqa->alarms = NULL;
		d(printf("%s:%d (add_component_alarms) - Failed to add all : %p\n",__FILE__, __LINE__, cqa));
		g_free (cqa);
		return;
	}

	cqa->queued_alarms = g_slist_reverse (cqa->queued_alarms);
	cqa->id = id;
	d(printf("%s:%d (add_component_alarms) - Alarm added for %s\n",__FILE__, __LINE__, id->uid));
	g_hash_table_insert (ca->uid_alarms_hash, cqa->id, cqa);
}

/* Loads the alarms of a client for a given range of time */
static void
load_alarms (ClientAlarms *ca, time_t start, time_t end)
{
	gchar *str_query, *iso_start, *iso_end;

	d(printf("%s:%d (load_alarms) \n",__FILE__, __LINE__));

	iso_start = isodate_from_time_t (start);
	if (!iso_start)
		return;

	iso_end = isodate_from_time_t (end);
	if (!iso_end) {
		g_free (iso_start);
		return;
	}

	str_query = g_strdup_printf ("(has-alarms-in-range? (make-time \"%s\") (make-time \"%s\"))",
				     iso_start, iso_end);
	g_free (iso_start);
	g_free (iso_end);

	/* create the live query */
	if (ca->query) {
		d(printf("%s:%d (load_alarms) - Disconnecting old queries \n",__FILE__, __LINE__));
		g_signal_handlers_disconnect_matched (ca->query, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, ca);
		g_object_unref (ca->query);
		ca->query = NULL;
	}

	/* FIXME: handle errors */
	if (!e_cal_get_query (ca->client, str_query, &ca->query, NULL)) {
		g_warning (G_STRLOC ": Could not get query for client");
	} else {
		d(printf("%s:%d (load_alarms) - Setting Call backs \n",__FILE__, __LINE__));

		g_signal_connect (G_OBJECT (ca->query), "objects_added",
				  G_CALLBACK (query_objects_changed_cb), ca);
		g_signal_connect (G_OBJECT (ca->query), "objects_modified",
				  G_CALLBACK (query_objects_changed_cb), ca);
		g_signal_connect (G_OBJECT (ca->query), "objects_removed",
				  G_CALLBACK (query_objects_removed_cb), ca);

		e_cal_view_start (ca->query);
	}

	g_free (str_query);
}

/* Loads today's remaining alarms for a client */
static void
load_alarms_for_today (ClientAlarms *ca)
{
	time_t now, from, day_end, day_start;
	icaltimezone *zone;

	now = time (NULL);
	zone = config_data_get_timezone ();
	day_start = time_day_begin_with_zone (now, zone);

	/* Make sure we don't miss some events from the last notification.
	 * We add 1 to the saved notification time to make the time ranges
	 * half-open; we do not want to display the "last" displayed alarm
	 * twice, once when it occurs and once when the alarm daemon restarts.
	 */
	from = MAX (config_data_get_last_notification_time (ca->client) + 1, day_start);

	day_end = time_day_end_with_zone (now, zone);
	d(printf("%s:%d (load_alarms_for_today) - From %s to %s\n",__FILE__, __LINE__,
		 g_strdup (ctime (&from)), g_strdup (e_ctime(&day_end))));
	load_alarms (ca, from, day_end);
}

/* Called when a calendar client finished loading; we load its alarms */
static void
cal_opened_cb (ECal *client, const GError *error, gpointer data)
{
	ClientAlarms *ca;

	ca = data;

	d(printf("%s:%d (cal_opened_cb) - Opened Calendar %p (Status %d%s%s%s)\n",__FILE__, __LINE__, client, error ? error->code : 0, error ? " (" : "", error ? error->message : "", error ? ")" : ""));
	if (error)
		return;

	load_alarms_for_today (ca);
}

/* Looks up a component's queued alarm structure in a client alarms structure */
static CompQueuedAlarms *
lookup_comp_queued_alarms (ClientAlarms *ca, const ECalComponentId *id)
{
	return g_hash_table_lookup (ca->uid_alarms_hash, id);
}

static void
remove_alarms (CompQueuedAlarms *cqa, gboolean free_object)
{
	GSList *l;

	d(printf("%s:%d (remove_alarms) - Removing for %p\n",__FILE__, __LINE__, cqa));
	for (l = cqa->queued_alarms; l;) {
		QueuedAlarm *qa;

		qa = l->data;

		/* Get the next element here because the list element will go
		 * away in remove_queued_alarm().  The qa will be freed there as
		 * well.
		 */
		l = l->next;

		e_alarm_remove (qa->alarm_id);
		remove_queued_alarm (cqa, qa->alarm_id, free_object, FALSE);
	}

}

/* Removes a component an its alarms */
static void
remove_comp (ClientAlarms *ca, ECalComponentId *id)
{
	CompQueuedAlarms *cqa;

	d(printf("%s:%d (remove_comp) - Removing uid %s\n",__FILE__, __LINE__, id->uid));

	if (id->rid && !(*(id->rid))) {
		g_free (id->rid);
		id->rid = NULL;
	}

	cqa = lookup_comp_queued_alarms (ca, id);
	if (!cqa)
		return;

	/* If a component is present, then it means we must have alarms queued
	 * for it.
	 */
	g_return_if_fail (cqa->queued_alarms != NULL);

	d(printf("%s:%d (remove_comp) - Removing CQA %p\n",__FILE__, __LINE__, cqa));
	remove_alarms (cqa, TRUE);
}

/* Called when a calendar component changes; we must reload its corresponding
 * alarms.
 */
struct _query_msg {
	Message header;
	GList *objects;
	gpointer data;
};

static GList *
duplicate_ical (GList *in_list)
{
	GList *l, *out_list = NULL;
	for (l = in_list; l; l = l->next) {
		out_list = g_list_prepend (out_list, icalcomponent_new_clone (l->data));
	}

	return g_list_reverse (out_list);
}

static void
query_objects_changed_async (struct _query_msg *msg)
{
	ClientAlarms *ca;
	time_t from, day_end;
	ECalComponentAlarms *alarms;
	gboolean found;
	icaltimezone *zone;
	CompQueuedAlarms *cqa;
	GList *l;
	GList *objects;

	ca = msg->data;
	objects = msg->objects;

	from = config_data_get_last_notification_time (ca->client);
	if (from == -1)
		from = time (NULL);
	else
		from += 1; /* we add 1 to make sure the alarm is not displayed twice */

	zone = config_data_get_timezone ();

	day_end = time_day_end_with_zone (time (NULL), zone);

	d(printf("%s:%d (query_objects_changed_async) - Querying for object between %s to %s\n",__FILE__, __LINE__, e_ctime(&from), e_ctime(&day_end)));

	for (l = objects; l != NULL; l = l->next) {
		ECalComponentId *id;
		GSList *sl;
		ECalComponent *comp = e_cal_component_new ();

		e_cal_component_set_icalcomponent (comp, l->data);

		id = e_cal_component_get_id (comp);
		found = e_cal_get_alarms_for_object (ca->client, id, from, day_end, &alarms);

		if (!found) {
			d(printf("%s:%d (query_objects_changed_async) - No Alarm found for client %p\n",__FILE__, __LINE__, ca->client));
#if 0
EMIT SIGNAL
			tray_list_remove_cqa (lookup_comp_queued_alarms (ca, id));
#endif
			remove_comp (ca, id);
			g_hash_table_remove (ca->uid_alarms_hash, id);
			e_cal_component_free_id (id);
			g_object_unref (comp);
			comp = NULL;
			continue;
		}

		cqa = lookup_comp_queued_alarms (ca, id);
		if (!cqa) {
			d(printf("%s:%d (query_objects_changed_async) - No currently queued alarms for %s\n",__FILE__, __LINE__, id->uid));
			add_component_alarms (ca, alarms);
			g_object_unref (comp);
			comp = NULL;
			continue;
		}

		d(printf("%s:%d (query_objects_changed_async) - Alarm Already Exist for %s\n",__FILE__, __LINE__, id->uid));
		/* if the alarms or the alarms list is empty remove it after updating the cqa structure */
		if (alarms == NULL || alarms->alarms == NULL) {

			/* update the cqa and its queued alarms for changes in summary and alarm_uid  */
			update_cqa (cqa, comp);

			if (alarms)
				e_cal_component_alarms_free (alarms);
			continue;
		}

		/* if already in the list, just update it */
		remove_alarms (cqa, FALSE);
		cqa->alarms = alarms;
		cqa->queued_alarms = NULL;

		/* add the new alarms */
		for (sl = cqa->alarms->alarms; sl; sl = sl->next) {
			ECalComponentAlarmInstance *instance;
			gpointer alarm_id;
			QueuedAlarm *qa;

			instance = sl->data;

			if (!has_known_notification (cqa->alarms->comp, instance->auid)) {
				g_debug ("Could not recognize alarm's notification type, discarding.");
				continue;
			}

			alarm_id = e_alarm_add (instance->trigger, alarm_trigger_cb, cqa, NULL);
			if (!alarm_id) {
				d(printf("%s:%d (query_objects_changed_async) -Unable to schedule trigger for %s \n",__FILE__, __LINE__, e_ctime(&(instance->trigger))));
				continue;
			}

			qa = g_new (QueuedAlarm, 1);
			qa->alarm_id = alarm_id;
			qa->instance = instance;
			qa->snooze = FALSE;
			qa->orig_trigger = instance->trigger;
			cqa->queued_alarms = g_slist_prepend (cqa->queued_alarms, qa);
			d(printf("%s:%d (query_objects_changed_async) - Adding %p to queue \n",__FILE__, __LINE__, qa));
		}

		cqa->queued_alarms = g_slist_reverse (cqa->queued_alarms);
		g_object_unref (comp);
		comp = NULL;
	}
	g_list_free (objects);

	g_slice_free (struct _query_msg, msg);
}

static void
query_objects_changed_cb (ECal *client, GList *objects, gpointer data)
{
	struct _query_msg *msg;

	msg = g_slice_new0 (struct _query_msg);
	msg->header.func = (MessageFunc) query_objects_changed_async;
	msg->objects = duplicate_ical (objects);
	msg->data = data;

	message_push ((Message *) msg);
}

/* Called when a calendar component is removed; we must delete its corresponding
 * alarms.
 */
static void
query_objects_removed_cb (ECal *client, GList *objects, gpointer data)
{
	ClientAlarms *ca;
	GList *l;

	ca = data;

	d(printf("%s:%d (query_objects_removed_cb) - Removing %d objects\n",__FILE__, __LINE__, g_list_length (objects)));

	for (l = objects; l != NULL; l = l->next) {
		/* If the alarm is already triggered remove it. */
#if 0
EMIT SIGNAL
		tray_list_remove_cqa (lookup_comp_queued_alarms (ca, l->data));
#endif
		remove_comp (ca, l->data);
		g_hash_table_remove (ca->uid_alarms_hash, l->data);
	}
}

static gboolean
check_midnight_refresh (gpointer user_data)
{
	time_t new_midnight;
	icaltimezone *zone;

	d(printf("%s:%d (check_midnight_refresh)\n",__FILE__, __LINE__));

	zone = config_data_get_timezone ();
	new_midnight = time_day_end_with_zone (time (NULL), zone);

	if (new_midnight > midnight) {
		struct _midnight_refresh_msg *msg;

		msg = g_slice_new0 (struct _midnight_refresh_msg);
		msg->header.func = (MessageFunc) midnight_refresh_async;
		msg->remove = FALSE;

		message_push ((Message *) msg);
	}

	return TRUE;
}

/**
 * alarm_queue_init:
 *
 * Initializes the alarm queueing system.  This should be called near the
 * beginning of the program.
 **/
void
alarm_queue_init (gpointer notify)
{
	g_return_if_fail (alarm_queue_inited == FALSE);

	an = notify;

	d(printf("%s:%d (alarm_queue_init)\n",__FILE__, __LINE__));

	client_alarms_hash = g_hash_table_new (g_direct_hash, g_direct_equal);
	queue_midnight_refresh ();

	if (config_data_get_last_notification_time (NULL) == -1) {
		time_t tmval = time_day_begin (time (NULL));
		d(printf("%s:%d (alarm_queue_init) - Setting last notification time to %s\n",__FILE__, __LINE__, e_ctime(&tmval)));
		config_data_set_last_notification_time (NULL, tmval);
	}

	/* install timeout handler (every 30 mins) for not missing the midnight refresh */
	g_timeout_add_seconds (1800, (GSourceFunc) check_midnight_refresh, NULL);

	alarm_queue_inited = TRUE;
}

static gboolean
free_client_alarms_cb (gpointer key, gpointer value, gpointer user_data)
{
	ClientAlarms *ca = value;

	d(printf("%s:%d (free_client_alarms_cb) - %p\n",__FILE__, __LINE__, ca));

	if (ca) {
		remove_client_alarms (ca);
		if (ca->client) {
			d(printf("%s:%d (free_client_alarms_cb) - Disconnecting Client \n",__FILE__, __LINE__));

			g_signal_handlers_disconnect_matched (ca->client, G_SIGNAL_MATCH_DATA,
							      0, 0, NULL, NULL, ca);
			g_object_unref (ca->client);
		}

		if (ca->query) {
			d(printf("%s:%d (free_client_alarms_cb) - Disconnecting Query \n",__FILE__, __LINE__));

			g_signal_handlers_disconnect_matched (ca->query, G_SIGNAL_MATCH_DATA,
							      0, 0, NULL, NULL, ca);
			g_object_unref (ca->query);
		}

		g_hash_table_destroy (ca->uid_alarms_hash);

		g_free (ca);
		return TRUE;
	}

	return FALSE;
}

/**
 * alarm_queue_done:
 *
 * Shuts down the alarm queueing system.  This should be called near the end
 * of the program.  All the monitored calendar clients should already have been
 * unregistered with alarm_queue_remove_client().
 **/
void
alarm_queue_done (void)
{
	g_return_if_fail (alarm_queue_inited);

	/* All clients must be unregistered by now */
	g_return_if_fail (g_hash_table_size (client_alarms_hash) == 0);

	d(printf("%s:%d (alarm_queue_done)\n",__FILE__, __LINE__));

	g_hash_table_foreach_remove (client_alarms_hash, (GHRFunc) free_client_alarms_cb, NULL);
	g_hash_table_destroy (client_alarms_hash);
	client_alarms_hash = NULL;

	if (midnight_refresh_id != NULL) {
		e_alarm_remove (midnight_refresh_id);
		midnight_refresh_id = NULL;
	}

	alarm_queue_inited = FALSE;
}

static gboolean
compare_ids (gpointer a, gpointer b)
{
	ECalComponentId *id, *id1;

	id = a;
	id1 = b;
	if (id->uid != NULL && id1->uid != NULL) {
		if (g_str_equal (id->uid, id1->uid)) {

			if (id->rid && id1->rid)
				return g_str_equal (id->rid, id1->rid);
			else if (!(id->rid && id1->rid))
				return TRUE;
		}
	}
	return FALSE;
}

static guint
hash_ids (gpointer a)
{
	ECalComponentId *id =a;

	return g_str_hash (id->uid);
}

struct _alarm_client_msg {
	Message header;
	ECal *client;
};

static void
alarm_queue_add_async (struct _alarm_client_msg *msg)
{
	ClientAlarms *ca;
	ECal *client = msg->client;

	g_return_if_fail (alarm_queue_inited);
	g_return_if_fail (client != NULL);
	g_return_if_fail (E_IS_CAL (client));

	ca = lookup_client (client);
	if (ca) {
		/* We already have it. Unref the passed one*/
		g_object_unref(client);
		return;
	}

	d(printf("%s:%d (alarm_queue_add_async) - %p\n",__FILE__, __LINE__, client));

	ca = g_new (ClientAlarms, 1);

	ca->client = client;
	ca->query = NULL;

	g_hash_table_insert (client_alarms_hash, client, ca);

	ca->uid_alarms_hash = g_hash_table_new ((GHashFunc) hash_ids, (GEqualFunc) compare_ids);

	if (e_cal_get_load_state (client) == E_CAL_LOAD_LOADED) {
		load_alarms_for_today (ca);
	} else {
		g_signal_connect (client, "cal_opened_ex",
				  G_CALLBACK (cal_opened_cb),
				  ca);
	}

	g_slice_free (struct _alarm_client_msg, msg);
}

/**
 * alarm_queue_add_client:
 * @client: A calendar client.
 *
 * Adds a calendar client to the alarm queueing system.  Alarm trigger
 * notifications will be presented at the appropriate times.  The client should
 * be removed with alarm_queue_remove_client() when receiving notifications
 * from it is no longer desired.
 *
 * A client can be added any number of times to the alarm queueing system,
 * but any single alarm trigger will only be presented once for a particular
 * client.  The client must still be removed the same number of times from the
 * queueing system when it is no longer wanted.
 **/
void
alarm_queue_add_client (ECal *client)
{
	struct _alarm_client_msg *msg;

	msg = g_slice_new0 (struct _alarm_client_msg);
	msg->header.func = (MessageFunc) alarm_queue_add_async;
	msg->client = g_object_ref (client);

	message_push ((Message *) msg);
}

/* Removes a component an its alarms */
static void
remove_cqa (ClientAlarms *ca, ECalComponentId *id, CompQueuedAlarms *cqa)
{

	/* If a component is present, then it means we must have alarms queued
	 * for it.
	 */
	g_return_if_fail (cqa->queued_alarms != NULL);

	d(printf("%s:%d (remove_cqa) - removing %d alarms\n",__FILE__, __LINE__, g_slist_length(cqa->queued_alarms)));
	remove_alarms (cqa, TRUE);
}

static gboolean
remove_comp_by_id (gpointer key, gpointer value, gpointer userdata) {

	ClientAlarms *ca = (ClientAlarms *)userdata;

	d(printf("%s:%d (remove_comp_by_id)\n",__FILE__, __LINE__));

/*	if (!g_hash_table_size (ca->uid_alarms_hash)) */
/*		return; */

	remove_cqa (ca, (ECalComponentId *)key, (CompQueuedAlarms *) value);

	return TRUE;
}

/* Removes all the alarms queued for a particular calendar client */
static void
remove_client_alarms (ClientAlarms *ca)
{
	d(printf("%s:%d (remove_client_alarms) - size %d \n",__FILE__, __LINE__, g_hash_table_size (ca->uid_alarms_hash)));

	g_hash_table_foreach_remove  (ca->uid_alarms_hash, (GHRFunc)remove_comp_by_id, ca);

	/* The hash table should be empty now */
	g_return_if_fail (g_hash_table_size (ca->uid_alarms_hash) == 0);
}

/**
 * alarm_queue_remove_client:
 * @client: A calendar client.
 *
 * Removes a calendar client from the alarm queueing system.
 **/
static void
alarm_queue_remove_async (struct _alarm_client_msg *msg)
{
	ClientAlarms *ca;
	ECal *client = msg->client;

	g_return_if_fail (alarm_queue_inited);
	g_return_if_fail (client != NULL);
	g_return_if_fail (E_IS_CAL (client));

	ca = lookup_client (client);
	g_return_if_fail (ca != NULL);

	d(printf("%s:%d (alarm_queue_remove_async) \n",__FILE__, __LINE__));
	remove_client_alarms (ca);

	/* Clean up */
	if (ca->client) {
		d(printf("%s:%d (alarm_queue_remove_async) - Disconnecting Client \n",__FILE__, __LINE__));

		g_signal_handlers_disconnect_matched (ca->client, G_SIGNAL_MATCH_DATA,
						      0, 0, NULL, NULL, ca);
		g_object_unref (ca->client);
		ca->client = NULL;
	}

	if (ca->query) {
		d(printf("%s:%d (alarm_queue_remove_async) - Disconnecting Query \n",__FILE__, __LINE__));

		g_signal_handlers_disconnect_matched (ca->query, G_SIGNAL_MATCH_DATA,
						      0, 0, NULL, NULL, ca);
		g_object_unref (ca->query);
		ca->query = NULL;
	}

	g_hash_table_destroy (ca->uid_alarms_hash);
	ca->uid_alarms_hash = NULL;

	g_free (ca);

	g_hash_table_remove (client_alarms_hash, client);

	g_slice_free (struct _alarm_client_msg, msg);
}

/** alarm_queue_remove_client
 *
 * asynchronously remove client from alarm queue.
 * @param client Client to remove.
 * @param immediately Indicates whether use thread or do it right now.
 */

void
alarm_queue_remove_client (ECal *client, gboolean immediately)
{
	struct _alarm_client_msg *msg;

	msg = g_slice_new0 (struct _alarm_client_msg);
	msg->header.func = (MessageFunc) alarm_queue_remove_async;
	msg->client = client;

	if (immediately) {
		alarm_queue_remove_async (msg);
	} else
		message_push ((Message *) msg);
}

/* Update non-time related variables for various structures on modification of an existing component
   to be called only from query_objects_changed_cb */
static void
update_cqa (CompQueuedAlarms *cqa, ECalComponent *newcomp)
{
	ECalComponent *oldcomp;
	ECalComponentAlarms *alarms = NULL;
	GSList *qa_list;	/* List of current QueuedAlarms corresponding to cqa */
	time_t from, to;
	icaltimezone *zone;
	ECalComponentAlarmAction omit[] = {-1};

	oldcomp = cqa->alarms->comp;

	zone = config_data_get_timezone ();
	from = time_day_begin_with_zone (time (NULL), zone);
	to = time_day_end_with_zone (time (NULL), zone);

	d(printf("%s:%d (update_cqa) - Generating alarms between %s and %s\n",__FILE__, __LINE__, e_ctime(&from), e_ctime(&to)));
	alarms = e_cal_util_generate_alarms_for_comp (newcomp, from, to, omit,
					e_cal_resolve_tzid_cb, cqa->parent_client->client, zone);

	/* Update auids in Queued Alarms*/
	for (qa_list = cqa->queued_alarms; qa_list; qa_list = qa_list->next) {
		QueuedAlarm *qa = qa_list->data;
		gchar *check_auid = (gchar *) qa->instance->auid;
		ECalComponentAlarm *alarm;

		alarm = e_cal_component_get_alarm (newcomp, check_auid);
		if (alarm) {
			e_cal_component_alarm_free (alarm);
			continue;
		} else {
			alarm = e_cal_component_get_alarm (oldcomp, check_auid);
			if (alarm) { /* Need to update QueuedAlarms */
				e_cal_component_alarm_free (alarm);
				if (alarms == NULL) {
					d(printf("%s:%d (update_cqa) - No alarms found in the modified component\n",__FILE__, __LINE__));
					break;
				}
				update_qa (alarms, qa);
			}
			else
				g_warning ("Failed in auid lookup for old component also\n");
		}
	}

	/* Update the actual component stored in CompQueuedAlarms structure */
	g_object_unref (cqa->alarms->comp);
	cqa->alarms->comp = newcomp;
	if (alarms != NULL )
		e_cal_component_alarms_free (alarms);
}

static void
update_qa (ECalComponentAlarms *alarms, QueuedAlarm *qa)
{
	ECalComponentAlarmInstance *al_inst;
	GSList *instance_list;

	d(printf("%s:%d (update_qa)\n",__FILE__, __LINE__));
	for (instance_list = alarms->alarms; instance_list; instance_list = instance_list->next) {
		al_inst = instance_list->data;
		if (al_inst->trigger == qa->orig_trigger) {  /* FIXME if two or more alarm instances (audio, note)									  for same component have same trigger */
			g_free ((gchar *) qa->instance->auid);
			qa->instance->auid = g_strdup (al_inst->auid);
			break;
		}
	}
}


#if 0
EXPOSE API FOR THIS
/* Creates a snooze alarm based on an existing one.  The snooze offset is
 * compued with respect to the current time.
 */
static void
create_snooze (CompQueuedAlarms *cqa, gpointer alarm_id, gint snooze_mins)
{
	QueuedAlarm *orig_qa;
	time_t t;
	gpointer new_id;

	orig_qa = lookup_queued_alarm (cqa, alarm_id);
	if (!orig_qa)
		return;

	t = time (NULL);
	t += snooze_mins * 60;

	new_id = e_alarm_add (t, alarm_trigger_cb, cqa, NULL);
	if (!new_id) {
		d(printf("%s:%d (create_snooze) -Unable to schedule trigger for %s \n",__FILE__, __LINE__, e_ctime(&t)));
		return;
	}

	orig_qa->instance->trigger = t;
	orig_qa->alarm_id = new_id;
	orig_qa->snooze = TRUE;
	d(printf("%s:%d (create_snooze) - Adding a alarm at %s\n",__FILE__, __LINE__, e_ctime(&t)));
}
#endif
