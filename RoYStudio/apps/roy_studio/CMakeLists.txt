# RoY Studio GUI (Dear ImGui). Windows: Win32 + Direct3D 11. Other platforms: GLFW + OpenGL 3.
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(roy_imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp)
target_include_directories(roy_imgui SYSTEM PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
if(NOT MSVC)
  target_compile_options(roy_imgui PRIVATE -w)
endif()

set(ROY_GUI_SOURCES App.cpp Theme.cpp UiMain.cpp UiPlaylist.cpp UiMusic.cpp UiMixer.cpp UiTools.cpp UiSession.cpp MusicSession.cpp PlatformDialogs.cpp)
if(WIN32)
  add_executable(roy_studio WIN32 ${ROY_GUI_SOURCES} main_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
  target_link_libraries(roy_studio PRIVATE roy_core roy_imgui d3d11 dxgi d3dcompiler dwmapi ole32 uuid shell32)
  if(MINGW)
    target_link_options(roy_studio PRIVATE -municode)
  endif()
else()
  find_package(glfw3 3.3 REQUIRED)
  find_package(OpenGL REQUIRED)
  add_executable(roy_studio ${ROY_GUI_SOURCES} main_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
  target_link_libraries(roy_studio PRIVATE roy_core roy_imgui glfw OpenGL::GL)
endif()
target_include_directories(roy_studio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# next to roy_plugin_host and roy_mp3lame (found via the executable directory)
set_target_properties(roy_studio PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_dependencies(roy_studio roy_plugin_host roy_mp3lame)
if(WIN32)
  set_target_properties(roy_studio PROPERTIES OUTPUT_NAME RoYStudio) # RoYStudio.exe
endif()
