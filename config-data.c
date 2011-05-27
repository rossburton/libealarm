/*
 * Evolution calendar - Configuration values for the alarm notification daemon
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

#ifdef HAVE_CONFIOH
#include <config.h>
#endif

#include <string.h>
#include <libedataserver/e-source-list.h>
#include "config-data.h"

#define KEY_LAST_NOTIFICATION_TIME \
	"/apps/evolution/calendar/notify/last_notification_time"

/* Whether we have initied ourselves by reading
 * the data from the configuration engine. */
static gboolean inited = FALSE;
static GConfClient *conf_client = NULL;
static ESourceList *calendar_source_list = NULL, *tasks_source_list = NULL;

static void
do_cleanup (void)
{
	if (calendar_source_list) {
		g_object_unref (calendar_source_list);
		calendar_source_list = NULL;
	}

	if (tasks_source_list) {
		g_object_unref (tasks_source_list);
		tasks_source_list = NULL;
	}

	g_object_unref (conf_client);
	conf_client = NULL;

	inited = FALSE;
}

/* Ensures that the configuration values have been read */
static void
ensure_inited (void)
{
	if (inited)
		return;

	inited = TRUE;

	conf_client = gconf_client_get_default ();
	if (!GCONF_IS_CLIENT (conf_client)) {
		inited = FALSE;
		return;
	}

	g_atexit ((GVoidFunc) do_cleanup);

	/* load the sources for calendars and tasks */
	calendar_source_list = e_source_list_new_for_gconf (conf_client,
							    "/apps/evolution/calendar/sources");
	tasks_source_list = e_source_list_new_for_gconf (conf_client,
							 "/apps/evolution/tasks/sources");

}

ESourceList *
config_data_get_calendars (const gchar *key)
{
	ESourceList *cal_sources;
	gboolean state;
	GSList *gconf_list;

	if (!inited)
		conf_client = gconf_client_get_default ();

	gconf_list = gconf_client_get_list (conf_client,
					    key,
					    GCONF_VALUE_STRING,
					    NULL);
	cal_sources = e_source_list_new_for_gconf (conf_client, key);

	if (cal_sources && g_slist_length (gconf_list)) {
		g_slist_foreach (gconf_list, (GFunc) g_free, NULL);
		g_slist_free (gconf_list);
		return cal_sources;
	}

	state = gconf_client_get_bool (conf_client,
				      "/apps/evolution/calendar/notify/notify_with_tray",
				      NULL);
	if (!state) /* Should be old client*/ {
		GSList *source;
		gconf_client_set_bool (conf_client,
				      "/apps/evolution/calendar/notify/notify_with_tray",
				      TRUE,
				      NULL);
		source = gconf_client_get_list (conf_client,
						"/apps/evolution/calendar/sources",
						GCONF_VALUE_STRING,
						NULL);
		gconf_client_set_list (conf_client,
				       key,
				       GCONF_VALUE_STRING,
				       source,
				       NULL);
		cal_sources = e_source_list_new_for_gconf (conf_client, key);

		if (source) {
			g_slist_foreach (source, (GFunc) g_free, NULL);
			g_slist_free (source);
		}
	}

	if (gconf_list) {
		g_slist_foreach (gconf_list, (GFunc) g_free, NULL);
		g_slist_free (gconf_list);
	}

	return cal_sources;

}

GConfClient *
config_data_get_conf_client (void)
{
	ensure_inited ();
	return conf_client;
}

icaltimezone *
config_data_get_timezone (void)
{
	gchar *location;
	const gchar *key;
	icaltimezone *local_timezone;

	ensure_inited ();

	key = "/apps/evolution/calendar/display/user_system_timezone";
	if (gconf_client_get_bool (conf_client, key, NULL))
		location = e_cal_util_get_system_timezone_location ();
	else {
		key = "/apps/evolution/calendar/display/timezone";
		location = gconf_client_get_string (conf_client, key, NULL);
	}

	if (location && location[0])
		local_timezone = icaltimezone_get_builtin_timezone (location);
	else
		local_timezone = icaltimezone_get_utc_timezone ();

	g_free (location);

	return local_timezone;
}

/**
 * config_data_set_last_notification_time:
 * @t: A time value.
 *
 * Saves the last notification time so that it can be fetched the next time the
 * alarm daemon is run.  This way the daemon can show alarms that should have
 * triggered while it was not running.
 **/
void
config_data_set_last_notification_time (ECal *cal, time_t t)
{
	GConfClient *client;
	time_t current_t, now = time (NULL);

	g_return_if_fail (t != -1);

	if (cal) {
		ESource *source = e_cal_get_source (cal);
		if (source) {
			GTimeVal tmval = {0};
			gchar *as_text;

			tmval.tv_sec = (glong) t;
			as_text = g_time_val_to_iso8601 (&tmval);

			if (as_text) {
				e_source_set_property (source, "last-notified", as_text);
				g_free (as_text);
				return;
			}
		}
	}

	if (!(client = config_data_get_conf_client ()))
		return;

	/* we only store the new notification time if it is bigger
	   than the already stored one */
	current_t = gconf_client_get_int (client, KEY_LAST_NOTIFICATION_TIME, NULL);
	if (t > current_t || current_t > now)
		gconf_client_set_int (client, KEY_LAST_NOTIFICATION_TIME, t, NULL);
}

/**
 * config_data_get_last_notification_time:
 *
 * Queries the last saved value for alarm notification times.
 *
 * Return value: The last saved value, or -1 if no value had been saved before.
 **/
time_t
config_data_get_last_notification_time (ECal *cal)
{
	GConfValue *value;
	GConfClient *client;

	if (cal) {
		ESource *source = e_cal_get_source (cal);
		if (source) {
			const gchar *last_notified = e_source_get_property (source, "last-notified");
			GTimeVal tmval = {0};

			if (last_notified && *last_notified &&
				g_time_val_from_iso8601 (last_notified, &tmval)) {
				time_t now = time (NULL), val = (time_t) tmval.tv_sec;

				if (val > now)
					val = now;
				return val;
			}
		}
	}

	if (!(client = config_data_get_conf_client ()))
		return -1;

	value = gconf_client_get_without_default (client, KEY_LAST_NOTIFICATION_TIME, NULL);
	if (value) {
		time_t val = (time_t) gconf_value_get_int (value), now = time (NULL);

		if (val > now)
			val = now;

		return val;
	}

	return -1;
}
