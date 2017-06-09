cmake_minimum_required(VERSION 3.5)
project(zadanie2)

set(CMAKE_CXX_STANDARD 14)

set(CMAKE_CXX_FLAGS "-lz")
set(SOURCE_FILES siktacka.h util.h)

find_package(ZLIB REQUIRED)
include_directories(${ZLIB_INCLUDE_DIRS})

add_executable(siktacka-server ${SOURCE_FILES} server.cpp)
add_executable(siktacka-client ${SOURCE_FILES} client.cpp)

target_link_libraries(siktacka-client ${ZLIB_LIBRARIES})
target_link_libraries(siktacka-server ${ZLIB_LIBRARIES})