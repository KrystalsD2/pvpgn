/*
 * Copyright (C) 2000,2001	Onlyer	(onlyer@263.net)
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
#include "setup.h"
#include "d2gs.h"

#include <limits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <cctype>
#include <limits.h>
#include <string>
#include <map>
#include <vector>

#include "compat/psock.h"
#include "compat/strncasecmp.h"
#include "compat/strcasecmp.h"
#include "common/addr.h"
#include "common/eventlog.h"
#include "common/xalloc.h"
#include "common/introtate.h"
#include "prefs.h"
#include "cmdline.h"
#include "game.h"
#include "net.h"
#include "common/setup_after.h"

namespace pvpgn
{

	namespace d2cs
	{

		static t_list		* d2gslist_head = NULL;
		/* the configured gslist addresses, kept in config order so a client can
		 * request a specific server by its position (game description "GSn") */
		static t_addrlist	* d2gs_addrs = NULL;
		static unsigned int	d2gs_id = 0;
		static unsigned int	total_d2gs = 0;

		extern t_list *	d2gslist(void)
		{
			return d2gslist_head;
		}

		extern int d2gslist_create(void)
		{
			d2gslist_head = list_create();
			return d2gslist_reload(prefs_get_d2gs_list());
		}

		extern int d2gslist_reload(char const * gslist)
		{
			t_d2gs		* gs;

			if (!d2gslist_head) return -1;

			BEGIN_LIST_TRAVERSE_DATA(d2gslist_head, gs, t_d2gs)
			{
				BIT_CLR_FLAG(gs->flag, D2GS_FLAG_VALID);
			}
			END_LIST_TRAVERSE_DATA()

			std::string input(gslist ? gslist : "");
			std::size_t pos = 0;
			while (pos <= input.size()) {
				std::size_t end = input.find(',', pos);
				std::string item = input.substr(pos,
					end == std::string::npos ? std::string::npos : end - pos);
				while (!item.empty() && std::isspace((unsigned char)item[0])) item.erase(0, 1);
				while (!item.empty() && std::isspace((unsigned char)item[item.size() - 1])) item.erase(item.size() - 1);

				if (!item.empty()) {
					std::string name;
					std::string ipaddr;
					std::size_t equals = item.find('=');
					if (equals == std::string::npos) {
						name = item;
						ipaddr = item;
					} else {
						name = item.substr(0, equals);
						ipaddr = item.substr(equals + 1);
						while (!name.empty() && std::isspace((unsigned char)name[name.size() - 1])) name.erase(name.size() - 1);
						while (!ipaddr.empty() && std::isspace((unsigned char)ipaddr[0])) ipaddr.erase(0, 1);
					}

					unsigned int ip = net_inet_addr(ipaddr.c_str());
					if (ip == ~0U) {
						eventlog(eventlog_level_error, __FUNCTION__,
							"bad named game server entry {}", item);
					} else {
						if (!(gs = d2gslist_find_gs_by_ip(ntohl(ip))))
							gs = d2gs_create(ipaddr.c_str(), name.c_str());
						else {
							std::strncpy(gs->name, name.c_str(), sizeof(gs->name) - 1);
							gs->name[sizeof(gs->name) - 1] = '\0';
						}
						if (gs) BIT_SET_FLAG(gs->flag, D2GS_FLAG_VALID);
					}
				}
				if (end == std::string::npos) break;
				pos = end + 1;
			}

			BEGIN_LIST_TRAVERSE_DATA(d2gslist_head, gs, t_d2gs)
			{
				if (!BIT_TST_FLAG(gs->flag, D2GS_FLAG_VALID)) {
					d2gs_destroy(gs, &curr_elem_);
				}
			}
			END_LIST_TRAVERSE_DATA()
				return 0;
		}

		struct t_live_gs_entry
		{
			std::string name;
			std::string ipaddr;
			unsigned int ip;
		};

		static std::string d2gs_trim(std::string value)
		{
			while (!value.empty() && std::isspace((unsigned char)value[0])) value.erase(0, 1);
			while (!value.empty() && std::isspace((unsigned char)value[value.size() - 1])) value.erase(value.size() - 1);
			return value;
		}

		extern int d2gslist_reload_live(char const * gslist)
		{
			if (!d2gslist_head)
				return -1;

			std::vector<t_live_gs_entry> parsed;
			std::string input(gslist ? gslist : "");
			std::size_t pos = 0;
			while (pos <= input.size())
			{
				std::size_t end = input.find(',', pos);
				std::string item = d2gs_trim(input.substr(pos,
					end == std::string::npos ? std::string::npos : end - pos));
				if (!item.empty())
				{
					std::size_t equals = item.find('=');
					t_live_gs_entry entry;
					entry.name = d2gs_trim(equals == std::string::npos ? item : item.substr(0, equals));
					entry.ipaddr = d2gs_trim(equals == std::string::npos ? item : item.substr(equals + 1));
					unsigned int network_ip = net_inet_addr(entry.ipaddr.c_str());
					if (entry.name.empty() || entry.ipaddr.empty() || network_ip == ~0U)
					{
						eventlog(eventlog_level_error, __FUNCTION__,
							"[GS-LIVE-RELOAD] invalid game server entry '{}'; active list unchanged", item);
						return -1;
					}
					entry.ip = ntohl(network_ip);
					for (std::vector<t_live_gs_entry>::const_iterator it = parsed.begin(); it != parsed.end(); ++it)
					{
						if (it->ip == entry.ip || strcasecmp(it->name.c_str(), entry.name.c_str()) == 0)
						{
							eventlog(eventlog_level_error, __FUNCTION__,
								"[GS-LIVE-RELOAD] duplicate game server name/IP in entry '{}'; active list unchanged", item);
							return -1;
						}
					}
					parsed.push_back(entry);
				}
				if (end == std::string::npos) break;
				pos = end + 1;
			}

			if (parsed.empty())
			{
				eventlog(eventlog_level_error, __FUNCTION__,
					"[GS-LIVE-RELOAD] refusing empty gameservlist; active list unchanged");
				return -1;
			}

			t_d2gs * gs;
			BEGIN_LIST_TRAVERSE_DATA(d2gslist_head, gs, t_d2gs)
			{
				BIT_CLR_FLAG(gs->flag, D2GS_FLAG_VALID);
			}
			END_LIST_TRAVERSE_DATA()

			for (std::vector<t_live_gs_entry>::const_iterator it = parsed.begin(); it != parsed.end(); ++it)
			{
				gs = d2gslist_find_gs_by_ip(it->ip);
				if (!gs)
					gs = d2gs_create(it->ipaddr.c_str(), it->name.c_str());
				else
				{
					std::strncpy(gs->name, it->name.c_str(), sizeof(gs->name) - 1);
					gs->name[sizeof(gs->name) - 1] = '\0';
				}
				if (!gs)
					return -1;
				BIT_SET_FLAG(gs->flag, D2GS_FLAG_VALID);
			}

			BEGIN_LIST_TRAVERSE_DATA(d2gslist_head, gs, t_d2gs)
			{
				if (BIT_TST_FLAG(gs->flag, D2GS_FLAG_VALID))
					continue;
				if (gs->active && gs->connection)
				{
					eventlog(eventlog_level_info, __FUNCTION__,
						"[GS-LIVE-RELOAD] draining removed game server {}={} (id {}): existing connection/games retained, new games disabled",
						gs->name, addr_num_to_ip_str(gs->ip), gs->id);
					continue;
				}
				eventlog(eventlog_level_info, __FUNCTION__,
					"[GS-LIVE-RELOAD] removing inactive game server {}={} (id {})",
					gs->name, addr_num_to_ip_str(gs->ip), gs->id);
				d2gs_destroy(gs, &curr_elem_);
			}
			END_LIST_TRAVERSE_DATA()

			eventlog(eventlog_level_info, __FUNCTION__,
				"[GS-LIVE-RELOAD] applied {} configured game server(s) without disconnecting active D2GS connections", parsed.size());
			return 0;
		}

		extern int d2gslist_destroy(void)
		{
			t_d2gs	* gs;

			BEGIN_LIST_TRAVERSE_DATA_CONST(d2gslist_head, gs, t_d2gs)
			{
				d2gs_destroy(gs, (t_elem **)&curr_elem_);
			}
			END_LIST_TRAVERSE_DATA_CONST()
				d2cs_connlist_reap();

			if (list_destroy(d2gslist_head) < 0) {
				eventlog(eventlog_level_error, __FUNCTION__, "error destroy d2gs list");
				return -1;
			}
			d2gslist_head = NULL;
			if (d2gs_addrs) addrlist_destroy(d2gs_addrs);
			d2gs_addrs = NULL;
			return 0;
		}

		extern t_d2gs * d2gslist_find_gs_by_ip(unsigned int ip)
		{
			t_d2gs	* gs;

			BEGIN_LIST_TRAVERSE_DATA_CONST(d2gslist_head, gs, t_d2gs)
			{
				if (gs->ip == ip) return gs;
			}
			END_LIST_TRAVERSE_DATA_CONST()
				return NULL;
		}

		extern t_d2gs * d2gslist_find_gs(unsigned int id)
		{
			t_d2gs	* gs;

			BEGIN_LIST_TRAVERSE_DATA_CONST(d2gslist_head, gs, t_d2gs)
			{
				if (gs->id == id) return gs;
			}
			END_LIST_TRAVERSE_DATA_CONST()
				return NULL;
		}

		extern t_d2gs * d2gs_create(char const * ipaddr, char const * name)
		{
			t_d2gs 	* gs;
			unsigned int	ip;

			ASSERT(ipaddr, NULL);
			if ((ip = net_inet_addr(ipaddr)) == ~0U) {
				eventlog(eventlog_level_error, __FUNCTION__, "got bad ip address {}", ipaddr);
				return NULL;
			}
			if (d2gslist_find_gs_by_ip(ntohl(ip))) {
				eventlog(eventlog_level_error, __FUNCTION__, "game server {} already in list", ipaddr);
				return NULL;
			}
			gs = (t_d2gs*)xmalloc(sizeof(t_d2gs));
			gs->ip = ntohl(ip);
			std::strncpy(gs->name, (name && *name) ? name : ipaddr, sizeof(gs->name) - 1);
			gs->name[sizeof(gs->name) - 1] = '\0';
			gs->id = ++d2gs_id;
			gs->active = 0;
			gs->token = 0;
			gs->state = d2gs_state_none;
			gs->gamenum = 0;
			gs->maxgame = 0;
			gs->connection = NULL;

			if (list_append_data(d2gslist_head, gs) < 0) {
				eventlog(eventlog_level_error, __FUNCTION__, "error add gs to list");
				xfree(gs);
				return NULL;
			}
			eventlog(eventlog_level_info, __FUNCTION__, "added game server {} (id: {}) to list", ipaddr, gs->id);
			return gs;
		}

		extern int d2gs_destroy(t_d2gs * gs, t_elem ** curr)
		{
			ASSERT(gs, -1);
			if (list_remove_data(d2gslist_head, gs, curr) < 0) {
				eventlog(eventlog_level_error, __FUNCTION__, "error remove gs from list");
				return -1;
			}
			if (gs->active && gs->connection) {
				d2cs_conn_set_state(gs->connection, conn_state_destroy);
				d2gs_deactive(gs, gs->connection);
			}
			eventlog(eventlog_level_info, __FUNCTION__, "removed game server {} (id: {}) from list", addr_num_to_ip_str(gs->ip), gs->id);
			xfree(gs);
			return 0;
		}

		extern t_d2gs * d2gslist_get_server_by_id(unsigned int id)
		{
			t_d2gs	* gs;

			BEGIN_LIST_TRAVERSE_DATA_CONST(d2gslist_head, gs, t_d2gs)
			{
				if (gs->id == id) return gs;
			}
			END_LIST_TRAVERSE_DATA_CONST()
				return NULL;
		}

		/* a game server can host a game only if it is authed and has a free slot */
		static int d2gs_can_host(t_d2gs const * gs)
		{
			return gs->active && gs->connection && gs->state == d2gs_state_authed &&
				gs->maxgame && gs->gamenum < gs->maxgame;
		}

		/* a game description of "GSn" asks for the n-th server of the gslist */
		static t_d2gs * d2gslist_find_gs_by_desc(char const * game_desc)
		{
			t_addr	* gs_addr;
			int	gs_idx;

			if (!game_desc || strncasecmp(game_desc, "gs", 2)) return NULL;
			if ((gs_idx = std::atoi(&game_desc[2])) <= 0) return NULL;
			gs_idx--;
			if (!d2gs_addrs || gs_idx >= addrlist_get_length(d2gs_addrs)) return NULL;
			if (!(gs_addr = (t_addr *)list_get_data_by_pos(d2gs_addrs, gs_idx))) return NULL;
			return d2gslist_find_gs_by_ip(addr_get_ip(gs_addr));
		}

		extern t_d2gs * d2gslist_choose_server(char const * game_name, char const * game_desc)
		{
			t_d2gs			* gs;
			t_d2gs			* ogs;
			unsigned int		percent;
			unsigned int		min_percent = 100;

			if ((gs = d2gslist_find_gs_by_desc(game_desc))) {
				if (d2gs_can_host(gs)) {
					eventlog(eventlog_level_info, __FUNCTION__, "using requested game server {} (id: {}) for game {} ({})",
						addr_num_to_ip_str(gs->ip), gs->id, game_name, game_desc);
					return gs;
				}
				eventlog(eventlog_level_info, __FUNCTION__, "requested game server {} (id: {}) for game {} ({}) is unavailable, choosing another",
					addr_num_to_ip_str(gs->ip), gs->id, game_name, game_desc);
			}

			ogs = NULL;
			BEGIN_LIST_TRAVERSE_DATA_CONST(d2gslist_head, gs, t_d2gs)
			{
				if (!d2gs_can_host(gs)) continue;
				percent = 100 * gs->gamenum / gs->maxgame;
				if (percent < min_percent) {
					min_percent = percent;
					ogs = gs;
				}
			}
			END_LIST_TRAVERSE_DATA_CONST()
				return ogs;
		}

		static int d2gs_name_in_list(char const * list, char const * name)
		{
			if (!list || !*list || !name || !*name) return 0;
			std::string input(list);
			std::size_t pos = 0;
			while (pos <= input.size()) {
				std::size_t end = input.find(',', pos);
				std::string item = input.substr(pos,
					end == std::string::npos ? std::string::npos : end - pos);
				if (strcasecmp(item.c_str(), name) == 0) return 1;
				if (end == std::string::npos) break;
				pos = end + 1;
			}
			return 0;
		}

		extern int d2gs_is_excluded(t_d2gs const * gs, char const * excluded)
		{
			ASSERT(gs, 0);
			return d2gs_name_in_list(excluded, gs->name);
		}

		static int d2gs_is_available(t_d2gs const * gs)
		{
			return gs && BIT_TST_FLAG(gs->flag, D2GS_FLAG_VALID) && gs->active && gs->connection &&
				gs->state == d2gs_state_authed && gs->maxgame &&
				gs->gamenum < gs->maxgame;
		}

		extern t_d2gs * d2gslist_choose_server_for_player(char const * preferred,
			char const * excluded)
		{
			t_d2gs * gs;
			t_d2gs * best = NULL;
			unsigned int min_percent = 101;

			BEGIN_LIST_TRAVERSE_DATA_CONST(d2gslist_head, gs, t_d2gs)
			{
				if (!d2gs_is_available(gs) || d2gs_is_excluded(gs, excluded)) continue;
				if (preferred && *preferred && !d2gs_name_in_list(preferred, gs->name)) continue;
				unsigned int percent = 100 * gs->gamenum / gs->maxgame;
				if (percent < min_percent) {
					min_percent = percent;
					best = gs;
				}
			}
			END_LIST_TRAVERSE_DATA_CONST()

			if (best || !preferred || !*preferred) return best;

			min_percent = 101;
			BEGIN_LIST_TRAVERSE_DATA_CONST(d2gslist_head, gs, t_d2gs)
			{
				if (!d2gs_is_available(gs) || d2gs_is_excluded(gs, excluded)) continue;
				unsigned int percent = 100 * gs->gamenum / gs->maxgame;
				if (percent < min_percent) {
					min_percent = percent;
					best = gs;
				}
			}
			END_LIST_TRAVERSE_DATA_CONST()
			return best;
		}

		extern char const * d2gs_get_name(t_d2gs const * gs)
		{
			ASSERT(gs, NULL);
			return gs->name;
		}

		extern int d2gs_set_state(t_d2gs * gs, t_d2gs_state state)
		{
			ASSERT(gs, -1);
			gs->state = state;
			return 0;
		}

		extern t_d2gs_state d2gs_get_state(t_d2gs const * gs)
		{
			ASSERT(gs, d2gs_state_none);
			return gs->state;
		}

		extern int d2gs_add_gamenum(t_d2gs * gs, int number)
		{
			ASSERT(gs, -1);
			gs->gamenum += number;
			return 0;
		}

		extern unsigned int d2gs_get_gamenum(t_d2gs const * gs)
		{
			ASSERT(gs, 0);
			return gs->gamenum;
		}


		static std::string d2gs_shared_var_path(char const * preffile, char const * leaf)
		{
			if (!leaf || !*leaf)
				return std::string();

			std::string conf = preffile ? preffile : "";
			if (conf.empty())
				return std::string("var/") + leaf;

			/*
			 * Resolve the exact configuration file that this process loaded before
			 * deriving the shared var directory.  Using only a relative string such
			 * as "conf/d2cs.conf" makes bnetd, D2CS and D2DBS disagree whenever
			 * Windows starts them with different current working directories.
			 */
		#ifdef WIN32
			char resolved[_MAX_PATH];
			if (_fullpath(resolved, conf.c_str(), _MAX_PATH) != NULL)
				conf.assign(resolved);
		#else
			char resolved[PATH_MAX];
			if (realpath(conf.c_str(), resolved) != NULL)
				conf.assign(resolved);
		#endif

			std::size_t last = conf.find_last_of("/\\");
			if (last == std::string::npos)
				return std::string("var/") + leaf;

			char sep = conf[last];
			std::string confdir = conf.substr(0, last);
			std::string base = confdir;
			std::size_t parent_sep = confdir.find_last_of("/\\");
			std::string dirname = parent_sep == std::string::npos ? confdir : confdir.substr(parent_sep + 1);
			if (strcasecmp(dirname.c_str(), "conf") == 0)
			{
				if (parent_sep == std::string::npos)
					base = ".";
				else
					base = confdir.substr(0, parent_sep);
			}
			if (base.empty())
				base = ".";
			return base + sep + "var" + sep + leaf;
		}

		struct t_d2gs_status_totals
		{
			unsigned int games;
			unsigned int players;
			bool online;
			bool configured;
			t_d2gs_status_totals() : games(0), players(0), online(false), configured(false) {}
		};

		extern int d2gs_write_status_file(void)
		{
			std::map<std::string, t_d2gs_status_totals> totals;
			t_d2gs * gs;
			BEGIN_LIST_TRAVERSE_DATA_CONST(d2gslist_head, gs, t_d2gs)
			{
				totals[gs->name].games += gs->gamenum;
				if (gs->state == d2gs_state_authed)
					totals[gs->name].online = true;
				if (BIT_TST_FLAG(gs->flag, D2GS_FLAG_VALID))
					totals[gs->name].configured = true;
			}
			END_LIST_TRAVERSE_DATA_CONST()

			t_game * game;
			BEGIN_LIST_TRAVERSE_DATA_CONST(d2cs_gamelist(), game, t_game)
			{
				gs = game_get_d2gs(game);
				if (gs)
					totals[gs->name].players += game_get_currchar(game);
			}
			END_LIST_TRAVERSE_DATA_CONST()

			std::string status_tmp = d2gs_shared_var_path(cmdline_get_preffile(), "d2gs_status.tmp");
			std::string status = d2gs_shared_var_path(cmdline_get_preffile(), "d2gs_status.txt");
			std::FILE * out = std::fopen(status_tmp.c_str(), "wt");
			if (!out)
				return -1;
			for (std::map<std::string, t_d2gs_status_totals>::const_iterator it = totals.begin(); it != totals.end(); ++it)
				std::fprintf(out, "%s|%u|%u|%u|%u\n", it->first.c_str(), it->second.games,
					it->second.players, it->second.online ? 1U : 0U, it->second.configured ? 1U : 0U);
			if (std::fclose(out) != 0)
				return -1;
			std::remove(status.c_str());
			if (std::rename(status_tmp.c_str(), status.c_str()) != 0)
				return -1;
			{
				static bool path_logged = false;
				if (!path_logged)
				{
					eventlog(eventlog_level_info, __FUNCTION__,
						"[GS-STATUS] publishing live game-server status to {} (config={})",
						status, cmdline_get_preffile());
					path_logged = true;
				}
			}
			return 0;
		}

		extern int d2gs_set_maxgame(t_d2gs * gs, unsigned int maxgame)
		{
			ASSERT(gs, -1);
			gs->maxgame = maxgame;
			return 0;
		}

		extern unsigned int d2gs_get_maxgame(t_d2gs const * gs)
		{
			ASSERT(gs, 0);
			return gs->maxgame;
		}

		extern unsigned int d2gs_get_id(t_d2gs const * gs)
		{
			ASSERT(gs, 0);
			return gs->id;
		}

		extern unsigned int d2gs_get_ip(t_d2gs const * gs)
		{
			ASSERT(gs, 0);
			return gs->ip;
		}

		extern unsigned int d2gs_get_token(t_d2gs const * gs)
		{
			return gs->token;
		}

		extern unsigned int d2gs_make_token(t_d2gs * gs)
		{
			return ((unsigned int)std::rand()) ^ ((++(gs->token)) + ((unsigned int)std::time(NULL)));
		}

		extern t_connection * d2gs_get_connection(t_d2gs const * gs)
		{
			ASSERT(gs, NULL);
			return gs->connection;
		}

		extern int d2gs_active(t_d2gs * gs, t_connection * c)
		{
			ASSERT(gs, -1);
			ASSERT(c, -1);

			if (gs->active && gs->connection) {
				eventlog(eventlog_level_warn, __FUNCTION__, "game server {} is already actived, deactive previous connection first", gs->id);
				d2gs_deactive(gs, gs->connection);
			}
			total_d2gs++;
			eventlog(eventlog_level_info, __FUNCTION__, "game server {} (id: {}) actived ({} total)", addr_num_to_addr_str(d2cs_conn_get_addr(c),
				d2cs_conn_get_port(c)), gs->id, total_d2gs);
			gs->state = d2gs_state_authed;
			gs->connection = c;
			gs->active = 1;
			gs->gamenum = 0;
			gs->maxgame = 0;
			return 0;
		}

		extern int d2gs_deactive(t_d2gs * gs, t_connection * c)
		{
			t_game * game;

			ASSERT(gs, -1);
			if (!gs->active || !gs->connection) {
				eventlog(eventlog_level_warn, __FUNCTION__, "game server {} is not actived yet", gs->id);
				return -1;
			}
			if (gs->connection != c) {
				eventlog(eventlog_level_debug, __FUNCTION__, "game server {} connection mismatch,ignore it", gs->id);
				return 0;
			}
			total_d2gs--;
			eventlog(eventlog_level_info, __FUNCTION__, "game server {} (id: {}) deactived ({} left)", addr_num_to_addr_str(d2cs_conn_get_addr(gs->connection), d2cs_conn_get_port(gs->connection)), gs->id, total_d2gs);
			gs->state = d2gs_state_none;
			gs->connection = NULL;
			gs->active = 0;
			gs->maxgame = 0;
			eventlog(eventlog_level_info, __FUNCTION__, "destroying all games on game server {}", gs->id);
			BEGIN_LIST_TRAVERSE_DATA(d2cs_gamelist(), game, t_game)
			{
				if (game_get_d2gs(game) == gs) game_destroy(game, &curr_elem_);
			}
			END_LIST_TRAVERSE_DATA()
			if (gs->gamenum != 0) {
				eventlog(eventlog_level_error, __FUNCTION__, "game server {} deactived but still with games left", gs->id);
			}
			gs->gamenum = 0;
			return 0;
		}

		extern unsigned int d2gs_calc_checksum(t_connection * c)
		{
			unsigned int	sessionnum, checksum, port, addr;
			unsigned int	i, len, ch;
			char const	* realmname;
			char const	* password;

			ASSERT(c, 0);
			sessionnum = d2cs_conn_get_sessionnum(c);
			checksum = prefs_get_d2gs_checksum();
			port = d2cs_conn_get_port(c);
			addr = d2cs_conn_get_addr(c);
			realmname = prefs_get_realmname();
			password = prefs_get_d2gs_password();

			len = std::strlen(realmname);
			for (i = 0; i < len; i++) {
				ch = (unsigned int)(unsigned char)realmname[i];
				checksum ^= ROTL(sessionnum, i, (std::numeric_limits<unsigned int>::digits));
				checksum ^= ROTL(port, ch, (std::numeric_limits<unsigned int>::digits));
			}
			len = std::strlen(password);
			for (i = 0; i < len; i++) {
				ch = (unsigned int)(unsigned char)password[i];
				checksum ^= ROTL(sessionnum, i, (std::numeric_limits<unsigned int>::digits));
				checksum ^= ROTL(port, ch, (std::numeric_limits<unsigned int>::digits));
			}
			checksum ^= addr;
			return checksum;
		}

		extern int d2gs_keepalive(void)
		{
			t_packet	* packet;
			t_d2gs *	gs;

			if (!(packet = packet_create(packet_class_d2gs))) {
				eventlog(eventlog_level_error, __FUNCTION__, "error creating packet");
				return -1;
			}
			packet_set_size(packet, sizeof(t_d2cs_d2gs_echoreq));
			packet_set_type(packet, D2CS_D2GS_ECHOREQ);
			/* FIXME: sequence number not set */
			bn_int_set(&packet->u.d2cs_d2gs.h.seqno, 0);
			BEGIN_LIST_TRAVERSE_DATA(d2gslist_head, gs, t_d2gs)
			{
				if (gs->active && gs->connection) {
					conn_push_outqueue(gs->connection, packet);
				}
			}
			END_LIST_TRAVERSE_DATA()
				packet_del_ref(packet);
			return 0;
		}

		extern int d2gs_restart_all_gs(void)
		{
			t_packet        * packet;
			t_d2gs          * gs;

			if (!(packet = packet_create(packet_class_d2gs))) {
				eventlog(eventlog_level_error, __FUNCTION__, "error creating packet");
				return -1;
			}
			packet_set_size(packet, sizeof(t_d2cs_d2gs_control));
			packet_set_type(packet, D2CS_D2GS_CONTROL);
			/* FIXME: sequence number not set */
			bn_int_set(&packet->u.d2cs_d2gs.h.seqno, 0);
			bn_int_set(&packet->u.d2cs_d2gs_control.cmd, D2CS_D2GS_CONTROL_CMD_RESTART);
			bn_int_set(&packet->u.d2cs_d2gs_control.value, prefs_get_d2gs_restart_delay());

			BEGIN_LIST_TRAVERSE_DATA(d2gslist_head, gs, t_d2gs)
			{
				if (gs->connection) {
					conn_push_outqueue(gs->connection, packet);
				}
			}

			END_LIST_TRAVERSE_DATA()
				packet_del_ref(packet);
			return 0;
		}

	}

}
