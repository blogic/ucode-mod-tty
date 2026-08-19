# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 John Crispin <john@phrozen.org>

cmake_minimum_required(VERSION 3.13)
project(ucode-mod-tty C)

# Set build type if not specified
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# Build in source directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

find_package(PkgConfig)

# Try pkg-config first, fall back to direct library search
if(PKG_CONFIG_FOUND)
	pkg_check_modules(UBOX libubox)
endif()

# Find ucode includes and library
find_path(UCODE_INCLUDE_DIR
	NAMES ucode/module.h
	PATHS /usr/include /usr/local/include
)

find_library(UCODE_LIBRARY
	NAMES ucode
	PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
)

# Find ubox if not found by pkg-config
if(NOT UBOX_FOUND)
	find_path(UBOX_INCLUDE_DIR
		NAMES libubox/uloop.h
		PATHS /usr/include /usr/local/include
	)

	find_library(UBOX_LIBRARY
		NAMES ubox
		PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
	)
endif()

add_library(tty_lib MODULE ucode-mod-tty.c)

# Set include directories
if(UCODE_INCLUDE_DIR)
	target_include_directories(tty_lib PRIVATE ${UCODE_INCLUDE_DIR})
endif()

if(UBOX_FOUND)
	target_include_directories(tty_lib PRIVATE ${UBOX_INCLUDE_DIRS})
	target_link_libraries(tty_lib PRIVATE ${UBOX_LIBRARIES})
elseif(UBOX_INCLUDE_DIR)
	target_include_directories(tty_lib PRIVATE ${UBOX_INCLUDE_DIR})
	target_link_libraries(tty_lib PRIVATE ${UBOX_LIBRARY})
endif()

# Link ucode library
if(UCODE_LIBRARY)
	target_link_libraries(tty_lib PRIVATE ${UCODE_LIBRARY})
endif()

set_target_properties(tty_lib PROPERTIES
	OUTPUT_NAME tty
	PREFIX ""
)

install(TARGETS tty_lib
	LIBRARY DESTINATION lib/ucode
)