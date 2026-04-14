cmake_minimum_required(VERSION 3.10)

project(Linux_Server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    src/service/user_service.cc
)

target_include_directories(server PRIVATE include third_party)

add_executable(client
    client/client.cc
)

add_executable(main_test
    tests/main_test.cc
    src/app/main_runner.cc
)

target_include_directories(main_test PRIVATE include third_party)

add_executable(json_codec_test
    tests/json_codec_test.cc
    src/codec/json_codec.cc
)

target_include_directories(json_codec_test PRIVATE include third_party)
