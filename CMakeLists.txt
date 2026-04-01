cmake_minimum_required(VERSION 3.10)

project(Linux_Server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(server
    src/main.cc
    src/TcpConnection.cc
    src/EchoHandler.cc
    src/TcpServer.cc
)

target_include_directories(server PRIVATE include)

add_executable(client
    client/client.cc
)
