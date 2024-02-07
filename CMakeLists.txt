cmake_minimum_required(VERSION 3.12)

project(tasky)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")

add_executable(tasky ${SOURCES} ${HEADERS})

if(MSVC)
	add_definitions(
		-DNOMINMAX
		-DUNICODE
		-D_UNICODE
	)

	add_compile_options(
		/W4
		/WX
		/analyze
		/wd4324
		/O2
	)
else()
	target_compile_options(tasky PUBLIC -Wall -Wextra -pedantic -Werror -Wno-unused-variable -O3 -fsanitize=undefined)
	target_link_options(tasky PUBLIC -fsanitize=undefined)
endif()

target_include_directories(tasky PUBLIC include)
target_precompile_headers(tasky PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include/pch.hpp")
