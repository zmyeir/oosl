cmake_minimum_required(VERSION 3.5)

project("fakedeviceinfo")

find_package(cxx REQUIRED CONFIG)

link_libraries(cxx::cxx)

add_library(${CMAKE_PROJECT_NAME} SHARED main.cpp)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE log)
