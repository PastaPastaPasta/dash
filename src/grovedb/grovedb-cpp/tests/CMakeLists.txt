# GroveDB C++ Tests using Catch2

# Find Catch2
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    # Try to find Catch2 v2 as fallback
    find_package(Catch2 2 QUIET)
endif()

if(Catch2_FOUND)
    message(STATUS "Found Catch2: ${Catch2_VERSION}")

    # Integration tests
    add_executable(grovedb_tests
        test_integration.cpp
    )

    target_link_libraries(grovedb_tests PRIVATE
        grovedb-cpp
        Catch2::Catch2WithMain
    )

    # Enable testing
    include(CTest)
    include(Catch)
    catch_discover_tests(grovedb_tests)

else()
    message(WARNING "Catch2 not found. Tests will not be built.")
    message(STATUS "Install Catch2: apt install catch2 (Ubuntu) or brew install catch2 (macOS)")
endif()
