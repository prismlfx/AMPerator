cmake_minimum_required(VERSION 3.24) # 3.24: $<LINK_LIBRARY:WHOLE_ARCHIVE> for NAM core
project(AMPerator VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

# CMake 4 dropped compatibility with cmake_minimum_required < 3.5 used by some dependencies.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(CTest)
include(cmake/Dependencies.cmake)
include(cmake/AmpJuce.cmake)

add_subdirectory(engine)
add_subdirectory(app)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
