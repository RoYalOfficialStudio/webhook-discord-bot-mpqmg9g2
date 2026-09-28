# LAME 3.100 (LGPL-2.0) - MP3 encoder. Built as a SEPARATE SHARED LIBRARY (roy_mp3lame)
# that RoY Studio loads at runtime (dlopen/LoadLibrary). This keeps LGPL compliance simple:
# users may replace the library, and RoY runs (without MP3 export) if it is missing.
set(LAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lame)
file(GLOB LAME_SOURCES ${LAME_DIR}/libmp3lame/*.c)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  list(APPEND LAME_SOURCES ${LAME_DIR}/libmp3lame/vector/xmm_quantize_sub.c)
endif()
add_library(roy_mp3lame SHARED ${LAME_SOURCES})
target_include_directories(roy_mp3lame PRIVATE ${LAME_DIR}/roy_config ${LAME_DIR}/include ${LAME_DIR}/libmp3lame)
target_compile_definitions(roy_mp3lame PRIVATE HAVE_CONFIG_H)
set_target_properties(roy_mp3lame PROPERTIES C_VISIBILITY_PRESET default WINDOWS_EXPORT_ALL_SYMBOLS ON
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} PREFIX "")
if(MSVC)
  target_compile_options(roy_mp3lame PRIVATE /W0)
else()
  target_compile_options(roy_mp3lame PRIVATE -w)
  # Upstream LAME 3.100 left-shifts negative ints in VbrTag.c (well defined on GCC/Clang,
  # formally UB in C). The unmodified third-party code is not our finding to "fix":
  # exclude only that UBSan check so sanitizer runs stay clean for RoY's own code.
  if(CMAKE_C_FLAGS MATCHES "sanitize=.*undefined")
    target_compile_options(roy_mp3lame PRIVATE -fno-sanitize=shift)
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_compile_options(roy_mp3lame PRIVATE -msse2)
  endif()
endif()
if(UNIX)
  target_link_libraries(roy_mp3lame PRIVATE m)
endif()
