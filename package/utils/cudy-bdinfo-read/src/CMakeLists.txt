cmake_minimum_required(VERSION 3.10)

PROJECT(cudy-bdinfo-read C)
ADD_DEFINITIONS(-Wall -Werror --std=gnu99 -Wmissing-declarations)

SET(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")

ADD_EXECUTABLE(cudy-bdinfo-read cudy-bdinfo-read.c)

INSTALL(TARGETS cudy-bdinfo-read RUNTIME DESTINATION bin)
