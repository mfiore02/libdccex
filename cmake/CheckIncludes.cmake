# Fails if any library source includes a standard header outside the
# freestanding allow-list. Run with: cmake -DROOT=<repo> -P CheckIncludes.cmake
cmake_minimum_required(VERSION 3.21)

set(ALLOWED cstddef cstdint limits type_traits new)

file(GLOB_RECURSE SOURCES
  "${ROOT}/include/*.hpp" "${ROOT}/include/*.h"
  "${ROOT}/src/*.cpp" "${ROOT}/src/*.hpp" "${ROOT}/src/*.h")

set(VIOLATIONS "")
foreach(path IN LISTS SOURCES)
  file(STRINGS "${path}" lines REGEX "^[ \t]*#[ \t]*include[ \t]*<")
  foreach(line IN LISTS lines)
    string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*<([^>]+)>.*$" "\\1" header "${line}")
    if(NOT header IN_LIST ALLOWED)
      file(RELATIVE_PATH rel "${ROOT}" "${path}")
      string(APPEND VIOLATIONS "  ${rel}: <${header}>\n")
    endif()
  endforeach()
endforeach()

if(VIOLATIONS)
  message(FATAL_ERROR "Non-freestanding includes found (allowed: ${ALLOWED}):\n${VIOLATIONS}")
endif()
list(LENGTH SOURCES n)
message(STATUS "Include allow-list OK (${n} files)")
