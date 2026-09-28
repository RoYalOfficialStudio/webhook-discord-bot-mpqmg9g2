# Steinberg VST3 SDK 3.8.1 (MIT) - vendored subset in third_party/vst3sdk.
#   roy_vst3_base     : base + pluginterfaces + common helpers
#   roy_vst3_hosting  : hosting helpers (module loading, process data, events, parameter changes)
#   roy_vst3_plugin   : plugin-side classes (only used to build the RoY VST3 TEST plugins)
set(VST3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk)
set(VST3_PS ${VST3_DIR}/public.sdk/source)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(VST3_DEF DEVELOPMENT=1)
else()
  set(VST3_DEF RELEASE=1)
endif()

add_library(roy_vst3_base STATIC
  ${VST3_DIR}/base/source/baseiids.cpp ${VST3_DIR}/base/source/fbuffer.cpp ${VST3_DIR}/base/source/fdebug.cpp
  ${VST3_DIR}/base/source/fdynlib.cpp ${VST3_DIR}/base/source/fobject.cpp ${VST3_DIR}/base/source/fstreamer.cpp
  ${VST3_DIR}/base/source/fstring.cpp ${VST3_DIR}/base/source/timer.cpp ${VST3_DIR}/base/source/updatehandler.cpp
  ${VST3_DIR}/base/thread/source/fcondition.cpp ${VST3_DIR}/base/thread/source/flock.cpp
  ${VST3_DIR}/pluginterfaces/base/conststringtable.cpp ${VST3_DIR}/pluginterfaces/base/coreiids.cpp
  ${VST3_DIR}/pluginterfaces/base/funknown.cpp ${VST3_DIR}/pluginterfaces/base/ustring.cpp
  ${VST3_PS}/common/commoniids.cpp ${VST3_PS}/common/memorystream.cpp ${VST3_PS}/common/pluginview.cpp
  ${VST3_PS}/common/commonstringconvert.cpp
  ${VST3_PS}/vst/vstinitiids.cpp ${VST3_PS}/vst/utility/stringconvert.cpp)
target_include_directories(roy_vst3_base SYSTEM PUBLIC ${VST3_DIR})
target_compile_definitions(roy_vst3_base PUBLIC ${VST3_DEF})
if(NOT MSVC)
  target_compile_options(roy_vst3_base PRIVATE -w)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(roy_vst3_base PUBLIC pthread dl)
endif()

set(VST3_HOST_SOURCES
  ${VST3_PS}/vst/hosting/connectionproxy.cpp ${VST3_PS}/vst/hosting/eventlist.cpp ${VST3_PS}/vst/hosting/hostclasses.cpp
  ${VST3_PS}/vst/hosting/module.cpp ${VST3_PS}/vst/hosting/parameterchanges.cpp ${VST3_PS}/vst/hosting/pluginterfacesupport.cpp
  ${VST3_PS}/vst/hosting/plugprovider.cpp ${VST3_PS}/vst/hosting/processdata.cpp)
if(WIN32)
  list(APPEND VST3_HOST_SOURCES ${VST3_PS}/vst/hosting/module_win32.cpp ${VST3_PS}/common/threadchecker_win32.cpp)
elseif(APPLE)
  list(APPEND VST3_HOST_SOURCES ${VST3_PS}/vst/hosting/module_mac.mm ${VST3_PS}/common/threadchecker_mac.mm)
else()
  list(APPEND VST3_HOST_SOURCES ${VST3_PS}/vst/hosting/module_linux.cpp ${VST3_PS}/common/threadchecker_linux.cpp)
endif()
add_library(roy_vst3_hosting STATIC ${VST3_HOST_SOURCES})
target_link_libraries(roy_vst3_hosting PUBLIC roy_vst3_base)
if(NOT MSVC)
  target_compile_options(roy_vst3_hosting PRIVATE -w)
endif()
if(WIN32)
  target_link_libraries(roy_vst3_hosting PUBLIC ole32 shell32)
endif()

add_library(roy_vst3_plugin STATIC
  ${VST3_PS}/vst/vstaudioeffect.cpp ${VST3_PS}/vst/vstbus.cpp ${VST3_PS}/vst/vstcomponent.cpp
  ${VST3_PS}/vst/vstcomponentbase.cpp ${VST3_PS}/vst/vsteditcontroller.cpp ${VST3_PS}/vst/vstparameters.cpp
  ${VST3_PS}/vst/vstnoteexpressiontypes.cpp ${VST3_PS}/vst/vstpresetfile.cpp ${VST3_PS}/main/pluginfactory.cpp
  ${VST3_PS}/main/moduleinit.cpp)
target_link_libraries(roy_vst3_plugin PUBLIC roy_vst3_base)
if(NOT MSVC)
  target_compile_options(roy_vst3_plugin PRIVATE -w)
endif()
