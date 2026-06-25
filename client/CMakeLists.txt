add_executable(client client.cc)
target_link_libraries(client PRIVATE chat_codec)
