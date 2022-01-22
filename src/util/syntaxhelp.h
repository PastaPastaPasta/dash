// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_SYNTAXHELP_H
#define BITCOIN_UTIL_SYNTAXHELP_H

/* Syntactic sugar / helpers
 * Perhaps can be replaced by C++20 designated initializers.
 */

namespace util {

/* Fakes named arguments in C++.
 *
 * If you have:
 *
 *    bool _f(int x, int y) { ... }
 *
 * Then writing:
 *
 *    static constexpr NamedArgument<int, struct x_arg> _x;
 *    static constexpr NamedArgument<int, struct y_arg> _y;
 *    bool f(decltype(_x)::Arg x, decltype(_y)::Arg y) { return f_orig(x.v, y.v); }
 *
 * allows you to invoke f(_x=3, _y=4) instead of f_orig(3, 4) and
 * will give a compile time error if you reverse the labels
 * (ie, f(_y=3, _x=4)).
 */

template<typename T, typename TAG>
struct NamedArgument
{
    struct Arg {
        using Tag = TAG;
        const T v;
        constexpr explicit Arg(T&& v) : v{std::move(v)} { }
        constexpr explicit Arg(const T& v) : v{v} { }
        constexpr Arg(const Arg&) = delete;
        constexpr Arg(Arg&&) = delete;
        constexpr Arg() = delete;
    };

    constexpr Arg operator=(T&& v) const
    {
        return Arg(std::move(v));
    }
    constexpr Arg operator=(const T& v) const
    {
        return Arg(v);
    }
};

/* Helper for std::array initialization.
 *
 * Allows for specifying the expected index of each initializer to make
 * review easier, and will give a compile-time error if any initializers
 * are missing, extraneous or out of order.
 */

template<size_t Offset, typename T>
struct ArrayInitElement
{
    static constexpr size_t offset = Offset;
    const T v;

    template<typename... Init>
    constexpr explicit ArrayInitElement(Init&&... i) : v{std::forward<Init>(i)...} { }
};

template<typename T, size_t N>
class ArrayInitHelper
{
private:
    using Arr = std::array<T, N>;

    template<size_t E>
    static constexpr void step(Arr& a)
    {
        static_assert(E == N, "too few initializers for array");
    }

    template<size_t E, size_t Offset, typename... Init>
    static constexpr void step(Arr& a, const ArrayInitElement<Offset, T>& aie, const Init&... i)
    {
        static_assert(E < N, "too many initializers for array");
        static_assert(E == Offset, "array initializer element is out of order");
        if constexpr (E < N) {
            std::get<E>(a) = aie.v;
            step<E+1>(a, i...);
        }
    }

public:
    template<typename... Init>
    static constexpr void init(Arr& a, const Init&... i)
    {
        step<0>(a, i...);
    }
};

template<typename T, size_t N, size_t... Offset>
constexpr void ArrayInit(std::array<T,N>& a, const ArrayInitElement<Offset, T>&... i)
{
    ArrayInitHelper<T,N>::init(a, i...);
}

} // namespace util

#endif // BITCOIN_UTIL_SYNTAXHELP_H
