cmake_minimum_required(VERSION 3.20)
project(RoYStudio VERSION 0.2.0 LANGUAGES C CXX)

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

include(cmake/vst3sdk.cmake)
include(cmake/lame.cmake)

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
add_dependencies(roy_cli roy_mp3lame)

add_executable(roy_bench apps/roy_bench/main.cpp)
target_link_libraries(roy_bench PRIVATE roy_core)
if(WIN32)
  target_link_libraries(roy_bench PRIVATE psapi)
endif()

# Accelerated long-session soak test (memory/handle growth under hours of simulated work).
add_executable(roy_soak apps/roy_soak/main.cpp)
target_link_libraries(roy_soak PRIVATE roy_core)
if(WIN32)
  target_link_libraries(roy_soak PRIVATE psapi)
endif()

# Code that loads third-party plugins lives ONLY in the sandbox process.
file(GLOB ROY_PLUGINHOST_SOURCES CONFIGURE_DEPENDS src_pluginhost/pluginhost/*.cpp src_pluginhost/pluginhost/*.h)
add_library(roy_pluginhost STATIC ${ROY_PLUGINHOST_SOURCES})
target_include_directories(roy_pluginhost PUBLIC src_pluginhost)
target_link_libraries(roy_pluginhost PUBLIC roy_core roy_vst3_hosting)
if(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)
  target_link_libraries(roy_pluginhost PUBLIC X11::X11)
elseif(WIN32)
  target_link_libraries(roy_pluginhost PUBLIC user32 gdi32)
endif()
add_executable(roy_plugin_host apps/roy_plugin_host/main.cpp)
target_link_libraries(roy_plugin_host PRIVATE roy_pluginhost)
add_dependencies(roy_soak roy_plugin_host)

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

# VST3 test module as a real bundle: RoYTest.vst3/Contents/<arch>-<os>/RoYTest.(so|vst3)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/plugins_test/vst3/roy_vst3_test.cpp)
  if(WIN32)
    set(ROY_VST3_MAIN ${VST3_PS}/main/dllmain.cpp)
    set(ROY_VST3_ARCHDIR x86_64-win)
    set(ROY_VST3_SUFFIX .vst3)
  else()
    set(ROY_VST3_MAIN ${VST3_PS}/main/linuxmain.cpp)
    set(ROY_VST3_ARCHDIR x86_64-linux)
    set(ROY_VST3_SUFFIX .so)
  endif()
  add_library(roy_vst3_test MODULE plugins_test/vst3/roy_vst3_test.cpp ${ROY_VST3_MAIN})
  target_link_libraries(roy_vst3_test PRIVATE roy_vst3_plugin)
  if(NOT MSVC)
    target_compile_options(roy_vst3_test PRIVATE -w)
  endif()
  set_target_properties(roy_vst3_test PROPERTIES PREFIX "" OUTPUT_NAME RoYTest SUFFIX ${ROY_VST3_SUFFIX}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test_plugins/RoYTest.vst3/Contents/${ROY_VST3_ARCHDIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test_plugins/RoYTest.vst3/Contents/${ROY_VST3_ARCHDIR}
    CXX_VISIBILITY_PRESET hidden)
  list(APPEND ROY_TEST_PLUGINS roy_vst3_test)
endif()

# ---------------------------------------------------------------- tests
if(ROY_BUILD_TESTS)
  enable_testing()
  file(GLOB ROY_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
  add_executable(roy_tests ${ROY_TEST_SOURCES})
  target_link_libraries(roy_tests PRIVATE roy_core)
  # MOCK external stem-separation engine (tests the AdvancedStemSeparator adapter only)
  add_executable(roy_test_stem_engine tests/tools/roy_test_stem_engine.cpp)
  target_link_libraries(roy_test_stem_engine PRIVATE roy_core)
  set_target_properties(roy_test_stem_engine PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test_plugins)
  target_compile_definitions(roy_tests PRIVATE
    ROY_TEST_PLUGIN_DIR_BUILD="${CMAKE_BINARY_DIR}/test_plugins"
    ROY_PLUGIN_HOST_EXE_BUILD="$<TARGET_FILE:roy_plugin_host>"
    ROY_CLI_EXE_BUILD="$<TARGET_FILE:roy_cli>"
    ROY_TEST_STEM_ENGINE_BUILD="$<TARGET_FILE:roy_test_stem_engine>")
  add_dependencies(roy_tests roy_plugin_host roy_cli roy_mp3lame roy_test_stem_engine ${ROY_TEST_PLUGINS})
  add_test(NAME roy_tests COMMAND roy_tests)
endif()

# ---------------------------------------------------------------- package for native testing
# `cmake --build <dir> --target package` -> RoYStudio-<version>-<system>.zip with the programs, the
# LAME library, all third-party licence texts and (component "tests") the self-contained test kit
# (roy_tests + test plugins + MOCK engine) for validation on a real Windows PC.
set(ROY_PKG_TARGETS roy_cli roy_plugin_host roy_bench roy_soak)
if(TARGET roy_studio)
  list(APPEND ROY_PKG_TARGETS roy_studio)
endif()
install(TARGETS ${ROY_PKG_TARGETS} RUNTIME DESTINATION . COMPONENT app)
install(TARGETS roy_mp3lame RUNTIME DESTINATION . LIBRARY DESTINATION . COMPONENT app)
install(FILES third_party/THIRD_PARTY_NOTICES.md DESTINATION licenses COMPONENT app)
foreach(lic clap/LICENSE imgui/LICENSE.txt lame/COPYING lame/LICENSE miniaudio/LICENSE nlohmann/LICENSE.MIT vst3sdk/LICENSE.txt)
  get_filename_component(lic_dir ${lic} DIRECTORY)
  install(FILES third_party/${lic} DESTINATION licenses/${lic_dir} COMPONENT app)
endforeach()
install(FILES packaging/README_TESTKIT.txt DESTINATION . COMPONENT app)
if(ROY_BUILD_TESTS)
  install(TARGETS roy_tests RUNTIME DESTINATION . COMPONENT tests)
  install(TARGETS roy_test_stem_engine RUNTIME DESTINATION test_plugins COMPONENT tests)
  install(DIRECTORY ${CMAKE_BINARY_DIR}/test_plugins/ DESTINATION test_plugins COMPONENT tests
          FILES_MATCHING PATTERN "*.clap" PATTERN "*.vst3" PATTERN "*.so" PATTERN "*.dll" PATTERN "*.json")
endif()
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME RoYStudio)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "RoY Studio")
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF) # one zip with app + test kit
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
