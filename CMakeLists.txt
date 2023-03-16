cmake_minimum_required(VERSION 3.20)

set(COMPANY "Au-Zone Technologies")
set(EMAIL "support@au-zone.com")
set(COPYRIGHT "Copyright 2023 Au-Zone Technologies.  All Rights Reserved.")

project(detect CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(Version)
set(PROJECT_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

add_definitions(-DVERSION=\"\")

include(CTest)
include(GenerateExportHeader)
include(GNUInstallDirs)
include(CMakeToolsHelpers OPTIONAL)

find_package(Threads)
find_package(VideoStream REQUIRED)
find_package(VAAL REQUIRED)
find_package(DeepViewRT REQUIRED)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(ext/include)
add_executable(detect detect.cpp)
target_link_libraries(detect Threads::Threads zmq videostream vaal DeepView::RT)
install(TARGETS detect RUNTIME DESTINATION bin)
