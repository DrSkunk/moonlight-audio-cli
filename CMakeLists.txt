cmake_minimum_required(VERSION 3.21)
project(moonlight-audio VERSION 0.1.0 LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Homebrew deliberately does not link Qt into the system CMake search path.
if(APPLE AND NOT DEFINED Qt6_DIR)
  foreach(_qt_prefix /opt/homebrew/opt/qt /opt/homebrew/opt/qtbase /usr/local/opt/qt /usr/local/opt/qtbase)
    if(EXISTS "${_qt_prefix}/lib/cmake/Qt6/Qt6Config.cmake")
      list(PREPEND CMAKE_PREFIX_PATH "${_qt_prefix}")
      break()
    endif()
  endforeach()
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Network)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

include(FetchContent)
FetchContent_Declare(moonlight_common_c
  GIT_REPOSITORY https://github.com/moonlight-stream/moonlight-common-c.git
  # Pinned to the upstream revision inspected for this project.
  GIT_TAG 62e066388f1a1b133e0bee947b9a374311a3354b)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(moonlight_common_c)

add_executable(moonlight-audio
  src/main.cpp
  src/moonlight_config.cpp
  src/moonlight_client.cpp
  src/audio_renderer.mm
  src/null_video_renderer.cpp)

target_include_directories(moonlight-audio PRIVATE src)
target_link_libraries(moonlight-audio PRIVATE
  Qt6::Core Qt6::Network OpenSSL::Crypto PkgConfig::OPUS moonlight-common-c
  "-framework AudioToolbox" "-framework CoreAudio" "-framework AudioUnit")

target_compile_definitions(moonlight-audio PRIVATE QT_NO_KEYWORDS)
