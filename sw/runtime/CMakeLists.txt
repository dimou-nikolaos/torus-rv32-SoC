# libtorus: the host runtime (C++, no simulator dependency)
add_library(torus STATIC ${CMAKE_CURRENT_SOURCE_DIR}/torus.cpp)
target_include_directories(torus PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
