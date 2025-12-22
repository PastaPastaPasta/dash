ACLOCAL_AMFLAGS = -I m4

# Header-only library - just install headers
nobase_include_HEADERS = \
    include/grovedb/grovedb.hpp \
    include/grovedb/fwd.hpp \
    include/grovedb/result.hpp \
    include/grovedb/byte_buffer.hpp \
    include/grovedb/path.hpp \
    include/grovedb/element.hpp \
    include/grovedb/query.hpp \
    include/grovedb/transaction.hpp \
    include/grovedb/proof.hpp \
    include/grovedb/database.hpp \
    include/grovedb/detail/ffi_helpers.hpp

# pkg-config file
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = grovedb-cpp.pc

EXTRA_DIST = \
    cmake/grovedb-cpp-config.cmake.in \
    CMakeLists.txt \
    examples/basic_usage.cpp
