/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Test Suite library: what every tsf-sql test needs
 *
 * An RPC server on the agent, the engine a test iteration is about,
 * connection parameters for the accounts the suite's servers carry, a
 * few schema helpers that hide the two places the engines' DDL differs,
 * and the chart every test ends with.
 */

#ifndef __TSAPI_SQL_H__
#define __TSAPI_SQL_H__

#include "rcf_rpc.h"
#include "te_errno.h"
#include "te_string.h"

#include "tapi_sql.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The agent this suite works on. */
#define TSAPI_SQL_TA        "Agt_A"

/** Where, on the agent, the suite keeps its certificates and files. */
#define TSAPI_SQL_WORKDIR   "/tmp/tsf-sql-ts"

/**
 * @name Fixture accounts
 * The accounts scripts/sql-servers.sh creates in the containers it
 * starts; keep in step with it. They are fixtures of servers that live
 * for one run on loopback and protect nothing - which is exactly why
 * the suite can use them to check that the module never shows a
 * secret: a test knows what must not appear.
 * @{ */
#define TSAPI_SQL_ADMIN_PW  "tsf-fixture-admin-7Qx"
#define TSAPI_SQL_APP_PW    "tsf-fixture-app-4Zr"
#define TSAPI_SQL_RO_PW     "tsf-fixture-ro-9Lm"
#define TSAPI_SQL_TLS_PW    "tsf-fixture-tls-2Kd"
/** @} */

/** The accounts of a server. */
typedef enum tsapi_sql_role {
    TSAPI_SQL_ROLE_ADMIN,   /**< The server's administrator. */
    TSAPI_SQL_ROLE_APP,     /**< An ordinary account owning the test
                                 database: what the tests run as. */
    TSAPI_SQL_ROLE_RO,      /**< An account with no privileges of its
                                 own; a test grants it what it needs. */
    TSAPI_SQL_ROLE_TLS,     /**< An account let in over TLS only. */
} tsapi_sql_role;

/** What a test needs: an agent, an RPC server on it, an engine. */
typedef struct tsapi_sql_sess {
    const char     *ta;         /**< Agent name. */
    rcf_rpc_server *pco;        /**< RPC server - what tapi_sql takes. */
    tapi_sql_engine engine;     /**< The engine of this iteration. */
    char            file[128];  /**< The SQLite database file. */
} tsapi_sql_sess;

/**
 * Open a session: an RPC server on the agent.
 *
 * @param[out] sess  Session.
 * @param[in]  name  Name for the RPC server, unique within the test.
 *
 * @return Status code.
 */
extern te_errno tsapi_sql_sess_init(tsapi_sql_sess *sess, const char *name);

/**
 * Choose the engine the session's connections use.
 *
 * Every test takes the engine as its @c backend parameter, so one test
 * checks each engine and a difference between them shows up as a
 * difference between two iterations.
 *
 * @param sess     Session.
 * @param backend  @c "sqlite", @c "postgres" or @c "mariadb".
 *
 * @return @c true when the test can run: the agent has the driver and,
 *         for a server engine, the suite has its server. When it
 *         returns @c false the test has nothing to run on and skips.
 */
extern bool tsapi_sql_sess_select(tsapi_sql_sess *sess, const char *backend);

/**
 * Connection parameters of an account on the session's engine. The
 * connection is plain (TLS disabled) except for #TSAPI_SQL_ROLE_TLS,
 * which requires encryption; a test changes @c tls_mode and @c tls_ca
 * as it needs. SQLite has no accounts: every role is the same file.
 *
 * @param[in]  sess  Session.
 * @param[in]  role  Account.
 * @param[out] cfg   Parameters; the strings are static or the
 *                   session's.
 */
extern void tsapi_sql_sess_config(const tsapi_sql_sess *sess,
                                  tsapi_sql_role role, tapi_sql_config *cfg);

/**
 * Connect as an account of the session's engine.
 *
 * @param[in]  sess  Session.
 * @param[in]  role  Account.
 * @param[out] conn  Connection.
 *
 * @return Status code.
 */
extern te_errno tsapi_sql_sess_connect(const tsapi_sql_sess *sess,
                                       tsapi_sql_role role,
                                       tapi_sql_conn **conn);

/**
 * Close a session: remove the SQLite file, destroy the RPC server.
 * Connections left open die with the RPC server.
 *
 * @param sess  Session.
 */
extern void tsapi_sql_sess_fini(tsapi_sql_sess *sess);

/** The engine's name for a column of bytes ("BLOB" or "BYTEA"). */
extern const char *tsapi_sql_blob_type(tapi_sql_engine engine);

/** An isolation level by its test-parameter name ("read_committed"). */
extern tapi_sql_isolation tsapi_sql_level(const char *name);

/**
 * Drop tables, last to first so that a referencing table goes before
 * the one it refers to. Tables that do not exist are skipped.
 *
 * @param conn    Connection.
 * @param tables  Table names, parents first.
 * @param n       How many.
 */
extern void tsapi_sql_drop(tapi_sql_conn *conn, const char *const *tables,
                           unsigned int n);

/**
 * Create a table of the ten digits, @c d from 0 to 9. Crossed with
 * itself it makes any number of rows on every engine without a
 * recursive query, and nine of them crossed make a query slow enough
 * to cancel or time out.
 *
 * @param conn   Connection.
 * @param table  Name for the table (dropped first).
 *
 * @return Status code.
 */
extern te_errno tsapi_sql_digits(tapi_sql_conn *conn, const char *table);

/**
 * Write a query that counts a thousand million rows over a digits
 * table: slow on every engine.
 *
 * @param[in]  table  The digits table.
 * @param[out] sql    The query is appended here.
 */
extern void tsapi_sql_slow_query(const char *table, te_string *sql);

/**
 * Start the suite's database servers on the agent (the prologue).
 *
 * @param pco  RPC server.
 *
 * @return 0 when the servers are up; TE_EOPNOTSUPP when the agent
 *         cannot run them (no docker), in which case the tests of the
 *         server engines skip; another status when starting failed.
 */
extern te_errno tsapi_sql_servers_up(rcf_rpc_server *pco);

/** Stop them (the epilogue). */
extern te_errno tsapi_sql_servers_down(rcf_rpc_server *pco);

/**
 * Emit a measurement artifact with one line graph: @p ys over @p xs,
 * or over the sample number when @p xs is @c NULL.
 *
 * @param title  Graph title.
 * @param xname  Name of the X quantity (ignored without @p xs).
 * @param xs     X values, or @c NULL.
 * @param yname  Name of the Y quantity.
 * @param ys     Y values.
 * @param n      Number of points.
 */
extern void tsapi_sql_chart(const char *title, const char *xname,
                            const double *xs, const char *yname,
                            const double *ys, unsigned int n);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSAPI_SQL_H__ */
