/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * The life of a connection: connect, ask what the engine can do, ping,
 * close, connect again. What was committed is still there after a
 * reconnection; what a session left uncommitted is not. Closing a
 * connection leaves nothing behind on the agent.
 */

#define TE_TEST_NAME    "sql/lifecycle"

#include "te_config.h"

#include <string.h>

#include "tapi_test.h"
#include "te_string.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tapi_sql_perf.h"
#include "tsapi_sql.h"

#define TABLE       "t_life"
#define CONNECTS    5

int
main(int argc, char **argv)
{
    static const char *const tables[] = { TABLE };
    static const double      pct[] = { 50, 95 };

    tsapi_sql_sess  sess = { 0 };
    tapi_sql_conn  *conn = NULL;
    tapi_sql_config cfg;
    tapi_sql_caps   caps;
    tapi_sql_perf   perf;
    te_string       text = TE_STRING_INIT;
    const char     *backend;
    unsigned int    conns = 0;
    unsigned int    stmts = 0;

    TEST_START;
    TEST_GET_STRING_PARAM(backend);
    tapi_sql_perf_init(&perf, "connect");

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_life"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");

    TEST_STEP("Connect, ping, and read the capabilities");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &conn));
    CHECK_RC(tapi_sql_ping(conn));
    CHECK_RC(tapi_sql_capabilities(conn, &caps));
    tapi_sql_conn_describe(conn, &text);
    RING("Connected: %s; server %s; caps 0x%x, isolation levels 0x%x, "
         "default %s", text.ptr, caps.server, caps.caps, caps.isolation,
         tapi_sql_isolation2str(caps.default_iso));
    if ((caps.caps & TAPI_SQL_CAP_TX) == 0)
        TEST_VERDICT("The engine reports no transactions");
    if ((caps.isolation & (unsigned int)caps.default_iso) == 0)
        TEST_VERDICT("The default isolation level is not among the "
                     "supported ones");
    if (caps.server[0] == '\0')
        TEST_VERDICT("The server version is empty");

    TEST_STEP("Commit some state");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(conn, "CREATE TABLE " TABLE
                              " (id INTEGER PRIMARY KEY, v VARCHAR(20))",
                              NULL));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " TABLE
                              " VALUES (1, 'one'), (2, 'two'), (3, 'three')",
                              NULL));

    TEST_STEP("The committed state survives a reconnection");
    CHECK_RC(tapi_sql_assert_persistent(conn, "SELECT id, v FROM " TABLE
                                        " ORDER BY id"));
    CHECK_RC(tapi_sql_ping(conn));

    TEST_STEP("Uncommitted work of a closed session is gone");
    CHECK_RC(tapi_sql_begin(conn));
    CHECK_RC(tapi_sql_execute(conn, "INSERT INTO " TABLE
                              " VALUES (4, 'never committed')", NULL));
    CHECK_RC(tapi_sql_reconnect(conn));
    CHECK_RC(tapi_sql_assert_row_count(conn, TABLE, 3));

    TEST_STEP("A closed connection leaves nothing on the agent");
    tsapi_sql_drop(conn, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_disconnect(conn));
    conn = NULL;
    CHECK_RC(tapi_sql_stats(sess.pco, &conns, &stmts, NULL));
    if (conns != 0 || stmts != 0)
        TEST_VERDICT("The agent still holds %u connection(s) and %u "
                     "statement(s) after the disconnect", conns, stmts);

    TEST_STEP("Connect and disconnect repeatedly");
    tsapi_sql_sess_config(&sess, TSAPI_SQL_ROLE_APP, &cfg);
    CHECK_RC(tapi_sql_perf_run_connect(sess.pco, &cfg, CONNECTS, &perf));
    if (perf.count != CONNECTS)
        TEST_VERDICT("Only %u of %u connections were established",
                     perf.count, CONNECTS);
    tapi_sql_perf_log(&perf, pct, TE_ARRAY_LEN(pct));
    CHECK_RC(tapi_sql_stats(sess.pco, &conns, NULL, NULL));
    if (conns != 0)
        TEST_VERDICT("Connections leaked across reconnects");

    TEST_SUCCESS;

cleanup:
    tapi_sql_perf_free(&perf);
    te_string_free(&text);
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
