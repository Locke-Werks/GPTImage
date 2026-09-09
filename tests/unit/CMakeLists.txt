# tests/unit — one executable per test file. Each is registered with ctest so
# `ctest --output-on-failure` works out of the box.

set(GPTIMAGE_UNIT_TESTS
    test_auth
    test_config
    test_config_write
    test_image_client
    test_jwt_sign
    test_password_hash
    test_realm_grants
)

foreach(T IN LISTS GPTIMAGE_UNIT_TESTS)
    add_executable(${T} ${T}.cpp)
    target_link_libraries(${T} PRIVATE gptimage_common doctest::doctest)
    add_test(NAME ${T} COMMAND ${T})
endforeach()

# test_oauth exercises the AS's pure helpers (PKCE, redirect-host policy), and
# test_job_store the async render cache + hosted-URL result shaping — both live
# in gptimage_mcp_lib rather than gptimage_common.
add_executable(test_oauth test_oauth.cpp)
target_link_libraries(test_oauth PRIVATE gptimage_mcp_lib doctest::doctest)
add_test(NAME test_oauth COMMAND test_oauth)

add_executable(test_job_store test_job_store.cpp)
target_link_libraries(test_job_store PRIVATE gptimage_mcp_lib doctest::doctest)
add_test(NAME test_job_store COMMAND test_job_store)

# test_tools checks the advertised tools/list surface: one generate and one edit
# tool per model, built from a shared template so the two cannot drift, with the
# cost caps reflected in the schema a client actually sees.
add_executable(test_tools test_tools.cpp)
target_link_libraries(test_tools PRIVATE gptimage_mcp_lib doctest::doctest)
add_test(NAME test_tools COMMAND test_tools)
