# Tests Makefile.am

if HAVE_GTEST
check_PROGRAMS = grovedb_cpp_tests
grovedb_cpp_tests_SOURCES = test_basic.cpp
grovedb_cpp_tests_CXXFLAGS = $(AM_CXXFLAGS) $(GTEST_CFLAGS)
grovedb_cpp_tests_LDADD = $(GTEST_LIBS) $(GROVEDB_FFI_LIBS)

TESTS = grovedb_cpp_tests
endif
