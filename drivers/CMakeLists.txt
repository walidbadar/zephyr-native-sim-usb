# SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_UDC_NATIVE_LINUX udc_native_linux)
add_subdirectory_ifdef(CONFIG_UHC_NATIVE_LINUX uhc_native_linux)
