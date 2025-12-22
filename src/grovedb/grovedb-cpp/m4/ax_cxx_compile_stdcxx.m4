# ===========================================================================
#  https://www.gnu.org/software/autoconf-archive/ax_cxx_compile_stdcxx.html
# ===========================================================================
#
# SYNOPSIS
#
#   AX_CXX_COMPILE_STDCXX(VERSION, [ext|noext], [mandatory|optional])
#
# DESCRIPTION
#
#   Check for baseline language coverage in the compiler for the specified
#   version of the C++ standard.
#
# LICENSE
#
#   Copyright (c) 2008 Benjamin Kosnik <bkoz@redhat.com>
#   Copyright (c) 2012 Zack Weinberg <zackw@panix.com>
#   Copyright (c) 2013 Roy Stogner <roystgnr@ices.utexas.edu>
#   Copyright (c) 2014, 2015 Google Inc.; contributed by Alexey Sokolov <sokolov@google.com>
#   Copyright (c) 2015 Paul Norman <penorman@mac.com>
#   Copyright (c) 2015 Moritz Klammler <moritz@klammler.eu>
#   Copyright (c) 2016, 2018 Krzesimir Nowak <qdlacz@gmail.com>
#   Copyright (c) 2019 Enji Cooper <yaneurabeya@gmail.com>
#   Copyright (c) 2020 Jason Merrill <jason@redhat.com>
#   Copyright (c) 2021 Jörn Heusipp <osmanx@problemlos.de>
#
#   Copying and distribution of this file, with or without modification, are
#   permitted in any medium without royalty provided the copyright notice
#   and this notice are preserved.

#serial 18

AC_DEFUN([AX_CXX_COMPILE_STDCXX], [dnl
  m4_if([$1], [17], [ax_cxx_compile_alternatives="17 1z"],
        [m4_fatal([Unsupported C++ version: $1])])dnl
  m4_if([$2], [], [],
        [$2], [ext], [],
        [$2], [noext], [],
        [m4_fatal([invalid second argument `$2' to AX_CXX_COMPILE_STDCXX])])dnl
  m4_if([$3], [], [ax_cxx_compile_cxx$1_required=true],
        [$3], [mandatory], [ax_cxx_compile_cxx$1_required=true],
        [$3], [optional], [ax_cxx_compile_cxx$1_required=false],
        [m4_fatal([invalid third argument `$3' to AX_CXX_COMPILE_STDCXX])])dnl
  AC_LANG_PUSH([C++])dnl
  ac_success=no

  m4_if([$2], [noext], [], [dnl
  for alternative in ${ax_cxx_compile_alternatives}; do
    for switch in -std=gnu++${alternative} -std=c++${alternative}; do
      cachevar=AS_TR_SH([ax_cv_cxx_compile_cxx$1_$switch])
      AC_CACHE_CHECK(whether $CXX supports C++$1 features with $switch,
                     $cachevar,
        [ac_save_CXX="$CXX"
         CXX="$CXX $switch"
         AC_COMPILE_IFELSE([AC_LANG_SOURCE([_AX_CXX_COMPILE_STDCXX_testbody_new_in_17])],
          [eval $cachevar=yes],
          [eval $cachevar=no])
         CXX="$ac_save_CXX"])
      if eval test x\$$cachevar = xyes; then
        CXX="$CXX $switch"
        if test -n "$CXXCPP" ; then
          CXXCPP="$CXXCPP $switch"
        fi
        ac_success=yes
        break
      fi
    done
    if test x$ac_success = xyes; then
      break
    fi
  done])

  AC_LANG_POP([C++])
  if test x$ax_cxx_compile_cxx$1_required = xtrue; then
    if test x$ac_success = xno; then
      AC_MSG_ERROR([*** A compiler with support for C++$1 language features is required.])
    fi
  fi
  if test x$ac_success = xno; then
    HAVE_CXX$1=0
    AC_MSG_NOTICE([No compiler with C++$1 support was found])
  else
    HAVE_CXX$1=1
    AC_DEFINE(HAVE_CXX$1,1,
              [define if the compiler supports basic C++$1 syntax])
  fi
  AC_SUBST(HAVE_CXX$1)
])

m4_define([_AX_CXX_COMPILE_STDCXX_testbody_new_in_17], [[
// Test C++17 features

// Nested namespace definitions
namespace A::B::C {
  int x = 0;
}

// If constexpr
template <bool B>
int constexpr_if_test() {
  if constexpr (B) {
    return 1;
  } else {
    return 0;
  }
}

// Structured bindings
#include <utility>
void structured_bindings_test() {
  std::pair<int, int> p{1, 2};
  auto [a, b] = p;
  (void)a; (void)b;
}

// Fold expressions
template <typename... Args>
auto fold_test(Args... args) {
  return (args + ...);
}

// Inline variables
inline constexpr int inline_var = 42;

// __has_include
#if __has_include(<optional>)
#include <optional>
#endif

int main() {
  constexpr_if_test<true>();
  fold_test(1, 2, 3);
  return 0;
}
]])
