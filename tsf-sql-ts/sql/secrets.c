/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief SQL Group
 *
 * Credentials do not leak. The description of a connection has no
 * password in it; neither has the message of a refused connection; and
 * when a client library does quote the secret back - here it is
 * provoked into it by a configuration mistake that puts the password
 * where the host name belongs - the agent blanks it out before the
 * message leaves it.
 */

#define TE_TEST_NAME    "sql/secrets"

#include "te_config.h"

#include <string.h>

#include "tapi_test.h"
#include "te_string.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"
#include "tsapi_sql.h"

/* A secret unlike anything else in a log, so it can be searched for. */
#define MARKER  "s3cr3t-Zq7-marker-Lk2"

int
main(int argc, char **argv)
{
    tsapi_sql_sess  sess = { 0 };
    tapi_sql_conn  *conn = NULL;
    tapi_sql_config cfg;
    tapi_sql_error  err;
    te_string       text = TE_STRING_INIT;
    const char     *backend;
    double          clean[3];

    TEST_START;
    TEST_GET_STRING_PARAM(backend);

    TEST_STEP("Open a session; skip when the engine is not there");
    CHECK_RC(tsapi_sql_sess_init(&sess, "pco_sql_secrets"));
    if (!tsapi_sql_sess_select(&sess, backend))
        TEST_SKIP("The engine is not available here");
    if (sess.engine == TAPI_SQL_ENGINE_SQLITE)
        TEST_SKIP("The engine has no credentials");

    TEST_STEP("The description of a connection carries no password");
    tsapi_sql_sess_config(&sess, TSAPI_SQL_ROLE_APP, &cfg);
    CHECK_RC(tapi_sql_connect(sess.pco, &cfg, &conn));
    tapi_sql_conn_describe(conn, &text);
    RING("Connection: %s", text.ptr);
    if (strstr(text.ptr, cfg.password) != NULL)
        TEST_VERDICT("The description of a connection shows its password");
    if (strstr(text.ptr, cfg.user) == NULL)
        TEST_VERDICT("The description of a connection does not name the "
                     "account");
    clean[0] = 1.0;
    CHECK_RC(tapi_sql_disconnect(conn));
    conn = NULL;

    TEST_STEP("A refused login does not quote the password it refused");
    memset(&err, 0, sizeof(err));
    cfg.password = MARKER;
    rc = tapi_sql_connect_err(sess.pco, &cfg, &conn, &err);
    if (rc == 0)
        TEST_VERDICT("A made-up password was accepted");
    RING("Refused: %s", err.message);
    if (strstr(err.message, MARKER) != NULL)
        TEST_VERDICT("The message of a refused login shows the password");
    clean[1] = 1.0;

    TEST_STEP("A message that would quote the secret is blanked on the "
              "agent");
    /* The mistake: the password pasted into the host field as well.
     * Resolving it fails, and the client library names what it could
     * not resolve. */
    cfg.host = MARKER;
    cfg.timeout_ms = 3000;
    memset(&err, 0, sizeof(err));
    rc = tapi_sql_connect_err(sess.pco, &cfg, &conn, &err);
    if (rc == 0)
        TEST_VERDICT("A connection to a host that does not exist "
                     "succeeded");
    RING("Refused: %s", err.message);
    if (strstr(err.message, MARKER) != NULL)
        TEST_VERDICT("A client library's message with the secret in it "
                     "reached the test unredacted");
    if (err.message[0] == '\0')
        TEST_VERDICT("The refusal has no message at all");
    clean[2] = 1.0;

    tsapi_sql_chart("Places checked and found clean of the secret", NULL,
                    NULL, "clean", clean, 3);

    TEST_SUCCESS;

cleanup:
    te_string_free(&text);
    tapi_sql_disconnect(conn);
    tsapi_sql_sess_fini(&sess);
    TEST_END;
}
