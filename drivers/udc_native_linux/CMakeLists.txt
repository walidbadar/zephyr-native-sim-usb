# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0

if(NOT ${CMAKE_HOST_SYSTEM_NAME} STREQUAL Linux)
  message(FATAL_ERROR "CONFIG_UDC_NATIVE_LINUX is only available on Linux")
endif()

zephyr_library()
zephyr_library_sources(udc_native_linux.c)

# udc_common.h, which Zephyr keeps next to its own UDC drivers
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/usb/udc)

target_sources(native_simulator INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/udc_native_linux_bottom.c
)
