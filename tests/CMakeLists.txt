add_executable(heap_test heap.cpp)
target_link_libraries(heap_test PRIVATE multiqueue Threads::Threads Catch2::Catch2WithMain)

add_executable(buffered_pq_test buffered_pq.cpp)
target_link_libraries(buffered_pq_test PRIVATE multiqueue Threads::Threads Catch2::Catch2WithMain)

add_executable(multiqueue_test multiqueue.cpp)
target_link_libraries(multiqueue_test PRIVATE multiqueue Threads::Threads Catch2::Catch2WithMain)

# Instantiates every mode, which is what keeps the mode templates compiling
add_executable(modes_test modes.cpp)
target_link_libraries(modes_test PRIVATE multiqueue Threads::Threads Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/third_party/Catch2/extras")
include(Catch)

if(BUILD_TESTING)
  catch_discover_tests(heap_test)
  catch_discover_tests(buffered_pq_test)
  catch_discover_tests(multiqueue_test)
  catch_discover_tests(modes_test)
endif()
