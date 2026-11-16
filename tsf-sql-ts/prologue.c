/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2025-2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Test Suite
 *
 * Test Suite prologue: start the database servers the suite tests
 * against.
 *
 * @author Maxim Menshikov <maxim.menshikov@interpretica.io>
 */

#define TE_TEST_NAME    "prologue"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_sql.h"
#include "tapi_sql_rpc.h"
#include "tsapi_sql.h"

int
main(int argc, char **argv)
{
    tsapi_sql_sess sess = { 0 };
    te_string      versions = TE_STRING_INIT;
    unsigned int   engines = 0;
    double         present[3];

    TEST_START;

    TEST_STEP("See which database drivers the agent carries");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_prologue"));
    if (rpc_sql_available(sess.pco, &engines, &versions) == 0)
        RING("Database drivers on the agent: %s", versions.ptr);
    else
        WARN("The agent has no database driver: every test will skip");
    present[0] = (engines & TAPI_SQL_ENGINE_SQLITE) != 0;
    present[1] = (engines & TAPI_SQL_ENGINE_POSTGRES) != 0;
    present[2] = (engines & TAPI_SQL_ENGINE_MYSQL) != 0;
    tsapi_sql_chart("Drivers present (SQLite, PostgreSQL, MariaDB)", NULL,
                    NULL, "present", present, 3);

    TEST_STEP("Start PostgreSQL and MariaDB in containers");
    rc = tsapi_sql_servers_up(sess.pco);
    if (rc != 0 && TE_RC_GET_ERROR(rc) != TE_EOPNOTSUPP)
        TEST_VERDICT("The database servers could not be started");

    TEST_SUCCESS;

cleanup:
    te_string_free(&versions);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
