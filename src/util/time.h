// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_TIME_H
#define BITCOIN_UTIL_TIME_H

#include <array>
#include <stdint.h>
#include <string>
#include <chrono>

void UninterruptibleSleep(const std::chrono::microseconds& n);

/**
 * Helper to count the seconds of a duration.
 *
 * All durations should be using std::chrono and calling this should generally be avoided in code. Though, it is still
 * preferred to an inline t.count() to protect against a reliance on the exact type of t.
 */
inline int64_t count_seconds(std::chrono::seconds t) { return t.count(); }

/**
 * DEPRECATED
 * Use either GetSystemTimeInSeconds (not mockable) or GetTime<T> (mockable)
 */
int64_t GetTime();

/** Returns the system time (not mockable) */
int64_t GetTimeMillis();
/** Returns the system time (not mockable) */
int64_t GetTimeMicros();
/** Returns the system time (not mockable) */
int64_t GetSystemTimeInSeconds(); // Like GetTime(), but not mockable

/** For testing. Set e.g. with the setmocktime rpc, or -mocktime argument */
void SetMockTime(int64_t nMockTimeIn);
/** For testing */
int64_t GetMockTime();

/** Return system time (or mocked time, if set) */
template <typename T>
T GetTime();

/**
 * ISO 8601 formatting is preferred. Use the FormatISO8601{DateTime,Date,Time}
 * helper functions if possible.
 */
std::string FormatISO8601DateTime(int64_t nTime);
std::string FormatISO8601Date(int64_t nTime);
std::string FormatISO8601Time(int64_t nTime);

enum MONTH {
    JAN = 0,
    FEB,
    MAR,
    APR,
    MAY,
    JUN,
    JUL,
    AUG,
    SEP,
    OCT,
    NOV,
    DEC
};

constexpr int calculate_timestamp(int year, MONTH m, int day) {
    constexpr std::array seconds_per_month = {
            std::pair{MONTH::JAN, 2678400},
            std::pair{MONTH::FEB, 2419200},
            std::pair{MONTH::MAR, 2678400},
            std::pair{MONTH::APR, 2592000},
            std::pair{MONTH::MAY, 2678400},
            std::pair{MONTH::JUN, 2592000},
            std::pair{MONTH::JUL, 2678400},
            std::pair{MONTH::AUG, 2678400},
            std::pair{MONTH::SEP, 2592000},
            std::pair{MONTH::OCT, 2678400},
            std::pair{MONTH::NOV, 2592000},
            std::pair{MONTH::DEC, 2678400},
    };

    constexpr int seconds_per_year = 31536000;
    constexpr int seconds_per_day  = 86400;

    int ret = 0;
    for (int i = 1970; i < year; i++) {
        if (i % 4 != 0) {
            ret += seconds_per_year;
        } else {
            ret += seconds_per_year + seconds_per_day;
        }
    }
    for (int i = JAN; i < m; i++) {
        if (i == MONTH::FEB && year % 4 == 0) {
            ret += seconds_per_day; // Add an extra day for feb in a leap year
        }
        ret += seconds_per_month[i].second;
    }
    ret += seconds_per_day * (day - 1);
    return ret;
}

static_assert(calculate_timestamp(1970, MONTH::JAN, 1) == 0);
static_assert(calculate_timestamp(1974, MONTH::JAN, 1) == 31536000 * 4 + 86400);
static_assert(calculate_timestamp(2020, MONTH::OCT, 1) == 1601510400);
static_assert(calculate_timestamp(2021, MONTH::JUL, 1) == 1625097600);
#endif // BITCOIN_UTIL_TIME_H
