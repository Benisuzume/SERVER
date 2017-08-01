/*
 * Copyright (C) 1998  Mark Baysinger (mbaysing@ucsd.edu)
 * Copyright (C) 1998,1999,2000,2001  Ross Combs (rocombs@cs.nmsu.edu)
 * Copyright (C) 1999  Gediminas (gediminas_lt@mailexcite.com)
 * Copyright (C) 1999  Rob Crittenden (rcrit@greyoak.com)
 * Copyright (C) 2000,2001  Marco Ziech (mmz@gmx.net)
 * Copyright (C) 2000  Dizzy
 * Copyright (C) 2000  Onlyer (onlyer@263.net)
 * Copyright (C) 2003,2004  Aaron
 * Copyright (C) 2004  Donny Redmond (dredmond@linuxmail.org)
 * Copyright (C) 2008  Pelish (pelish@gmail.com)
 * Copyright (C) 2014  HarpyWar (harpywar@gmail.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#include "common/setup_before.h"
#include "command.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>

#include "compat/strcasecmp.h"
#include "common/tag.h"
#include "common/util.h"
#include "common/version.h"
#include "common/eventlog.h"
#include "common/bnettime.h"
#include "common/addr.h"
#include "common/packet.h"
#include "common/bnethash.h"
#include "common/list.h"
#include "common/proginfo.h"
#include "common/queue.h"
#include "common/bn_type.h"
#include "common/xalloc.h"
#include "common/xstr.h"
#include "common/trans.h"
#include "common/lstr.h"
#include "common/hashtable.h"
#include "common/xstring.h"

#include "connection.h"
#include "message.h"
#include "channel.h"
#include "game.h"
#include "team.h"
#include "account.h"
#include "account_wrap.h"
#include "server.h"
#include "prefs.h"
#include "ladder.h"
#include "timer.h"
#include "helpfile.h"
#include "mail.h"
#include "runprog.h"
#include "alias_command.h"
#include "realm.h"
#include "ipban.h"
#include "command_groups.h"
#include "news.h"
#include "topic.h"
#include "friends.h"
#include "clan.h"
#include "common/flags.h"
#include "icons.h"
#include "userlog.h"
#include "i18n.h"

#include "attrlayer.h"

#ifdef WITH_LUA
#include "luainterface.h"
#endif
#include "common/setup_after.h"

namespace pvpgn
{

	namespace bnetd
	{
		static char const * bnclass_get_str(unsigned int cclass);
		static void do_whisper(t_connection * user_c, char const * dest, char const * text);
		static void do_whois(t_connection * c, char const * dest);
		static void user_timer_cb(t_connection * c, std::time_t now, t_timer_data str);

		std::string msgtemp, msgtemp2;
		char msgtemp0[MAX_MESSAGE_LEN];


		static char const * bnclass_get_str(unsigned int cclass)
		{
			switch (cclass)
			{
			case PLAYERINFO_DRTL_CLASS_WARRIOR:
				return "warrior";
			case PLAYERINFO_DRTL_CLASS_ROGUE:
				return "rogue";
			case PLAYERINFO_DRTL_CLASS_SORCERER:
				return "sorcerer";
			default:
				return "unknown";
			}
		}

		/*
		* Split text by spaces and return array of arguments.
		*   First text argument is a command name (index = 0)
		*   Last text argument always reads to end
		*/
		extern std::vector<std::string> split_command(char const * text, int args_count)
		{
			std::vector<std::string> result(args_count + 1);

			std::string s(text);
			// remove slash from the command
			if (!s.empty())
				s.erase(0, 1);

			std::istringstream iss(s);

			int i = 0;
			std::string tmp = std::string(); // to end
			do
			{
				std::string sub;
				iss >> sub;

				if (sub.empty())
					continue;

				if (i < args_count)
				{
					result[i] = sub;
					i++;
				}
				else
				{
					if (!tmp.empty())
						tmp += " ";
					tmp += sub;
				}

			} while (iss);

			// push remaining text at the end
			if (tmp.length() > 0)
				result[args_count] = tmp;

			return result;
		}
		std::string msgt;
		static void do_whisper(t_connection * user_c, char const * dest, char const * text)
		{
			t_connection * dest_c;
			char const *   tname;

			if (account_get_auth_mute(conn_get_account(user_c)) == 1)
			{
				message_send_text(user_c, message_type_error, user_c, localize(user_c, "Your account has been muted, you can't whisper to other users."));
				return;
			}

			if (!(dest_c = connlist_find_connection_by_name(dest, conn_get_realm(user_c))))
			{
				message_send_text(user_c, message_type_error, user_c, localize(user_c, "That user is not logged on."));
				return;
			}


#ifdef WITH_LUA
			if (lua_handle_user(user_c, dest_c, text, luaevent_user_whisper) == 1)
				return;
#endif

			if (conn_get_dndstr(dest_c))
			{
				msgtemp = localize(user_c, "{} is unavailable ({})", conn_get_username(dest_c), conn_get_dndstr(dest_c));
				message_send_text(user_c, message_type_info, user_c, msgtemp);
				return;
			}

			message_send_text(user_c, message_type_whisperack, dest_c, text);

			if (conn_get_awaystr(dest_c))
			{
				msgtemp = localize(user_c, "{} is away ({})", conn_get_username(dest_c), conn_get_awaystr(dest_c));
				message_send_text(user_c, message_type_info, user_c, msgtemp);
			}

			message_send_text(dest_c, message_type_whisper, user_c, text);

			if ((tname = conn_get_username(user_c)))
			{
				char username[1 + MAX_USERNAME_LEN]; /* '*' + username (including NUL) */

				if (std::strlen(tname) < MAX_USERNAME_LEN)
				{
					std::sprintf(username, "*%s", tname);
					conn_set_lastsender(dest_c, username);
				}
			}
		}

		static void do_botchat(t_connection * user_c, char const * dest, char const * text)
		{
			t_connection * dest_c;
			char const *   tname;

			if (!(dest_c = connlist_find_connection_by_name(dest, conn_get_realm(user_c))))
			{
				message_send_text(user_c, message_type_error, user_c, localize(user_c, "System down, please wait ..."));
				return;
			}

			message_send_text(dest_c, message_type_whisper, user_c, text);

			if ((tname = conn_get_username(user_c)))
			{
				char username[1 + MAX_USERNAME_LEN]; /* '*' + username (including NUL) */

				if (std::strlen(tname) < MAX_USERNAME_LEN)
				{
					std::sprintf(username, "*%s", tname);
					conn_set_lastsender(dest_c, username);
				}
			}
		}
		
		static void do_botchatblue(t_connection * user_c, char const * dest, char const * text)
		{
			t_connection * dest_c;
			char const *   tname;

			if (!(dest_c = connlist_find_connection_by_name(dest, conn_get_realm(user_c)))) { return; }
			message_send_text(dest_c, message_type_info, user_c, text);

			if ((tname = conn_get_username(user_c)))
			{
				char username[1 + MAX_USERNAME_LEN]; /* '*' + username (including NUL) */

				if (std::strlen(tname) < MAX_USERNAME_LEN)
				{
					std::sprintf(username, "*%s", tname);
					conn_set_lastsender(dest_c, username);
				}
			}
		}
		
		static void do_botchatred(t_connection * user_c, char const * dest, char const * text)
		{
			t_connection * dest_c;
			char const *   tname;

			if (!(dest_c = connlist_find_connection_by_name(dest, conn_get_realm(user_c)))) { return; }
			message_send_text(dest_c, message_type_error, user_c, text);

			if ((tname = conn_get_username(user_c)))
			{
				char username[1 + MAX_USERNAME_LEN]; /* '*' + username (including NUL) */

				if (std::strlen(tname) < MAX_USERNAME_LEN)
				{
					std::sprintf(username, "*%s", tname);
					conn_set_lastsender(dest_c, username);
				}
			}
		}
		
		static void do_whois(t_connection * c, char const * dest)
		{
			t_connection *    dest_c;
			std::string  namepart, verb; /* 64 + " (" + 64 + ")" + NUL */
			t_game const *    game;
			t_channel const * channel;

			if ((!(dest_c = connlist_find_connection_by_accountname(dest))) &&
				(!(dest_c = connlist_find_connection_by_name(dest, conn_get_realm(c)))))
			{
				t_account * dest_a;
				t_bnettime btlogin;
				std::time_t ulogin;
				struct std::tm * tmlogin;

				if (!(dest_a = accountlist_find_account(dest))) {
					message_send_text(c, message_type_error, c, localize(c, "Unknown user."));
					return;
				}

				if (conn_get_class(c) == conn_class_bnet) {
					btlogin = time_to_bnettime((std::time_t)account_get_ll_time(dest_a), 0);
					btlogin = bnettime_add_tzbias(btlogin, conn_get_tzbias(c));
					ulogin = bnettime_to_time(btlogin);
					if (!(tmlogin = std::gmtime(&ulogin)))
						std::strcpy(msgtemp0, "?");
					else
						std::strftime(msgtemp0, sizeof(msgtemp0), "%a %b %d %H:%M:%S", tmlogin);
					msgtemp = localize(c, "User was last seen on: {}", msgtemp0);
				}
				else
					msgtemp = localize(c, "User is offline");
				message_send_text(c, message_type_info, c, msgtemp);
				return;
			}

			if (c == dest_c)
			{
				namepart = localize(c, "You");
				verb = localize(c, "are");
			}
			else
			{
				char const * tname;

				namepart = (tname = conn_get_chatcharname(dest_c, c));
				conn_unget_chatcharname(dest_c, tname);
				verb = localize(c, "is");
			}

			if ((game = conn_get_game(dest_c)))
			{
				msgtemp = localize(c, "{} {} using {} and {} currently in {} game \"{}\".",
					namepart,
					verb,
					clienttag_get_title(conn_get_clienttag(dest_c)),
					verb,
					game_get_flag(game) == game_flag_private ? localize(c, "private") : "",
					game_get_name(game));
			}
			else if ((channel = conn_get_channel(dest_c)))
			{
				msgtemp = localize(c, "{} {} using {} and {} currently in channel \"{}\".",
					namepart,
					verb,
					clienttag_get_title(conn_get_clienttag(dest_c)),
					verb,
					channel_get_name(channel));
			}
			else
				msgtemp = localize(c, "{} {} using {}.",
				namepart,
				verb,
				clienttag_get_title(conn_get_clienttag(dest_c)));
			message_send_text(c, message_type_info, c, msgtemp);

			if (conn_get_dndstr(dest_c))
			{
				msgtemp = localize(c, "{} {} refusing messages ({})",
					namepart,
					verb,
					conn_get_dndstr(dest_c));
				message_send_text(c, message_type_info, c, msgtemp);
			}
			else
			if (conn_get_awaystr(dest_c))
			{
				msgtemp = localize(c, "{} away ({})",
					namepart,
					conn_get_awaystr(dest_c));
				message_send_text(c, message_type_info, c, msgtemp);
			}
		}


		static void user_timer_cb(t_connection * c, std::time_t now, t_timer_data str)
		{
			if (!c)
			{
				eventlog(eventlog_level_error, __FUNCTION__, "got NULL connection");
				return;
			}
			if (!str.p)
			{
				eventlog(eventlog_level_error, __FUNCTION__, "got NULL str");
				return;
			}

			if (now != (std::time_t)0) /* zero means user logged out before expiration */
				message_send_text(c, message_type_info, c, (char*)str.p);
			xfree(str.p);
		}

		typedef int(*t_command)(t_connection * c, char const * text);

		typedef struct {
			const char * command_string;
			t_command    command_handler;
		} t_command_table_row;
		
		// New
		static int _handle_botchatblue_command(t_connection * c, char const * text);
		static int _handle_botchatred_command(t_connection * c, char const * text);
		static int _handle_admin_command(t_connection * c, char const * text);
		static int _handle_operator_command(t_connection * c, char const * text);
		static int _handle_watch_command(t_connection * c, char const * text);
		static int _handle_unwatch_command(t_connection * c, char const * text);
		static int _handle_watchall_command(t_connection * c, char const * text);
		static int _handle_unwatchall_command(t_connection * c, char const * text);
		static int _handle_squelch_command(t_connection * c, char const * text);
		static int _handle_unsquelch_command(t_connection * c, char const * text);
		static int _handle_quit_command(t_connection * c, char const * text);
		static int _handle_lockacct_command(t_connection * c, char const * text);
		static int _handle_unlockacct_command(t_connection * c, char const * text);
		static int _handle_muteacct_command(t_connection * c, char const * text);
		static int _handle_unmuteacct_command(t_connection * c, char const * text);
		static int _handle_users_command(t_connection * c, char const * text);
		static int _handle_away_command(t_connection * c, char const * text);
		static int _handle_dnd_command(t_connection * c, char const * text);
		static int _handle_join_command(t_connection * c, char const * text);
		static int _handle_rejoin_command(t_connection * c, char const * text);
		static int _handle_flag_command(t_connection * c, char const * text);
		static int _handle_setdefault_command(t_connection * c, char const * text);
		static int _handle_tag_command(t_connection * c, char const * text);
		static int _handle_topic_command(t_connection * c, char const * text);
		static int _handle_time_command(t_connection * c, char const * text);
		static int _handle_voice_command(t_connection * c, char const * text);
		static int _handle_devoice_command(t_connection * c, char const * text);
		static int _handle_tmpop_command(t_connection * c, char const * text);
		static int _handle_deop_command(t_connection * c, char const * text);
		static int _handle_ping_command(t_connection * c, char const * text);
		static int _handle_servertime_command(t_connection * c, char const * text);
		static int _handle_me_command(t_connection * c, char const * text);
		static int _handle_whisper_command(t_connection * c, char const * text);
		static int _handle_whois_command(t_connection * c, char const * text);
		static int _handle_kick_command(t_connection * c, char const * text);
		static int _handle_reply_command(t_connection * c, char const * text);
		static int _handle_netinfo_command(t_connection * c, char const * text);
		static int _handle_ipscan_command(t_connection * c, char const * text);
		static int _handle_addacct_command(t_connection * c, char const * text);
		static int _handle_chpass_command(t_connection * c, char const * text);
		static int _handle_set_command(t_connection * c, char const * text);
		static int _handle_commandgroups_command(t_connection * c, char const * text);
		static int _handle_who_command(t_connection * c, char const * text);
		static int _handle_whoami_command(t_connection * c, char const * text);
		static int _handle_botannounce_command(t_connection * c, char const * text);
		static int _handle_nullannounceblue_command(t_connection * c, char const * text);
		static int _handle_nullannouncered_command(t_connection * c, char const * text);
		static int _handle_nullannouncegreen_command(t_connection * c, char const * text);
		static int _handle_kill_command(t_connection * c, char const * text);
		static int _handle_serverban_command(t_connection * c, char const * text);
		static int _handle_finger_command(t_connection * c, char const * text);
		static int _handle_moderate_command(t_connection * c, char const * text);
		static int _handle_friends_command(t_connection * c, char const * text);
		static int _handle_clan_command(t_connection * c, char const * text);
		
		// New
		static int _handle_botrules_command(t_connection * c, char const * text);
		static int _handle_botdonate_command(t_connection * c, char const * text);
		static int _handle_botevent_command(t_connection * c, char const * text);
		static int _handle_botrequest_command(t_connection * c, char const * text);
		static int _handle_botonline_command(t_connection * c, char const * text);
		static int _handle_botbuy_command(t_connection * c, char const * text);
		static int _handle_botcash_command(t_connection * c, char const * text);
		static int _handle_botcreate_command(t_connection * c, char const * text);
		static int _handle_bottop10_command(t_connection * c, char const * text);
		static int _handle_botstats_command(t_connection * c, char const * text);
		static int _handle_botstatus_command(t_connection * c, char const * text);
		static int _handle_botdota_command(t_connection * c, char const * text);
		static int _handle_botlod_command(t_connection * c, char const * text);
		static int _handle_botimba_command(t_connection * c, char const * text);
		static int _handle_botchat_command(t_connection * c, char const * text);
		static int _handle_botaccept_command(t_connection * c, char const * text);
		static int _handle_botdecline_command(t_connection * c, char const * text);
		static int _handle_botlock_command(t_connection * c, char const * text);
		static int _handle_botunlock_command(t_connection * c, char const * text);
		static int _handle_botmute_command(t_connection * c, char const * text);
		static int _handle_botunmute_command(t_connection * c, char const * text);
		
		static int command_set_flags(t_connection * c); // [Omega]
		// command handler prototypes
		static int _handle_games_command(t_connection * c, char const * text);
		static int _handle_channels_command(t_connection * c, char const * text);

		static const t_command_table_row standard_command_table[] =
		{
			// New
			{ "/ipban", handle_ipban_command }, // in ipban.c
			{ "/help", handle_help_command },
			{ "/?", handle_help_command },
			{ "/botchatblue", _handle_botchatblue_command },
			{ "/botchatred", _handle_botchatred_command },
			{ "/admin", _handle_admin_command },
			{ "/operator", _handle_operator_command },
			{ "/watch", _handle_watch_command },
			{ "/unwatch", _handle_unwatch_command },
			{ "/watchall", _handle_watchall_command },
			{ "/unwatchall", _handle_unwatchall_command },
			{ "/ignore", _handle_squelch_command },
			{ "/squelch", _handle_squelch_command },
			{ "/unignore", _handle_unsquelch_command },
			{ "/unsquelch", _handle_unsquelch_command },
			{ "/logout", _handle_quit_command },
			{ "/quit", _handle_quit_command },
			{ "/exit", _handle_quit_command },
			{ "/lockacct", _handle_lockacct_command },
			{ "/unlockacct", _handle_unlockacct_command },
			{ "/muteacct", _handle_muteacct_command },
			{ "/unmuteacct", _handle_unmuteacct_command },
			{ "/users", _handle_users_command },
			{ "/away", _handle_away_command },
			{ "/dnd", _handle_dnd_command },
			{ "/join", _handle_join_command },
			{ "/j", _handle_join_command },
			{ "/rejoin", _handle_rejoin_command },
			{ "/flag", _handle_flag_command },
			{ "/setdefault", _handle_setdefault_command },
			{ "/tag", _handle_tag_command },
			{ "/topic", _handle_topic_command },
			{ "/time", _handle_time_command },
			{ "/icon", handle_icon_command },
			{ "/voice", _handle_voice_command },
			{ "/devoice", _handle_devoice_command },
			{ "/tmpop", _handle_tmpop_command },
			{ "/deop", _handle_deop_command },
			{ "/latency", _handle_ping_command },
			{ "/ping", _handle_ping_command },
			{ "/p", _handle_ping_command },
			{ "/servertime", _handle_servertime_command },
			{ "/me", _handle_me_command },
			{ "/emote", _handle_me_command },
			{ "/msg", _handle_whisper_command },
			{ "/whisper", _handle_whisper_command },
			{ "/w", _handle_whisper_command },
			{ "/m", _handle_whisper_command },
			{ "/whois", _handle_whois_command },
			{ "/whereis", _handle_whois_command },
			{ "/where", _handle_whois_command },
			{ "/kick", _handle_kick_command },
			{ "/r", _handle_reply_command },
			{ "/reply", _handle_reply_command },
			{ "/netinfo", _handle_netinfo_command },
			{ "/ipscan", _handle_ipscan_command },
			{ "/addacct", _handle_addacct_command },
			{ "/chpass", _handle_chpass_command },
			{ "/set", _handle_set_command },
			{ "/commandgroups", _handle_commandgroups_command },
			{ "/cg", _handle_commandgroups_command },
			{ "/who", _handle_who_command },
			{ "/whoami", _handle_whoami_command },
			{ "/botannounce", _handle_botannounce_command },
			{ "/nullannounceblue", _handle_nullannounceblue_command },
			{ "/nullannouncered", _handle_nullannouncered_command },
			{ "/nullannouncegreen", _handle_nullannouncegreen_command },
			{ "/kill", _handle_kill_command },
			{ "/serverban", _handle_serverban_command },
			{ "/finger", _handle_finger_command },
			{ "/moderate", _handle_moderate_command },
			{ "/f", _handle_friends_command },
			{ "/friends", _handle_friends_command },
			{ "/clan", _handle_clan_command },
			{ "/c", _handle_clan_command },
			
			{ "/games", _handle_games_command },
			{ "/channels", _handle_channels_command },
			{ "/chs", _handle_channels_command },
			
			// New
			{ "/rules", _handle_botrules_command },
			{ "/donate", _handle_botdonate_command },
			{ "/event", _handle_botevent_command },
			{ "/request", _handle_botrequest_command },
			{ "/online", _handle_botonline_command },
			{ "/buy", _handle_botbuy_command },
			{ "/cash", _handle_botcash_command },
			{ "/create", _handle_botcreate_command },
			{ "/host", _handle_botcreate_command },
			{ "/top10", _handle_bottop10_command },
			{ "/stats", _handle_botstats_command },
			{ "/status", _handle_botstatus_command },
			{ "/dota", _handle_botdota_command },
			{ "/lod", _handle_botlod_command },
			{ "/imba", _handle_botimba_command },
			{ "/chat", _handle_botchat_command },
			{ "/accept", _handle_botaccept_command },
			{ "/decline", _handle_botdecline_command },
			{ "/lock", _handle_botlock_command },
			{ "/unlock", _handle_botunlock_command },
			{ "/mute", _handle_botmute_command },
			{ "/unmute", _handle_botunmute_command },

			{ NULL, NULL }

		};


		extern int handle_command(t_connection * c, char const * text)
		{
			int result = 0;
			t_command_table_row const *p;

#ifdef WITH_LUA
			// feature to ignore flood protection
			result = lua_handle_command(c, text, luaevent_command_before);
#endif
			if (result == -1)
				return result;

			if (result == 0)
			if ((text[0] != '\0') && (conn_quota_exceeded(c, text)))
			{
				msgtemp = localize(c, "You are sending commands to {} too quickly and risk being disconnected for flooding. Please slow down.", prefs_get_servername());
				message_send_text(c, message_type_error, c, msgtemp);
				return 0;
			}

#ifdef WITH_LUA
			result = lua_handle_command(c, text, luaevent_command);
			// -1 = unsuccess, 0 = success, 1 = execute next c++ code
			if (result == 0)
			{
				// log command
				if (t_account * account = conn_get_account(c))
					userlog_append(account, text);
			}
			if (result == 0 || result == -1)
				return result;
#endif

			for (p = standard_command_table; p->command_string != NULL; p++)
			{
				if (strstart(text, p->command_string) == 0)
				{
					if (!(command_get_group(p->command_string)))
					{
						message_send_text(c, message_type_error, c, localize(c, "This command has been deactivated"));
						return 0;
					}
					if (!((command_get_group(p->command_string) & account_get_command_groups(conn_get_account(c)))))
					{
						message_send_text(c, message_type_error, c, localize(c, "This command is reserved for admins."));
						return 0;
					}
					if (p->command_handler != NULL)
					{
						result = ((p->command_handler)(c, text));
						// -1 = unsuccess, 0 = success
						if (result == 0)
						{
							// log command
							if (t_account * account = conn_get_account(c))
								userlog_append(account, text);
						}
						return result;
					}
				}
			}

			if (std::strlen(text) >= 2 && std::strncmp(text, "//", 2) == 0)
			{
				handle_alias_command(c, text);
				return 0;
			}

			message_send_text(c, message_type_error, c, localize(c, "Unknown command."));
			eventlog(eventlog_level_debug, __FUNCTION__, "got unknown command \"{}\"", text);
			return -1;
		}





		// +++++++++++++++++++++++++++++++++ command implementations +++++++++++++++++++++++++++++++++++++++
		// New
		static int _handle_botchatblue_command(t_connection * c, char const *text)
		{
			char const * username; /* both include NUL, so no need to add one for middle @ or * */

			std::vector<std::string> args = split_command(text, 2);

			if (args[2].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /botchatblue [username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Whispers a message to [username] type blue."));
				
				return -1;
			}
			username = args[1].c_str(); // username
			text = args[2].c_str(); // message

			do_botchatblue(c, username, text);

			return 0;
		}
		
		static int _handle_botchatred_command(t_connection * c, char const *text)
		{
			char const * username; /* both include NUL, so no need to add one for middle @ or * */

			std::vector<std::string> args = split_command(text, 2);

			if (args[2].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /botchatred [username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Whispers a message to [username] type red."));
				return -1;
			}
			username = args[1].c_str(); // username
			text = args[2].c_str(); // message

			do_botchatred(c, username, text);

			return 0;
		}
		
		static int _handle_admin_command(t_connection * c, char const * text)
		{
			char const *	username;
			char		command;
			t_account *		acc;
			t_connection *	dst_c;
			int			changed = 0;

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty() || (args[1][0] != '+' && args[1][0] != '-')) {
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /admin +[username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Promotes [username] to administrators list."));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /admin -[username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Demotes [username] from administrators list."));
				return -1;
			}

			text = args[1].c_str();
			command = text[0]; // command type (+/-)
			username = &text[1]; // username

			if (!*username) {
				message_send_text(c, message_type_error, c, localize(c, "You must supply a username!"));
				return -1;
			}

			if (!(acc = accountlist_find_account(username))) {
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}
			
			dst_c = account_get_conn(acc);

			if (command == '+') {
				if (account_get_auth_admin(acc, NULL) == 1) {
					message_send_text(c, message_type_error, c, "That user already on administrators!");
				}
				else {
					account_set_auth_admin(acc, NULL, 1);
					msgtemp = localize(c, "User {} has been promoted to administrators.", account_get_name(acc));
					message_send_text(c, message_type_info, c, msgtemp);
					changed = 1;
				}
			}
			else {
				if (account_get_auth_admin(acc, NULL) != 1) {
					msgtemp = localize(c, "User {} is not registered on administrators, so you can't demote him!", account_get_name(acc));
					message_send_text(c, message_type_error, c, msgtemp);
				}
				else {
					account_set_auth_admin(acc, NULL, 0);
					msgtemp = localize(c, "User {} has been demoted from administrators.", account_get_name(acc));
					message_send_text(c, message_type_info, c, msgtemp);
					changed = 1;
				}
			}
			
			command_set_flags(dst_c);
			return 0;
		}
		
		static int _handle_operator_command(t_connection * c, char const * text)
		{
			char const *	username;
			char		command;
			t_account *		acc;
			t_connection *	dst_c;
			int			changed = 0;

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty() || (args[1][0] != '+' && args[1][0] != '-')) {
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /operator +[username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Promotes [username] to operators list."));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /operator -[username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Demotes [username] from operators list."));
				return -1;
			}

			text = args[1].c_str();
			command = text[0]; // command type (+/-)
			username = &text[1]; // username

			if (!*username) {
				message_send_text(c, message_type_error, c, localize(c, "You must supply a username!"));
				return -1;
			}

			if (!(acc = accountlist_find_account(username))) {
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}
			dst_c = account_get_conn(acc);

			if (command == '+') {
				if (account_get_auth_operator(acc, NULL) == 1)
					message_send_text(c, message_type_error, c, "That user already on operators!");
				else {
					account_set_auth_operator(acc, NULL, 1);
					msgtemp = localize(c, "User {} has been promoted to operators.", account_get_name(acc));
					message_send_text(c, message_type_info, c, msgtemp);
					changed = 1;
				}
			}
			else {
				if (account_get_auth_operator(acc, NULL) != 1) {
					msgtemp = localize(c, "User {} is not registered on operators, so you can't demote him!", account_get_name(acc));
					message_send_text(c, message_type_error, c, msgtemp);
				}
				else {
					account_set_auth_operator(acc, NULL, 0);
					msgtemp = localize(c, "User {} has been demoted from operators.", account_get_name(acc));
					message_send_text(c, message_type_info, c, msgtemp);
					changed = 1;
				}
			}
			
			command_set_flags(dst_c);
			return 0;
		}
		
		static int _handle_watch_command(t_connection * c, char const *text)
		{
			t_account *  account;

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /watch [username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Enables notifications for [username]."));
				return -1;
			}
			text = args[1].c_str(); // username

			if (!(account = accountlist_find_account(text)))
			{
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}

			if (conn_add_watch(c, account, 0) < 0) /* FIXME: adds all events for now */
			{
				message_send_text(c, message_type_error, c, localize(c, "Add to watch list failed!"));
				return -1;
			}
			else
			{
				msgtemp = localize(c, "User {} added to your watch list.", account_get_name(account));
				message_send_text(c, message_type_info, c, msgtemp);
			}

			return 0;
		}

		static int _handle_unwatch_command(t_connection * c, char const *text)
		{
			t_account *  account;

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /unwatch [username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Disables notifications for [username]."));
				return -1;
			}
			text = args[1].c_str(); // username
			if (!(account = accountlist_find_account(text)))
			{
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}

			if (conn_del_watch(c, account, 0) < 0) /* FIXME: deletes all events for now */
			{
				message_send_text(c, message_type_error, c, localize(c, "Removal from watch list failed."));
				return -1;
			}
			else
			{
				msgtemp = localize(c, "User {} removed from your watch list.", account_get_name(account));
				message_send_text(c, message_type_info, c, msgtemp);
			}

			return 0;
		}
		
		static int _handle_watchall_command(t_connection * c, char const *text)
		{
			t_clienttag clienttag = 0;
			char const * clienttag_str;

			std::vector<std::string> args = split_command(text, 1);
			clienttag_str = args[1].c_str(); // clienttag

			if (clienttag_str[0] != '\0')
			{
				if ( !(clienttag = tag_validate_client(args[1].c_str())) )
				{
					describe_command(c, args[0].c_str());
					return -1;
				}
			}

			if (conn_add_watch(c, NULL, clienttag) < 0) /* FIXME: adds all events for now */
				message_send_text(c, message_type_error, c, localize(c, "Add to watch list failed!"));
			else
			if (clienttag) {
				msgtemp = localize(c, "All {} users added to your watch list.", tag_uint_to_str((char*)clienttag_str, clienttag));
				message_send_text(c, message_type_info, c, msgtemp);
			}
			else
				message_send_text(c, message_type_info, c, localize(c, "All users added to your watch list."));

			return 0;
		}

		static int _handle_unwatchall_command(t_connection * c, char const *text)
		{
			t_clienttag clienttag = 0;
			char const * clienttag_str;

			std::vector<std::string> args = split_command(text, 1);
			clienttag_str = args[1].c_str(); // clienttag

			if (clienttag_str[0] != '\0')
			{
				if (!(clienttag = tag_validate_client(args[1].c_str())))
				{
					describe_command(c, args[0].c_str());
					return -1;
				}
			}

			if (conn_del_watch(c, NULL, clienttag) < 0) /* FIXME: deletes all events for now */
				message_send_text(c, message_type_error, c, localize(c, "Removal from watch list failed!"));
			else
			if (clienttag) {
				msgtemp = localize(c, "All {} users removed from your watch list.", tag_uint_to_str((char*)clienttag_str, clienttag));
				message_send_text(c, message_type_info, c, msgtemp);
			}
			else
				message_send_text(c, message_type_info, c, localize(c, "All users removed from your watch list."));

			return 0;
		}
		
		static int _handle_squelch_command(t_connection * c, char const *text)
		{
			t_account *  account;

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /squelch [username] (alias: ignore, see also: /unsquelch)"));
				message_send_text(c, message_type_info, c, localize(c, "** Blocks future messages sent from [username]."));
				return -1;
			}
			text = args[1].c_str(); // username

			/* D2 std::puts * before username */
			if (text[0] == '*')
				text++;

			if (!(account = accountlist_find_account(text)))
			{
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}

			if (conn_get_account(c) == account)
			{
				message_send_text(c, message_type_error, c, localize(c, "You can't squelch yourself!"));
				return -1;
			}

			if (conn_add_ignore(c, account) < 0)
			{
				message_send_text(c, message_type_error, c, localize(c, "Could not squelch user!"));
				return -1;
			}
			else
			{
				msgtemp = localize(c, "User {} has been squelched.", account_get_name(account));
				message_send_text(c, message_type_info, c, msgtemp);
			}

			return 0;
		}

		static int _handle_unsquelch_command(t_connection * c, char const *text)
		{
			t_account * account;
			t_connection * dest_c;

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /unsquelch [username] (alias: /unignore)"));
				message_send_text(c, message_type_info, c, localize(c, "** Allows a previously squelched [player] to talk to you normally."));
				return -1;
			}
			text = args[1].c_str(); // username

			/* D2 std::puts * before username */
			if (text[0] == '*')
				text++;

			if (!(account = accountlist_find_account(text)))
			{
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}

			if (conn_del_ignore(c, account) < 0)
			{
				message_send_text(c, message_type_info, c, localize(c, "User was not being ignored."));
				return -1;
			}
			else
			{
				t_message * message;

				message_send_text(c, message_type_info, c, localize(c, "No longer ignoring."));

				if ((dest_c = account_get_conn(account)))
				{
					if (!(message = message_create(message_type_userflags, dest_c, NULL))) /* handles NULL text */
						return -1;
					message_send(message, c);
					message_destroy(message);
				}
			}

			return 0;
		}
		
		static int _handle_quit_command(t_connection * c, char const *text)
		{
			if (conn_get_game(c))
				eventlog(eventlog_level_warn, __FUNCTION__, "[{}] user '{}' tried to disconnect while in game, cheat attempt ?", conn_get_socket(c), conn_get_loggeduser(c));
			else {
				message_send_text(c, message_type_info, c, localize(c, "Thanks for login today."));
				message_send_text(c, message_type_info, c, localize(c, "See you tomorrow :)"));
				conn_set_state(c, conn_state_destroy);
			}

			return 0;
		}
		
		static int _handle_lockacct_command(t_connection * c, char const *text)
		{
			t_connection * user;
			t_account *    account;
			char const * username, *reason = "", *hours = "24"; // default time 24 hours
			unsigned int sectime;

			std::vector<std::string> args = split_command(text, 3);
			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /lockacct [username] [days] [reason] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Locks [username] to prevent him/her from logging in with it."));
				return -1;
			}
			
			username = args[1].c_str(); // username
			if (!args[2].empty())
				hours = args[2].c_str(); // hours
			if (!args[3].empty())
				reason = args[3].c_str(); // reason

			if (!(account = accountlist_find_account(username)))
			{
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}

			account_set_auth_lock(account, 1);
			sectime = (atoi(hours) == 0) ? 0 : (atoi(hours) * 3600 * 24) + now; // get unlock time in the future
			account_set_auth_locktime(account, sectime);
			account_set_auth_lockreason(account, reason);
			account_set_auth_lockby(account, conn_get_username(c));


			// send message to author
			msgtemp = localize(c, "User {} is now locked from server.", account_get_name(account));
			message_send_text(c, message_type_info, c, msgtemp);

			return 0;
		}

		static int _handle_unlockacct_command(t_connection * c, char const *text)
		{
			t_connection * user;
			t_account *    account;

			std::vector<std::string> args = split_command(text, 1);
			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /unlockacct [username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Unlocks [username] to allow him/her to log in with it."));
				return -1;
			}
			text = args[1].c_str(); // username

			if (!(account = accountlist_find_account(text)))
			{
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}

			account_set_auth_lock(account, 0);
			msgtemp = localize(c, "User {} is now unlocked from server.", account_get_name(account));
			message_send_text(c, message_type_info, c, msgtemp);
			
			return 0;
		}
		
		static int _handle_muteacct_command(t_connection * c, char const *text)
		{
			t_connection * user;
			t_account *    account;
			char const * username, *reason = "", *hours = "1"; // default time 1 hour
			unsigned int sectime;

			std::vector<std::string> args = split_command(text, 3);
			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /muteacct [username] [hours] [reason] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Mutes [username] to prevent him/her from talking on channels."));
				return -1;
			}
			username = args[1].c_str(); // username
			if (!args[2].empty())
				hours = args[2].c_str(); // hours
			if (!args[3].empty())
				reason = args[3].c_str(); // reason

			if (!(account = accountlist_find_account(username)))
			{
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}

			account_set_auth_mute(account, 1);
			// get unlock time in the future
			sectime = (atoi(hours) == 0) ? 0 : (atoi(hours) * 60 * 60) + now;
			account_set_auth_mutetime(account, sectime);
			account_set_auth_mutereason(account, reason);
			account_set_auth_muteby(account, conn_get_username(c));

			// send message to author
			msgtemp = localize(c, "User {} is now muted", account_get_name(account));
			message_send_text(c, message_type_info, c, msgtemp);

			return 0;
		}

		static int _handle_unmuteacct_command(t_connection * c, char const *text)
		{
			t_connection * user;
			t_account *    account;

			std::vector<std::string> args = split_command(text, 1);
			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /unmuteacct [username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Unmutes [username] to allow him/her to talk on channels."));
				return -1;
			}
			text = args[1].c_str(); // username

			if (!(account = accountlist_find_account(text)))
			{
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}

			account_set_auth_mute(account, 0);
			msgtemp = localize(c, "User {} is now unmuted from server.", account_get_name(account));
			message_send_text(c, message_type_info, c, msgtemp);
			
			return 0;
		}
		
		static int _handle_users_command(t_connection * c, char const *text)
		{
			t_clienttag clienttag;

			// get clienttag
			std::vector<std::string> args = split_command(text, 1);

			if (!args[1].empty() && (clienttag = tag_validate_client(args[1].c_str())))
			{
				// clienttag status
				msgtemp = localize(c, "There are currently {} user(s) in {} games of {}",
					conn_get_user_count_by_clienttag(clienttag),
					game_get_count_by_clienttag(clienttag),
					clienttag_get_title(clienttag));
				message_send_text(c, message_type_info, c, msgtemp);
			}
			else
			{
				// overall status
				msgtemp = localize(c, "There are currently {} users online, in {} games, and in {} channels.",
					connlist_login_get_length(),
					gamelist_get_length(),
					channellist_get_length());
				message_send_text(c, message_type_info, c, msgtemp);
			}

			return 0;
		}
		
		static int _handle_away_command(t_connection * c, char const *text)
		{
			std::vector<std::string> args = split_command(text, 1);
			text = args[1].c_str(); // message

			if (text[0] == '\0') /* toggle away mode */
			{
				if (!conn_get_awaystr(c))
				{
					message_send_text(c, message_type_info, c, localize(c, "You are now marked as being away."));
					conn_set_awaystr(c, "Currently not available");
				}
				else
				{
					message_send_text(c, message_type_info, c, localize(c, "You are no longer marked as away."));
					conn_set_awaystr(c, NULL);
				}
			}
			else
			{
				message_send_text(c, message_type_info, c, localize(c, "You are now marked as being away."));
				conn_set_awaystr(c, text);
			}

			return 0;
		}

		static int _handle_dnd_command(t_connection * c, char const *text)
		{
			std::vector<std::string> args = split_command(text, 1);
			text = args[1].c_str(); // message

			if (text[0] == '\0') /* toggle dnd mode */
			{
				if (!conn_get_dndstr(c))
				{
					message_send_text(c, message_type_info, c, localize(c, "Don't disturb mode engaged."));
					conn_set_dndstr(c, localize(c, "Not available").c_str());
				}
				else
				{
					message_send_text(c, message_type_info, c, localize(c, "Don't disturb mode canceled."));
					conn_set_dndstr(c, NULL);
				}
			}
			else
			{
				message_send_text(c, message_type_info, c, localize(c, "Don't disturb mode engaged."));
				conn_set_dndstr(c, text);
			}

			return 0;
		}
		
		static int _handle_join_command(t_connection * c, char const *text)
		{
			t_channel * channel;

			std::vector<std::string> args = split_command(text, 1);
			
			if ((conn_get_clienttag(c) == CLIENTTAG_WARCRAFT3_UINT) || (conn_get_clienttag(c) == CLIENTTAG_WAR3XP_UINT)) {
				if (args[1].empty())
				{
					message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
					message_send_text(c, message_type_error, c, localize(c, "Usage: /join [channel] (alias: /j)"));
					message_send_text(c, message_type_info, c, localize(c, "** Moves you to [channel]."));
					return -1;
				}
				text = args[1].c_str(); // channelname

				if (!conn_get_game(c)) {
					if (strcasecmp(text, "Arranged Teams") == 0)
					{
						message_send_text(c, message_type_error, c, msgtemp = localize(c, "Channel Arranged Teams is a RESTRICTED Channel!"));
						return -1;
					}

					if (!(std::strlen(text) < MAX_CHANNELNAME_LEN))
					{
						msgtemp = localize(c, "Max channel name length exceeded (max {} symbols)", MAX_CHANNELNAME_LEN - 1);
						message_send_text(c, message_type_error, c, msgtemp);
						return -1;
					}

					if ((channel = conn_get_channel(c)) && (strcasecmp(channel_get_name(channel), text) == 0))
						return -1; // we don't have to do anything, we are already in this channel

					if (conn_set_channel(c, text) < 0)
						conn_set_channel(c, CHANNEL_NAME_BANNED); /* should not fail */
					if ((conn_get_clienttag(c) == CLIENTTAG_WARCRAFT3_UINT) || (conn_get_clienttag(c) == CLIENTTAG_WAR3XP_UINT))
						conn_update_w3_playerinfo(c);
					command_set_flags(c);
				}
				else
					message_send_text(c, message_type_error, c, localize(c, "Command disabled while inside a game."));

				return 0;
			}
			else
				message_send_text(c, message_type_error, c, localize(c, "You don't have access to that command."));

			return 0;
		}
		
		static int _handle_rejoin_command(t_connection * c, char const *text)
		{

			if (channel_rejoin(c) != 0)
				message_send_text(c, message_type_error, c, localize(c, "You are not in a channel!"));
			if ((conn_get_clienttag(c) == CLIENTTAG_WARCRAFT3_UINT) || (conn_get_clienttag(c) == CLIENTTAG_WAR3XP_UINT))
				conn_update_w3_playerinfo(c);
			command_set_flags(c);

			return 0;
		}
		
		static int _handle_flag_command(t_connection * c, char const *text)
		{
			char const * flag_s;
			unsigned int newflag;

			std::vector<std::string> args = split_command(text, 1);
			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, "--------------------------------------------------------");
				message_send_text(c, message_type_error, c, "Usage: /flag [number] (no have alias)");
				message_send_text(c, message_type_info, c, "** A debug tool for icon flags.");
				return -1;
			}
			flag_s = args[1].c_str(); // flag

			newflag = std::strtoul(flag_s, NULL, 0);
			conn_set_flags(c, newflag);

			std::snprintf(msgtemp0, sizeof(msgtemp0), "0x%08x.", newflag);

			msgtemp = localize(c, "Flags set to {}.", msgtemp0);
			message_send_text(c, message_type_info, c, msgtemp);
			return 0;
		}
		
		static int _handle_setdefault_command(t_connection * c, char const *text)
		{
			char const * flag_s;
			unsigned int newflag;

			std::vector<std::string> args = split_command(text, 1);
			flag_s = args[1].c_str(); // flag

			newflag = std::strtoul(flag_s, NULL, 0);
			conn_set_flags(c, 2);
			
			message_send_text(c, message_type_info, c, "Successfully set to default.");
			return 0;
		}
		
		static int _handle_tag_command(t_connection * c, char const *text)
		{
			char const * tag_s;
			t_clienttag clienttag;

			std::vector<std::string> args = split_command(text, 1);
			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, "--------------------------------------------------------");
				message_send_text(c, message_type_error, c, "Usage: /tag [gamename] (no have alias)");
				message_send_text(c, message_type_info, c, "** A debug tool for client tags.");
				return -1;
			}
			tag_s = args[1].c_str(); // flag

			if (clienttag = tag_validate_client(tag_s))
			{
				unsigned int oldflags = conn_get_flags(c);
				conn_set_clienttag(c, clienttag);
				if ((clienttag == CLIENTTAG_WARCRAFT3_UINT) || (clienttag == CLIENTTAG_WAR3XP_UINT))
					conn_update_w3_playerinfo(c);
				channel_rejoin(c);
				conn_set_flags(c, oldflags);
				channel_update_userflags(c);
				msgtemp = localize(c, "Client tag set to {}.", tag_s);
				message_send_text(c, message_type_info, c, msgtemp);
			}
			else
				msgtemp = localize(c, "Invalid clienttag {} specified!", tag_s);
			message_send_text(c, message_type_error, c, msgtemp);
			return 0;
		}
		
		static int _handle_topic_command(t_connection * c, char const * text)
		{
			std::vector<std::string> args = split_command(text, 1);
			std::string topicstr = args[1];

			
			t_channel * channel = conn_get_channel(c);
			if (channel == nullptr)
			{
				message_send_text(c, message_type_error, c, localize(c, "This command can only be used inside a channel."));
				return -1;
			}
			
			class_topic Topic;
			char const * channel_name = channel_get_name(channel);

			// set channel topic
			if (!topicstr.empty())
			{
				if ((topicstr.size() + 1) > MAX_TOPIC_LEN)
				{
					msgtemp = localize(c, "Max topic length exceeded (max {} symbols)", MAX_TOPIC_LEN);
					message_send_text(c, message_type_error, c, msgtemp);
					return -1;
				}

				if (!(account_is_operator_or_admin(conn_get_account(c), channel_name)))
				{
					msgtemp = localize(c, "You must be at least a Channel Operator of {} to set the topic", channel_name);
					message_send_text(c, message_type_error, c, msgtemp);
					return -1;
				}

				bool do_save;
				if (channel_get_permanent(channel))
					do_save = true;
				else
					do_save = false;

				Topic.set(std::string(channel_name), std::string(topicstr), do_save);
			}

			// display channel topic
			if (Topic.display(c, std::string(channel_name)) == false)
			{
				msgtemp = localize(c, "{} topic: no topic", channel_name);
				message_send_text(c, message_type_info, c, msgtemp);
			}

			return 0;
		}
		
		static int _handle_time_command(t_connection * c, char const *text)
		{
			t_bnettime  btsystem;
			t_bnettime  btlocal;
			std::time_t      now;
			struct std::tm * tmnow;

			btsystem = bnettime();

			/* Battle.net time: Wed Jun 23 15:15:29 */
			btlocal = bnettime_add_tzbias(btsystem, local_tzbias());
			now = bnettime_to_time(btlocal);
			if (!(tmnow = std::gmtime(&now)))
				std::strcpy(msgtemp0, "?");
			else
				std::strftime(msgtemp0, sizeof(msgtemp0), "%a, %d %b %Y -- %H:%M", tmnow);
			msgtemp = localize(c, "Server Time: {}", msgtemp0);
			message_send_text(c, message_type_info, c, msgtemp);
			if (conn_get_class(c) == conn_class_bnet)
			{
				btlocal = bnettime_add_tzbias(btsystem, conn_get_tzbias(c));
				now = bnettime_to_time(btlocal);
				if (!(tmnow = std::gmtime(&now)))
					std::strcpy(msgtemp0, "?");
				else
					std::strftime(msgtemp0, sizeof(msgtemp0), "%a, %d %b %Y -- %H:%M", tmnow);
				msgtemp = localize(c, "Your local time: {}", msgtemp0);
				message_send_text(c, message_type_info, c, msgtemp);
			}

			return 0;
		}
		
		static int _handle_voice_command(t_connection * c, char const * text)
		{
			char const *	username;
			char const *	channel;
			t_account *		acc;
			t_connection *	dst_c;
			int			changed = 0;

			if (!(conn_get_channel(c)) || !(channel = channel_get_name(conn_get_channel(c)))) {
				message_send_text(c, message_type_error, c, localize(c, "This command can only be used inside a channel!"));
				return -1;
			}

			if (!(account_is_operator_or_admin(conn_get_account(c), channel_get_name(conn_get_channel(c))))) {
				message_send_text(c, message_type_error, c, localize(c, "You must be at least a channel operator to use this command!"));
				return -1;
			}

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty()) {
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /voice [username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Temporarily gives voice privileges to [username]."));
				return -1;
			}
			username = args[1].c_str(); // username

			if (!(acc = accountlist_find_account(username))) {
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}
			dst_c = account_get_conn(acc);
			if (account_get_auth_voice(acc, channel) == 1) {
				msgtemp = localize(c, "User {} is already on voice list, no need to voice him!", account_get_name(acc));
				message_send_text(c, message_type_error, c, msgtemp);
			}
			else
			{
				if ((!dst_c) || conn_get_channel(c) != conn_get_channel(dst_c))
				{
					msgtemp = localize(c, "User {} must be on the same channel to voice him!", account_get_name(acc));
					message_send_text(c, message_type_error, c, msgtemp);
				}
				else
				{
					if (channel_conn_has_tmpVOICE(conn_get_channel(c), dst_c)) {
						msgtemp = localize(c, "User {} already has voice in this channel!", account_get_name(acc));
						message_send_text(c, message_type_error, c, msgtemp);
					}
					else {
						if (account_is_operator_or_admin(acc, channel)) {
							msgtemp = localize(c, "User {} is already an operator or admin!", account_get_name(acc));
							message_send_text(c, message_type_error, c, msgtemp);
						}
						else
						{
							conn_set_tmpVOICE_channel(dst_c, channel);
							msgtemp = localize(c, "User {} has been granted voice in this channel.", account_get_name(acc));
							message_send_text(c, message_type_info, c, msgtemp);
							changed = 1;
						}
					}
				}
			}

			command_set_flags(dst_c);
			return 0;
		}

		static int _handle_devoice_command(t_connection * c, char const * text)
		{
			char const *	username;
			char const *	channel;
			t_account *		acc;
			t_connection *	dst_c;
			int			done = 0;
			int			changed = 0;

			if (!(conn_get_channel(c)) || !(channel = channel_get_name(conn_get_channel(c)))) {
				message_send_text(c, message_type_error, c, localize(c, "This command can only be used inside a channel!"));
				return -1;
			}

			if (!(account_is_operator_or_admin(conn_get_account(c), channel_get_name(conn_get_channel(c))))) {
				message_send_text(c, message_type_error, c, localize(c, "You must be at least a channel operator to use this command!"));
				return -1;
			}

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty()) {
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /devoice [username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Removes [username] from the VOP list and removes temporary voice privileges."));
				return -1;
			}
			username = args[1].c_str(); // username

			if (!(acc = accountlist_find_account(username))) {
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}
			dst_c = account_get_conn(acc);

			if (account_get_auth_voice(acc, channel) == 1)
			{
				if ((account_get_auth_admin(conn_get_account(c), channel) == 1) || (account_get_auth_admin(conn_get_account(c), NULL) == 1))
				{
					account_set_auth_voice(acc, channel, 0);
					msgtemp = localize(c, "User {} has been removed from voice list.", account_get_name(acc));
					message_send_text(c, message_type_info, c, msgtemp);
					changed = 1;
				}
				else
				{
					msgtemp = localize(c, "You must be at least channel admin to remove {} from the voice list!", account_get_name(acc));
					message_send_text(c, message_type_error, c, msgtemp);
				}
				done = 1;
			}

			changed = 0;

			if ((dst_c) && channel_conn_has_tmpVOICE(conn_get_channel(c), dst_c) == 1)
			{
				conn_set_tmpVOICE_channel(dst_c, NULL);
				msgtemp = localize(c, "Voice has been taken from user {} in this channel", account_get_name(acc));
				changed = 1;
				done = 1;
			}

			message_send_text(c, message_type_info, c, msgtemp);

			if (!done)
			{
				msgtemp = localize(c, "User {} has no voice in this channel, so it can't be taken away!", account_get_name(acc));
				message_send_text(c, message_type_error, c, msgtemp);
			}

			command_set_flags(dst_c);
			return 0;
		}
		
		static int _handle_tmpop_command(t_connection * c, char const * text)
		{
			char const *	username;
			char const *	channel;
			t_account *		acc;
			t_connection *	dst_c;
			int			changed = 0;

			if (!(conn_get_channel(c)) || !(channel = channel_get_name(conn_get_channel(c)))) {
				message_send_text(c, message_type_error, c, localize(c, "This command can only be used inside a channel!"));
				return -1;
			}

			if (!(account_is_operator_or_admin(conn_get_account(c), channel_get_name(conn_get_channel(c))) || channel_conn_is_tmpOP(conn_get_channel(c), c))) {
				message_send_text(c, message_type_error, c, localize(c, "You must be at least a channel operator or tmpOP to use this command!"));
				return -1;
			}

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty()) {
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /tmpop [username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Promotes [username] to temporary channel operator."));
				return -1;
			}
			username = args[1].c_str(); // username

			if (!(acc = accountlist_find_account(username))) {
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				message_send_text(c, message_type_info, c, msgtemp);
				return -1;
			}

			dst_c = account_get_conn(acc);

			if (channel_conn_is_tmpOP(conn_get_channel(c), dst_c)) {
				msgtemp = localize(c, "User {} has already tmpOP in this channel!", username);
				message_send_text(c, message_type_error, c, msgtemp);
			}
			else
			{
				if ((!(dst_c)) || (conn_get_channel(c) != conn_get_channel(dst_c))) {
					msgtemp = localize(c, "User {} must be on the same channel to tempOP him!", username);
					message_send_text(c, message_type_error, c, msgtemp);
				}
				else
				{
					if (account_is_operator_or_admin(acc, channel)) {
						msgtemp = localize(c, "User {} already is operator or admin, no need to tempOP him!", username);
						message_send_text(c, message_type_error, c, msgtemp);
					}
					else
					{
						conn_set_tmpOP_channel(dst_c, channel);
						msgtemp = localize(c, "User {} has been promoted to tmpOP in this channel.", username);
						message_send_text(c, message_type_info, c, msgtemp);
						changed = 1;
					}
				}
			}

			command_set_flags(dst_c);
			return 0;
		}
		
		static int _handle_deop_command(t_connection * c, char const * text)
		{
			char const *	username;
			char const *	channel;
			t_account *		acc;
			int			OP_lvl;
			t_connection *	dst_c;
			int			done = 0;

			if (!(conn_get_channel(c)) || !(channel = channel_get_name(conn_get_channel(c)))) {
				message_send_text(c, message_type_error, c, localize(c, "This command can only be used inside a channel."));
				return -1;
			}

			acc = conn_get_account(c);
			OP_lvl = 0;

			if (account_is_operator_or_admin(acc, channel))
				OP_lvl = 1;
			else if (channel_conn_is_tmpOP(conn_get_channel(c), account_get_conn(acc)))
				OP_lvl = 2;

			if (OP_lvl == 0)
			{
				message_send_text(c, message_type_error, c, localize(c, "You must be at least a Channel Operator or tempOP to use this command."));
				return -1;
			}

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty()) {
				describe_command(c, args[0].c_str());
				return -1;
			}
			username = args[1].c_str(); // username


			if (!(acc = accountlist_find_account(username))) {
				msgtemp = localize(c, "There's no account with username {}.", username);
				message_send_text(c, message_type_info, c, msgtemp);
				return -1;
			}

			dst_c = account_get_conn(acc);

			if (OP_lvl == 1) // user is real OP and allowed to deOP
			{
				if (account_get_auth_admin(acc, channel) == 1 || account_get_auth_operator(acc, channel) == 1) {
					if (account_get_auth_admin(acc, channel) == 1) {
						if (account_get_auth_admin(conn_get_account(c), channel) != 1 && account_get_auth_admin(conn_get_account(c), NULL) != 1)
							message_send_text(c, message_type_info, c, localize(c, "You must be at least a Channel Admin to demote another Channel Admin"));
						else {
							account_set_auth_admin(acc, channel, 0);
							msgtemp = localize(c, "{} has been demoted from a Channel Admin.", username);
							message_send_text(c, message_type_info, c, msgtemp);
							if (dst_c)
							{
								msgtemp2 = localize(c, "{} has demoted you from a Channel Admin of channel \"{}\"", conn_get_loggeduser(c), channel);
								message_send_text(dst_c, message_type_info, c, msgtemp2);
							}
						}
					}
					if (account_get_auth_operator(acc, channel) == 1) {
						account_set_auth_operator(acc, channel, 0);
						msgtemp = localize(c, "{} has been demoted from a Channel Operator", username);
						message_send_text(c, message_type_info, c, msgtemp);
						if (dst_c)
						{
							msgtemp2 = localize(c, "{} has demoted you from a Channel Operator of channel \"{}\"", conn_get_loggeduser(c), channel);
							message_send_text(dst_c, message_type_info, c, msgtemp2);
						}
					}
					done = 1;
				}
				if ((dst_c) && channel_conn_is_tmpOP(conn_get_channel(c), dst_c))
				{
					conn_set_tmpOP_channel(dst_c, NULL);
					msgtemp = localize(c, "{} has been demoted from a tempOP of this channel", username);
					message_send_text(c, message_type_info, c, msgtemp);
					if (dst_c)
					{
						msgtemp2 = localize(c, "{} has demoted you from a tmpOP of channel \"{}\"", conn_get_loggeduser(c), channel);
						message_send_text(dst_c, message_type_info, c, msgtemp2);
					}
					done = 1;
				}
				if (!done) {
					msgtemp = localize(c, "{} is no Channel Admin or Channel Operator or tempOP, so you can't demote him.", username);
					message_send_text(c, message_type_info, c, msgtemp);
				}
			}
			else //user is just a tempOP and may only deOP other tempOPs
			{
				if (dst_c && channel_conn_is_tmpOP(conn_get_channel(c), dst_c))
				{
					conn_set_tmpOP_channel(account_get_conn(acc), NULL);
					msgtemp = localize(c, "{} has been demoted from a tempOP of this channel", username);
					message_send_text(c, message_type_info, c, msgtemp);
					msgtemp2 = localize(c, "{} has demoted you from a tempOP of channel \"{}\"", conn_get_loggeduser(c), channel);
					if (dst_c) message_send_text(dst_c, message_type_info, c, msgtemp2);
				}
				else
				{
					msgtemp = localize(c, "{} is no tempOP in this channel, so you can't demote him", username);
					message_send_text(c, message_type_info, c, msgtemp);
				}
			}

			command_set_flags(connlist_find_connection_by_accountname(username));
			return 0;
		}
		
		static int _handle_servertime_command(t_connection * c, char const *text)
		{
			t_bnettime  btsystem;
			t_bnettime  btlocal;
			std::time_t      now;
			struct std::tm * tmnow;

			btsystem = bnettime();

			/* Battle.net time: Wed Jun 23 15:15:29 */
			btlocal = bnettime_add_tzbias(btsystem, local_tzbias());
			now = bnettime_to_time(btlocal);
			if (!(tmnow = std::gmtime(&now)))
				std::strcpy(msgtemp0, "?");
			else
				std::strftime(msgtemp0, sizeof(msgtemp0), "%a, %d %b %Y -- %H:%M", tmnow);
			msgtemp = localize(c, "Time: {}", msgtemp0);
			message_send_text(c, message_type_error, c, msgtemp);

			return 0;
		}
		
		static int _handle_ping_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *	user;
			t_game 	*	game;

			std::vector<std::string> args = split_command(text, 1);
			text = args[1].c_str(); // username

			if (text[0] == '\0')
			{
				if ((game = conn_get_game(c)))
				{
					for (i = 0; i < game_get_count(game); i++)
					{
						if ((user = game_get_player_conn(game, i)))
						{
							_handle_servertime_command(c, text);
							msgtemp = localize(c, "Your latency {}ms", conn_get_latency(c));
							message_send_text(c, message_type_info, c, msgtemp);
							message_send_text(c, message_type_info, c, "Thanks for playing in Battlenet.");
						}
					}
					return 0;
				}
				
				_handle_servertime_command(c, text);
				msgtemp = localize(c, "Your latency {}ms", conn_get_latency(c));
				message_send_text(c, message_type_info, c, msgtemp);
				message_send_text(c, message_type_info, c, "Thanks for playing in Battlenet.");
			}
			else if ((user = connlist_find_connection_by_accountname(text))) {
				_handle_servertime_command(c, text);
				msgtemp = localize(c, "{} latency {}ms", conn_get_username(user), conn_get_latency(user));
				message_send_text(c, message_type_info, c, msgtemp);
				message_send_text(c, message_type_info, c, "Thanks for playing in Battlenet.");
			}
			else
			{
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}

			return 0;
		}
		
		static int _handle_me_command(t_connection * c, char const * text)
		{
			t_channel const * channel;

			if (!(channel = conn_get_channel(c)))
			{
				message_send_text(c, message_type_error, c, localize(c, "You are not in a channel!"));
				return -1;
			}
			
			std::vector<std::string> args = split_command(text, 1);
			
			if (args[1].empty()) {
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage:/me [message] (alias: /emote)"));
				message_send_text(c, message_type_info, c, localize(c, "** Displays your name and [message] in a different color."));
				return -1;
			}
			text = args[1].c_str(); // message

			if (!conn_quota_exceeded(c, text))
				channel_message_send(channel, message_type_emote, c, text);
			return 0;
		}

		static int _handle_whisper_command(t_connection * c, char const *text)
		{
			char const * username; /* both include NUL, so no need to add one for middle @ or * */

			std::vector<std::string> args = split_command(text, 2);

			if (args[2].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /whisper [username] [message] (aliases: /w /m /msg)"));
				message_send_text(c, message_type_info, c, localize(c, "** Sends a private [message] to [username]."));
				return -1;
			}
			username = args[1].c_str(); // username
			text = args[2].c_str(); // message

			do_whisper(c, username, text);

			return 0;
		}
		
		static int _handle_whois_command(t_connection * c, char const * text)
		{
			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, "--------------------------------------------------------");
				message_send_text(c, message_type_error, c, "Usage: /whois <player> (aliases: /where /whereis)");
				message_send_text(c, message_type_info, c, "** Displays where a <player> is on the server.");
				return -1;
			}
			text = args[1].c_str(); // username

			do_whois(c, text);

			return 0;
		}
		
		static int _handle_kick_command(t_connection * c, char const *text)
		{
			char const * username;
			t_channel const * channel;
			t_connection *    kuc;
			t_account *	    acc;

			std::vector<std::string> args = split_command(text, 2);

			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /kick [username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Kicks [username] from the channel."));
				return -1;
			}
			username = args[1].c_str(); // username
			text = args[2].c_str(); // reason

			if (!(channel = conn_get_channel(c)))
			{
				message_send_text(c, message_type_error, c, localize(c, "This command can only be used inside a channel!"));
				return -1;
			}

			acc = conn_get_account(c);
			if (account_get_auth_admin(acc, NULL) != 1 && /* default to false */
				account_get_auth_admin(acc, channel_get_name(channel)) != 1 && /* default to false */
				account_get_auth_operator(acc, NULL) != 1 && /* default to false */
				account_get_auth_operator(acc, channel_get_name(channel)) != 1 && /* default to false */
				!channel_conn_is_tmpOP(channel, account_get_conn(acc)))
			{
				message_send_text(c, message_type_error, c, localize(c, "You have to be at least a channel operator or tempOP to use this command!"));
				return -1;
			}
			if (!(kuc = connlist_find_connection_by_accountname(username)))
			{
				message_send_text(c, message_type_error, c, localize(c, "That user is not logged in!"));
				return -1;
			}
			if (conn_get_channel(kuc) != channel)
			{
				message_send_text(c, message_type_error, c, localize(c, "That user is not in this channel!"));
				return -1;
			}
			if (account_get_auth_admin(conn_get_account(kuc), NULL) == 1 ||
				account_get_auth_admin(conn_get_account(kuc), channel_get_name(channel)) == 1)
			{
				message_send_text(c, message_type_error, c, localize(c, "You cannot kick administrators!"));
				return -1;
			}
			else if (account_get_auth_operator(conn_get_account(kuc), NULL) == 1 ||
				account_get_auth_operator(conn_get_account(kuc), channel_get_name(channel)) == 1)
			{
				message_send_text(c, message_type_error, c, localize(c, "You cannot kick operators!"));
				return -1;
			}

			{
				char const * tname1;
				char const * tname2;

				tname1 = conn_get_loggeduser(kuc);
				tname2 = conn_get_loggeduser(c);
				if (!tname1 || !tname2) {
					eventlog(eventlog_level_error, __FUNCTION__, "got NULL username");
					return -1;
				}

				if (text[0] != '\0')
					msgtemp = localize(c, "User {} has been kicked by {} ({}).", tname1, tname2, text);
				else
					msgtemp = localize(c, "User {} has been kicked by {}.", tname1, tname2);
				channel_message_send(channel, message_type_info, c, msgtemp.c_str());
			}
			conn_kick_channel(kuc, "Bye");
			if (conn_get_class(kuc) == conn_class_bnet)
				conn_set_channel(kuc, CHANNEL_NAME_KICKED); /* should not fail */

			return 0;
		}
		
		static int _handle_reply_command(t_connection * c, char const *text)
		{
			char const * dest;

			if (!(dest = conn_get_lastsender(c)))
			{
				message_send_text(c, message_type_error, c, localize(c, "No one messaged you, use /m instead"));
				return -1;
			}

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /reply <message> (alias: /r)"));
				message_send_text(c, message_type_info, c, localize(c, "** Replies to the last player who whispered you with [message]."));
				return -1;
			}
			text = args[1].c_str(); // message

			do_whisper(c, dest, text);
			return 0;
		}
		
		static int _handle_netinfo_command(t_connection * c, char const *text)
		{
			char const * username;
			t_connection * conn;
			t_game const * game;
			unsigned int   addr;
			unsigned short port;
			unsigned int   taddr;
			unsigned short tport;

			std::vector<std::string> args = split_command(text, 1);
			username = args[1].c_str(); // username

			if (username[0] == '\0')
				username = conn_get_username(c);

			if (!(conn = connlist_find_connection_by_accountname(username)))
			{
				message_send_text(c, message_type_error, c, localize(c, "That user is not logged on!"));
				return -1;
			}

			if (conn_get_account(conn) != conn_get_account(c) &&
				prefs_get_hide_addr() && !(account_get_command_groups(conn_get_account(c)) & command_get_group("/admin-addr"))) // default to false
			{
				message_send_text(c, message_type_error, c, localize(c, "Address information for other users is only available to admins."));
				return -1;
			}

			taddr = addr = conn_get_game_addr(conn);
			tport = port = conn_get_game_port(conn);
			trans_net(conn_get_addr(c), &taddr, &tport);

			if (taddr == addr && tport == port)
				msgtemp = localize(c, "Network ID: {}",
				addr_num_to_addr_str(addr, port));
			else
				msgtemp = localize(c, "Network ID: {} (trans {})",
				addr_num_to_addr_str(addr, port),
				addr_num_to_addr_str(taddr, tport));
			message_send_text(c, message_type_info, c, msgtemp);

			return 0;
		}

		static int _handle_ipscan_command(t_connection * c, char const * text)
		{
			t_account * account;
			t_connection * conn;

			std::vector<std::string> args = split_command(text, 1);
			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /ipscan [name or IP-address] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Finds all currently logged in users with the given [name] or [IP-address]."));
				return -1;
			}
			text = args[1].c_str(); // ip or username

			std::string ip;
			if (account = accountlist_find_account(text))
			{
				conn = account_get_conn(account);
				if (conn)
				{
					// conn_get_addr returns int, so there can never be a NULL string construct
					ip = addr_num_to_ip_str(conn_get_addr(conn));
				}
				else
				{
					message_send_text(c, message_type_info, c, localize(c, "Warning: That user is not online, using last known address."));
					ip = account_get_ll_ip(account);
					if (ip.empty())
					{
						message_send_text(c, message_type_error, c, localize(c, "Sorry, no IP address could be retrieved!"));
						return 0;
					}
				}
			}
			else
			{
				ip = text;
			}

			message_send_text(c, message_type_error, c, localize(c, "Scanning online users for IP {}:", ip));

			t_elem const * curr;
			int count = 0;
			LIST_TRAVERSE_CONST(connlist(), curr) {
				conn = (t_connection *)elem_get_data(curr);
				if (!conn) {
					// got empty element
					continue;
				}

				if (ip.compare(addr_num_to_ip_str(conn_get_addr(conn))) == 0)
				{
					std::snprintf(msgtemp0, sizeof(msgtemp0), "[+] %s", conn_get_loggeduser(conn));
					message_send_text(c, message_type_info, c, msgtemp0);
					count++;
				}
			}

			if (count == 0) {
				message_send_text(c, message_type_error, c, localize(c, "There are no online users with that IP address!"));
			}

			return 0;
		}
		
		static int _handle_addacct_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_account  * temp;
			t_hash       passhash;
			char const * username, *pass;

			std::vector<std::string> args = split_command(text, 2);

			if (args[2].empty())
			{
				message_send_text(c, message_type_info, c, "--------------------------------------------------------");
				message_send_text(c, message_type_error, c, "Usage: /addacct [username] [password] (no have alias)");
				message_send_text(c, message_type_info, c, "** Creates a new account named [username] with password [password].");
				return -1;
			}
			username = args[1].c_str(); // username

			if (args[2].length() > MAX_USERPASS_LEN)
			{
				msgtemp = localize(c, "Maximum password length allowed is {}", MAX_USERPASS_LEN);
				message_send_text(c, message_type_error, c, msgtemp);
				return -1;
			}
			for (i = 0; i < args[2].length(); i++)
				args[2][i] = safe_tolower(args[2][i]);
			pass = args[2].c_str(); // password


			bnet_hash(&passhash, std::strlen(pass), pass);

			msgtemp = localize(c, "Trying to add account \"{}\" with password \"{}\"", username, pass);
			message_send_text(c, message_type_info, c, msgtemp);

			temp = accountlist_create_account(username, hash_get_str(passhash));
			if (!temp) {
				message_send_text(c, message_type_error, c, localize(c, "Failed to create account!"));
				eventlog(eventlog_level_debug, __FUNCTION__, "[{}] account \"{}\" not created (failed)", conn_get_socket(c), username);
				return -1;
			}

			msgtemp = localize(c, "Account {} created.", account_get_uid(temp));
			message_send_text(c, message_type_info, c, msgtemp);
			eventlog(eventlog_level_debug, __FUNCTION__, "[{}] account \"{}\" created", conn_get_socket(c), username);

			return 0;
		}

		static int _handle_chpass_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_account  * account;
			t_account  * temp;
			t_hash       passhash;
			char const * username;
			std::string       pass;

			std::vector<std::string> args = split_command(text, 2);

			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /chpass [password] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Changes your password to [password]."));
				return -1;
			}

			if (args[2].empty())
			{
				username = conn_get_username(c);
				pass = args[1];
			}
			else
			{
				username = args[1].c_str();
				pass = args[2];
			}

			temp = accountlist_find_account(username);

			account = conn_get_account(c);

			if ((temp == account && account_get_auth_changepass(account) == 0) || /* default to true */
				(temp != account && !(account_get_command_groups(conn_get_account(c)) & command_get_group("/admin-chpass")))) /* default to false */
			{
				eventlog(eventlog_level_info, __FUNCTION__, "[{}] password change for \"{}\" refused (no change access)", conn_get_socket(c), username);
				message_send_text(c, message_type_error, c, localize(c, "Only admins may change passwords for other accounts!"));
				return -1;
			}

			if (!temp)
			{
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}

			if (pass.length() > MAX_USERPASS_LEN)
			{
				msgtemp = localize(c, "Maximum password length allowed is {}", MAX_USERPASS_LEN);
				message_send_text(c, message_type_error, c, msgtemp);
				return -1;
			}

			for (i = 0; i < pass.length(); i++)
				pass[i] = safe_tolower(pass[i]);

			bnet_hash(&passhash, pass.length(), pass.c_str());

			msgtemp = localize(c, "Trying to change password for account \"{}\" to \"{}\"", username, pass.c_str());
			message_send_text(c, message_type_info, c, msgtemp);

			if (account_set_pass(temp, hash_get_str(passhash)) < 0)
			{
				message_send_text(c, message_type_error, c, localize(c, "Unable to set password!"));
				return -1;
			}

			if (account_get_auth_admin(account, NULL) == 1 ||
				account_get_auth_operator(account, NULL) == 1) {
				msgtemp = localize(c, "Password for account {} updated.", account_get_uid(temp));
				message_send_text(c, message_type_info, c, msgtemp);
			}
			else {
				msgtemp = localize(c, "Password for account {} updated.", username);
				message_send_text(c, message_type_info, c, msgtemp);
			}

			return 0;
		}
		
		static int _handle_set_command(t_connection * c, char const *text)
		{
			t_account * account;
			char const * username, *key, *value;

			std::vector<std::string> args = split_command(text, 3);
			if (args[2].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /set [username] [key] [value] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Sets or returns the value of [key] for account [username]."));
				message_send_text(c, message_type_info, c, localize(c, "** Set [value] = null to erase value."));
				return -1;
			}
			username = args[1].c_str(); // username
			key = args[2].c_str(); // key
			value = args[3].c_str(); // value


			// disallow get/set value for password hash and username (hash can be cracked easily, account name should be permanent)
			if (strcasecmp(key, "bnet\\acct\\passhash1") == 0 || strcasecmp(key, "bnet\\acct\\username") == 0 || strcasecmp(key, "bnet\\username") == 0)
			{
				message_send_text(c, message_type_info, c, localize(c, "Access denied due to security reasons."));
				return -1;
			}

			if (!(account = accountlist_find_account(username)))
			{
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}

			if (*value == '\0')
			{
				if (account_get_strattr(account, key))
				{
					msgtemp = localize(c, "Current value of {} is \"{}\"", key, account_get_strattr(account, key));
					message_send_text(c, message_type_error, c, msgtemp);
				}
				else
					message_send_text(c, message_type_error, c, localize(c, "Value currently not set!"));
				return 0;
			}

			// unset value
			if (strcasecmp(value, "null") == 0)
				value = NULL;

			std::sprintf(msgtemp0, " \"%.64s\" (%.128s = \"%.128s\")", account_get_name(account), key, value);

			if (account_set_strattr(account, key, value) < 0)
			{
				msgtemp = localize(c, "Unable to set key for");
				msgtemp += msgtemp0;
				message_send_text(c, message_type_error, c, msgtemp);
			}
			else
			{
				msgtemp = localize(c, "Key set successfully for");
				msgtemp += msgtemp0;
				message_send_text(c, message_type_info, c, msgtemp);
				eventlog(eventlog_level_warn, __FUNCTION__, "Key set by \"{}\" for {}", account_get_name(conn_get_account(c)),msgtemp0);
			}
			return 0;
		}

		static int _handle_commandgroups_command(t_connection * c, char const * text)
		{
			t_account *	account;
			char const *	command, *username;

			unsigned int usergroups;	// from user account
			unsigned int groups = 0;	// converted from arg3
			char	tempgroups[9];	// converted from usergroups

			std::vector<std::string> args = split_command(text, 3);
			// display help if [list] without [username], or not [list] without [groups]
			if ( ((args[1] == "list" || args[1] == "l") && args[2].empty()) 
				|| (!(args[1] == "list" || args[1] == "l") && args[3].empty()) )
			{
				message_send_text(c, message_type_info, c, "--------------------------------------------------------");
				message_send_text(c, message_type_error, c, "Usage: /cg list [username] (no have alias)");
				message_send_text(c, message_type_info, c, "** Displays [username] command groups.");
				message_send_text(c, message_type_error, c, "Usage: /cg add [username] [group(s) (no have alias)");
				message_send_text(c, message_type_info, c, "** Adds command group(s)to [username].");
				message_send_text(c, message_type_error, c, "Usage: /cg del [username] group(s) (no have alias)");
				message_send_text(c, message_type_info, c, "** Deletes command group(s) from [username].");
				return -1;
			}
			command = args[1].c_str(); // command
			username = args[2].c_str(); // username
			//args[3]; // groups


			if (!(account = accountlist_find_account(username))) {
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}

			usergroups = account_get_command_groups(account);

			if (!std::strcmp(command, "list") || !std::strcmp(command, "l")) {
				if (usergroups & 1) tempgroups[0] = '1'; else tempgroups[0] = ' ';
				if (usergroups & 2) tempgroups[1] = '2'; else tempgroups[1] = ' ';
				if (usergroups & 4) tempgroups[2] = '3'; else tempgroups[2] = ' ';
				if (usergroups & 8) tempgroups[3] = '4'; else tempgroups[3] = ' ';
				if (usergroups & 16) tempgroups[4] = '5'; else tempgroups[4] = ' ';
				if (usergroups & 32) tempgroups[5] = '6'; else tempgroups[5] = ' ';
				if (usergroups & 64) tempgroups[6] = '7'; else tempgroups[6] = ' ';
				if (usergroups & 128) tempgroups[7] = '8'; else tempgroups[7] = ' ';
				tempgroups[8] = '\0';
				msgtemp = localize(c, "{}'s command group(s): {}", username, tempgroups);
				message_send_text(c, message_type_info, c, msgtemp);
				return 0;
			}

			// iterate chars in string
			for (std::string::iterator g = args[3].begin(); g != args[3].end(); ++g) {
				if (*g == '1') groups |= 1;
				else if (*g == '2') groups |= 2;
				else if (*g == '3') groups |= 4;
				else if (*g == '4') groups |= 8;
				else if (*g == '5') groups |= 16;
				else if (*g == '6') groups |= 32;
				else if (*g == '7') groups |= 64;
				else if (*g == '8') groups |= 128;
				else {
					msgtemp = localize(c, "Got bad group: {}", *g);
					message_send_text(c, message_type_info, c, msgtemp);
					return -1;
				}
			}

			if (!std::strcmp(command, "add") || !std::strcmp(command, "a")) {
				account_set_command_groups(account, usergroups | groups);
				msgtemp = localize(c, "Groups {} has been added to {}", args[3].c_str(), username);
				message_send_text(c, message_type_info, c, msgtemp);
				return 0;
			}

			if (!std::strcmp(command, "del") || !std::strcmp(command, "d")) {
				account_set_command_groups(account, usergroups & (255 - groups));
				msgtemp = localize(c, "Groups {} has been deleted from {}", args[3].c_str(), username);
				message_send_text(c, message_type_info, c, msgtemp);
				return 0;
			}

			msgtemp = localize(c, "Got unknown command: {}", command);
			message_send_text(c, message_type_error, c, msgtemp);
			return -1;
		}
		
		static int _handle_who_command(t_connection * c, char const *text)
		{
			t_connection const * conn;
			t_channel const *    channel;
			unsigned int         i;
			char const *         tname;

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /who [channel] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Displays a list of users in [channel]."));
				return -1;
			}
			char const * cname = args[1].c_str(); // channel name


			if (!(channel = channellist_find_channel_by_name(cname, conn_get_country(c), realm_get_name(conn_get_realm(c)))))
			{
				message_send_text(c, message_type_error, c, localize(c, "That channel does not exist!"));
				return -1;
			}
			if (channel_check_banning(channel, c) == 1)
			{
				message_send_text(c, message_type_error, c, localize(c, "You are banned from that channel!"));
				return -1;
			}

			std::snprintf(msgtemp0, sizeof msgtemp0, "%s", localize(c, "Users in channel {}:", cname).c_str());
			i = std::strlen(msgtemp0);
			for (conn = channel_get_first(channel); conn; conn = channel_get_next())
			{
				if (i + std::strlen((tname = conn_get_username(conn))) + 2 > sizeof(msgtemp0)) /* " ", name, '\0' */
				{
					message_send_text(c, message_type_info, c, msgtemp0);
					i = 0;
				}
				std::sprintf(&msgtemp0[i], " %s", tname);
				i += std::strlen(&msgtemp0[i]);
			}
			if (i > 0)
				message_send_text(c, message_type_info, c, msgtemp0);

			return 0;
		}

		static int _handle_whoami_command(t_connection * c, char const *text)
		{
			char const * tname;

			if (!(tname = conn_get_username(c)))
			{
				message_send_text(c, message_type_error, c, localize(c, "Unable to obtain your account name."));
				return -1;
			}

			do_whois(c, tname);

			return 0;
		}
		
		static int _handle_botannounce_command(t_connection * c, char const *text)
		{
			t_message *  message;

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, "--------------------------------------------------------");
				message_send_text(c, message_type_error, c, "Usage: /botannounce [message] (no have alias)");
				message_send_text(c, message_type_info, c, "** Announces [message] to everyone.");
				return -1;
			}
			text = args[1].c_str(); // message

			msgtemp = localize(c, "Announcement: {}", text);
			if (!(message = message_create(message_type_broadcast, c, msgtemp.c_str())))
				message_send_text(c, message_type_info, c, localize(c, "Could not broadcast message."));
			else
			{
				if (message_send_all(message) < 0)
					message_send_text(c, message_type_info, c, localize(c, "Could not broadcast message."));
				message_destroy(message);
			}

			return 0;
		}
		
		static int _handle_nullannounceblue_command(t_connection * c, char const *text)
		{
			t_message *  message;

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, "--------------------------------------------------------");
				message_send_text(c, message_type_error, c, "Usage: /nullannounceblue [message] (no have alias)");
				message_send_text(c, message_type_info, c, "** Announces [message] to everyone.");
				return -1;
			}
			text = args[1].c_str(); // message

			msgtemp = localize(c, "{}", text);
			if (!(message = message_create(message_type_info, c, msgtemp.c_str())))
				message_send_text(c, message_type_info, c, localize(c, "Could not broadcast message."));
			else
			{
				if (message_send_all(message) < 0)
					message_send_text(c, message_type_info, c, localize(c, "Could not broadcast message."));
				message_destroy(message);
			}

			return 0;
		}
		
		static int _handle_nullannouncered_command(t_connection * c, char const *text)
		{
			t_message *  message;

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, "--------------------------------------------------------");
				message_send_text(c, message_type_error, c, "Usage: /nullannouncered [message] (no have alias)");
				message_send_text(c, message_type_info, c, "** Announces [message] to everyone.");
				return -1;
			}
			text = args[1].c_str(); // message

			msgtemp = localize(c, "{}", text);
			if (!(message = message_create(message_type_error, c, msgtemp.c_str())))
				message_send_text(c, message_type_info, c, localize(c, "Could not broadcast message."));
			else
			{
				if (message_send_all(message) < 0)
					message_send_text(c, message_type_info, c, localize(c, "Could not broadcast message."));
				message_destroy(message);
			}

			return 0;
		}
		
		static int _handle_nullannouncegreen_command(t_connection * c, char const *text)
		{
			t_message *  message;

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, "--------------------------------------------------------");
				message_send_text(c, message_type_error, c, "Usage: /nullannouncegreen [message] (no have alias)");
				message_send_text(c, message_type_info, c, "** Announces [message] to everyone.");
				return -1;
			}
			text = args[1].c_str(); // message

			msgtemp = localize(c, "{}", text);
			if (!(message = message_create(message_type_whisper, c, msgtemp.c_str())))
				message_send_text(c, message_type_info, c, localize(c, "Could not broadcast message."));
			else
			{
				if (message_send_all(message) < 0)
					message_send_text(c, message_type_info, c, localize(c, "Could not broadcast message."));
				message_destroy(message);
			}

			return 0;
		}
		
		static int _handle_kill_command(t_connection * c, char const *text)
		{
			t_connection *	user;
			char const * username, * min;

			std::vector<std::string> args = split_command(text, 2);
			username = args[1].c_str(); // username
			min = args[2].c_str(); // minutes of ban

			if (username[0] == '\0' || (username[0] == '#' && (username[1] < '0' || username[1] > '9')))
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /kill {<username>|#<socket>} [min] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Disconnects [username] from the server."));
				return -1;
			}

			if (username[0] == '#') {
				if (!(user = connlist_find_connection_by_socket(std::atoi(username + 1)))) {
					message_send_text(c, message_type_error, c, localize(c, "That connection doesn't exist!"));
					return -1;
				}
			}
			else {
				if (!(user = connlist_find_connection_by_accountname(username))) {
					message_send_text(c, message_type_error, c, localize(c, "That user is not logged in?"));
					return -1;
				}
			}

			if (min[0] != '\0' && ipbanlist_add(c, addr_num_to_ip_str(conn_get_addr(user)), ipbanlist_str_to_time_t(c, min)) == 0)
			{
				ipbanlist_save(prefs_get_ipbanfile());
				message_send_text(user, message_type_info, user, localize(c, "An admin has closed your connection and banned your IP address."));
			}
			else
				conn_set_state(user, conn_state_destroy);

			message_send_text(c, message_type_info, c, localize(c, "Operation successful."));

			return 0;
		}

		static int _handle_serverban_command(t_connection *c, char const *text)
		{
			char const * username;
			t_connection * dest_c;

			std::vector<std::string> args = split_command(text, 1);
			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /kill {<username>|#<socket>} [min] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Disconnects [username] from the server."));
				return -1;
			}
			username = args[1].c_str(); // username

			if (!(dest_c = connlist_find_connection_by_accountname(username)))
			{
				message_send_text(c, message_type_error, c, localize(c, "That user is not logged on."));
				return -1;
			}
			msgtemp = localize(c, "Banning {} who is using IP address {}", conn_get_username(dest_c), addr_num_to_ip_str(conn_get_game_addr(dest_c)));
			message_send_text(c, message_type_info, c, msgtemp);
			message_send_text(c, message_type_info, c, localize(c, "User's account is also LOCKED! Only an admin can unlock it!"));
			msgtemp = localize(c, "/ipban a {}", addr_num_to_ip_str(conn_get_game_addr(dest_c)));
			handle_ipban_command(c, msgtemp.c_str());
			account_set_auth_lock(conn_get_account(dest_c), 1);
			//now kill the connection
			msgtemp = localize(c, "You have been banned by Admin: {}", conn_get_username(c));
			message_send_text(dest_c, message_type_error, dest_c, msgtemp);
			message_send_text(dest_c, message_type_error, dest_c, localize(c, "Your account is also LOCKED! Only an admin can UNLOCK it!"));
			conn_set_state(dest_c, conn_state_destroy);
			return 0;
		}
		
		static int _handle_finger_command(t_connection * c, char const *text)
		{
			char const * dest;
			t_account *    account;
			t_connection * conn;
			char *         tok;
			t_clanmember * clanmemb;
			std::time_t      then;
			struct std::tm * tmthen;

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty())
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /finger [username] (no have alias)"));
				message_send_text(c, message_type_info, c, localize(c, "** Displays detailed information about [username]."));
				return -1;
			}
			dest = args[1].c_str(); // username;

			if (!(account = accountlist_find_account(dest)))
			{
				message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
				return -1;
			}

			then = account_get_ll_ctime(account);
			tmthen = std::localtime(&then); /* FIXME: determine user's timezone */
			
			if ((conn = connlist_find_connection_by_accountname(dest))) { }

			message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
			std::strftime(msgtemp0, sizeof(msgtemp0), "%a, %d %b %Y -- %H:%M", tmthen);
			msgtemp = localize(c, "Created: {}", msgtemp0);
			message_send_text(c, message_type_info, c, msgtemp);
			
			if ((account_get_command_groups(conn_get_account(c)) & command_get_group("/admin-addr")))
			{
				msgtemp = localize(c, "Email: {}", account_get_email(account));
				message_send_text(c, message_type_info, c, msgtemp);
			}
			
			msgtemp = localize(c, "Username: {}", account_get_name(account));
			message_send_text(c, message_type_info, c, msgtemp);
			msgtemp = localize(c, "Class: {}", account_get_auth_class(account));
			message_send_text(c, message_type_info, c, msgtemp);

			if ((clanmemb = account_get_clanmember(account)))
			{
				t_clan *	 clan;
				char	 status;

				if ((clan = clanmember_get_clan(clanmemb)))
				{
					msgtemp = localize(c, "Clan: {}", clan_get_name(clan));
					message_send_text(c, message_type_info, c, msgtemp);
				}
			}

			

			/* check /admin-addr for admin privileges */
			if ((account_get_command_groups(conn_get_account(c)) & command_get_group("/admin-addr")))
			{
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				std::string yes = localize(c, "Yes");
				std::string no = localize(c, "No");
				/* the player who requested /finger has admin privileges
				give him more info about the one he queries;
				is_admin, is_operator, is_locked, email */
				msgtemp = localize(c, "[+] Admin: {}", account_get_auth_admin(account, NULL) == 1 ? yes : no);
				message_send_text(c, message_type_info, c, msgtemp);
				msgtemp = localize(c, "[+] Operator: {}", account_get_auth_operator(account, NULL) == 1 ? yes : no);
				message_send_text(c, message_type_info, c, msgtemp);
				msgtemp = localize(c, "[+] Banned: {}", account_get_auth_lock(account) == 1 ? yes : no);
				message_send_text(c, message_type_info, c, msgtemp);
				msgtemp = localize(c, "[+] Muted: {}", account_get_auth_mute(account) == 1 ? yes : no);
				message_send_text(c, message_type_info, c, msgtemp);
				msgtemp = localize(c, "[+] Last login Owner: {}", account_get_ll_owner(account));
				message_send_text(c, message_type_info, c, msgtemp);
			}
			
			message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
			
			const char* const ip_tmp = account_get_ll_ip(account);
			std::string ip(ip_tmp ? ip_tmp : "");
			if (ip.empty() == true ||
				!(account_get_command_groups(conn_get_account(c)) & command_get_group("/admin-addr"))) /* default to false */
				ip = localize(c, " ");

			{

				then = account_get_ll_time(account);
				tmthen = std::localtime(&then); /* FIXME: determine user's timezone */
				if (tmthen)
					std::strftime(msgtemp0, sizeof(msgtemp0), "%a, %d %b %Y -- %H:%M", tmthen);
				else
					std::strcpy(msgtemp0, "?");

				if (!(conn))
					msgtemp = localize(c, "Last login {} * ", msgtemp0);
				else
					msgtemp = localize(c, "On since {} * ", msgtemp0);
			}
			msgtemp += ip;
			message_send_text(c, message_type_info, c, msgtemp);
			
			if (conn)
			{
				msgtemp = localize(c, "Idle {}", seconds_to_timestr(conn_get_idletime(conn)));
				message_send_text(c, message_type_info, c, msgtemp);
			}

			return 0;
		}

		static int _handle_moderate_command(t_connection * c, char const * text)
		{
			unsigned oldflags;
			t_channel * channel;

			if (!(channel = conn_get_channel(c))) {
				message_send_text(c, message_type_error, c, localize(c, "This command can only be used inside a channel!"));
				return -1;
			}

			if (!(account_is_operator_or_admin(conn_get_account(c), channel_get_name(channel)))) {
				message_send_text(c, message_type_error, c, localize(c, "You must be at least a channel operator to use this command!"));
				return -1;
			}

			oldflags = channel_get_flags(channel);

			if (channel_set_flags(channel, oldflags ^ channel_flags_moderated)) {
				eventlog(eventlog_level_error, __FUNCTION__, "could not set channel {} flags", channel_get_name(channel));
				message_send_text(c, message_type_error, c, localize(c, "Unable to change channel flags!"));
				return -1;
			}
			else {
				if (oldflags & channel_flags_moderated)
					channel_message_send(channel, message_type_info, c, localize(c, "Channel is now unmoderated.").c_str());
				else
					channel_message_send(channel, message_type_info, c, localize(c, "Channel is now moderated.").c_str());
			}

			return 0;
		}
		
		static int _handle_friends_command(t_connection * c, char const * text)
		{
			int i;
			t_account *my_acc = conn_get_account(c);

			std::vector<std::string> args = split_command(text, 2);

			if (args[1] == "add" || args[1] == "a") {
				t_packet 	* rpacket;
				t_connection 	* dest_c;
				t_account    	* friend_acc;
				t_server_friendslistreply_status status;
				t_game * game;
				t_channel * channel;
				char stat;
				t_list * flist;
				t_friend * fr;

				if (args[2].empty()) {
					message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
					message_send_text(c, message_type_error, c, localize(c, "Usage: /friends add [username] (alias: a)"));
					message_send_text(c, message_type_info, c, localize(c, "** Adds [username] to your friends list."));
					return -1;
				}
				text = args[2].c_str(); // username

				if (!(friend_acc = accountlist_find_account(text))) {
					message_send_text(c, message_type_error, c, localize(c, "That user doesn't exist!"));
					return -1;
				}

				switch (account_add_friend(my_acc, friend_acc)) {
				case -1:
					message_send_text(c, message_type_error, c, localize(c, "Server error."));
					return -1;
				case -2:
					message_send_text(c, message_type_error, c, localize(c, "You can't add yourself to your friends list!"));
					return -1;
				case -3:
					msgtemp = localize(c, "You can only have a maximum of {} friends.", prefs_get_max_friends());
					message_send_text(c, message_type_info, c, msgtemp);
					return -1;
				case -4:
					msgtemp = localize(c, "{} is already on your friends list!", account_get_name(friend_acc));
					message_send_text(c, message_type_error, c, msgtemp);
					return -1;
				}

				msgtemp = localize(c, "Added {} to your friends list.", text);
				message_send_text(c, message_type_info, c, msgtemp);
				dest_c = connlist_find_connection_by_account(friend_acc);
				if (dest_c != NULL) {
					msgtemp = localize(c, "{} added you to his/her friends list.", conn_get_username(c));
					message_send_text(dest_c, message_type_info, dest_c, msgtemp);
				}

				if ((conn_get_class(c) != conn_class_bnet) || (!(rpacket = packet_create(packet_class_bnet))))
					return 0;

				packet_set_size(rpacket, sizeof(t_server_friendadd_ack));
				packet_set_type(rpacket, SERVER_FRIENDADD_ACK);

				packet_append_string(rpacket, account_get_name(friend_acc));

				game = NULL;
				channel = NULL;

				if (!(dest_c))
				{
					bn_byte_set(&status.location, FRIENDSTATUS_OFFLINE);
					bn_byte_set(&status.status, 0);
					bn_int_set(&status.clienttag, 0);
				}
				else
				{
					bn_int_set(&status.clienttag, conn_get_clienttag(dest_c));
					stat = 0;
					flist = account_get_friends(my_acc);
					fr = friendlist_find_account(flist, friend_acc);
					if ((friend_get_mutual(fr)))    stat |= FRIEND_TYPE_MUTUAL;
					if ((conn_get_dndstr(dest_c)))  stat |= FRIEND_TYPE_DND;
					if ((conn_get_awaystr(dest_c))) stat |= FRIEND_TYPE_AWAY;
					bn_byte_set(&status.status, stat);
					if ((game = conn_get_game(dest_c)))
					{
						if (game_get_flag(game) != game_flag_private)
							bn_byte_set(&status.location, FRIENDSTATUS_PUBLIC_GAME);
						else
							bn_byte_set(&status.location, FRIENDSTATUS_PRIVATE_GAME);
					}
					else if ((channel = conn_get_channel(dest_c)))
					{
						bn_byte_set(&status.location, FRIENDSTATUS_CHAT);
					}
					else
					{
						bn_byte_set(&status.location, FRIENDSTATUS_ONLINE);
					}
				}

				packet_append_data(rpacket, &status, sizeof(status));

				if (game) packet_append_string(rpacket, game_get_name(game));
				else if (channel) packet_append_string(rpacket, channel_get_name(channel));
				else packet_append_string(rpacket, "");

				conn_push_outqueue(c, rpacket);
				packet_del_ref(rpacket);
			}
			else if (args[1] == "msg" || args[1] == "w" || args[1] == "whisper" || args[1] == "m")
			{
				char const *msg;
				int cnt = 0;
				t_connection * dest_c;
				t_elem  * curr;
				t_friend * fr;
				t_list  * flist;

				if (args[2].empty()) {
					message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
					message_send_text(c, message_type_error, c, localize(c, "Usage: /friends msg [username] (aliases: w whisper m)"));
					message_send_text(c, message_type_info, c, localize(c, "** Whisper [msgtext] to all of your online friends."));
					return -1;
				}
				msg = args[2].c_str(); // message

				flist = account_get_friends(my_acc);
				if (flist == NULL)
					return -1;

				LIST_TRAVERSE(flist, curr)
				{
					if (!(fr = (t_friend*)elem_get_data(curr))) {
						eventlog(eventlog_level_error, __FUNCTION__, "found NULL entry in list");
						continue;
					}
					if (friend_get_mutual(fr)) {
						dest_c = connlist_find_connection_by_account(friend_get_account(fr));
						if (!dest_c) continue;
						message_send_text(dest_c, message_type_whisper, c, msg);
						cnt++;
					}
				}
				if (cnt)
					message_send_text(c, message_type_friendwhisperack, c, msg);
				else
					message_send_text(c, message_type_info, c, localize(c, "All of your friends are offline."));
			}
			else if (args[1] == "r" || args[1] == "remove" || args[1] == "del" || args[1] == "delete")
			{
				int num;
				t_packet * rpacket;

				if (args[2].empty()) {
					message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
					message_send_text(c, message_type_error, c, localize(c, "Usage: /friends del [username] (aliases: delete r remove)"));
					message_send_text(c, message_type_info, c, localize(c, "** Removes [username] from your friends list."));
					return -1;
				}
				text = args[2].c_str(); // username

				switch ((num = account_remove_friend2(my_acc, text))) {
				case -1: return -1;
				case -2:
					msgtemp = localize(c, "{} was not found on your friends list.", text);
					message_send_text(c, message_type_info, c, msgtemp);
					return -1;
				default:
					msgtemp = localize(c, "Removed {} from your friends list.", text);
					message_send_text(c, message_type_info, c, msgtemp);

					if ((conn_get_class(c) != conn_class_bnet) || (!(rpacket = packet_create(packet_class_bnet))))
						return 0;

					packet_set_size(rpacket, sizeof(t_server_frienddel_ack));
					packet_set_type(rpacket, SERVER_FRIENDDEL_ACK);

					bn_byte_set(&rpacket->u.server_frienddel_ack.friendnum, num);

					conn_push_outqueue(c, rpacket);
					packet_del_ref(rpacket);

					return 0;
				}
			}
			else if (args[1] == "list" || args[1] == "l" || args[1] == "online" || args[1] == "o") {
				char const * frienduid;
				std::string status, software;
				t_connection * dest_c;
				t_account * friend_acc;
				t_game const * game;
				t_channel const * channel;
				t_friend * fr;
				t_list  * flist;
				int num;
				unsigned int uid;
				bool online_only = false;

				if (args[1] == "online" || args[1] == "o")
				{
					online_only = true;
				}
				if (!online_only)
				{
					message_send_text(c, message_type_info, c, "** Your Friends List:");
				}
				else
				{
					message_send_text(c, message_type_info, c, "** Your Online Friends List:");
					message_send_text(c, message_type_info, c, msgtemp);
				}
				num = account_get_friendcount(my_acc);

				flist = account_get_friends(my_acc);
				if (flist != NULL) {
					for (i = 0; i < num; i++)
					{
						if ((!(uid = account_get_friend(my_acc, i))) || (!(fr = friendlist_find_uid(flist, uid))))
						{
							eventlog(eventlog_level_error, __FUNCTION__, "friend uid in list");
							continue;
						}
						friend_acc = friend_get_account(fr);
						if (!(dest_c = connlist_find_connection_by_account(friend_acc))) {
							if (online_only) {
								continue;
							}
							status = localize(c, ", offline");
						}
						else {
							software = localize(c, " using {}", clienttag_get_title(conn_get_clienttag(dest_c)));

							if (friend_get_mutual(fr)) {
								if ((game = conn_get_game(dest_c)))
									status = localize(c, ", in game \"{}\"", game_get_name(game));
								else if ((channel = conn_get_channel(dest_c))) {
									if (strcasecmp(channel_get_name(channel), "Arranged Teams") == 0)
										status = localize(c, ", in game AT Preparation");
									else
										status = localize(c, ", in channel \"{}\",", channel_get_name(channel));
								}
								else
									status = localize(c, ", is in AT Preparation");
							}
							else {
								if ((game = conn_get_game(dest_c)))
									status = localize(c, ", is in a game");
								else if ((channel = conn_get_channel(dest_c)))
									status = localize(c, ", is in a chat channel");
								else
									status = localize(c, ", is in AT Preparation");
							}
						}

						frienduid = account_get_name(friend_acc);
						if (!software.empty()) std::snprintf(msgtemp0, sizeof(msgtemp0), "%d: %s%.16s%.128s, %.64s", i + 1, friend_get_mutual(fr) ? "*" : " ", frienduid, status.c_str(), software.c_str());
						else std::snprintf(msgtemp0, sizeof(msgtemp0), "%d: %.16s%.128s", i + 1, frienduid, status.c_str());
						message_send_text(c, message_type_info, c, msgtemp0);
					}
				}
				message_send_text(c, message_type_info, c, "--------------------------------------------------------");
			}
			else {
				message_send_text(c, message_type_info, c, localize(c, "--------------------------------------------------------"));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /friends msg [username] (aliases: w whisper m)"));
				message_send_text(c, message_type_info, c, localize(c, "** Whisper [msgtext] to all of your online friends."));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /friends add [username] (alias: a)"));
				message_send_text(c, message_type_info, c, localize(c, "** Adds [username] to your friends list."));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /friends del [username] (aliases: delete r remove)"));
				message_send_text(c, message_type_info, c, localize(c, "** Removes [username] from your friends list."));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /friends list (alias: l)"));
				message_send_text(c, message_type_info, c, localize(c, "** Displays your friends list."));
				message_send_text(c, message_type_error, c, localize(c, "Usage: /friends online (alias: o)"));
				message_send_text(c, message_type_info, c, localize(c, "** Displays your online friends list."));
				
				return -1;
			}

			return 0;
		}
		
		static int _handle_clan_command(t_connection * c, char const * text)
		{
			t_account * acc;
			t_clanmember * member;
			t_clan * clan;

			if (!(acc = conn_get_account(c))){
				ERROR0("got NULL account");
			}

			std::vector<std::string> args = split_command(text, 2);

			// user in clan
			if ((member = account_get_clanmember_forced(acc)) && (clan = clanmember_get_clan(member)) && (clanmember_get_fullmember(member) == 1))
			{
				if (args[1].empty()) {
					message_send_text(c, message_type_info, c, "--------------------------------------------------------");
					message_send_text(c, message_type_error, c, "Usage: /clan msg [message] (aliases: m w whisper)"); // Fix
					message_send_text(c, message_type_info, c, "** Whispers a message to all your fellow clan members."); // Fix
					message_send_text(c, message_type_error, c, "Usage: /clan list (alias: l)"); // Fix
					message_send_text(c, message_type_info, c, "-- Displays your clan members."); // Fix
					if (clanmember_get_status(member) >= CLAN_SHAMAN) {
						message_send_text(c, message_type_error, c, "Usage: /clan invite [username] (alias: inv)");
						message_send_text(c, message_type_info, c, "** For invite player to your clan.");
						message_send_text(c, message_type_error, c, "Usage: /clan motd [message] (no have alias)");
						message_send_text(c, message_type_info, c, "** For update the clan message of the day to message.");
						message_send_text(c, message_type_error, c, "Usage: /clan [status] [username] (no have alias)");
						message_send_text(c, message_type_info, c, "** Change status member from your clan."); 
						message_send_text(c, message_type_info, c, "** Status: [chieftain], [shaman], [grunt] and [peon].");
					}
					if (clanmember_get_status(member) == CLAN_CHIEFTAIN) {
						message_send_text(c, message_type_info, c, "** Warning! You'll have multi chieftain on clan!");
						message_send_text(c, message_type_error, c, "Usage: /clan kick [username] (alias: k)");
						message_send_text(c, message_type_info, c, "** Kicks member from your clan.");
						message_send_text(c, message_type_error, c, "Usage: /clan [channel] (no have alias)");
						message_send_text(c, message_type_info, c, "-- For change clan channel.");
						message_send_text(c, message_type_error, c, "Usage: /clan disband (no have alias)"); // Fix
						message_send_text(c, message_type_info, c, "** For disband your clan."); // Fix
					}
					message_send_text(c, message_type_info, c, "--------------------------------------------------------");
					return 0;
				}
				
				if (args[1] == "msg" || args[1] == "m" || args[1] == "w" || args[1] == "whisper") // Fix
				{
					if (args[2].empty()) {
						message_send_text(c, message_type_info, c, "--------------------------------------------------------");
						message_send_text(c, message_type_error, c, "Usage: /clan msg [message] (alias: m w whisper)");
						message_send_text(c, message_type_info, c, "** Whispers a message to all your fellow clan members.");
						return -1;
					}
					
					char const *msg = args[2].c_str(); // message

					if (clan_send_message_to_online_members(clan, message_type_null, c, msg) >= 1)
						message_send_text(c, message_type_null, c, msg);
					else
						message_send_text(c, message_type_error, c, localize(c, "All fellow members of your clan are currently offline!"));

					return 0;
				}
				
				else if (args[1] == "list" || args[1] == "l" ) // Fix
				{
					char const * friend_;
					char status[128];
					char software[64];
					char msgtemp[MAX_MESSAGE_LEN];
					t_connection * dest_c;
					t_account * friend_acc;
					t_game const * game;
					t_channel const * channel;
					t_friend * fr;
					t_list  * flist;
					int num;
					unsigned int uid;
					t_elem  * curr;
					int i = -1;

					message_send_text(c, message_type_error, c, "-- Your Clan Members:");
					LIST_TRAVERSE(clan_get_members(clan), curr) {
						i++;
						if (!(member = (t_clanmember*)elem_get_data(curr))) {
							eventlog(eventlog_level_error, __FUNCTION__, "found NULL entry in list");
							continue;
						}
						if (!(friend_acc = clanmember_get_account(member))) {
							eventlog(eventlog_level_error, __FUNCTION__, "member has NULL account");
							continue;
						}

						if (clanmember_get_fullmember(member) == 0)
							continue;

						if (clanmember_get_account(member) && clanmember_get_status(member) == CLAN_CHIEFTAIN) {
							software[0] = '\0';
							if (!(dest_c = connlist_find_connection_by_account(friend_acc)))
								sprintf(status, ", offline");
							else {
								sprintf(software, " using %s", clienttag_get_title(conn_get_clienttag(dest_c)));
								if ((game = conn_get_game(dest_c)))
									sprintf(status, ", in game \"%.64s\"", game_get_name(game));
								else if ((channel = conn_get_channel(dest_c))) {
									if (strcasecmp(channel_get_name(channel), "Arranged Teams") == 0)
										sprintf(status, ", in game AT Preparation");
									else
										sprintf(status, ", in channel \"%.64s\"", channel_get_name(channel));
								}
								else
									sprintf(status, ", is in AT Preparation");
							}

							friend_ = account_get_name(friend_acc);
							if (software[0]) sprintf(msgtemp, "%d: %.16s%.128s, %.64s", i + 1, friend_, status, software);
							else sprintf(msgtemp, "%d: %.16s%.128s", i + 1, friend_, status);
							message_send_text(c, message_type_info, c, msgtemp);
						}

						if (clanmember_get_account(member) && clanmember_get_status(member) == CLAN_SHAMAN) {
							software[0] = '\0';
							if (!(dest_c = connlist_find_connection_by_account(friend_acc)))
								sprintf(status, ", offline");
							else {
								sprintf(software, " using %s", clienttag_get_title(conn_get_clienttag(dest_c)));
								if ((game = conn_get_game(dest_c)))
									sprintf(status, ", in game \"%.64s\"", game_get_name(game));
								else if ((channel = conn_get_channel(dest_c))) {
									if (strcasecmp(channel_get_name(channel), "Arranged Teams") == 0)
										sprintf(status, ", in game AT Preparation");
									else
										sprintf(status, ", in channel \"%.64s\"", channel_get_name(channel));
								}
								else
									sprintf(status, ", is in AT Preparation");
							}

							friend_ = account_get_name(friend_acc);
							if (software[0]) sprintf(msgtemp, "%d: %.16s%.128s, %.64s", i + 1, friend_, status, software);
							else sprintf(msgtemp, "%d: %.16s%.128s", i + 1, friend_, status);
							message_send_text(c, message_type_info, c, msgtemp);
						}

						if (clanmember_get_account(member) && clanmember_get_status(member) == CLAN_GRUNT) {
							software[0] = '\0';
							if (!(dest_c = connlist_find_connection_by_account(friend_acc)))
								sprintf(status, ", offline");
							else {
								sprintf(software, " using %s", clienttag_get_title(conn_get_clienttag(dest_c)));
								if ((game = conn_get_game(dest_c)))
									sprintf(status, ", in game \"%.64s\"", game_get_name(game));
								else if ((channel = conn_get_channel(dest_c))) {
									if (strcasecmp(channel_get_name(channel), "Arranged Teams") == 0)
										sprintf(status, ", in game AT Preparation");
									else
										sprintf(status, ", in channel \"%.64s\"", channel_get_name(channel));
								}
								else
									sprintf(status, ", is in AT Preparation");
							}

							friend_ = account_get_name(friend_acc);
							if (software[0]) sprintf(msgtemp, "%d: %.16s%.128s, %.64s", i + 1, friend_, status, software);
							else sprintf(msgtemp, "%d: %.16s%.128s", i + 1, friend_, status);
							message_send_text(c, message_type_info, c, msgtemp);
						}

						if (clanmember_get_account(member) && clanmember_get_status(member) == CLAN_PEON) {
							software[0] = '\0';
							if (!(dest_c = connlist_find_connection_by_account(friend_acc)))
								sprintf(status, ", offline");
							else {
								sprintf(software, " using %s", clienttag_get_title(conn_get_clienttag(dest_c)));
								if ((game = conn_get_game(dest_c)))
									sprintf(status, ", in game \"%.64s\"", game_get_name(game));
								else if ((channel = conn_get_channel(dest_c))) {
									if (strcasecmp(channel_get_name(channel), "Arranged Teams") == 0)
										sprintf(status, ", in game AT Preparation");
									else
										sprintf(status, ", in channel \"%.64s\"", channel_get_name(channel));
								}
								else
									sprintf(status, ", is in AT Preparation");
							}

							friend_ = account_get_name(friend_acc);
							if (software[0]) sprintf(msgtemp, "%d: %.16s%.128s, %.64s", i + 1, friend_, status, software);
							else sprintf(msgtemp, "%d: %.16s%.128s", i + 1, friend_, status);
							message_send_text(c, message_type_info, c, msgtemp);
						}
					}
					message_send_text(c, message_type_info, c, "--------------------------------------------------------");
					return 0;
				}
				
				else if (clanmember_get_status(member) == CLAN_SHAMAN) {
					if (args[1] == "motd")
					{
						if (args[2].empty())
						{
							message_send_text(c, message_type_info, c, "--------------------------------------------------------");
							message_send_text(c, message_type_error, c, "Usage: /clan motd [message] (no have alias)");
							message_send_text(c, message_type_info, c, "** Update the clan's Message of the Day to [message].");
						}
						
						return 0;
						
						const char * msg = args[2].c_str(); // message

						clan_set_motd(clan, msg);

						message_send_text(c, message_type_info, c, localize(c, "Clan message of day is updated!"));
						return 0;
					}
					
					else if (args[1] == "invite" || args[1] == "inv")
					{
						t_account * dest_account;
						t_connection * dest_conn;
						if (args[2].empty())
						{
							message_send_text(c, message_type_info, c, "--------------------------------------------------------");
							message_send_text(c, message_type_error, c, "Usage: /clan invite [username] (alias: inv)");
							message_send_text(c, message_type_info, c, "** Invite [username] to your clan.");
							return -1;
						}
						const char * username = args[2].c_str();

						if ((dest_account = accountlist_find_account(username)) && (dest_conn = account_get_conn(dest_account)) && (account_get_clan(dest_account) == NULL) && (account_get_creating_clan(dest_account) == NULL))
						{
							if (prefs_get_clan_newer_time() > 0)
								clan_add_member(clan, dest_account, CLAN_NEW);
							else
								clan_add_member(clan, dest_account, CLAN_PEON);
							msgtemp = localize(c, "User {} was invited to your clan!", username);
							message_send_text(c, message_type_error, c, msgtemp);
							msgtemp = localize(c, "You are invited to {} by {}!", clan_get_name(clan), conn_get_chatname(c));
							message_send_text(dest_conn, message_type_error, c, msgtemp);
						}
						else {
							msgtemp = localize(c, "User {} is not online or is already member of clan!", username);
							message_send_text(c, message_type_error, c, msgtemp);
						}
						return 0;
					}
					
					else if (args[1] == "kick" || args[1] == "k") {
						t_connection     * dest_c;
						t_account        * friend_acc;
						t_game * game;
						t_channel * channel;
						char stat;
						t_clanmember *member;

						const char * text = args[2].c_str();
						if (text[0] == '\0') {
							message_send_text(c, message_type_info, c, "--------------------------------------------------------");
							message_send_text(c, message_type_error, c, "Usage: /clan kick [username] (alias: k)");
							message_send_text(c, message_type_info, c, "** Kicks member from your clan.");
							return 0;
						}

						if (!(friend_acc = accountlist_find_account(text))) { message_send_text(c, message_type_error, c, "That user doesn't exist!"); return 0; }
						if (acc == friend_acc) { message_send_text(c, message_type_error, c, "You can't choose yourself!"); return 0; }
						if (!account_get_clan(friend_acc)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }
						if (clan_get_clanid(account_get_clan(friend_acc)) != clan_get_clanid(clan)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }
						

						if (member = account_get_clanmember(friend_acc)) {
							char	 status;
							if (status = clanmember_get_status(member)) {
								switch (status) {
								case CLAN_CHIEFTAIN: msgtemp += localize(c, "Chieftain"); break;
								case CLAN_SHAMAN: msgtemp += localize(c, "Shaman"); break;
								case CLAN_GRUNT: msgtemp += localize(c, "Grunt"); break;
								case CLAN_PEON: msgtemp += localize(c, "Peon"); break;
								default:;
								}
							}
							if (status == CLAN_CHIEFTAIN) {
								msgtemp = localize(c, "You can't kick chieftain!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							if (status == CLAN_SHAMAN) {
								msgtemp = localize(c, "You can't kick shaman!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
						}

						if (!clan_remove_member(clan, account_get_clanmember(friend_acc))) { 
							msgtemp = localize(c, "Successfully kick user {} from clan.", account_get_name(friend_acc));
							message_send_text(c, message_type_info, c, msgtemp);
							return 0; 
						}
						
					}
					
					else if (args[1] == "shaman") {
						t_connection * dest_c;
						t_account * friend_acc;
						t_server_friendslistreply_status status;
						t_clanmember *member;

						const char * text = args[2].c_str();
						if (text[0] == '\0') {
							message_send_text(c, message_type_info, c, "--------------------------------------------------------");
							message_send_text(c, message_type_error, c, "Usage: /clan shaman [username] (no have alias)");
							message_send_text(c, message_type_info, c, "** Change status member to shaman.");
							return 0;
						}

						if (!(friend_acc = accountlist_find_account(text))) { message_send_text(c, message_type_error, c, "That user doesn't exist!"); return 0; }
						if (acc == friend_acc) { message_send_text(c, message_type_error, c, "You can't choose yourself!"); return 0; }
						if (!account_get_clan(friend_acc)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }
						if (clan_get_clanid(account_get_clan(friend_acc)) != clan_get_clanid(clan)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }

						if (member = account_get_clanmember(friend_acc)) {
							char	 status;
							if (status = clanmember_get_status(member)) {
								switch (status) {
								case CLAN_CHIEFTAIN: msgtemp += localize(c, "Chieftain"); break;
								case CLAN_SHAMAN: msgtemp += localize(c, "Shaman"); break;
								case CLAN_GRUNT: msgtemp += localize(c, "Grunt"); break;
								case CLAN_PEON: msgtemp += localize(c, "Peon"); break;
								default:;
								}
							}
							if (status == CLAN_CHIEFTAIN) {
								msgtemp = localize(c, "User {} has already on chieftain! You can't change chieftain to shaman!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							if (status == CLAN_SHAMAN) {
								msgtemp = localize(c, "Only chieftain can acess that command!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							if (status == CLAN_SHAMAN) {
								msgtemp = localize(c, "User {} has already on shaman!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							
						}

						clanmember_set_status(account_get_clanmember(friend_acc), CLAN_SHAMAN);
						msgtemp = localize(c, "Successfully change status shaman for user {}.", account_get_name(friend_acc));
						message_send_text(c, message_type_info, c, msgtemp);
					}
					
					else if (args[1] == "grunt") {
						t_connection * dest_c;
						t_account * friend_acc;
						t_server_friendslistreply_status status;
						t_clanmember *member;

						const char * text = args[2].c_str();
						if (text[0] == '\0') {
							message_send_text(c, message_type_info, c, "--------------------------------------------------------");
							message_send_text(c, message_type_error, c, "Usage: /clan grunt [username] (no have alias)");
							message_send_text(c, message_type_info, c, "** Change status member to grunt.");
							return 0;
						}

						if (!(friend_acc = accountlist_find_account(text))) { message_send_text(c, message_type_error, c, "That user doesn't exist!"); return 0; }
						if (acc == friend_acc) { message_send_text(c, message_type_error, c, "You can't choose yourself!"); return 0; }
						if (!account_get_clan(friend_acc)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }
						if (clan_get_clanid(account_get_clan(friend_acc)) != clan_get_clanid(clan)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }

						if (member = account_get_clanmember(friend_acc)) {
							char	 status;
							if (status = clanmember_get_status(member)) {
								switch (status) {
								case CLAN_CHIEFTAIN: msgtemp += localize(c, "Chieftain"); break;
								case CLAN_SHAMAN: msgtemp += localize(c, "Shaman"); break;
								case CLAN_GRUNT: msgtemp += localize(c, "Grunt"); break;
								case CLAN_PEON: msgtemp += localize(c, "Peon"); break;
								default:;
								}
							}
							if (status == CLAN_CHIEFTAIN) {
								msgtemp = localize(c, "User {} has already on chieftain! You can't change chieftain to grunt!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							if (status == CLAN_SHAMAN) {
								msgtemp = localize(c, "User {} has already on shaman! You can't change shaman to grunt!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							if (status == CLAN_GRUNT) {
								msgtemp = localize(c, "User {} has already on grunt!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							
						}

						clanmember_set_status(account_get_clanmember(friend_acc), CLAN_GRUNT);
						msgtemp = localize(c, "Successfully change status grunt for user {}.", account_get_name(friend_acc));
						message_send_text(c, message_type_info, c, msgtemp);
					}
					
					else if (args[1] == "peon") {
						t_connection * dest_c;
						t_account * friend_acc;
						t_server_friendslistreply_status status;
						t_clanmember *member;

						const char * text = args[2].c_str();
						if (text[0] == '\0') {
							message_send_text(c, message_type_info, c, "--------------------------------------------------------");
							message_send_text(c, message_type_error, c, "Usage: /clan peon [username] (no have alias)");
							message_send_text(c, message_type_info, c, "** Change status member to peon.");
							return 0;
						}

						if (!(friend_acc = accountlist_find_account(text))) { message_send_text(c, message_type_error, c, "That user doesn't exist!"); return 0; }
						if (acc == friend_acc) { message_send_text(c, message_type_error, c, "You can't choose yourself!"); return 0; }
						if (!account_get_clan(friend_acc)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }
						if (clan_get_clanid(account_get_clan(friend_acc)) != clan_get_clanid(clan)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }

						if (member = account_get_clanmember(friend_acc)) {
							char	 status;
							if (status = clanmember_get_status(member)) {
								switch (status) {
								case CLAN_CHIEFTAIN: msgtemp += localize(c, "Chieftain"); break;
								case CLAN_SHAMAN: msgtemp += localize(c, "Shaman"); break;
								case CLAN_GRUNT: msgtemp += localize(c, "Grunt"); break;
								case CLAN_PEON: msgtemp += localize(c, "Peon"); break;
								default:;
								}
							}
							if (status == CLAN_CHIEFTAIN) {
								msgtemp = localize(c, "User {} has already on chieftain! You can't change chieftain to peon!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							if (status == CLAN_SHAMAN) {
								msgtemp = localize(c, "User {} has already on shaman! You can't change shaman to peon!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							if (status == CLAN_PEON) {
								msgtemp = localize(c, "User {} has already on peon!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							
						}

						clanmember_set_status(account_get_clanmember(friend_acc), CLAN_PEON);
						msgtemp = localize(c, "Successfully change status peon for user {}.", account_get_name(friend_acc));
						message_send_text(c, message_type_info, c, msgtemp);
					}
					
				}
				
				else if (clanmember_get_status(member) == CLAN_CHIEFTAIN) {
					
					if (args[1] == "motd") // Fix
					{
						if (args[2].empty())
						{
							message_send_text(c, message_type_info, c, "--------------------------------------------------------");
							message_send_text(c, message_type_error, c, "Usage: /clan motd [message] (no have alias)");
							message_send_text(c, message_type_info, c, "** Update the clan's Message of the Day to [message].");
							return 0;
						}
						
						const char * msg = args[2].c_str(); // message

						clan_set_motd(clan, msg);

						message_send_text(c, message_type_info, c, localize(c, "Clan message of day is updated!"));
						return 0;
					}
					
					else if (args[1] == "invite" || args[1] == "inv") // Fix
					{
						t_account * dest_account;
						t_connection * dest_conn;
						if (args[2].empty())
						{
							message_send_text(c, message_type_info, c, "--------------------------------------------------------");
							message_send_text(c, message_type_error, c, "Usage: /clan invite [username] (alias: inv)");
							message_send_text(c, message_type_info, c, "** Invite [username] to your clan.");
							return -1;
						}
						const char * username = args[2].c_str();

						if ((dest_account = accountlist_find_account(username)) && (dest_conn = account_get_conn(dest_account)) && (account_get_clan(dest_account) == NULL) && (account_get_creating_clan(dest_account) == NULL))
						{
							if (prefs_get_clan_newer_time() > 0)
								clan_add_member(clan, dest_account, CLAN_NEW);
							else
								clan_add_member(clan, dest_account, CLAN_PEON);
							msgtemp = localize(c, "User {} was invited to your clan!", username);
							message_send_text(c, message_type_error, c, msgtemp);
							msgtemp = localize(c, "You are invited to {} by {}!", clan_get_name(clan), conn_get_chatname(c));
							message_send_text(dest_conn, message_type_error, c, msgtemp);
							
							// msgtemp = localize(c, "{}", account_get_channel(c));
							std::snprintf(msgtemp0, sizeof(msgtemp0), "%s", account_get_channel(conn_get_account(c)));
							// account_set_channel(username, msgtemp);
							account_set_strattr(dest_account,"BNET\\clan\\channel", msgtemp0);
							
						}
						else {
							msgtemp = localize(c, "User {} is not online or is already member of clan!", username);
							message_send_text(c, message_type_error, c, msgtemp);
						}
						return 0;
					}
					
					else if (args[1] == "kick" || args[1] == "k") {
						t_connection     * dest_c;
						t_account        * friend_acc;
						t_game * game;
						t_channel * channel;
						char stat;
						t_clanmember *member;

						const char * text = args[2].c_str();
						if (text[0] == '\0') {
							message_send_text(c, message_type_info, c, "--------------------------------------------------------");
							message_send_text(c, message_type_error, c, "Usage: /clan kick [username] (alias: k)");
							message_send_text(c, message_type_info, c, "** Kicks member from your clan.");
							return 0;
						}

						if (!(friend_acc = accountlist_find_account(text))) { message_send_text(c, message_type_error, c, "That user doesn't exist!"); return 0; }
						if (acc == friend_acc) { message_send_text(c, message_type_error, c, "You can't choose yourself!"); return 0; }
						if (!account_get_clan(friend_acc)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }
						if (clan_get_clanid(account_get_clan(friend_acc)) != clan_get_clanid(clan)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }
						

						if (member = account_get_clanmember(friend_acc)) {
							char	 status;
							if (status = clanmember_get_status(member)) {
								switch (status) {
								case CLAN_CHIEFTAIN: msgtemp += localize(c, "Chieftain"); break;
								case CLAN_SHAMAN: msgtemp += localize(c, "Shaman"); break;
								case CLAN_GRUNT: msgtemp += localize(c, "Grunt"); break;
								case CLAN_PEON: msgtemp += localize(c, "Peon"); break;
								default:;
								}
							}
							if (status == CLAN_CHIEFTAIN) {
								msgtemp = localize(c, "You can't kick chieftain!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
						}

						if (!clan_remove_member(clan, account_get_clanmember(friend_acc))) { 
							msgtemp = localize(c, "Successfully kick user {} from clan.", account_get_name(friend_acc));
							message_send_text(c, message_type_info, c, msgtemp);
							return 0; 
						}
						
					}
					
					else if (args[1] == "chieftain") {
						t_connection * dest_c;
						t_account * friend_acc;
						t_server_friendslistreply_status status;
						t_clanmember *member;

						const char * text = args[2].c_str();
						if (text[0] == '\0') {
							message_send_text(c, message_type_info, c, "--------------------------------------------------------");
							message_send_text(c, message_type_error, c, "Usage: /clan chieftain [username] (no have alias)");
							message_send_text(c, message_type_info, c, "** Change status member to chieftain.");
							message_send_text(c, message_type_info, c, "** Warning! You'll have multi chieftain on clan!");
							return 0;
						}

						if (!(friend_acc = accountlist_find_account(text))) { message_send_text(c, message_type_error, c, "That user doesn't exist!"); return 0; }
						if (acc == friend_acc) { message_send_text(c, message_type_error, c, "You can't choose yourself!"); return 0; }
						if (!account_get_clan(friend_acc)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }
						if (clan_get_clanid(account_get_clan(friend_acc)) != clan_get_clanid(clan)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }

						if (member = account_get_clanmember(friend_acc)) {
							char	 status;
							if (status = clanmember_get_status(member)) {
								switch (status) {
								case CLAN_CHIEFTAIN: msgtemp += localize(c, "Chieftain"); break;
								case CLAN_SHAMAN: msgtemp += localize(c, "Shaman"); break;
								case CLAN_GRUNT: msgtemp += localize(c, "Grunt"); break;
								case CLAN_PEON: msgtemp += localize(c, "Peon"); break;
								default:;
								}
							}
							if (status == CLAN_CHIEFTAIN) {
								msgtemp = localize(c, "User {} has already on chieftain!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
						}

						clanmember_set_status(account_get_clanmember(friend_acc), CLAN_CHIEFTAIN);
						msgtemp = localize(c, "Successfully change status chieftain for user {}.", account_get_name(friend_acc));
						message_send_text(c, message_type_info, c, msgtemp);
					}
					
					else if (args[1] == "shaman") {
						t_connection * dest_c;
						t_account * friend_acc;
						t_server_friendslistreply_status status;
						t_clanmember *member;

						const char * text = args[2].c_str();
						if (text[0] == '\0') {
							message_send_text(c, message_type_info, c, "--------------------------------------------------------");
							message_send_text(c, message_type_error, c, "Usage: /clan shaman [username] (no have alias)");
							message_send_text(c, message_type_info, c, "** Change status member to shaman.");
							return 0;
						}

						if (!(friend_acc = accountlist_find_account(text))) { message_send_text(c, message_type_error, c, "That user doesn't exist!"); return 0; }
						if (acc == friend_acc) { message_send_text(c, message_type_error, c, "You can't choose yourself!"); return 0; }
						if (!account_get_clan(friend_acc)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }
						if (clan_get_clanid(account_get_clan(friend_acc)) != clan_get_clanid(clan)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }

						if (member = account_get_clanmember(friend_acc)) {
							char	 status;
							if (status = clanmember_get_status(member)) {
								switch (status) {
								case CLAN_CHIEFTAIN: msgtemp += localize(c, "Chieftain"); break;
								case CLAN_SHAMAN: msgtemp += localize(c, "Shaman"); break;
								case CLAN_GRUNT: msgtemp += localize(c, "Grunt"); break;
								case CLAN_PEON: msgtemp += localize(c, "Peon"); break;
								default:;
								}
							}
							if (status == CLAN_CHIEFTAIN) {
								msgtemp = localize(c, "User {} has already on chieftain! You can't change chieftain to shaman!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							if (status == CLAN_SHAMAN) {
								msgtemp = localize(c, "User {} has already on shaman!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							
						}

						clanmember_set_status(account_get_clanmember(friend_acc), CLAN_SHAMAN);
						msgtemp = localize(c, "Successfully change status shaman for user {}.", account_get_name(friend_acc));
						message_send_text(c, message_type_info, c, msgtemp);
					}
					
					else if (args[1] == "grunt") {
						t_connection * dest_c;
						t_account * friend_acc;
						t_server_friendslistreply_status status;
						t_clanmember *member;

						const char * text = args[2].c_str();
						if (text[0] == '\0') {
							message_send_text(c, message_type_info, c, "--------------------------------------------------------");
							message_send_text(c, message_type_error, c, "Usage: /clan grunt [username] (no have alias)");
							message_send_text(c, message_type_info, c, "** Change status member to grunt.");
							return 0;
						}

						if (!(friend_acc = accountlist_find_account(text))) { message_send_text(c, message_type_error, c, "That user doesn't exist!"); return 0; }
						if (acc == friend_acc) { message_send_text(c, message_type_error, c, "You can't choose yourself!"); return 0; }
						if (!account_get_clan(friend_acc)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }
						if (clan_get_clanid(account_get_clan(friend_acc)) != clan_get_clanid(clan)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }

						if (member = account_get_clanmember(friend_acc)) {
							char	 status;
							if (status = clanmember_get_status(member)) {
								switch (status) {
								case CLAN_CHIEFTAIN: msgtemp += localize(c, "Chieftain"); break;
								case CLAN_SHAMAN: msgtemp += localize(c, "Shaman"); break;
								case CLAN_GRUNT: msgtemp += localize(c, "Grunt"); break;
								case CLAN_PEON: msgtemp += localize(c, "Peon"); break;
								default:;
								}
							}
							if (status == CLAN_CHIEFTAIN) {
								msgtemp = localize(c, "User {} has already on chieftain! You can't change chieftain to grunt!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							if (status == CLAN_GRUNT) {
								msgtemp = localize(c, "User {} has already on grunt!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							
						}

						clanmember_set_status(account_get_clanmember(friend_acc), CLAN_GRUNT);
						msgtemp = localize(c, "Successfully change status grunt for user {}.", account_get_name(friend_acc));
						message_send_text(c, message_type_info, c, msgtemp);
					}
					
					else if (args[1] == "peon") {
						t_connection * dest_c;
						t_account * friend_acc;
						t_server_friendslistreply_status status;
						t_clanmember *member;

						const char * text = args[2].c_str();
						if (text[0] == '\0') {
							message_send_text(c, message_type_info, c, "--------------------------------------------------------");
							message_send_text(c, message_type_error, c, "Usage: /clan peon [username] (no have alias)");
							message_send_text(c, message_type_info, c, "** Change status member to peon.");
							return 0;
						}

						if (!(friend_acc = accountlist_find_account(text))) { message_send_text(c, message_type_error, c, "That user doesn't exist!"); return 0; }
						if (acc == friend_acc) { message_send_text(c, message_type_error, c, "You can't choose yourself!"); return 0; }
						if (!account_get_clan(friend_acc)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }
						if (clan_get_clanid(account_get_clan(friend_acc)) != clan_get_clanid(clan)) { msgtemp = localize(c, "{} is not members!", account_get_name(friend_acc)); message_send_text(c, message_type_info, c, msgtemp); return 0; }

						if (member = account_get_clanmember(friend_acc)) {
							char	 status;
							if (status = clanmember_get_status(member)) {
								switch (status) {
								case CLAN_CHIEFTAIN: msgtemp += localize(c, "Chieftain"); break;
								case CLAN_SHAMAN: msgtemp += localize(c, "Shaman"); break;
								case CLAN_GRUNT: msgtemp += localize(c, "Grunt"); break;
								case CLAN_PEON: msgtemp += localize(c, "Peon"); break;
								default:;
								}
							}
							if (status == CLAN_CHIEFTAIN) {
								msgtemp = localize(c, "User {} has already on chieftain! You can't change chieftain to peon!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							if (status == CLAN_PEON) {
								msgtemp = localize(c, "User {} has already on peon!", account_get_name(friend_acc));
								message_send_text(c, message_type_error, c, msgtemp);
								return 0;
							}
							
						}

						clanmember_set_status(account_get_clanmember(friend_acc), CLAN_PEON);
						msgtemp = localize(c, "Successfully change status peon for user {}.", account_get_name(friend_acc));
						message_send_text(c, message_type_info, c, msgtemp);
					}
					
					else if (args[1] == "channel") {
						const char * channel = args[2].c_str();
						clan = clanmember_get_clan(member);
						if (channel[0] == '\0') {
							message_send_text(c, message_type_info, c, "--------------------------------------------------------");
							message_send_text(c, message_type_error, c, "Usage: /clan [channel] (no have alias)");
							message_send_text(c, message_type_info, c, "** Change your clan channel to [channel].");
							return 0;
						}
						if (clan_set_channel(clan, channel)<0)
							message_send_text(c, message_type_error, c, "Failed to change Clan channel!");
						else
							message_send_text(c, message_type_info, c, "Sucsessfully to change your clan channel.");
					}
					
					else if (args[1] == "disband" || args[1] == "dis") // Fix
					{
						if (args[2].empty()) {
							message_send_text(c, message_type_info, c, localize(c, "This is one-way action! If you really want"));
							message_send_text(c, message_type_info, c, localize(c, "to disband your clan, type /clan disband yes"));
						}
						
						else if (args[2] == "yes") {
							if (clanlist_remove_clan(clan) == 0) {
								if (clan_get_created(clan) == 1)
									clan_remove(clan_get_clantag(clan));
								clan_destroy(clan);
								message_send_text(c, message_type_info, c, "Your clan was disbanded :(");
							}
							return 0;
						}
					}
				}
			}
			else if ((member = account_get_clanmember_forced(acc)) && (clan = clanmember_get_clan(member)) && (clanmember_get_fullmember(member) == 0)) {
				if (args[1].empty())
				{
					message_send_text(c, message_type_info, c, "--------------------------------------------------------");
					message_send_text(c, message_type_error, c, "Usage: /clan invite get (no have alias)");
					message_send_text(c, message_type_info, c, "** For show clanname wich you have been invited.");
					message_send_text(c, message_type_error, c, "Usage: /clan invite accept (alias: acc)");
					message_send_text(c, message_type_info, c, "** For Accept invitation to clan");
					message_send_text(c, message_type_error, c, "Usage: /clan invite decline (alias: dec)");
					message_send_text(c, message_type_info, c, "** For decline invitation to clan");
				}
				
				else if (args[1] == "inv" || args[1] == "invite") { // Fix
					if (args[2].empty()) {
						message_send_text(c, message_type_info, c, "--------------------------------------------------------");
						message_send_text(c, message_type_error, c, "Usage: /clan invite get (no have alias)");
						message_send_text(c, message_type_info, c, "** Show clanname wich you have been invited.");
						message_send_text(c, message_type_error, c, "Usage: /clan invite accept (alias: acc)");
						message_send_text(c, message_type_info, c, "** Accept invitation clan.");
						message_send_text(c, message_type_error, c, "Usage: /clan invite decline (alias: dec)");
						message_send_text(c, message_type_info, c, "** Decline invitation clan.");
					}
					
					else if (args[2] == "get") { // Fix
						msgtemp = localize(c, "You have been invited to {}", clan_get_name(clan));
						message_send_text(c, message_type_info, c, msgtemp);
						return 0;
					}
					
					else if (args[2] == "accept" || args[2] == "acc") { // Fix
						int created = clan_get_created(clan);

						clanmember_set_fullmember(member, 1);
						clanmember_set_join_time(member, std::time(NULL));
						msgtemp = localize(c, "You are now a clan member of {}", clan_get_name(clan));
						message_send_text(c, message_type_info, c, msgtemp);
						if (created > 0) {
							DEBUG1("clan {} has already been created", clan_get_name(clan));
							return -1;
						}
						created++;
						if (created >= 0) {
							clan_set_created(clan, 1);
							clan_set_creation_time(clan, std::time(NULL));
							/* FIXME: send message "CLAN was be created" to members */
							msgtemp = localize(c, "Clan {} was be created", clan_get_name(clan));
							clan_send_message_to_online_members(clan, message_type_whisper, c, msgtemp.c_str()); /* Send message to all members */
							message_send_text(c, message_type_whisper, c, msgtemp);                      /* also to self */
							clan_save(clan);
						}
						else {
							clan_set_created(clan, created);
							clan_save(clan);
						}
						return 0;
					}
					else if (args[2] == "decline" || args[2] == "dec") { // Fix
						clan_remove_member(clan, member);
						msgtemp = localize(c, "You are no longer ivited to {}", clan_get_name(clan));
						message_send_text(c, message_type_info, c, msgtemp);
						return 0;
					}
				}
			}
			else {
				if (args[1].empty())
				{
					message_send_text(c, message_type_info, c, "--------------------------------------------------------");
					message_send_text(c, message_type_error, c, "Usage: /clan create [clantag] [clanname] (alias: cre)");
					message_send_text(c, message_type_info, c, "** Create a new clan (max <clantag> length = 4).");
				}
				
				if ((args[1] == "create" || args[1] == "cre")) // Fix
				{
					char const *clantag, *clanname;
					std::vector<std::string> args = split_command(text, 3);

					if (args[3].empty())
					{
						message_send_text(c, message_type_info, c, "--------------------------------------------------------");
						message_send_text(c, message_type_error, c, "Usage: /clan create [clantag] [clanname] (alias: cre)");
						message_send_text(c, message_type_info, c, "** Create a new clan (max <clantag> length = 4).");
						return -1;
					}
					clantag = args[2].c_str(); // clan tag
					clanname = args[3].c_str(); // clan name

					/* if (clan = clanlist_find_clan_by_clantag(str_to_clantag(clantag))) {
						message_send_text(c, message_type_error, c, localize(c, "Clan with your specified <clantag> already exist!"));
						message_send_text(c, message_type_error, c, localize(c, "Please choice another one!"));
						return -1;
					} */

					if ((clan = clan_create(conn_get_account(c), str_to_clantag(clantag), clanname, NULL)) && clanlist_add_clan(clan))
					{
						member = account_get_clanmember_forced(acc);
						if (prefs_get_clan_min_invites() == 0) {
							clan_set_created(clan, 1);
							clan_set_creation_time(clan, std::time(NULL));
							msgtemp = localize(c, "Clan {} is created!", clan_get_name(clan));
							message_send_text(c, message_type_info, c, msgtemp);
							std::snprintf(msgtemp0, sizeof(msgtemp0), "CLAN %s", clantag);
							account_set_strattr(acc,"BNET\\clan\\channel", msgtemp0);
							clan_save(clan);
						}
						else {
							clan_set_created(clan, -prefs_get_clan_min_invites() + 1); //Pelish: +1 means that creator of clan is already invited
							msgtemp = localize(c, "Clan {} is pre-created, please invite", clan_get_name(clan));
							message_send_text(c, message_type_info, c, msgtemp);
							msgtemp = localize(c, "at least {} players to your clan by using", prefs_get_clan_min_invites());
							message_send_text(c, message_type_info, c, msgtemp);
							message_send_text(c, message_type_info, c, localize(c, "/clan invite <username> command."));
						}
					}
					return 0;
				}
			}
			
		return 0;
		}

		static int command_set_flags(t_connection * c)
		{
			return channel_set_userflags(c);
		}

		struct glist_cb_struct {
			t_game_difficulty diff;
			t_clienttag tag;
			t_connection *c;
			bool lobby;
		};

		static int _glist_cb(t_game *game, void *data)
		{
			struct glist_cb_struct *cbdata = (struct glist_cb_struct*)data;

			if ((!cbdata->tag || !prefs_get_hide_pass_games() || game_get_flag(game) != game_flag_private) &&
				(!cbdata->tag || game_get_clienttag(game) == cbdata->tag) &&
				(cbdata->diff == game_difficulty_none || game_get_difficulty(game) == cbdata->diff) &&
				(cbdata->lobby == false || (game_get_status(game) != game_status_started && game_get_status(game) != game_status_done)))
			{
				std::snprintf(msgtemp0, sizeof(msgtemp0), " %-16.16s %1.1s %-8.8s %-21.21s %5u ",
					game_get_name(game),
					game_get_flag(game) != game_flag_private ? "n" : "y",
					game_status_get_str(game_get_status(game)),
					game_type_get_str(game_get_type(game)),
					game_get_ref(game));

				if (!cbdata->tag)
				{

					std::strcat(msgtemp0, clienttag_uint_to_str(game_get_clienttag(game)));
					std::strcat(msgtemp0, " ");
				}

				if ((!prefs_get_hide_addr()) || (account_get_command_groups(conn_get_account(cbdata->c)) & command_get_group("/admin-addr"))) /* default to false */
					std::strcat(msgtemp0, addr_num_to_addr_str(game_get_addr(game), game_get_port(game)));

				message_send_text(cbdata->c, message_type_info, cbdata->c, msgtemp0);
			}

			return 0;
		}

		static int _handle_games_command(t_connection * c, char const *text)
		{
			char           clienttag_str[5];
			char const         * dest;
			char const         * difficulty;
			struct glist_cb_struct cbdata;

			std::vector<std::string> args = split_command(text, 2);

			dest = args[1].c_str(); // clienttag
			difficulty = args[1].c_str(); // difficulty (only for diablo)

			cbdata.c = c;
			cbdata.lobby = false;

			if (strcasecmp(difficulty, "norm") == 0)
				cbdata.diff = game_difficulty_normal;
			else if (strcasecmp(difficulty, "night") == 0)
				cbdata.diff = game_difficulty_nightmare;
			else if (strcasecmp(difficulty, "hell") == 0)
				cbdata.diff = game_difficulty_hell;
			else
				cbdata.diff = game_difficulty_none;

			if (dest[0] == '\0')
			{
				cbdata.tag = conn_get_clienttag(c);
				message_send_text(c, message_type_info, c, localize(c, "Currently accessible games:"));
			}
			else if (strcasecmp(dest, "all") == 0)
			{
				cbdata.tag = 0;
				message_send_text(c, message_type_info, c, localize(c, "All current games:"));
			}
			else if (strcasecmp(dest, "lobby") == 0 || strcasecmp(dest, "l") == 0)
			{
				cbdata.tag = conn_get_clienttag(c);
				cbdata.lobby = true;
				message_send_text(c, message_type_info, c, localize(c, "Games in lobby:"));
			}
			else
			{
				if (!(cbdata.tag = tag_validate_client(dest)))
				{
					describe_command(c, args[0].c_str());
					return -1;
				}

				if (cbdata.diff == game_difficulty_none)
					msgtemp = localize(c, "Current games of type {}", tag_uint_to_str(clienttag_str, cbdata.tag));
				else
					msgtemp = localize(c, "Current games of type {} {}", tag_uint_to_str(clienttag_str, cbdata.tag), difficulty);
				message_send_text(c, message_type_info, c, msgtemp);
			}

			msgtemp = localize(c, " ------name------ p -status- --------type--------- count ");
			if (!cbdata.tag)
				msgtemp += localize(c, "ctag ");
			if ((!prefs_get_hide_addr()) || (account_get_command_groups(conn_get_account(c)) & command_get_group("/admin-addr"))) /* default to false */
				msgtemp += localize(c, "--------addr--------");
			message_send_text(c, message_type_info, c, msgtemp);
			gamelist_traverse(_glist_cb, &cbdata, gamelist_source_none);

			return 0;
		}

		static int _handle_channels_command(t_connection * c, char const *text)
		{
			t_elem const *    curr;
			t_channel const * channel;
			t_clienttag       clienttag;
			t_connection const * conn;
			t_account * acc;
			char const * name;
			int first;

			std::vector<std::string> args = split_command(text, 1);

			if (args[1].empty())
			{
				describe_command(c, args[0].c_str());
				return -1;
			}
			text = args[1].c_str(); // clienttag

			if (text[0] == '\0')
			{
				clienttag = conn_get_clienttag(c);
				message_send_text(c, message_type_info, c, localize(c, "Currently accessible channels:"));
			}
			else if (strcasecmp(text, "all") == 0)
			{
				clienttag = 0;
				message_send_text(c, message_type_info, c, localize(c, "All current channels:"));
			}
			else
			{
				if (!(clienttag = tag_validate_client(text)))
				{
					describe_command(c, args[0].c_str());
					return -1;
				}
				msgtemp = localize(c, "Current channels of type {}", text);
				message_send_text(c, message_type_info, c, msgtemp);
			}

			msgtemp = localize(c, " -----------name----------- users ----admin/operator----");
			message_send_text(c, message_type_info, c, msgtemp);
			LIST_TRAVERSE_CONST(channellist(), curr)
			{
				channel = (t_channel*)elem_get_data(curr);
				if ((!(channel_get_flags(channel) & channel_flags_clan)) && (!clienttag || !prefs_get_hide_temp_channels() || channel_get_permanent(channel)) &&
					(!clienttag || !channel_get_clienttag(channel) ||
					channel_get_clienttag(channel) == clienttag) &&
					((channel_get_max(channel) != 0) || //only show restricted channels to OPs and Admins
					((channel_get_max(channel) == 0 && account_is_operator_or_admin(conn_get_account(c), NULL)))) &&
					(!(channel_get_flags(channel) & channel_flags_thevoid)) // don't list TheVoid
					)
				{

					std::snprintf(msgtemp0, sizeof(msgtemp0), " %-26.26s %5d - ",
						channel_get_name(channel),
						channel_get_length(channel));

					first = 1;

					for (conn = channel_get_first(channel); conn; conn = channel_get_next())
					{
						acc = conn_get_account(conn);
						if (account_is_operator_or_admin(acc, channel_get_name(channel)) ||
							channel_conn_is_tmpOP(channel, account_get_conn(acc)))
						{
							name = conn_get_loggeduser(conn);
							if (std::strlen(msgtemp0) + std::strlen(name) + 6 >= MAX_MESSAGE_LEN) break;
							if (!first) std::strcat(msgtemp0, " ,");
							std::strcat(msgtemp0, name);
							if (account_get_auth_admin(acc, NULL) == 1) std::strcat(msgtemp0, "(A)");
							else if (account_get_auth_operator(acc, NULL) == 1) std::strcat(msgtemp0, "(O)");
							else if (account_get_auth_admin(acc, channel_get_name(channel)) == 1) std::strcat(msgtemp0, "(a)");
							else if (account_get_auth_operator(acc, channel_get_name(channel)) == 1) std::strcat(msgtemp0, "(o)");
							first = 0;
						}
					}

					message_send_text(c, message_type_info, c, msgtemp0);
				}
			}

			return 0;
		}
		
		// New
		static int _handle_botrules_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!rules %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!rules %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botdonate_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!donate %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!donate %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botevent_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!event %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!event %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botrequest_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!request %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!request %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botonline_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!online %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!online %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botbuy_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!buy %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!buy %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botcash_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!cash %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!cash %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botcreate_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!create %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!create %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_bottop10_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!top10 %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!top10 %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botstats_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!stats %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!stats %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botstatus_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!status %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!status %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botdota_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!dota %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!dota %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botlod_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!lod %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!lod %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botimba_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!imba %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!imba %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botchat_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!chat %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!chat %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botaccept_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!accept %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!accept %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botdecline_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!decline %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!decline %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botlock_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!lock %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!lock %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botunlock_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!unlock %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!unlock %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botmute_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!mute %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!mute %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
		static int _handle_botunmute_command(t_connection * c, char const *text)
		{
			unsigned int i;
			t_connection *    user;
			t_game     *    game;
			char const * bot="|c00FF4444Battlenet";
			
			std::vector<std::string> args = split_command(text, 1);
			const char * username = args[1].c_str();
			text = args[1].c_str(); // message
			
			if (args[1].empty()) {
				std::snprintf(msgtemp0, sizeof(msgtemp0), "!unmute %s", text);
				do_botchat(c,bot,msgtemp0);
				return 0;
			}
			
			std::snprintf(msgtemp0, sizeof(msgtemp0), "!unmute %s", text);
			do_botchat(c,bot,msgtemp0);
			
			return 0;
		}
		
	}
}
