cmake_minimum_required(VERSION 3.10)
project(example)

add_definitions(-g -O0)
add_executable(example demo.cc)
target_include_directories(example PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src)
target_include_directories(example PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../tsc_clock/src)
target_link_libraries(example PRIVATE static_log)
target_link_directories(example PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../tsc_clock/output/lib)
target_link_libraries(example PRIVATE libtscns.a)
target_link_libraries(example PRIVATE pthread)
