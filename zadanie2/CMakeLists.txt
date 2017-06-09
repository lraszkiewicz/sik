cmake_minimum_required(VERSION 3.5)
project(zadanie2)

set(CMAKE_CXX_STANDARD 14)

set(SOURCE_FILES siktacka.h util.h)

add_executable(siktacka-server ${SOURCE_FILES} server.cpp)
add_executable(siktacka-client ${SOURCE_FILES} client.cpp)