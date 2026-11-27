cmake_minimum_required(VERSION 3.21)

project(libdccex
  VERSION 0.1.0
  DESCRIPTION "Platform-independent C++17 client library for the DCC-EX native protocol"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(DCCEX_SOURCES
  src/decoder.cpp)

add_library(dccex STATIC ${DCCEX_SOURCES})
add_library(dccex::dccex ALIAS dccex)
target_include_directories(dccex PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(dccex PUBLIC cxx_std_17)

# ---------------------------------------------------------------------------
# Development: tests and design-rule checks (top-level builds only)
# ---------------------------------------------------------------------------
option(DCCEX_BUILD_TESTS "Build libdccex tests and checks" ${PROJECT_IS_TOP_LEVEL})
option(DCCEX_WERROR "Treat warnings as errors in libdccex targets" ${PROJECT_IS_TOP_LEVEL})

if(DCCEX_BUILD_TESTS)
  enable_testing()

  set(DCCEX_WARNINGS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(DCCEX_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(DCCEX_WERROR)
      list(APPEND DCCEX_WARNINGS -Werror)
    endif()
  endif()
  target_compile_options(dccex PRIVATE ${DCCEX_WARNINGS})

  # --- Unit tests (host, full standard library allowed) ---------------------
  include(FetchContent)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.5.3
    GIT_SHALLOW    TRUE)
  set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(doctest)

  add_executable(dccex_tests
    tests/main.cpp
    tests/decoder_test.cpp
    tests/framer_test.cpp
    tests/string_view_test.cpp
    tests/tokenizer_test.cpp)
  target_link_libraries(dccex_tests PRIVATE dccex::dccex doctest::doctest)
  target_include_directories(dccex_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_options(dccex_tests PRIVATE ${DCCEX_WARNINGS})

  include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
  doctest_discover_tests(dccex_tests)

  # --- Design rule: only freestanding headers -------------------------------
  add_test(NAME check.include_allowlist
    COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckIncludes.cmake)

  # --- Design rules: freestanding, no exceptions, no RTTI, no allocation ----
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Library sources recompiled with freestanding flags, plus a TU that
    # instantiates every header template.
    add_library(dccex_freestanding_check OBJECT
      checks/freestanding_check.cpp ${DCCEX_SOURCES})
    target_include_directories(dccex_freestanding_check PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(dccex_freestanding_check PRIVATE cxx_std_17)
    target_compile_options(dccex_freestanding_check PRIVATE
      ${DCCEX_WARNINGS} -ffreestanding -fno-exceptions -fno-rtti)

    add_test(NAME check.no_allocation
      COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM}
              "-DOBJECTS=$<TARGET_OBJECTS:dccex_freestanding_check>"
              -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoAlloc.cmake)
  endif()
endif()
