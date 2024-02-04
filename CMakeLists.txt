cmake_minimum_required(VERSION 3.12)

project(tasky)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_definitions(
	-DNOMINMAX
	-DUNICODE
	-D_UNICODE
)

add_compile_options(
	/W4
	/WX
	/analyze
)

project(tasky)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")

add_executable(tasky ${SOURCES} ${HEADERS})

target_include_directories(tasky PUBLIC include)
target_precompile_headers(tasky PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include/pch.hpp")
