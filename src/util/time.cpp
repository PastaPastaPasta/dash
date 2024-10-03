// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/time.h>

#include <compat.h>
#include <util/check.h>

#include <atomic>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <thread>

#include <tinyformat.h>

void UninterruptibleSleep(const std::chrono::microseconds& n) { std::this_thread::sleep_for(n); }

static std::atomic<int64_t> nMockTime(0); //!< For testing

template <typename T>
T GetTime()
{
    const std::chrono::seconds mocktime{nMockTime.load(std::memory_order_relaxed)};
    const auto ret{
        mocktime.count() ?
            mocktime :
            std::chrono::duration_cast<T>(std::chrono::system_clock::now().time_since_epoch())};
    assert(ret > 0s);
    return ret;
}
template std::chrono::seconds GetTime();
template std::chrono::milliseconds GetTime();
template std::chrono::microseconds GetTime();

template <typename T>
static T GetSystemTime()
{
    const auto now = std::chrono::duration_cast<T>(std::chrono::system_clock::now().time_since_epoch());
    assert(now.count() > 0);
    return now;
}

void SetMockTime(int64_t nMockTimeIn)
{
    Assert(nMockTimeIn >= 0);
    nMockTime.store(nMockTimeIn, std::memory_order_relaxed);
}

void SetMockTime(std::chrono::seconds mock_time_in)
{
    nMockTime.store(mock_time_in.count(), std::memory_order_relaxed);
}

std::chrono::seconds GetMockTime()
{
    return std::chrono::seconds(nMockTime.load(std::memory_order_relaxed));
}

int64_t GetTimeMillis()
{
    return int64_t{GetSystemTime<std::chrono::milliseconds>().count()};
}

int64_t GetTimeMicros()
{
    return int64_t{GetSystemTime<std::chrono::microseconds>().count()};
}

int64_t GetTimeSeconds()
{
    return int64_t{GetSystemTime<std::chrono::seconds>().count()};
}

int64_t GetTime() { return GetTime<std::chrono::seconds>().count(); }

std::string FormatISO8601DateTime(int64_t nTime)
{
    const std::chrono::sys_seconds secs{std::chrono::seconds{nTime}};
    const auto days{std::chrono::floor<std::chrono::days>(secs)};
    const std::chrono::year_month_day ymd{days};
    const std::chrono::hh_mm_ss hms{secs - days};
    return strprintf("%04i-%02u-%02uT%02i:%02i:%02iZ", signed{ymd.year()}, unsigned{ymd.month()}, unsigned{ymd.day()}, hms.hours().count(), hms.minutes().count(), hms.seconds().count());
}

std::string FormatISO8601Date(int64_t nTime)
{
    const std::chrono::sys_seconds secs{std::chrono::seconds{nTime}};
    const auto days{std::chrono::floor<std::chrono::days>(secs)};
    const std::chrono::year_month_day ymd{days};
    return strprintf("%04i-%02u-%02u", signed{ymd.year()}, unsigned{ymd.month()}, unsigned{ymd.day()});
}

std::string FormatISO8601Time(int64_t nTime) {
    struct tm ts;
    time_t time_val = nTime;
    gmtime_r(&time_val, &ts);
    return strprintf("%02i:%02i:%02iZ", ts.tm_hour, ts.tm_min, ts.tm_sec);
}

int64_t ParseISO8601DateTime(const std::string& str)
{
    static const boost::posix_time::ptime epoch = boost::posix_time::from_time_t(0);
    static const std::locale loc(std::locale::classic(),
        new boost::posix_time::time_input_facet("%Y-%m-%dT%H:%M:%SZ"));
    std::istringstream iss(str);
    iss.imbue(loc);
    boost::posix_time::ptime ptime(boost::date_time::not_a_date_time);
    iss >> ptime;
    if (ptime.is_not_a_date_time() || epoch > ptime)
        return 0;
    return (ptime - epoch).total_seconds();
}

struct timeval MillisToTimeval(int64_t nTimeout)
{
    struct timeval timeout;
    timeout.tv_sec  = nTimeout / 1000;
    timeout.tv_usec = (nTimeout % 1000) * 1000;
    return timeout;
}

struct timeval MillisToTimeval(std::chrono::milliseconds ms)
{
    return MillisToTimeval(count_milliseconds(ms));
}
