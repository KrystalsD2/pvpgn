#include "common/setup_before.h"
#include "common/iprate.h"

#include "common/setup_after.h"

namespace pvpgn
{

IPRateLimiter::IPRateLimiter()
    : entries_(), last_purge_(0)
{}

bool IPRateLimiter::allow(unsigned int address,
                          unsigned int limit,
                          unsigned int window_seconds,
                          unsigned int ban_seconds,
                          std::time_t now,
                          bool * newly_blocked)
{
    if (newly_blocked)
        *newly_blocked = false;

    if (!limit || !window_seconds)
        return true;

    if (!last_purge_ || now - last_purge_ >= 60)
    {
        unsigned int retention = window_seconds + ban_seconds + 60;
        purge(now, retention);
        last_purge_ = now;
    }

    Entry & entry = entries_[address];
    entry.last_seen = now;

    if (entry.blocked_until > now)
        return false;

    if (!entry.window_start || now - entry.window_start >= (std::time_t)window_seconds)
    {
        entry.window_start = now;
        entry.count = 0;
        entry.blocked_until = 0;
    }

    ++entry.count;
    if (entry.count > limit)
    {
        unsigned int block_for = ban_seconds ? ban_seconds : window_seconds;
        entry.blocked_until = now + (std::time_t)block_for;
        if (newly_blocked)
            *newly_blocked = true;
        return false;
    }

    return true;
}

void IPRateLimiter::purge(std::time_t now, unsigned int retention_seconds)
{
    std::map<unsigned int, Entry>::iterator it = entries_.begin();
    while (it != entries_.end())
    {
        if (it->second.blocked_until <= now &&
            now - it->second.last_seen > (std::time_t)retention_seconds)
        {
            std::map<unsigned int, Entry>::iterator dead = it++;
            entries_.erase(dead);
        }
        else
        {
            ++it;
        }
    }
}

}
