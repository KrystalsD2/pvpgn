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
#include "server.h"

#include <cctype>
#include <limits.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <string>

#ifdef WIN32
# include <conio.h>
#endif

#include "compat/psock.h"
#include "compat/strcasecmp.h"
#include "compat/strerror.h"
#include "common/addr.h"
#include "common/eventlog.h"
#include "common/iprate.h"
#include "common/list.h"
#include "prefs.h"
#include "cmdline.h"
#include "net.h"
#include "connection.h"
#include "serverqueue.h"
#include "gamequeue.h"
#include "d2gs.h"
#include "s2s.h"
#include "handle_signal.h"
#include "game.h"
#include "d2ladder.h"

#ifdef HAVE_ARPA_INET_H
# include <arpa/inet.h>
#endif
#ifdef HAVE_WS2TCPIP_H
# include <Ws2tcpip.h>
#endif

#include "common/setup_after.h"


#ifdef WIN32
extern int g_ServiceStatus;
#endif

namespace pvpgn
{

namespace d2cs
{

static int server_listen(void);
static int server_accept(int sock);
static int server_handle_timed_event(void);
static int server_handle_socket(void);
static int server_loop(void);
static int server_cleanup(void);

t_addrlist		* server_listen_addrs;
static IPRateLimiter connection_rate_limiter;
static std::map<unsigned int, std::time_t> connection_cap_notice_times;
static std::time_t connection_cap_notice_last_purge = 0;
static std::time_t socket_pool_notice_time = 0;

static std::string reloadgs_shared_var_path(char const * preffile, char const * leaf)
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

static std::string reloadgs_trim(std::string value)
{
	while (!value.empty() && std::isspace((unsigned char)value[0])) value.erase(0, 1);
	while (!value.empty() && std::isspace((unsigned char)value[value.size() - 1])) value.erase(value.size() - 1);
	return value;
}

static int reloadgs_read_conf_value(char const * filename, char const * key, std::string * value)
{
	if (!filename || !key || !value) return -1;
	std::ifstream conf(filename);
	if (!conf) return -1;
	std::string line;
	while (std::getline(conf, line))
	{
		line = reloadgs_trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		std::size_t equals = line.find('=');
		if (equals == std::string::npos) continue;
		if (strcasecmp(reloadgs_trim(line.substr(0, equals)).c_str(), key) != 0) continue;
		*value = reloadgs_trim(line.substr(equals + 1));
		return value->empty() ? -1 : 0;
	}
	return -1;
}

static int reloadgs_read_trigger(unsigned long long * token)
{
	if (!token) return -1;
	std::string trigger = reloadgs_shared_var_path(cmdline_get_preffile(), "reloadgs.trigger");
	std::ifstream in(trigger.c_str());
	if (!in) return -1;
	unsigned long long value = 0;
	in >> value;
	if (!in || !value) return -1;
	*token = value;
	return 0;
}

static void reloadgs_check_live_request(void)
{
	static unsigned long long last_token = 0;
	unsigned long long token = 0;
	if (reloadgs_read_trigger(&token) < 0)
		return;
	if (token == last_token)
		return;
	last_token = token;

	std::string gameservlist;
	if (reloadgs_read_conf_value(cmdline_get_preffile(), "gameservlist", &gameservlist) < 0)
	{
		eventlog(eventlog_level_error, __FUNCTION__,
			"[GS-LIVE-RELOAD] token={} could not read gameservlist from {}; active list unchanged",
			token, cmdline_get_preffile());
		return;
	}
	if (d2gslist_reload_live(gameservlist.c_str()) < 0)
	{
		eventlog(eventlog_level_error, __FUNCTION__,
			"[GS-LIVE-RELOAD] token={} D2CS gameservlist validation/apply failed; active list unchanged",
			token);
		return;
	}
	prefs_set_d2gs_list_live(gameservlist.c_str());
	d2gs_write_status_file();
	eventlog(eventlog_level_info, __FUNCTION__,
		"[GS-LIVE-RELOAD] token={} D2CS gameservlist applied from {} without disconnecting active players/games; trigger={}; status={}",
		token, cmdline_get_preffile(), reloadgs_shared_var_path(cmdline_get_preffile(), "reloadgs.trigger"),
		reloadgs_shared_var_path(cmdline_get_preffile(), "d2gs_status.txt"));
}
static bool should_log_connection_cap(unsigned int address, std::time_t current_time)
{
	if (!connection_cap_notice_last_purge ||
		current_time - connection_cap_notice_last_purge >= 60)
	{
		std::map<unsigned int, std::time_t>::iterator it = connection_cap_notice_times.begin();
		while (it != connection_cap_notice_times.end())
		{
			if (current_time - it->second > 300)
			{
				std::map<unsigned int, std::time_t>::iterator dead = it++;
				connection_cap_notice_times.erase(dead);
			}
			else
			{
				++it;
			}
		}
		connection_cap_notice_last_purge = current_time;
	}

	std::map<unsigned int, std::time_t>::iterator found = connection_cap_notice_times.find(address);
	if (found != connection_cap_notice_times.end() &&
		current_time - found->second < 30)
		return false;

	connection_cap_notice_times[address] = current_time;
	return true;
}

static int server_listen(void)
{
	t_addr		* curr_laddr;
	t_addr_data	laddr_data;
	int		sock;

	if (!(server_listen_addrs=addrlist_create(prefs_get_servaddrs(),INADDR_ANY,D2CS_SERVER_PORT))) {
		eventlog(eventlog_level_error,__FUNCTION__,"error create listening address list");
		return -1;
	}
	BEGIN_LIST_TRAVERSE_DATA(server_listen_addrs,curr_laddr,t_addr)
	{
		sock=net_listen(addr_get_ip(curr_laddr),addr_get_port(curr_laddr),PSOCK_SOCK_STREAM);
		if (sock<0) {
			eventlog(eventlog_level_error,__FUNCTION__,"error listen socket");
			return -1;
		}

		if (psock_ctl(sock,PSOCK_NONBLOCK)<0) {
			eventlog(eventlog_level_error,__FUNCTION__,"error set listen socket in non-blocking mode");
		}

		laddr_data.i = sock;
		addr_set_data(curr_laddr,laddr_data);

		if (fdwatch_add_fd(sock, fdwatch_type_read, d2cs_server_handle_accept, curr_laddr)<0) {
		    eventlog(eventlog_level_error,__FUNCTION__,"error adding socket {} to fdwatch pool (max sockets?)",sock);
		    psock_close(sock);
		    return -1;
		}

		eventlog(eventlog_level_info,__FUNCTION__,"listen on {}", addr_num_to_addr_str(addr_get_ip(curr_laddr),addr_get_port(curr_laddr)));
	}
	END_LIST_TRAVERSE_DATA()
	return 0;
}

static int server_accept(int sock)
{
	int			csock;
	struct sockaddr_in	caddr, raddr;
	psock_t_socklen		caddr_len, raddr_len;
	int			val;
	unsigned int		ip;
	unsigned short		port;
	t_connection	       *cc;
	char			addrstr[INET_ADDRSTRLEN] = { 0 };

	caddr_len=sizeof(caddr);
	std::memset(&caddr,0,sizeof(caddr));
	csock=psock_accept(sock,(struct sockaddr *)&caddr,&caddr_len);
	if (csock<0) {
		int err = psock_errno();
		if (
#ifdef PSOCK_EWOULDBLOCK
			err == PSOCK_EWOULDBLOCK ||
#endif
#ifdef PSOCK_EAGAIN
			err == PSOCK_EAGAIN ||
#endif
			0)
			return 1;

		if (
#ifdef PSOCK_EINTR
			err == PSOCK_EINTR ||
#endif
#ifdef PSOCK_ECONNABORTED
			err == PSOCK_ECONNABORTED ||
#endif
#ifdef PSOCK_EPROTO
			err == PSOCK_EPROTO ||
#endif
			0)
			return 0;

		eventlog(eventlog_level_error,__FUNCTION__,"error accept new connection: {}",pstrerror(err));
		return -1;
	}
	else
	{
		unsigned int peer_addr = ntohl(caddr.sin_addr.s_addr);
		inet_ntop(AF_INET, &(caddr.sin_addr), addrstr, sizeof(addrstr));

		/* Apply the guard to every accepted socket.  A source IP that matches a
		 * configured D2GS address is not proof that this particular connection is
		 * a D2GS server.  Authentication establishes that trust later. */
		bool newly_blocked = false;
		std::time_t guard_now = std::time(NULL);
		if (!connection_rate_limiter.allow(peer_addr,
			prefs_get_connection_rate_limit(),
			prefs_get_connection_rate_window(),
			prefs_get_connection_rate_bantime(),
			guard_now, &newly_blocked))
		{
			if (newly_blocked)
				eventlog(eventlog_level_warn, __FUNCTION__,
					"[CONNECTION-GUARD] D2CS rate limit triggered for {}: more than {} new connections in {} seconds; rejecting new connections for {} seconds",
					addrstr, prefs_get_connection_rate_limit(), prefs_get_connection_rate_window(),
					prefs_get_connection_rate_bantime() ? prefs_get_connection_rate_bantime() : prefs_get_connection_rate_window());
			psock_close(csock);
			return 0;
		}

		unsigned int active_from_ip = connlist_count_connections(peer_addr);
		if (prefs_get_max_conns_per_IP() != 0 &&
			active_from_ip >= prefs_get_max_conns_per_IP())
		{
			if (should_log_connection_cap(peer_addr, guard_now))
				eventlog(eventlog_level_warn, __FUNCTION__,
					"[CONNECTION-GUARD] D2CS per-IP cap triggered for {}: {} active connections, limit {}; closing socket {} (repeat notices suppressed for 30 seconds)",
					addrstr, active_from_ip, prefs_get_max_conns_per_IP(), csock);
			psock_close(csock);
			return 0;
		}

		eventlog(eventlog_level_debug, __FUNCTION__, "accept connection from {}", addrstr);
	}

	val=1;
	if (psock_setsockopt(csock, PSOCK_SOL_SOCKET, PSOCK_SO_KEEPALIVE, &val,sizeof(val))<0) {
		eventlog(eventlog_level_warn,__FUNCTION__,"error set sock option keep alive");
	}
	if (psock_ctl(csock, PSOCK_NONBLOCK)<0) {
		eventlog(eventlog_level_error,__FUNCTION__,"error set socket to non-blocking mode");
		psock_close(csock);
		return -1;
	}

	raddr_len=sizeof(raddr);
	std::memset(&raddr,0,sizeof(raddr));
	ip=port=0;
	if (psock_getsockname(csock,(struct sockaddr *)&raddr,&raddr_len)<0) {
		eventlog(eventlog_level_warn,__FUNCTION__,"unable to get local socket info");
	} else {
		if (raddr.sin_family!=PSOCK_AF_INET) {
			eventlog(eventlog_level_warn,__FUNCTION__,"got bad socket family {}",raddr.sin_family);
		} else {
			ip=ntohl(raddr.sin_addr.s_addr);
			port=ntohs(raddr.sin_port);
		}
	}
	if (!(cc = d2cs_conn_create(csock,ip,port,ntohl(caddr.sin_addr.s_addr),ntohs(caddr.sin_port)))) {
		eventlog(eventlog_level_error,__FUNCTION__,"error create new connection");
		psock_close(csock);
		return -1;
	}
	if (conn_add_fd(cc, fdwatch_type_read, d2cs_server_handle_tcp)<0) {
		std::time_t guard_now = std::time(NULL);
		if (!socket_pool_notice_time || guard_now - socket_pool_notice_time >= 10)
		{
			eventlog(eventlog_level_error,__FUNCTION__,
				"[CONNECTION-GUARD] D2CS global socket pool exhausted while accepting {} on socket {}: configured max_connections={} (repeat notices suppressed for 10 seconds)",
				addrstr, csock, prefs_get_max_connections());
			socket_pool_notice_time = guard_now;
		}
		d2cs_conn_set_state(cc,conn_state_destroy);
		return -1;
	}

	return 0;
}

static int server_handle_timed_event(void)
{
	static  std::time_t	prev_list_purgetime=0;
	static  std::time_t	prev_gamequeue_checktime=0;
	static	std::time_t	prev_s2s_checktime=0;
	static	std::time_t	prev_sq_checktime=0;
	static  std::time_t	prev_d2ladder_refresh_time=0;
	static  std::time_t	prev_s2s_keepalive_time=0;
	static  std::time_t	prev_timeout_checktime;
	std::time_t		now;

	now=std::time(NULL);
	reloadgs_check_live_request();
	{
		static std::time_t prev_gs_status_time = 0;
		if (now - prev_gs_status_time >= 2)
		{
			d2gs_write_status_file();
			prev_gs_status_time = now;
		}
	}
	if (now-prev_list_purgetime>(signed)prefs_get_list_purgeinterval()) {
		hashtable_purge(d2cs_connlist());
		d2cs_gamelist_check_voidgame();
		prev_list_purgetime=now;
	}
	if (now-prev_gamequeue_checktime>(signed)prefs_get_gamequeue_checkinterval()) {
		gqlist_update_all_clients();
		prev_gamequeue_checktime=now;
	}
	if (now-prev_s2s_checktime>(signed)prefs_get_s2s_retryinterval()) {
		s2s_check();
		prev_s2s_checktime=now;
	}
	if (now-prev_sq_checktime>(signed)prefs_get_sq_checkinterval()) {
		sqlist_check_timeout();
		prev_sq_checktime=now;
	}
	if (now-prev_d2ladder_refresh_time>(signed)prefs_get_d2ladder_refresh_interval()) {
		d2ladder_refresh();
		prev_d2ladder_refresh_time=now;
	}
	if (now-prev_s2s_keepalive_time>(signed)prefs_get_s2s_keepalive_interval()) {
		d2gs_keepalive();
		prev_s2s_keepalive_time=now;
	}
	if (now-prev_timeout_checktime>(signed)prefs_get_timeout_checkinterval()) {
		connlist_check_timeout();
		prev_timeout_checktime=now;
	}
	return 0;
}

extern int d2cs_server_handle_accept(void *data, t_fdwatch_type rw)
{
    int sock;

    sock = addr_get_data((t_addr *)data).i;
    for (int i = 0; i < 32; ++i)
    {
        int rc = server_accept(sock);
        if (rc == 1)
            break;
        if (rc < 0)
            return rc;
    }
    return 0;
}

extern int d2cs_server_handle_tcp(void *data, t_fdwatch_type rw)
{
    t_connection *c = (t_connection *)data;

    if (rw & fdwatch_type_read) conn_add_socket_flag(c,SOCKET_FLAG_READ);
    if (rw & fdwatch_type_write) conn_add_socket_flag(c,SOCKET_FLAG_WRITE);
    if (conn_handle_socket(c)<0)
        d2cs_conn_set_state(c, conn_state_destroy);

    return 0;
}

static int server_handle_socket(void)
{
	switch (fdwatch(BNETD_POLL_INTERVAL)) {
		case -1:
			if (
#ifdef PSOCK_EINTR
			    psock_errno()!=PSOCK_EINTR &&
#endif
			    1) {
				eventlog(eventlog_level_error,__FUNCTION__,"select failed (select: {})",pstrerror(psock_errno()));
				return -1;
			}
			/* fall through */
		case 0:
			return 0;
		default:
			break;
	}

	fdwatch_handle();

	return 0;
}

static int server_loop(void)
{
	unsigned int count;

	count=0;
	while (1) {

#ifdef WIN32
	if (g_ServiceStatus<0 && kbhit() && getch()=='q')
	    signal_quit_wrapper();
	if (g_ServiceStatus == 0) signal_quit_wrapper();

	while (g_ServiceStatus == 2) Sleep(1000);
#endif

		if (handle_signal()<0) break;
		if (++count>=(1000/BNETD_POLL_INTERVAL)) {
			server_handle_timed_event();
			count=0;
		}
		server_handle_socket();
		d2cs_connlist_reap();
	}
	return 0;
}

static int server_cleanup(void)
{
	t_addr		* curr_laddr;
	int		sock;

	BEGIN_LIST_TRAVERSE_DATA(server_listen_addrs,curr_laddr,t_addr)
	{
		sock=addr_get_data(curr_laddr).i;
		psock_close(sock);
	}
	END_LIST_TRAVERSE_DATA()
	addrlist_destroy(server_listen_addrs);
	return 0;
}

extern int d2cs_server_process(void)
{
#ifndef WIN32
	handle_signal_init();
#endif
	if (psock_init()<0) {
		eventlog(eventlog_level_error,__FUNCTION__,"failed to init network");
		return -1;
	}
	eventlog(eventlog_level_info,__FUNCTION__,"network initialized");
	if (s2s_init()<0) {
		eventlog(eventlog_level_error,__FUNCTION__,"failed to init s2s connection");
		return -1;
	}
	if (server_listen()<0) {
		eventlog(eventlog_level_error,__FUNCTION__,"failed to setup listen socket");
		return -1;
	}
	eventlog(eventlog_level_info,__FUNCTION__,
		"[CONNECTION-GUARD] D2CS guards configured: max_connections={}, max_conns_per_IP={}, rate_limit={}/{}s, temporary_block={}s, authentication_timeout={}s, timeout_scan={}s, accept_time_IP_exemptions=none",
		prefs_get_max_connections(), prefs_get_max_conns_per_IP(),
		prefs_get_connection_rate_limit(), prefs_get_connection_rate_window(),
		prefs_get_connection_rate_bantime() ? prefs_get_connection_rate_bantime() : prefs_get_connection_rate_window(),
		prefs_get_auth_timeout(), prefs_get_timeout_checkinterval());
	eventlog(eventlog_level_info,__FUNCTION__,"entering server loop");
	server_loop();
	eventlog(eventlog_level_info,__FUNCTION__,"std::exit from server loop,cleanup");
	server_cleanup();
	return 0;
}

}

}
