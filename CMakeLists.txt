cmake_minimum_required(VERSION 3.10)

project(Linux_Server LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

include(FetchContent)

find_package(Threads REQUIRED)
find_library(CRYPT_LIB NAMES crypt REQUIRED)
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
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

add_library(chat_logger STATIC
    src/common/logger.cc
)

target_include_directories(chat_logger PUBLIC include)
target_link_libraries(chat_logger PUBLIC Threads::Threads)

add_library(security_runtime STATIC
    src/common/validator.cc
    src/security/password_hasher.cc
)
target_include_directories(security_runtime PUBLIC include)
target_link_libraries(security_runtime PUBLIC ${CRYPT_LIB})

add_executable(password_hasher_test tests/password_hasher_test.cc)
target_include_directories(password_hasher_test PRIVATE include)
target_link_libraries(password_hasher_test PRIVATE security_runtime)
add_test(NAME password_hasher_test COMMAND password_hasher_test)

add_executable(validator_test tests/validator_test.cc)
target_include_directories(validator_test PRIVATE include)
target_link_libraries(validator_test PRIVATE security_runtime)
add_test(NAME validator_test COMMAND validator_test)

add_library(redis_runtime
    src/cache/cached_user_repository.cc
    src/cache/message_dedup_cache.cc
    src/cache/redis_rate_limiter.cc
    src/config/redis_config.cc
    src/redis/redis_connection.cc
    src/redis/redis_pool.cc
    src/redis/redis_client.cc
    src/server/redis_session_store.cc
    src/stream/redis_push_stream.cc
)
target_include_directories(redis_runtime PUBLIC include third_party)
target_link_libraries(redis_runtime PUBLIC hiredis::hiredis security_runtime Threads::Threads)

add_executable(server
    src/app/main_runner.cc
    src/main.cc
    src/TcpConnection.cc
    src/ConnectionContext.cc
    src/EchoHandler.cc
    src/TcpServer.cc
    src/codec/json_codec.cc
    src/codec/packet_codec.cc
    src/config/config_loader.cc
    src/db/db_pool.cc
    src/db/db_connection.cc
    src/db/mysql_statement.cc
    src/db/user_repository.cc
    src/db/message_repository.cc
    src/db/friend_repository.cc
    src/db/group_repository.cc
    src/handler/message_handler.cc
    src/protocol/protocol_helper.cc
    src/server/session_manager.cc
    src/service/user_service.cc
    src/service/chat_service.cc
    src/service/friend_service.cc
    src/service/group_service.cc
    src/concurrency/thread_pool.cc
)

target_include_directories(server PRIVATE include third_party)
target_link_libraries(server PRIVATE chat_logger redis_runtime security_runtime ${MYSQLCLIENT_LIB} Threads::Threads)

add_executable(client
    client/client.cc
    src/codec/packet_codec.cc
)

target_include_directories(client PRIVATE include third_party)

add_executable(main_test
    tests/main_test.cc
    src/app/main_runner.cc
)

target_include_directories(main_test PRIVATE include third_party)
target_link_libraries(main_test PRIVATE chat_logger)
add_test(NAME main_test COMMAND main_test)

add_executable(json_codec_test
    tests/json_codec_test.cc
    src/codec/json_codec.cc
)

target_include_directories(json_codec_test PRIVATE include third_party)
add_test(NAME json_codec_test COMMAND json_codec_test)

add_executable(packet_codec_test
    tests/packet_codec_test.cc
    src/codec/packet_codec.cc
)

target_include_directories(packet_codec_test PRIVATE include third_party)
add_test(NAME packet_codec_test COMMAND packet_codec_test)

add_executable(connection_context_test
    tests/connection_context_test.cc
    src/ConnectionContext.cc
    src/TcpConnection.cc
    src/codec/packet_codec.cc
)

target_include_directories(connection_context_test PRIVATE include third_party)
add_test(NAME connection_context_test COMMAND connection_context_test)

add_executable(message_record_test
    tests/message_record_test.cc
)

target_include_directories(message_record_test PRIVATE include third_party)
add_test(NAME message_record_test COMMAND message_record_test)

add_executable(message_handler_test
    tests/message_handler_test.cc
    src/codec/json_codec.cc
    src/handler/message_handler.cc
    src/server/session_manager.cc
    src/service/user_service.cc
    src/service/chat_service.cc
    src/service/friend_service.cc
    src/service/group_service.cc
)

target_include_directories(message_handler_test PRIVATE include third_party)
target_link_libraries(message_handler_test PRIVATE redis_runtime security_runtime)
add_test(NAME message_handler_test COMMAND message_handler_test)

add_executable(token_handler_test
    tests/token_handler_test.cc
    src/codec/json_codec.cc
    src/handler/message_handler.cc
    src/server/session_manager.cc
    src/service/user_service.cc
    src/service/chat_service.cc
    src/service/friend_service.cc
    src/service/group_service.cc
)
target_include_directories(token_handler_test PRIVATE include third_party tests)
target_link_libraries(token_handler_test PRIVATE redis_runtime security_runtime)
add_test(NAME token_handler_test COMMAND token_handler_test)

add_executable(user_service_test
    tests/user_service_test.cc
    src/server/session_manager.cc
    src/service/user_service.cc
)

target_include_directories(user_service_test PRIVATE include third_party)
target_link_libraries(user_service_test PRIVATE redis_runtime security_runtime)
add_test(NAME user_service_test COMMAND user_service_test)

add_executable(user_service_security_test
    tests/user_service_security_test.cc
    src/server/session_manager.cc
    src/service/user_service.cc
)
target_include_directories(user_service_security_test PRIVATE include third_party)
target_link_libraries(user_service_security_test PRIVATE redis_runtime security_runtime)
add_test(NAME user_service_security_test COMMAND user_service_security_test)

add_executable(chat_service_test
    tests/chat_service_test.cc
    src/server/session_manager.cc
    src/service/chat_service.cc
)

target_include_directories(chat_service_test PRIVATE include third_party)
target_link_libraries(chat_service_test PRIVATE security_runtime)
add_test(NAME chat_service_test COMMAND chat_service_test)

add_executable(chat_service_validation_test
    tests/chat_service_validation_test.cc
    src/server/session_manager.cc
    src/service/chat_service.cc
)
target_include_directories(chat_service_validation_test PRIVATE include third_party tests)
target_link_libraries(chat_service_validation_test PRIVATE security_runtime)
add_test(NAME chat_service_validation_test COMMAND chat_service_validation_test)

add_executable(chat_service_redis_test
    tests/chat_service_redis_test.cc
    src/service/chat_service.cc
)

target_include_directories(chat_service_redis_test PRIVATE include third_party tests)
target_link_libraries(chat_service_redis_test PRIVATE redis_runtime security_runtime)
add_test(NAME chat_service_redis_test COMMAND chat_service_redis_test)

add_executable(friend_service_test
    tests/friend_service_test.cc
    src/service/friend_service.cc
)

target_include_directories(friend_service_test PRIVATE include third_party tests)
target_link_libraries(friend_service_test PRIVATE security_runtime)
add_test(NAME friend_service_test COMMAND friend_service_test)

add_executable(group_service_test
    tests/group_service_test.cc
    src/service/group_service.cc
)

target_include_directories(group_service_test PRIVATE include third_party tests)
target_link_libraries(group_service_test PRIVATE security_runtime)
add_test(NAME group_service_test COMMAND group_service_test)

add_executable(server_test_server
    tests/fake_server.cc
    src/app/main_runner.cc
    src/TcpServer.cc
    src/TcpConnection.cc
    src/ConnectionContext.cc
    src/codec/json_codec.cc
    src/codec/packet_codec.cc
    src/handler/message_handler.cc
    src/server/session_manager.cc
    src/service/user_service.cc
    src/service/chat_service.cc
    src/service/friend_service.cc
    src/service/group_service.cc
    src/concurrency/thread_pool.cc
)

target_include_directories(server_test_server PRIVATE include third_party)
target_link_libraries(server_test_server PRIVATE chat_logger security_runtime Threads::Threads)

add_executable(server_integration_test
    tests/server_integration_test.cc
    src/codec/packet_codec.cc
)

target_include_directories(server_integration_test PRIVATE include third_party)
target_compile_definitions(server_integration_test PRIVATE
    SERVER_BINARY_PATH="$<TARGET_FILE:server_test_server>"
)
add_dependencies(server_integration_test server_test_server)
add_test(NAME server_integration_test COMMAND server_integration_test)
set_tests_properties(server_integration_test PROPERTIES TIMEOUT 15)

add_executable(auth_test_server
    tests/fake_auth_server.cc
    src/app/main_runner.cc
    src/TcpServer.cc
    src/TcpConnection.cc
    src/ConnectionContext.cc
    src/codec/json_codec.cc
    src/codec/packet_codec.cc
    src/handler/message_handler.cc
    src/server/session_manager.cc
    src/service/user_service.cc
    src/service/chat_service.cc
    src/service/friend_service.cc
    src/service/group_service.cc
    src/concurrency/thread_pool.cc
)

target_include_directories(auth_test_server PRIVATE include third_party)
target_link_libraries(auth_test_server PRIVATE chat_logger security_runtime Threads::Threads)

add_executable(auth_integration_test
    tests/auth_integration_test.cc
    src/codec/packet_codec.cc
)

target_include_directories(auth_integration_test PRIVATE include third_party)
target_compile_definitions(auth_integration_test PRIVATE
    AUTH_TEST_SERVER_BINARY_PATH="$<TARGET_FILE:auth_test_server>"
)
add_dependencies(auth_integration_test auth_test_server)
add_test(NAME auth_integration_test COMMAND auth_integration_test)
set_tests_properties(auth_integration_test PROPERTIES TIMEOUT 15)

add_executable(session_manager_test
    tests/session_manager_test.cc
    src/server/session_manager.cc
)

target_include_directories(session_manager_test PRIVATE include third_party)
add_test(NAME session_manager_test COMMAND session_manager_test)

add_executable(redis_session_store_test tests/redis_session_store_test.cc)

target_include_directories(redis_session_store_test PRIVATE include third_party tests)
target_link_libraries(redis_session_store_test PRIVATE redis_runtime)
add_test(NAME redis_session_store_test COMMAND redis_session_store_test)

add_executable(redis_session_store_integration_test tests/redis_session_store_integration_test.cc)
target_include_directories(redis_session_store_integration_test PRIVATE include third_party)
target_link_libraries(redis_session_store_integration_test PRIVATE redis_runtime)
add_test(NAME redis_session_store_integration_test COMMAND redis_session_store_integration_test)
set_tests_properties(redis_session_store_integration_test PROPERTIES LABELS redis_integration TIMEOUT 10)
set_tests_properties(redis_session_store_integration_test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(redis_config_test tests/redis_config_test.cc)

target_include_directories(redis_config_test PRIVATE include third_party)
target_link_libraries(redis_config_test PRIVATE redis_runtime)
add_test(NAME redis_config_test COMMAND redis_config_test)

add_executable(redis_connection_test tests/redis_connection_test.cc)

target_include_directories(redis_connection_test PRIVATE include third_party tests)
target_link_libraries(redis_connection_test PRIVATE redis_runtime)
add_test(NAME redis_connection_test COMMAND redis_connection_test)

add_executable(redis_pool_test tests/redis_pool_test.cc)

target_include_directories(redis_pool_test PRIVATE include third_party tests)
target_link_libraries(redis_pool_test PRIVATE redis_runtime)
add_test(NAME redis_pool_test COMMAND redis_pool_test)

add_executable(cached_user_repository_test tests/cached_user_repository_test.cc)

target_include_directories(cached_user_repository_test PRIVATE include third_party tests)
target_link_libraries(cached_user_repository_test PRIVATE redis_runtime)
add_test(NAME cached_user_repository_test COMMAND cached_user_repository_test)

add_executable(redis_rate_limiter_test tests/redis_rate_limiter_test.cc)

target_include_directories(redis_rate_limiter_test PRIVATE include third_party tests)
target_link_libraries(redis_rate_limiter_test PRIVATE redis_runtime)
add_test(NAME redis_rate_limiter_test COMMAND redis_rate_limiter_test)

add_executable(message_dedup_cache_test tests/message_dedup_cache_test.cc)

target_include_directories(message_dedup_cache_test PRIVATE include third_party tests)
target_link_libraries(message_dedup_cache_test PRIVATE redis_runtime)
add_test(NAME message_dedup_cache_test COMMAND message_dedup_cache_test)

add_executable(redis_push_stream_test tests/redis_push_stream_test.cc)

target_include_directories(redis_push_stream_test PRIVATE include third_party tests)
target_link_libraries(redis_push_stream_test PRIVATE redis_runtime)
add_test(NAME redis_push_stream_test COMMAND redis_push_stream_test)

add_executable(cross_server_push_router_test
    tests/cross_server_push_router_test.cc
    src/service/chat_service.cc
)

target_include_directories(cross_server_push_router_test PRIVATE include third_party tests)
target_link_libraries(cross_server_push_router_test PRIVATE redis_runtime security_runtime)
add_test(NAME cross_server_push_router_test COMMAND cross_server_push_router_test)

add_executable(redis_connection_integration_test tests/redis_connection_integration_test.cc)

target_include_directories(redis_connection_integration_test PRIVATE include third_party)
target_link_libraries(redis_connection_integration_test PRIVATE redis_runtime)
add_test(NAME redis_connection_integration_test COMMAND redis_connection_integration_test)
set_tests_properties(redis_connection_integration_test PROPERTIES LABELS redis_integration)

add_executable(redis_push_stream_integration_test tests/redis_push_stream_integration_test.cc)

target_include_directories(redis_push_stream_integration_test PRIVATE include third_party)
target_link_libraries(redis_push_stream_integration_test PRIVATE redis_runtime)
add_test(NAME redis_push_stream_integration_test COMMAND redis_push_stream_integration_test)
set_tests_properties(redis_push_stream_integration_test PROPERTIES LABELS redis_integration)

add_executable(user_repository_test
    tests/user_repository_test.cc
    src/db/user_repository.cc
    src/db/db_pool.cc
    src/db/db_connection.cc
    src/db/mysql_statement.cc
)

target_include_directories(user_repository_test PRIVATE include third_party)
target_link_libraries(user_repository_test PRIVATE chat_logger ${MYSQLCLIENT_LIB})
add_test(NAME user_repository_test COMMAND user_repository_test)

add_executable(user_repository_integration_test
    tests/user_repository_integration_test.cc
    src/db/user_repository.cc
    src/db/db_pool.cc
    src/db/db_connection.cc
    src/db/mysql_statement.cc
)
target_include_directories(user_repository_integration_test PRIVATE include third_party)
target_link_libraries(user_repository_integration_test PRIVATE chat_logger security_runtime ${MYSQLCLIENT_LIB})
add_test(NAME user_repository_integration_test COMMAND user_repository_integration_test)
set_tests_properties(user_repository_integration_test PROPERTIES LABELS mysql_integration SKIP_RETURN_CODE 77)

add_executable(message_repository_test
    tests/message_repository_test.cc
    src/db/message_repository.cc
    src/db/db_pool.cc
    src/db/db_connection.cc
    src/db/mysql_statement.cc
)

target_include_directories(message_repository_test PRIVATE include third_party)
target_link_libraries(message_repository_test PRIVATE chat_logger ${MYSQLCLIENT_LIB})
add_test(NAME message_repository_test COMMAND message_repository_test)

add_executable(friend_repository_test
    tests/friend_repository_test.cc
    src/db/friend_repository.cc
    src/db/db_pool.cc
    src/db/db_connection.cc
    src/db/mysql_statement.cc
)

target_include_directories(friend_repository_test PRIVATE include third_party)
target_link_libraries(friend_repository_test PRIVATE chat_logger ${MYSQLCLIENT_LIB})
add_test(NAME friend_repository_test COMMAND friend_repository_test)

add_executable(group_repository_test
    tests/group_repository_test.cc
    src/db/group_repository.cc
    src/db/db_pool.cc
    src/db/db_connection.cc
    src/db/mysql_statement.cc
)

target_include_directories(group_repository_test PRIVATE include third_party)
target_link_libraries(group_repository_test PRIVATE chat_logger ${MYSQLCLIENT_LIB})
add_test(NAME group_repository_test COMMAND group_repository_test)

add_executable(message_repository_security_integration_test
    tests/message_repository_security_integration_test.cc
    src/db/message_repository.cc
    src/db/user_repository.cc
    src/db/db_pool.cc
    src/db/db_connection.cc
    src/db/mysql_statement.cc
)
target_include_directories(message_repository_security_integration_test PRIVATE include third_party)
target_link_libraries(message_repository_security_integration_test PRIVATE chat_logger security_runtime ${MYSQLCLIENT_LIB})
add_test(NAME message_repository_security_integration_test COMMAND message_repository_security_integration_test)
set_tests_properties(message_repository_security_integration_test PROPERTIES
    LABELS mysql_integration
    SKIP_RETURN_CODE 77
)

add_executable(thread_pool_test
    tests/thread_pool_test.cc
    src/concurrency/thread_pool.cc
)

target_include_directories(thread_pool_test PRIVATE include third_party)
target_link_libraries(thread_pool_test PRIVATE
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)

add_test(NAME thread_pool_test COMMAND thread_pool_test)

add_executable(server_shutdown_test
    tests/server_shutdown_test.cc
    src/TcpServer.cc
    src/TcpConnection.cc
    src/ConnectionContext.cc
    src/concurrency/thread_pool.cc
    src/codec/packet_codec.cc
)

target_include_directories(server_shutdown_test PRIVATE include third_party)
target_link_libraries(server_shutdown_test PRIVATE
    chat_logger
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)

add_test(NAME server_shutdown_test COMMAND server_shutdown_test)

add_executable(server_timeout_test
    tests/server_timeout_test.cc
    src/TcpServer.cc
    src/TcpConnection.cc
    src/ConnectionContext.cc
    src/concurrency/thread_pool.cc
    src/codec/packet_codec.cc
)

target_include_directories(server_timeout_test PRIVATE include third_party tests)
target_link_libraries(server_timeout_test PRIVATE
    chat_logger
    redis_runtime
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)

add_test(NAME server_timeout_test COMMAND server_timeout_test)
set_tests_properties(server_timeout_test PROPERTIES TIMEOUT 15)

add_executable(remote_push_io_test
    tests/remote_push_io_test.cc
    src/TcpServer.cc
    src/TcpConnection.cc
    src/ConnectionContext.cc
    src/concurrency/thread_pool.cc
    src/codec/packet_codec.cc
)

target_include_directories(remote_push_io_test PRIVATE include third_party)
target_link_libraries(remote_push_io_test PRIVATE chat_logger Threads::Threads)
add_test(NAME remote_push_io_test COMMAND remote_push_io_test)
set_tests_properties(remote_push_io_test PROPERTIES TIMEOUT 10)

add_executable(db_connection_test
    tests/db_connection_test.cc
    src/db/db_connection.cc
    src/db/mysql_statement.cc
)

target_include_directories(db_connection_test PRIVATE include third_party)
target_link_libraries(db_connection_test PRIVATE
    ${MYSQLCLIENT_LIB}
    GTest::gtest
    GTest::gtest_main
)
add_test(NAME db_connection_test COMMAND db_connection_test)

add_executable(db_pool_test
    tests/db_pool_test.cc
    src/db/db_pool.cc
    src/db/db_connection.cc
    src/db/mysql_statement.cc
)

target_include_directories(db_pool_test PRIVATE include third_party)
target_link_libraries(db_pool_test PRIVATE
    chat_logger
    ${MYSQLCLIENT_LIB}
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)
add_test(NAME db_pool_test COMMAND db_pool_test)

add_executable(config_loader_test
    tests/config_loader_test.cc
    src/config/config_loader.cc
)

target_include_directories(config_loader_test PRIVATE include third_party)
target_link_libraries(config_loader_test PRIVATE
    GTest::gtest
    GTest::gtest_main
)
add_test(NAME config_loader_test COMMAND config_loader_test)

add_executable(logger_test
    tests/logger_test.cc
)

target_include_directories(logger_test PRIVATE include third_party)
target_link_libraries(logger_test PRIVATE
    chat_logger
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
)
add_test(NAME logger_test COMMAND logger_test)
