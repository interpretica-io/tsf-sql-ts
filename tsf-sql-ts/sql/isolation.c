/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * What a transaction isolation level actually isolates. Two sessions
 * are stepped through the three classic read anomalies - a dirty read,
 * a non-repeatable read, a phantom - at the level under test, and what
 * happens is compared with what this engine is documented to do at
 * that level:
 *
 *                       dirty   non-repeatable   phantom
 *   SQLite (any)          no         no             no
 *   PostgreSQL RU / RC    no         yes            yes
 *   PostgreSQL RR / SER   no         no             no
 *   MariaDB RU            yes        yes            yes
 *   MariaDB RC            no         yes            yes
 *   MariaDB RR / SER      no         no             no
 *
 * (PostgreSQL has no read uncommitted of its own and gives read
 * committed; its repeatable read is a snapshot, so it has no phantoms
 * either. MariaDB's repeatable read reads from a snapshot too.) An
 * anomaly is "prevented" also when the engine does it by making one
 * side wait: a statement that gives up on a lock counts as "no".
 *
 * The synchronisation is the order of the calls: every step is one
 * statement on one session and returns before the next is sent.
 */

#define TE_TEST_NAME    "sql/isolation"

#include "te_config.h"

#include "tapi_test.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

#define TABLE       "t_iso"
#define LOCK_MS     1000

/* Did the statement give way to the other session, rather than fail? */
static bool
gave_way(tapi_sql_conn *conn, te_errno rc)
{
    tapi_sql_err_class cls;

    if (rc == 0)
        return false;
    cls = tapi_sql_last_error(conn)->cls;
    if (cls != TAPI_SQL_ERR_LOCK_TIMEOUT && cls != TAPI_SQL_ERR_DEADLOCK &&
        cls != TAPI_SQL_ERR_SERIALIZATION)
        TEST_FAIL("A statement failed for an unexpected reason: %s (%s)",
                  tapi_sql_last_error(conn)->message,
                  tapi_sql_err_class2str(cls));
    return true;
}

int
main(int argc, char **argv)
{
    static const char *const tables[] = { TABLE };

    tsapi_sql_sess     sess = { 0 };
    tapi_sql_conn     *a = NULL;
    tapi_sql_conn     *b = NULL;
    tapi_sql_caps      caps;
    tapi_sql_isolation iso;
    const char        *backend;
    const char        *level;
    int64_t            first = 0;
    int64_t            second = 0;
    bool               dirty = false;
    bool               nonrep = false;
    bool               phantom = false;
    bool               exp_dirty = false;
    bool               exp_nonrep = false;
    bool               exp_phantom = false;
    double             seen[3];

    TEST_START;
    TEST_GET_STRING_PARAM(backend);
    TEST_GET_STRING_PARAM(level);
    iso = tsapi_sql_level(level);

    TEST_STEP("Open two sessions; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_iso"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &a));
    CHECK_RC(tsapi_sql_sess_connect(&sess, TSAPI_SQL_ROLE_APP, &b));

    CHECK_RC(tapi_sql_capabilities(a, &caps));
    if ((caps.isolation & (unsigned int)iso) == 0)
        TEST_SKIP("The engine does not have this isolation level");

    /* What the engine is documented to do. */
    if (sess.engine == TAPI_SQL_ENGINE_POSTGRES)
    {
        exp_nonrep = iso <= TAPI_SQL_ISO_READ_COMMITTED;
        exp_phantom = exp_nonrep;
    }
    else if (sess.engine == TAPI_SQL_ENGINE_MYSQL)
    {
        exp_dirty = iso == TAPI_SQL_ISO_READ_UNCOMMITTED;
        exp_nonrep = iso <= TAPI_SQL_ISO_READ_COMMITTED;
        exp_phantom = exp_nonrep;
    }

    TEST_STEP("Two rows; neither session waits for a lock for long");
    tsapi_sql_drop(a, tables, TE_ARRAY_LEN(tables));
    CHECK_RC(tapi_sql_execute(a, "CREATE TABLE " TABLE
                              " (id INTEGER PRIMARY KEY, v INTEGER)", NULL));
    CHECK_RC(tapi_sql_execute(a, "INSERT INTO " TABLE
                              " VALUES (1, 10), (2, 20)", NULL));
    CHECK_RC(tapi_sql_set_timeout(a, TAPI_SQL_TIMEOUT_LOCK, LOCK_MS));
    CHECK_RC(tapi_sql_set_timeout(b, TAPI_SQL_TIMEOUT_LOCK, LOCK_MS));

    TEST_STEP("Dirty read: A reads what B has changed and not committed");
    CHECK_RC(tapi_sql_begin(b));
    CHECK_RC(tapi_sql_execute(b, "UPDATE " TABLE " SET v = 11 WHERE id = 1",
                              NULL));
    CHECK_RC(tapi_sql_begin_iso(a, iso));
    rc = tapi_sql_query_int(a, "SELECT v FROM " TABLE " WHERE id = 1",
                            &first);
    if (!gave_way(a, rc))
        dirty = first == 11;
    CHECK_RC(tapi_sql_rollback(a));
    CHECK_RC(tapi_sql_rollback(b));
    RING("dirty read at %s: %s", level, dirty ? "happened" : "prevented");

    TEST_STEP("Non-repeatable read: A reads a row twice, B commits a "
              "change in between");
    CHECK_RC(tapi_sql_begin_iso(a, iso));
    CHECK_RC(tapi_sql_query_int(a, "SELECT v FROM " TABLE " WHERE id = 1",
                                &first));
    rc = tapi_sql_execute(b, "UPDATE " TABLE " SET v = 12 WHERE id = 1",
                          NULL);
    if (!gave_way(b, rc))
    {
        rc = tapi_sql_query_int(a, "SELECT v FROM " TABLE " WHERE id = 1",
                                &second);
        if (!gave_way(a, rc))
            nonrep = second != first;
    }
    CHECK_RC(tapi_sql_rollback(a));
    RING("non-repeatable read at %s: %s", level,
         nonrep ? "happened" : "prevented");

    TEST_STEP("Phantom: A counts rows twice, B commits a new one in "
              "between");
    CHECK_RC(tapi_sql_begin_iso(a, iso));
    CHECK_RC(tapi_sql_query_int(a, "SELECT count(*) FROM " TABLE
                                " WHERE v >= 10", &first));
    rc = tapi_sql_execute(b, "INSERT INTO " TABLE " VALUES (3, 30)", NULL);
    if (!gave_way(b, rc))
    {
        rc = tapi_sql_query_int(a, "SELECT count(*) FROM " TABLE
                                " WHERE v >= 10", &second);
        if (!gave_way(a, rc))
            phantom = second != first;
    }
    CHECK_RC(tapi_sql_rollback(a));
    RING("phantom at %s: %s", level, phantom ? "happened" : "prevented");

    seen[0] = dirty;
    seen[1] = nonrep;
    seen[2] = phantom;
    tsapi_sql_chart("Anomalies seen: dirty read, non-repeatable read, "
                    "phantom", NULL, NULL, "seen", seen, 3);

    TEST_STEP("Compare with what the engine documents for this level");
    if (dirty != exp_dirty)
        TEST_VERDICT("A dirty read %s at this level",
                     dirty ? "happened" : "was prevented");
    if (nonrep != exp_nonrep)
        TEST_VERDICT("A non-repeatable read %s at this level",
                     nonrep ? "happened" : "was prevented");
    if (phantom != exp_phantom)
        TEST_VERDICT("A phantom %s at this level",
                     phantom ? "happened" : "was prevented");

    TEST_SUCCESS;

cleanup:
    if (a != NULL)
        tapi_sql_rollback(a);
    if (b != NULL)
        tapi_sql_rollback(b);
    tapi_sql_disconnect(b);
    tsapi_sql_drop(a, tables, TE_ARRAY_LEN(tables));
    tapi_sql_disconnect(a);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
