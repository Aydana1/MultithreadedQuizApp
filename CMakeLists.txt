cmake_minimum_required(VERSION 3.10)
project(MultipleQuizServer C)

set(CMAKE_C_STANDARD 99)

add_executable(server echoserver.c passivesock.c connectsock.c)

configure_file(file.txt ${CMAKE_CURRENT_BINARY_DIR}/file.txt)