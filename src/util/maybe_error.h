//
// Created by pasta on 1/22/22.
//

#ifndef DASH_MAYBE_ERROR_H
#define DASH_MAYBE_ERROR_H

#include <string_view>

struct maybe_error
{
    const bool did_err{false};
    const int8_t amount{0};
    const std::string_view err_str = "";

    constexpr maybe_error() = default;
    constexpr maybe_error(int8_t amount, std::string_view err_str) : did_err(true), amount(amount), err_str()
};

#endif //DASH_MAYBE_ERROR_H
