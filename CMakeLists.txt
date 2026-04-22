cmake_minimum_required(VERSION 3.10)

project(Linux_Server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

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

add_executable(server
    src/app/main_runner.cc
    src/main.cc
    src/TcpConnection.cc
    src/EchoHandler.cc
    src/TcpServer.cc
    src/codec/json_codec.cc
    src/codec/packet_codec.cc
    src/db/db_pool.cc
    src/db/user_repository.cc
    src/handler/message_handler.cc
    src/protocol/protocol_helper.cc
    src/server/session_manager.cc
    src/service/user_service.cc
)

target_include_directories(server PRIVATE include third_party)
target_link_libraries(server PRIVATE ${MYSQLCLIENT_LIB})

add_executable(client
    client/client.cc
)

add_executable(main_test
    tests/main_test.cc
    src/app/main_runner.cc
)

target_include_directories(main_test PRIVATE include third_party)
add_test(NAME main_test COMMAND main_test)

add_executable(json_codec_test
    tests/json_codec_test.cc
    src/codec/json_codec.cc
)

target_include_directories(json_codec_test PRIVATE include third_party)
add_test(NAME json_codec_test COMMAND json_codec_test)

add_executable(message_handler_test
    tests/message_handler_test.cc
    src/codec/json_codec.cc
    src/handler/message_handler.cc
    src/server/session_manager.cc
    src/service/user_service.cc
    src/db/user_repository.cc
)

target_include_directories(message_handler_test PRIVATE include third_party)
target_link_libraries(message_handler_test PRIVATE ${MYSQLCLIENT_LIB})
add_test(NAME message_handler_test COMMAND message_handler_test)

add_executable(user_service_test
    tests/user_service_test.cc
    src/server/session_manager.cc
    src/service/user_service.cc
    src/db/user_repository.cc
)

target_include_directories(user_service_test PRIVATE include third_party)
target_link_libraries(user_service_test PRIVATE ${MYSQLCLIENT_LIB})
add_test(NAME user_service_test COMMAND user_service_test)

add_executable(server_integration_test
    tests/server_integration_test.cc
)

target_include_directories(server_integration_test PRIVATE include third_party)
target_compile_definitions(server_integration_test PRIVATE
    SERVER_BINARY_PATH="$<TARGET_FILE:server>"
)
add_test(NAME server_integration_test COMMAND server_integration_test)

add_executable(session_manager_test
    tests/session_manager_test.cc
    src/server/session_manager.cc
)

target_include_directories(session_manager_test PRIVATE include third_party)
add_test(NAME session_manager_test COMMAND session_manager_test)

add_executable(user_repository_test
    tests/user_repository_test.cc
    src/db/user_repository.cc
)

target_include_directories(user_repository_test PRIVATE include third_party)
target_link_libraries(user_repository_test PRIVATE ${MYSQLCLIENT_LIB})
add_test(NAME user_repository_test COMMAND user_repository_test)
