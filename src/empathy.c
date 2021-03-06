/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2007-2008 Collabora Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 * 
 * Authors: Xavier Claessens <xclaesse@gmail.com>
 */

#include <config.h>

#include <stdlib.h>
#include <errno.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <libebook/e-book.h>

#include <telepathy-glib/util.h>
#include <libmissioncontrol/mc-account.h>
#include <libmissioncontrol/mission-control.h>

#include <libempathy/empathy-idle.h>
#include <libempathy/empathy-utils.h>
#include <libempathy/empathy-dispatcher.h>
#include <libempathy/empathy-tp-chat.h>
#include <libempathy/empathy-tp-group.h>

#include <libempathy-gtk/empathy-conf.h>

#include "empathy-main-window.h"
#include "empathy-status-icon.h"
#include "empathy-call-window.h"
#include "empathy-chat-window.h"
#include "bacon-message-connection.h"

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include <libempathy/empathy-debug.h>

static BaconMessageConnection *connection = NULL;

static void
dispatch_channel_cb (EmpathyDispatcher *dispatcher,
		     TpChannel         *channel,
		     gpointer           user_data)
{
	gchar *channel_type;

	g_object_get (channel, "channel-type", &channel_type, NULL);
	if (!tp_strdiff (channel_type, TP_IFACE_CHANNEL_TYPE_TEXT)) {
		EmpathyTpChat *tp_chat;
		EmpathyChat   *chat = NULL;
		const gchar   *id;

		tp_chat = empathy_tp_chat_new (channel);
		empathy_run_until_ready (tp_chat);

		id = empathy_tp_chat_get_id (tp_chat);
		if (!id) {
			EmpathyContact *contact;

			contact = empathy_tp_chat_get_remote_contact (tp_chat);
			if (contact) {
				id = empathy_contact_get_id (contact);
			}
		}

		if (id) {
			McAccount *account;

			account = empathy_tp_chat_get_account (tp_chat);
			chat = empathy_chat_window_find_chat (account, id);
		}

		if (chat) {
			empathy_chat_set_tp_chat (chat, tp_chat);
		} else {
			chat = empathy_chat_new (tp_chat);
		}

		empathy_chat_window_present_chat (chat);
		g_object_unref (tp_chat);
	}
	else if (!tp_strdiff (channel_type, TP_IFACE_CHANNEL_TYPE_STREAMED_MEDIA)) {
		empathy_call_window_new (channel);
	}
}

static void
service_ended_cb (MissionControl *mc,
		  gpointer        user_data)
{
	DEBUG ("Mission Control stopped");
}

static void
operation_error_cb (MissionControl *mc,
		    guint           operation_id,
		    guint           error_code,
		    gpointer        user_data)
{
	const gchar *message;

	switch (error_code) {
	case MC_DISCONNECTED_ERROR:
		message = _("Disconnected");
		break;
	case MC_INVALID_HANDLE_ERROR:
		message = _("Invalid handle");
		break;
	case MC_NO_MATCHING_CONNECTION_ERROR:
		message = _("No matching connection");
		break;
	case MC_INVALID_ACCOUNT_ERROR:
		message = _("Invalid account");
		break;
	case MC_PRESENCE_FAILURE_ERROR:
		message = _("Presence failure");
		break;
	case MC_NO_ACCOUNTS_ERROR:
		message = _("No accounts");
		break;
	case MC_NETWORK_ERROR:
		message = _("Network error");
		break;
	case MC_CONTACT_DOES_NOT_SUPPORT_VOICE_ERROR:
		message = _("Contact does not support voice");
		break;
	case MC_LOWMEM_ERROR:
		message = _("Lowmem");
		break;
	case MC_CHANNEL_REQUEST_GENERIC_ERROR:
		message = _("Channel request generic error");
		break;
	case MC_CHANNEL_BANNED_ERROR:
		message = _("Channel banned");
		break;
	case MC_CHANNEL_FULL_ERROR:
		message = _("Channel full");
		break;
	case MC_CHANNEL_INVITE_ONLY_ERROR:
		message = _("Channel invite only");
		break;
	default:
		message = _("Unknown error code");
	}

	DEBUG ("Error during operation %d: %s", operation_id, message);
}

static void
use_nm_notify_cb (EmpathyConf *conf,
		  const gchar *key,
		  gpointer     user_data)
{
	EmpathyIdle *idle = user_data;
	gboolean     use_nm;

	if (empathy_conf_get_bool (conf, key, &use_nm)) {
		empathy_idle_set_use_nm (idle, use_nm);
	}
}

static void
create_salut_account (void)
{
	McProfile  *profile;
	McProtocol *protocol;
	gboolean    salut_created = FALSE;
	McAccount  *account;
	GList      *accounts;
	EBook      *book;
	EContact   *contact;
	gchar      *nickname = NULL;
	gchar      *first_name = NULL;
	gchar      *last_name = NULL;
	gchar      *email = NULL;
	gchar      *jid = NULL;
	GError     *error = NULL;

	/* Check if we already created a salut account */
	empathy_conf_get_bool (empathy_conf_get(),
			       EMPATHY_PREFS_SALUT_ACCOUNT_CREATED,
			       &salut_created);
	if (salut_created) {
		return;
	}

	DEBUG ("Try to add a salut account...");

	/* Check if the salut CM is installed */
	profile = mc_profile_lookup ("salut");
	protocol = mc_profile_get_protocol (profile);
	if (!protocol) {
		DEBUG ("Salut not installed");
		g_object_unref (profile);
		return;
	}
	g_object_unref (protocol);

	/* Get self EContact from EDS */
	if (!e_book_get_self (&contact, &book, &error)) {
		DEBUG ("Failed to get self econtact: %s",
			error ? error->message : "No error given");
		g_clear_error (&error);
		g_object_unref (profile);
		return;
	}

	empathy_conf_set_bool (empathy_conf_get (),
			       EMPATHY_PREFS_SALUT_ACCOUNT_CREATED,
			       TRUE);

	/* Check if there is already a salut account */
	accounts = mc_accounts_list_by_profile (profile);
	if (accounts) {
		DEBUG ("There is already a salut account");
		mc_accounts_list_free (accounts);
		g_object_unref (profile);
		return;
	}

	account = mc_account_create (profile);
	mc_account_set_display_name (account, _("People nearby"));
	
	nickname = e_contact_get (contact, E_CONTACT_NICKNAME);
	first_name = e_contact_get (contact, E_CONTACT_GIVEN_NAME);
	last_name = e_contact_get (contact, E_CONTACT_FAMILY_NAME);
	email = e_contact_get (contact, E_CONTACT_EMAIL_1);
	jid = e_contact_get (contact, E_CONTACT_IM_JABBER_HOME_1);
	
	if (!tp_strdiff (nickname, "nickname")) {
		g_free (nickname);
		nickname = NULL;
	}

	DEBUG ("Salut account created:\nnickname=%s\nfirst-name=%s\n"
		"last-name=%s\nemail=%s\njid=%s\n",
		nickname, first_name, last_name, email, jid);

	mc_account_set_param_string (account, "nickname", nickname ? nickname : "");
	mc_account_set_param_string (account, "first-name", first_name ? first_name : "");
	mc_account_set_param_string (account, "last-name", last_name ? last_name : "");
	mc_account_set_param_string (account, "email", email ? email : "");
	mc_account_set_param_string (account, "jid", jid ? jid : "");

	g_free (nickname);
	g_free (first_name);
	g_free (last_name);
	g_free (email);
	g_free (jid);
	g_object_unref (account);
	g_object_unref (profile);
	g_object_unref (contact);
	g_object_unref (book);
}

/* The code that handles single-instance and startup notification is
 * copied from gedit.
 *
 * Copyright (C) 2005 - Paolo Maggi 
 */
static void
on_bacon_message_received (const char *message,
			   gpointer    data)
{
	GtkWidget *window = data;
	guint32    startup_timestamp;

	g_return_if_fail (message != NULL);

	DEBUG ("Other instance launched, presenting the main window. message='%s'",
		message);

	startup_timestamp = atoi (message);

	/* Set the proper interaction time on the window.
	 * Fall back to roundtripping to the X server when we
	 * don't have the timestamp, e.g. when launched from
	 * terminal. We also need to make sure that the window
	 * has been realized otherwise it will not work. lame. */
	if (startup_timestamp == 0) {
		/* Work if launched from the terminal */
		DEBUG ("Using X server timestamp as a fallback");

		if (!GTK_WIDGET_REALIZED (window)) {
			gtk_widget_realize (GTK_WIDGET (window));
		}

		startup_timestamp = gdk_x11_get_server_time (window->window);
	}

	gtk_window_present_with_time (GTK_WINDOW (window), startup_timestamp);
}

static guint32
get_startup_timestamp ()
{
	const gchar *startup_id_env;
	gchar       *startup_id = NULL;
	gchar       *time_str;
	gchar       *end;
	gulong       retval = 0;

	/* we don't unset the env, since startup-notification
	 * may still need it */
	startup_id_env = g_getenv ("DESKTOP_STARTUP_ID");
	if (startup_id_env == NULL) {
		goto out;
	}

	startup_id = g_strdup (startup_id_env);

	time_str = g_strrstr (startup_id, "_TIME");
	if (time_str == NULL) {
		goto out;
	}

	errno = 0;

	/* Skip past the "_TIME" part */
	time_str += 5;

	retval = strtoul (time_str, &end, 0);
	if (end == time_str || errno != 0)
		retval = 0;

 out:
	g_free (startup_id);

	return (retval > 0) ? retval : 0;
}

int
main (int argc, char *argv[])
{
	guint32            startup_timestamp;
	EmpathyStatusIcon *icon;
	EmpathyDispatcher *dispatcher;
	GtkWidget         *window;
	MissionControl    *mc;
	EmpathyIdle       *idle;
	gboolean           autoconnect = TRUE;
	gboolean           no_connect = FALSE; 
	gboolean           hide_contact_list = FALSE;
	GError            *error = NULL;
	GOptionEntry       options[] = {
		{ "no-connect", 'n',
		  0, G_OPTION_ARG_NONE, &no_connect,
		  N_("Don't connect on startup"),
		  NULL },
		{ "hide-contact-list", 'h',
		  0, G_OPTION_ARG_NONE, &hide_contact_list,
		  N_("Don't show the contact list on startup"),
		  NULL },
		{ NULL }
	};

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	startup_timestamp = get_startup_timestamp ();

	if (!gtk_init_with_args (&argc, &argv,
				 _("- Empathy Instant Messenger"),
				 options, GETTEXT_PACKAGE, &error)) {
		g_warning ("Error in gtk init: %s", error->message);
		return EXIT_FAILURE;
	}

	if (g_getenv ("EMPATHY_TIMING") != NULL) {
		g_log_set_default_handler (tp_debug_timestamped_log_handler, NULL);
	}
	empathy_debug_set_flags (g_getenv ("EMPATHY_DEBUG"));
	tp_debug_divert_messages (g_getenv ("EMPATHY_LOGFILE"));

	g_set_application_name (PACKAGE_NAME);

	gtk_window_set_default_icon_name ("empathy");
	gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (),
					   PKGDATADIR G_DIR_SEPARATOR_S "icons");

        /* Setting up the bacon connection */
	connection = bacon_message_connection_new ("empathy");
	if (connection != NULL) {
		if (!bacon_message_connection_get_is_server (connection)) {
			gchar *message;

			DEBUG ("Activating existing instance");

			message = g_strdup_printf ("%" G_GUINT32_FORMAT,
						   startup_timestamp);
			bacon_message_connection_send (connection, message);

			/* We never popup a window, so tell startup-notification
			 * that we are done. */
			gdk_notify_startup_complete ();

			g_free (message);
			bacon_message_connection_free (connection);

			return EXIT_SUCCESS;
		}
	} else {
		g_warning ("Cannot create the 'empathy' bacon connection.");
	}

	/* Setting up MC */
	mc = empathy_mission_control_new ();
	g_signal_connect (mc, "ServiceEnded",
			  G_CALLBACK (service_ended_cb),
			  NULL);
	g_signal_connect (mc, "Error",
			  G_CALLBACK (operation_error_cb),
			  NULL);

	/* Setting up Idle */
	idle = empathy_idle_new ();
	empathy_idle_set_auto_away (idle, TRUE);
	use_nm_notify_cb (empathy_conf_get (), EMPATHY_PREFS_USE_NM, idle);
	empathy_conf_notify_add (empathy_conf_get (), EMPATHY_PREFS_USE_NM,
				 use_nm_notify_cb, idle);

	/* Autoconnect */
	empathy_conf_get_bool (empathy_conf_get(),
			       EMPATHY_PREFS_AUTOCONNECT,
			       &autoconnect);
	if (autoconnect && ! no_connect &&
	    empathy_idle_get_state (idle) <= MC_PRESENCE_OFFLINE) {
		empathy_idle_set_state (idle, MC_PRESENCE_AVAILABLE);
	}
	
	create_salut_account ();

	/* Setting up UI */
	window = empathy_main_window_show ();
	icon = empathy_status_icon_new (GTK_WINDOW (window), hide_contact_list);

	if (connection) {
		/* We se the callback here because we need window */
		bacon_message_connection_set_callback (connection,
						       on_bacon_message_received,
						       window);
	}

	/* Handle channels */
	dispatcher = empathy_dispatcher_new ();
	g_signal_connect (dispatcher, "dispatch-channel",
			  G_CALLBACK (dispatch_channel_cb),
			  NULL);

	gtk_main ();

	empathy_idle_set_state (idle, MC_PRESENCE_OFFLINE);

	g_object_unref (mc);
	g_object_unref (idle);
	g_object_unref (icon);
	g_object_unref (dispatcher);

	return EXIT_SUCCESS;
}

