/*
 * Lightweight in-memory per-IP connection-rate limiter.
 *
 * This is intentionally process-local: bnetd and d2cs each maintain their own
 * limiter state.  It is designed to reject rapid TCP connection churn before
 * the connection is inserted into the normal PvPGN connection lists.
 */
#ifndef INCLUDED_IPRATE_H
#define INCLUDED_IPRATE_H

#include <ctime>
#include <map>

namespace pvpgn
{

class IPRateLimiter
{
public:
    IPRateLimiter();

    /*
     * Returns true when the connection attempt is allowed.
     *
     * limit == 0 disables rate limiting.  window_seconds controls the fixed
     * counting window.  When the limit is exceeded, the address is rejected
     * for ban_seconds (or window_seconds when ban_seconds is zero).
     *
     * newly_blocked is set only for the first rejected attempt that starts a
     * block period, allowing callers to avoid one log line per attack packet.
     */
    bool allow(unsigned int address,
               unsigned int limit,
               unsigned int window_seconds,
               unsigned int ban_seconds,
               std::time_t now,
               bool * newly_blocked = 0);

private:
    struct Entry
    {
        std::time_t window_start;
        std::time_t blocked_until;
        std::time_t last_seen;
        unsigned int count;

        Entry()
            : window_start(0), blocked_until(0), last_seen(0), count(0)
        {}
    };

    void purge(std::time_t now, unsigned int retention_seconds);

    std::map<unsigned int, Entry> entries_;
    std::time_t last_purge_;
};

}

#endif
