/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Group epilogue: nothing to do, every test brings its own tables and
 * removes them.
 */

#define TE_TEST_NAME    "sql/epilogue"

#include "te_config.h"
#include "tapi_test.h"

int
main(int argc, char **argv)
{
    TEST_START;
    TEST_STEP("SQL group epilogue");
    TEST_SUCCESS;
cleanup:
    TEST_END;
}
