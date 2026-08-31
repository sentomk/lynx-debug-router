cmake_minimum_required(VERSION 3.18)
project(lynx_debug_router LANGUAGES CXX)

add_library(lynx_debug_router STATIC
  native/src/session_registry.cc
  native/src/request_tracker.cc
)
add_library(lynx_debug_router::core ALIAS lynx_debug_router)
target_include_directories(lynx_debug_router PUBLIC native/include)
target_compile_features(lynx_debug_router PUBLIC cxx_std_17)
set_target_properties(lynx_debug_router PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(lynx_debug_router PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Consumers using add_subdirectory() do not inherit the test runner.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  include(CTest)
  if(BUILD_TESTING)
    function(add_router_test_suite suite)
      add_executable(${suite}_test native/tests/${suite}_test.cc)
      target_link_libraries(${suite}_test PRIVATE lynx_debug_router::core)
      set_target_properties(${suite}_test PROPERTIES CXX_EXTENSIONS OFF)
      if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${suite}_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
      endif()
      foreach(case IN LISTS ARGN)
        add_test(NAME ${suite}.${case} COMMAND ${suite}_test ${case})
        set_tests_properties(${suite}.${case} PROPERTIES TIMEOUT 15)
      endforeach()
    endfunction()

    add_router_test_suite(session_registry
        registration
        attach_validation
        duplicate_attach
        detach_ownership
        detach_and_reattach
        peer_removal
        target_removal
        unknown_removal
        peer_replacement
        target_replacement
        snapshot_queries
        shared_target_lifecycle
        relationship_consistency)
    add_router_test_suite(request_tracker
        invalid_limits
        registration_validation
        duplicate_request
        out_of_order_responses
        shared_backend_targets
        one_shot_response
        peer_invalidation
        target_invalidation
        detach_and_reattach
        missed_invalidation
        invalid_closure
        capacity_limit
        id_exhaustion
        invalidation_never_reuses_ids
        original_id_preservation
        shared_target_lifecycle)
  endif()
endif()
