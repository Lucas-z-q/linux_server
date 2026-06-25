function(add_chat_library target)
    add_library(${target} STATIC ${ARGN})
    target_include_directories(${target}
        PUBLIC
            "${PROJECT_SOURCE_DIR}/include"
    )
endfunction()

add_chat_library(chat_common
    common/logger.cc
    common/validator.cc
    security/password_hasher.cc
)
target_link_libraries(chat_common
    PUBLIC
        Threads::Threads
    PRIVATE
        ${CRYPT_LIB}
)
target_include_directories(chat_common PUBLIC "${PROJECT_SOURCE_DIR}/third_party")

add_chat_library(chat_codec
    codec/json_codec.cc
    codec/packet_codec.cc
)
target_link_libraries(chat_codec PUBLIC chat_common)

add_chat_library(chat_net
    ConnectionContext.cc
    TcpConnection.cc
    TcpServer.cc
    concurrency/thread_pool.cc
)
target_link_libraries(chat_net
    PUBLIC
        chat_codec
        chat_common
        Threads::Threads
)

add_chat_library(chat_db
    db/db_connection.cc
    db/db_pool.cc
    db/friend_repository.cc
    db/group_repository.cc
    db/message_repository.cc
    db/mysql_statement.cc
    db/user_repository.cc
)
target_link_libraries(chat_db
    PUBLIC
        chat_common
        Threads::Threads
    PRIVATE
        ${MYSQLCLIENT_LIB}
)

add_chat_library(chat_redis
    cache/cached_user_repository.cc
    cache/message_dedup_cache.cc
    cache/redis_rate_limiter.cc
    config/redis_config.cc
    redis/redis_client.cc
    redis/redis_connection.cc
    redis/redis_pool.cc
    server/redis_session_store.cc
    stream/redis_push_stream.cc
)
target_link_libraries(chat_redis
    PUBLIC
        chat_common
        chat_db
        Threads::Threads
    PRIVATE
        hiredis::hiredis
)

add_chat_library(chat_service
    server/session_manager.cc
    service/chat_service.cc
    service/friend_service.cc
    service/group_service.cc
    service/user_service.cc
)
target_link_libraries(chat_service
    PUBLIC
        chat_codec
        chat_common
        chat_db
        chat_redis
)

add_chat_library(chat_handler
    handler/message_handler.cc
)
target_link_libraries(chat_handler
    PUBLIC
        chat_codec
        chat_service
)

add_chat_library(chat_server_app
    app/main_runner.cc
    config/config_loader.cc
)
target_include_directories(chat_server_app PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_link_libraries(chat_server_app PUBLIC chat_common)

add_library(chat_logger ALIAS chat_common)
add_library(security_runtime ALIAS chat_common)
add_library(redis_runtime ALIAS chat_redis)

add_executable(server main.cc)
target_link_libraries(server
    PRIVATE
        chat_db
        chat_handler
        chat_net
        chat_redis
        chat_server_app
)
