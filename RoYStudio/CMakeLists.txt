cmake_minimum_required(VERSION 3.20)
project(RoYStudio VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ROY_BUILD_TESTS "Build RoY Studio tests" ON)
if(WIN32)
  set(ROY_GUI_DEFAULT ON)
else()
  set(ROY_GUI_DEFAULT OFF) # needs GLFW (libglfw3-dev) on Linux/macOS
endif()
option(ROY_BUILD_GUI "Build the RoY Studio GUI application" ${ROY_GUI_DEFAULT})

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()
if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
endif()

# ---------------------------------------------------------------- third party
add_library(roy_miniaudio STATIC third_party/miniaudio/miniaudio_impl.c)
target_include_directories(roy_miniaudio SYSTEM PUBLIC third_party/miniaudio)
if(NOT MSVC)
  target_compile_options(roy_miniaudio PRIVATE -w)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(roy_miniaudio PUBLIC dl pthread m)
endif()

add_library(roy_thirdparty_headers INTERFACE)
target_include_directories(roy_thirdparty_headers SYSTEM INTERFACE third_party third_party/clap/include)

# ---------------------------------------------------------------- core library
file(GLOB_RECURSE ROY_CORE_SOURCES CONFIGURE_DEPENDS
  src/*.cpp src/*.h)
add_library(roy_core STATIC ${ROY_CORE_SOURCES})
target_include_directories(roy_core PUBLIC src)
target_link_libraries(roy_core PUBLIC roy_miniaudio roy_thirdparty_headers)
target_compile_definitions(roy_core PUBLIC ROY_VERSION_STRING="${PROJECT_VERSION}")
if(UNIX AND NOT APPLE)
  target_link_libraries(roy_core PUBLIC dl pthread)
endif()
if(WIN32)
  target_link_libraries(roy_core PUBLIC ole32 winmm)
endif()

# ---------------------------------------------------------------- applications
add_executable(roy_cli apps/roy_cli/main.cpp)
target_link_libraries(roy_cli PRIVATE roy_core)

add_executable(roy_bench apps/roy_bench/main.cpp)
target_link_libraries(roy_bench PRIVATE roy_core)
if(WIN32)
  target_link_libraries(roy_bench PRIVATE psapi)
endif()

add_executable(roy_plugin_host apps/roy_plugin_host/main.cpp)
target_link_libraries(roy_plugin_host PRIVATE roy_core)

if(ROY_BUILD_GUI)
  add_subdirectory(apps/roy_studio)
endif()

# ---------------------------------------------------------------- test plugins
# Real CLAP plugins built from source, used to test the scanner, host and sandbox.
function(roy_test_plugin name src)
  add_library(${name} MODULE ${src})
  target_link_libraries(${name} PRIVATE roy_thirdparty_headers)
  set_target_properties(${name} PROPERTIES PREFIX "" SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test_plugins
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test_plugins)
endfunction()
set(ROY_TEST_PLUGINS)
foreach(tp roy_test_gain roy_test_crash roy_test_hang)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/plugins_test/${tp}.cpp)
    roy_test_plugin(${tp} plugins_test/${tp}.cpp)
    list(APPEND ROY_TEST_PLUGINS ${tp})
  endif()
endforeach()

# ---------------------------------------------------------------- tests
if(ROY_BUILD_TESTS)
  enable_testing()
  file(GLOB ROY_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
  add_executable(roy_tests ${ROY_TEST_SOURCES})
  target_link_libraries(roy_tests PRIVATE roy_core)
  target_compile_definitions(roy_tests PRIVATE
    ROY_TEST_PLUGIN_DIR="${CMAKE_BINARY_DIR}/test_plugins"
    ROY_PLUGIN_HOST_EXE="$<TARGET_FILE:roy_plugin_host>")
  add_dependencies(roy_tests roy_plugin_host ${ROY_TEST_PLUGINS})
  add_test(NAME roy_tests COMMAND roy_tests)
endif()
