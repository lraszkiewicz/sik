cmake_minimum_required(VERSION 3.7)
project(zadanie1)

set(CMAKE_CXX_STANDARD 11)

add_executable(client err.h datagram.h client.cc)
add_executable(server err.h datagram.h server.cc)