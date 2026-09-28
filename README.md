# PvPGN

This tree is stock [PvPGN](https://github.com/pvpgn/pvpgn-server) plus the two changes below. Clients, building, configuration, and support are documented in that repository.

The source is GPL-2.0, the same license as [pvpgn/pvpgn-server](https://github.com/pvpgn/pvpgn-server).

## Fake-player protection

bnetd and D2CS reject connection floods before a connection object is created. Log lines are tagged `[CONNECTION-GUARD]`. The limiter is `src/common/iprate.cpp` and `src/common/iprate.h`.

Every accepted address is counted, including a realm or D2GS address. A source that opens too many sockets in a short window is rejected for a short time. A source that already holds too many open connections is closed. Sockets that never log in are closed after the authentication timeout.

`conf/bnetd.conf.in` and `conf/bnetd.conf.win32`:

```
max_conns_per_IP = 32
connection_rate_limit = 30
connection_rate_window = 10
connection_rate_bantime = 60
initkill_timer = 120
```

`conf/d2cs.conf.in` and `conf/d2cs.conf.win32`:

```
max_conns_per_IP = 32
connection_rate_limit = 20
connection_rate_window = 10
connection_rate_bantime = 60
auth_timeout = 120
timeout_checkinterval = 5
```

`0` disables the per-IP cap or the rate limiter. A missing bnetd `max_conns_per_IP` defaults to 32. D2CS scans for expired logins every 5 seconds (`DEFAULT_TIMEOUT_CHECKINTERVAL` in `src/d2cs/setup.h`).

The listen backlog is 128 (`LISTEN_QUEUE` in `src/common/setup_before.h`). bnetd accepts up to 128 waiting sockets per turn. D2CS accepts up to 32. On Windows, large Winsock handles stay in the select set (`src/common/fdwatch_select.cpp`).

## Game-server list

D2CS names each game server. `conf/d2cs.conf.in` and `conf/d2cs.conf.win32`:

```
gameservlist = ServerName=1.2.3.4
```

Each entry is `Name=IP` with no port. A bare IP is accepted and shown under that address. Replace `ServerName` and `1.2.3.4` before running. `src/d2cs/setup.h` uses `ServerName=1.2.3.4` when the key is missing.

D2DBS keeps a plain address list. `conf/d2dbs.conf.in` and `conf/d2dbs.conf.win32`:

```
gameservlist = 1.2.3.4
```

`src/d2dbs/setup.h` uses `1.2.3.4` when the key is missing.

`conf/command_groups.conf.in`:

```
1	/gs
8	/shutdown /rehash /find /save /reloadgs
```

`/gs` lists each server with live Games and Players, or `DOWN`. `/gs Name` saves `BNET\route\preferred_gs` and clears `excluded_gs`. When the player is already in a realm, bnetd tells D2CS with internal packet `0x14`. The next new game uses that server when it can host. Otherwise D2CS uses the least loaded authenticated server. Join and the game list skip an excluded server.

On Diablo II channel entry, a saved preference is announced once: `Prefered GS: Name | Games: N | Players: N`, or `DOWN`.

`/reloadgs` is an admin command. It writes `var/reloadgs.trigger`. D2CS and D2DBS reread only `gameservlist`. An invalid or empty list is left unused and the active list stays. A removed D2CS server that is still connected keeps its current games. New games are not sent there, and it cannot reconnect after it drops. D2CS writes `var/d2gs_status.txt` about every 2 seconds. bnetd, D2CS, and D2DBS find `var` from the config file each process loaded.

Preferred and excluded names are appended to the D2CS account-login reply, packet `0x10`. The packet table includes an empty `0x13` slot so `0x14` is in range.
