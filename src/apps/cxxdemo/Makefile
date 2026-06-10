#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# cxxdemo uses C++ classes / new / delete -- pull in the SDK's
# minimal C++ ABI shim (operator new/delete, __cxa_*, static guards).
SDK_DIR  = ../../sdk
SRCS_CXX = main.cpp $(SDK_DIR)/of_cxxabi.cpp
include ../app.mk
