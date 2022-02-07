// Copyright (c) 2014-2021 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MAYBE_ERROR_H
#define BITCOIN_MAYBE_ERROR_H

#include <string>

struct maybe_error
{
    const bool did_err{false};

    constexpr maybe_error() = default;
    constexpr explicit maybe_error(bool did_err) : did_err(did_err) {}
};

struct maybe_error_str : maybe_error
{
    std::string err_str = ""; // we don't actually want this const, as we may want to move out of it

    constexpr maybe_error_str() = default;
    explicit maybe_error_str(std::string&& err_str) : maybe_error(true), err_str(err_str) {}
};

struct maybe_error_amount_err_str : maybe_error_str
{
    const int8_t amount{0};

    constexpr maybe_error_amount_err_str() = default;
    maybe_error_amount_err_str(int8_t amount, std::string&& err_str) : maybe_error_str(std::move(err_str)), amount(amount) {}
};

#endif //BITCOIN_MAYBE_ERROR_H
