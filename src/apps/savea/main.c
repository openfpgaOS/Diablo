//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * SaveA -- save cross-pollution test (step=3)
 * Run alternately with SaveB to detect cross-app save corruption.
 */

#define APP_ID          0xAA
#define APP_NAME        "SaveA"
#define STEP            3
#define VSAVE_SIZE_EVEN 16384

#include "../save_test.h"

int main(void) { return save_test_main(); }
