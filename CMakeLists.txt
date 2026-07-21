cmake_minimum_required(VERSION 3.16)
project(latency_lab C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

set(LAB_SANITIZE "none" CACHE STRING "Sanitizer preset: none | address | thread")
if(LAB_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
elseif(LAB_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
endif()

# Header-only core library.
add_library(lab_core INTERFACE)
target_include_directories(lab_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

find_package(Threads REQUIRED)

# --- liburing (for the io_uring receive mode) ---------------------------------
# Prefer a system install; otherwise build 2.9 from source. If neither works
# the project still builds, just without Mode::kIoUring.
option(LAB_WITH_IOURING "Enable the io_uring receive mode" ON)
if(LAB_WITH_IOURING)
  find_path(LIBURING_INCLUDE_DIR liburing.h)
  find_library(LIBURING_LIBRARY uring)
  if(LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
    message(STATUS "liburing: system (${LIBURING_LIBRARY})")
    target_include_directories(lab_core INTERFACE ${LIBURING_INCLUDE_DIR})
    target_link_libraries(lab_core INTERFACE ${LIBURING_LIBRARY})
    target_compile_definitions(lab_core INTERFACE LAB_HAVE_LIBURING)
  else()
    message(STATUS "liburing: building 2.9 from source")
    include(ExternalProject)
    ExternalProject_Add(liburing_ext
      GIT_REPOSITORY https://github.com/axboe/liburing.git
      GIT_TAG liburing-2.9
      GIT_SHALLOW ON
      BUILD_IN_SOURCE ON
      CONFIGURE_COMMAND ./configure --cc=${CMAKE_C_COMPILER} --cxx=${CMAKE_CXX_COMPILER}
      BUILD_COMMAND make -C src -j4 liburing.a
      INSTALL_COMMAND ""
      BUILD_BYPRODUCTS <SOURCE_DIR>/src/liburing.a
    )
    ExternalProject_Get_Property(liburing_ext SOURCE_DIR)
    file(MAKE_DIRECTORY ${SOURCE_DIR}/src/include)
    add_library(liburing_built STATIC IMPORTED)
    set_target_properties(liburing_built PROPERTIES
      IMPORTED_LOCATION ${SOURCE_DIR}/src/liburing.a
      INTERFACE_INCLUDE_DIRECTORIES ${SOURCE_DIR}/src/include)
    add_dependencies(liburing_built liburing_ext)
    target_link_libraries(lab_core INTERFACE liburing_built)
    target_compile_definitions(lab_core INTERFACE LAB_HAVE_LIBURING)
  endif()
endif()

# --- Executables -------------------------------------------------------------
add_executable(exchange_sim src/exchange_sim/main.cpp)
target_link_libraries(exchange_sim PRIVATE lab_core)

add_executable(latency_lab src/pipeline/main.cpp)
target_link_libraries(latency_lab PRIVATE lab_core Threads::Threads)

add_executable(book_bench benchmarks/book_bench.cpp)
target_link_libraries(book_bench PRIVATE lab_core)

# --- Tests -------------------------------------------------------------------
option(LAB_BUILD_TESTS "Build unit tests" ON)
if(LAB_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP ON
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(lab_tests
    tests/spsc_ring_test.cpp
    tests/flat_hash_map_test.cpp
    tests/pool_test.cpp
    tests/orderbook_test.cpp
    tests/matcher_test.cpp
    tests/histogram_test.cpp
    tests/receiver_test.cpp
  )
  target_link_libraries(lab_tests PRIVATE lab_core GTest::gtest_main Threads::Threads)

  include(GoogleTest)
  gtest_discover_tests(lab_tests)
endif()
