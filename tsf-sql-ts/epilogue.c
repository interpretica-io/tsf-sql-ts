/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2025-2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Test Suite
 *
 * Test Suite epilogue: remove the database servers.
 *
 * @author Maxim Menshikov <maxim.menshikov@interpretica.io>
 */

#define TE_TEST_NAME    "epilogue"

#include "te_config.h"
#include "tapi_test.h"

#include "tsapi_sql.h"

int
main(int argc, char **argv)
{
    tsapi_sql_sess sess = { 0 };

    TEST_START;

    TEST_STEP("Stop the database servers");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_epilogue"));
    if (tsapi_sql_servers_down(sess.pco) != 0)
        WARN("The database servers could not be removed");

    TEST_SUCCESS;

cleanup:
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
