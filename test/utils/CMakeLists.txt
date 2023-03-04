add_executable(test_timer)
target_sources(test_timer PRIVATE test_timer.cpp)
target_link_libraries(test_timer rt gtest)