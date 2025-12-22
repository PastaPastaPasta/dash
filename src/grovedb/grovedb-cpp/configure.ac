AC_PREREQ([2.69])
AC_INIT([grovedb-cpp], [1.0.0], [https://github.com/dashpay/grovedb/issues])
AC_CONFIG_SRCDIR([include/grovedb/grovedb.hpp])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([1.14 foreign subdir-objects -Wall -Werror])

# Check for C++ compiler with C++17 support
AC_PROG_CXX
AX_CXX_COMPILE_STDCXX([17], [noext], [mandatory])

# Check for grovedb_ffi
AC_ARG_WITH([grovedb-ffi],
    [AS_HELP_STRING([--with-grovedb-ffi=DIR],
        [specify the location of grovedb_ffi library])],
    [GROVEDB_FFI_DIR="$withval"],
    [GROVEDB_FFI_DIR=""])

if test -n "$GROVEDB_FFI_DIR"; then
    GROVEDB_FFI_CFLAGS="-I$GROVEDB_FFI_DIR/include"
    GROVEDB_FFI_LIBS="-L$GROVEDB_FFI_DIR/lib -lgrovedb_ffi"
else
    # Try pkg-config
    PKG_CHECK_MODULES([GROVEDB_FFI], [grovedb_ffi], [],
        [AC_MSG_WARN([grovedb_ffi not found via pkg-config, assuming default paths])])
fi

AC_SUBST([GROVEDB_FFI_CFLAGS])
AC_SUBST([GROVEDB_FFI_LIBS])

AC_CONFIG_FILES([
    Makefile
    grovedb-cpp.pc
])

AC_OUTPUT
