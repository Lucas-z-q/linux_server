include(FetchContent)

find_package(Threads REQUIRED)
find_library(CRYPT_LIB NAMES crypt REQUIRED)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

find_path(HIREDIS_INCLUDE_DIR NAMES hiredis.h PATH_SUFFIXES hiredis)
find_library(HIREDIS_LIB NAMES hiredis)
if(HIREDIS_INCLUDE_DIR AND HIREDIS_LIB)
    add_library(hiredis_external UNKNOWN IMPORTED)
    set_target_properties(hiredis_external PROPERTIES
        IMPORTED_LOCATION "${HIREDIS_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${HIREDIS_INCLUDE_DIR}"
    )
    add_library(hiredis::hiredis ALIAS hiredis_external)
else()
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(DISABLE_TESTS ON CACHE BOOL "" FORCE)
    set(ENABLE_SSL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        hiredis
        URL https://github.com/redis/hiredis/archive/refs/tags/v1.3.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(hiredis)
endif()

find_library(MYSQLCLIENT_LIB
    NAMES mysqlclient
    PATHS /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu
)
if(NOT MYSQLCLIENT_LIB AND EXISTS "/usr/lib/x86_64-linux-gnu/libmysqlclient.so.21")
    set(MYSQLCLIENT_LIB "/usr/lib/x86_64-linux-gnu/libmysqlclient.so.21")
endif()
if(NOT MYSQLCLIENT_LIB)
    message(FATAL_ERROR "mysqlclient library not found")
endif()
