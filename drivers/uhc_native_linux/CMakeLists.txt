# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0

if(NOT ${CMAKE_HOST_SYSTEM_NAME} STREQUAL Linux)
  message(FATAL_ERROR "CONFIG_UHC_NATIVE_LINUX is only available on Linux")
endif()

zephyr_library()
zephyr_library_sources(uhc_native_linux.c)

# uhc_common.h, which Zephyr keeps next to its own UHC drivers
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/usb/uhc)

target_sources(native_simulator INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/uhc_native_linux_bottom.c
)
