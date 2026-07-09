function(add_chat_test target)
    set(options GTEST NO_CTEST)
    set(one_value_args TIMEOUT SKIP_RETURN_CODE)
    set(multi_value_args SOURCES LIBRARIES DEFINITIONS DEPENDS LABELS)
    cmake_parse_arguments(
        CHAT_TEST
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        ${ARGN}
    )

    if(NOT CHAT_TEST_SOURCES)
        set(CHAT_TEST_SOURCES "${target}.cc")
    endif()

    add_executable(${target} ${CHAT_TEST_SOURCES})
    target_include_directories(${target}
        PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/third_party"
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(${target} PRIVATE ${CHAT_TEST_LIBRARIES})

    if(CHAT_TEST_GTEST)
        target_link_libraries(${target}
            PRIVATE
                GTest::gtest
                GTest::gtest_main
        )
    endif()

    if(CHAT_TEST_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE ${CHAT_TEST_DEFINITIONS})
    endif()

    if(CHAT_TEST_DEPENDS)
        add_dependencies(${target} ${CHAT_TEST_DEPENDS})
    endif()

    if(CHAT_TEST_NO_CTEST)
        return()
    endif()

    add_test(NAME ${target} COMMAND ${target})

    if(CHAT_TEST_TIMEOUT)
        set_tests_properties(${target} PROPERTIES TIMEOUT "${CHAT_TEST_TIMEOUT}")
    endif()
    if(CHAT_TEST_SKIP_RETURN_CODE)
        set_tests_properties(${target} PROPERTIES SKIP_RETURN_CODE "${CHAT_TEST_SKIP_RETURN_CODE}")
    endif()
    if(CHAT_TEST_LABELS)
        set_tests_properties(${target} PROPERTIES LABELS "${CHAT_TEST_LABELS}")
    endif()
endfunction()

add_chat_test(password_hasher_test LIBRARIES chat_common)
add_chat_test(validator_test LIBRARIES chat_common)
add_chat_test(main_test LIBRARIES chat_server_app)
add_chat_test(json_codec_test LIBRARIES chat_codec)
add_chat_test(packet_codec_test LIBRARIES chat_codec)
add_chat_test(connection_context_test LIBRARIES chat_net)
add_chat_test(message_record_test LIBRARIES chat_common)

add_chat_test(message_handler_test LIBRARIES chat_handler)
add_chat_test(token_handler_test LIBRARIES chat_handler)
add_chat_test(user_service_test LIBRARIES chat_service)
add_chat_test(user_service_security_test LIBRARIES chat_service)
add_chat_test(chat_service_test LIBRARIES chat_service)
add_chat_test(chat_service_validation_test LIBRARIES chat_service)
add_chat_test(chat_service_redis_test LIBRARIES chat_service)
add_chat_test(friend_service_test LIBRARIES chat_service)
add_chat_test(group_service_test LIBRARIES chat_service)

add_chat_test(server_test_server
    NO_CTEST
    SOURCES fake_server.cc
    LIBRARIES chat_handler chat_net chat_server_app
)
add_chat_test(server_integration_test
    LIBRARIES chat_codec
    DEFINITIONS SERVER_BINARY_PATH="$<TARGET_FILE:server_test_server>"
    DEPENDS server_test_server
    TIMEOUT 15
)

add_chat_test(auth_test_server
    NO_CTEST
    SOURCES fake_auth_server.cc
    LIBRARIES chat_handler chat_net chat_server_app
)
add_chat_test(auth_integration_test
    LIBRARIES chat_codec
    DEFINITIONS AUTH_TEST_SERVER_BINARY_PATH="$<TARGET_FILE:auth_test_server>"
    DEPENDS auth_test_server
    TIMEOUT 15
)

add_chat_test(business_stress_test
    NO_CTEST
    LIBRARIES chat_db chat_handler chat_net chat_redis chat_server_app
)

add_chat_test(session_manager_test LIBRARIES chat_service)

add_chat_test(redis_session_store_test LIBRARIES chat_redis)
add_chat_test(redis_session_store_integration_test
    LIBRARIES chat_redis
    LABELS redis_integration
    SKIP_RETURN_CODE 77
    TIMEOUT 10
)
add_chat_test(redis_config_test LIBRARIES chat_redis)
add_chat_test(redis_connection_test LIBRARIES chat_redis)
add_chat_test(redis_pool_test LIBRARIES chat_redis)
add_chat_test(cached_user_repository_test LIBRARIES chat_redis)
add_chat_test(redis_rate_limiter_test LIBRARIES chat_redis)
add_chat_test(message_dedup_cache_test LIBRARIES chat_redis)
add_chat_test(chat_service_cache_test LIBRARIES chat_service chat_redis)
add_chat_test(redis_push_stream_test LIBRARIES chat_redis)
add_chat_test(cross_server_push_router_test LIBRARIES chat_service)
add_chat_test(redis_connection_integration_test
    LIBRARIES chat_redis
    LABELS redis_integration
)
add_chat_test(redis_push_stream_integration_test
    LIBRARIES chat_redis
    LABELS redis_integration
)

add_chat_test(user_repository_test LIBRARIES chat_db)
add_chat_test(user_repository_integration_test
    LIBRARIES chat_db
    LABELS mysql_integration
    SKIP_RETURN_CODE 77
)
add_chat_test(message_repository_test LIBRARIES chat_db)
add_chat_test(friend_repository_test LIBRARIES chat_db)
add_chat_test(group_repository_test LIBRARIES chat_db)
add_chat_test(message_repository_security_integration_test
    LIBRARIES chat_db
    LABELS mysql_integration
    SKIP_RETURN_CODE 77
)

add_chat_test(thread_pool_test GTEST LIBRARIES chat_net)
add_chat_test(server_shutdown_test GTEST LIBRARIES chat_net)
add_chat_test(server_timeout_test
    GTEST
    LIBRARIES chat_net chat_redis
    TIMEOUT 15
)
add_chat_test(remote_push_io_test
    LIBRARIES chat_net
    TIMEOUT 10
)

add_chat_test(db_connection_test GTEST LIBRARIES chat_db)
add_chat_test(db_pool_test GTEST LIBRARIES chat_db)
add_chat_test(config_loader_test GTEST LIBRARIES chat_server_app)
add_chat_test(logger_test GTEST LIBRARIES chat_common)
