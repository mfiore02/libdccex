# Fails if the freestanding check object references allocation or exception
# runtime symbols. Run with: cmake -DNM=<nm> -DOBJECTS=<a;b> -P CheckNoAlloc.cmake
cmake_minimum_required(VERSION 3.21)

# Itanium-ABI operator new/delete (_Znw*, _Zna*, _Zdl*, _Zda*), C allocation,
# and exception machinery. macOS nm prefixes symbols with an extra underscore.
set(FORBIDDEN "^_?(_Znw|_Zna|_Zdl|_Zda|malloc$|calloc$|realloc$|free$|aligned_alloc$|posix_memalign$|__cxa_allocate_exception|__cxa_throw|__cxa_begin_catch|__gxx_personality)")

if(NOT OBJECTS)
  message(FATAL_ERROR "No objects to check")
endif()

set(VIOLATIONS "")
foreach(obj IN LISTS OBJECTS)
  execute_process(COMMAND "${NM}" -u "${obj}"
    OUTPUT_VARIABLE out RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nm failed on ${obj}: ${err}")
  endif()
  string(REPLACE "\n" ";" lines "${out}")
  foreach(line IN LISTS lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "")
      continue()
    endif()
    # GNU nm prints "U symbol"; macOS nm -u prints just the symbol.
    string(REGEX REPLACE "^.*[ \t]" "" sym "${line}")
    if(sym MATCHES "${FORBIDDEN}")
      string(APPEND VIOLATIONS "  ${sym}\n")
    endif()
  endforeach()
endforeach()

if(VIOLATIONS)
  message(FATAL_ERROR "Forbidden runtime symbols referenced:\n${VIOLATIONS}")
endif()
message(STATUS "No allocation or exception symbols referenced")
