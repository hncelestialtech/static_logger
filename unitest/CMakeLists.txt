cmake_minimum_required(VERSION 3.10)
project(unitest)
add_subdirectory(spdlog)
# add_subdirectory(nanolog)
add_compile_options(-g -O3)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../src)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../tsc_clock/src)
add_executable(test_utils test_utils.cc)
target_link_libraries(test_utils gtest)

add_executable(perf_throughtput perf_throughtput.cc)
target_link_libraries(perf_throughtput tscns static_log pthread)

add_executable(perf_latency perf_latency.cc)
target_include_directories(perf_latency PRIVATE ${CMAKE_CURRENT_LIST_DIR}/nanolog/runtime)
target_include_directories(perf_latency PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../)
target_link_directories(perf_latency PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../tsc_clock/output/lib)
target_link_libraries(perf_latency PRIVATE tscns static_log spdlog pthread)

add_executable(tst_api test_api.cc)
target_link_libraries(tst_api tscns static_log pthread)

add_executable(test_mt test_mt.cc)
target_link_libraries(test_mt tscns static_log pthread)