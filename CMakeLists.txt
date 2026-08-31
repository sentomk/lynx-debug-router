cmake_minimum_required(VERSION 3.18)
project(lynx_debug_router LANGUAGES CXX)

add_library(lynx_debug_router STATIC native/src/session_registry.cc)
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
    add_executable(session_registry_test native/tests/session_registry_test.cc)
    target_link_libraries(session_registry_test PRIVATE lynx_debug_router::core)
    set_target_properties(session_registry_test PROPERTIES CXX_EXTENSIONS OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(session_registry_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    foreach(case IN ITEMS
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
      add_test(NAME session_registry.${case} COMMAND session_registry_test ${case})
      set_tests_properties(session_registry.${case} PROPERTIES TIMEOUT 15)
    endforeach()
  endif()
endif()
