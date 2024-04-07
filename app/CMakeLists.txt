#-------------------------------------------------------------------------------
# Zephyr Example Application
#
# Copyright (c) 2022 François Gervais
# SPDX-License-Identifier: Apache-2.0

# set(OVERLAY_CONFIG "secret.conf")

cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

execute_process(COMMAND git describe
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE GIT_DESCRIBE
                ERROR_QUIET)
string(STRIP "${GIT_DESCRIBE}" GIT_DESCRIBE)
STRING(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+"
       VERSION "${GIT_DESCRIBE}")

project(app LANGUAGES C VERSION ${VERSION})


configure_file(app_version.h.in ${CMAKE_BINARY_DIR}/app/include/app_version.h)
target_include_directories(app PRIVATE ${CMAKE_BINARY_DIR}/app/include src)

target_sources(app PRIVATE
        src/reset.c
        src/watchdog.c
        src/main.c)
