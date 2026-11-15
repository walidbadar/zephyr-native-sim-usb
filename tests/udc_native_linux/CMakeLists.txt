# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The driver under test is in the Zephyr module two levels up
get_filename_component(module_dir ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
list(APPEND ZEPHYR_EXTRA_MODULES ${module_dir})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(udc_native_linux)

target_sources(app PRIVATE src/main.c)

target_compile_options(native_simulator INTERFACE -DUDC_NATIVE_LINUX_TEST_HOOKS)
target_sources(native_simulator INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/fake_raw_gadget_bottom.c
)
