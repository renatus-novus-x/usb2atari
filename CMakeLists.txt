cmake_minimum_required(VERSION 3.6)

project(usb2atari)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${PROJECT_SOURCE_DIR}/bin)

file(GLOB SRC "src/*.cpp")
add_executable(usb2atari ${SRC}
)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT usb2atari)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release")
set(CMAKE_SUPPRESS_REGENERATION true)

if(MSVC)
  add_definitions(-D_UNICODE -DUNICODE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++latest")
  set_target_properties(usb2atari PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$(ProjectDir)/../bin")

  set(CompilerFlags
    CMAKE_CXX_FLAGS
    CMAKE_CXX_FLAGS_DEBUG
    CMAKE_CXX_FLAGS_RELEASE
    CMAKE_C_FLAGS
    CMAKE_C_FLAGS_DEBUG
    CMAKE_C_FLAGS_RELEASE
  )
endif(MSVC)

if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
  add_definitions(-std=gnu++17)
#  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
  set(CMAKE_OSX_ARCHITECTURES "x86_64")
endif()

if(UNIX AND NOT APPLE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
endif()

find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)

target_link_libraries(usb2atari ${OPENGL_LIBRARIES} glfw)