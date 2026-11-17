# tsf-sql-ts

A Test Environment (TE) suite that verifies
[tsf-sql](https://github.com/interpretica-io/tsf-sql): SQL database
sessions from a Test Agent, over the engines' own client libraries
linked into the agent's RPC server.

One test matrix runs on three engines. Every test takes the engine as
its `backend` parameter (`sqlite`, `postgres` or `mariadb`), so a
difference between engines shows up as a difference between two
iterations of the same test. The `isolation` and `lost_update` results
in particular record what each engine does at each isolation level.

## What it runs against

- **PostgreSQL and MariaDB** in containers. The suite prologue starts
  them through the agent's docker client (`scripts/sql-servers.sh`).
  Each has TLS, with a certificate issued by a CA made for the run, and
  four accounts: an administrator, an ordinary account that owns the
  test database, a restricted account, and an account that is let in
  over TLS only. Each also gets a **replica** in a container of its own
  (PostgreSQL streaming replication from a base backup; MariaDB
  binary-log replication by GTID), read-only, for the `replication`
  test. The epilogue removes them all.
- **SQLite** in a temporary file on the agent, a fresh one per test.

The suite has no mock database. A test skips when its engine is not
there, which happens when the agent has no driver for it or no docker to
start a server with.

To use servers of your own instead, set `TSF_SQL_SERVERS=external` and
`TSF_SQL_PG_HOST` / `TSF_SQL_PG_PORT` / `TSF_SQL_MY_HOST` /
`TSF_SQL_MY_PORT` (and `TSF_SQL_PG_REPLICA_PORT` /
`TSF_SQL_MY_REPLICA_PORT` for their replicas on the same hosts; without
these the replication test skips). They must be set up as
`scripts/sql-servers.sh` sets its own up: a database `tsf`, the accounts
and passwords of
`lib/tsapi_sql.h`, TLS with the CA certificate at
`/tmp/tsf-sql-ts/ca.crt` on the agent.

## Tests

Group `sql`. `S` = SQLite, `P` = PostgreSQL, `M` = MariaDB.

| Test | Engines | What it checks |
|---|---|---|
| `lifecycle` | S P M | Connect, capabilities, ping, disconnect, reconnect; committed state survives a reconnection, uncommitted state does not; nothing is left on the agent |
| `crud` | S P M | Insert, select, update, delete; rows-changed counts |
| `params` | S P M | Parameterised statements: every value type bound and read back - awkward text, binary with every byte, a 64-bit integer, NULLs, empty values, full unsigned range where the engine has it; rebinding; a parameter out of range refused |
| `transactions` | S P M | Commit seen by another session, rollback by nobody, savepoints, a hostile savepoint name refused, atomicity of a failing transaction |
| `schema` | S P M | Tables, views, columns, indexes, constraints, foreign keys in the same structures on every engine |
| `aggregates` | S P M | Counts, sums, minima, maxima, averages, GROUP BY, HAVING, DISTINCT, conditional aggregation, a window total; NULLs skipped, empty input - against values worked out by hand |
| `compare` | S P M | Result comparison: ordered and unordered, NULL semantics, numbers by value or type, absolute and relative tolerance - each decided both ways |
| `assertions` | S P M | Every database assertion holds on a database in good order |
| `assert_violation` | S P M x 8 properties | Every database assertion **fails with its verdict** on a database broken on purpose (expected failures, see below) |
| `isolation` | S; P M x 4 levels | Dirty read, non-repeatable read, phantom at each isolation level, against what the engine documents |
| `lost_update` | S; P M x 3 levels | The lost update at each level, with a blocked writer started, checked waiting, released and collected |
| `deadlock` | P M | A deadlock is detected: exactly one victim, the survivor's rows remain |
| `concurrent` | S P M | Four sessions insert and bump a counter at once, in rounds; everything adds up |
| `errors` | S P M | Syntax, missing table and column, duplicate key, dangling reference, NULL - same error class on every engine, connection usable afterwards |
| `connloss` | P M | A session killed on the server is reported as a lost connection; reconnection works; the agent releases what it held |
| `replication` | P M | A replica follows its primary: schema and rows arrive (timed), identical; rolled back and uncommitted work never arrives; a transaction arrives whole; the replica refuses writes as read-only |
| `timeouts` | S P M | Statement deadline, cancellation, lock deadline; statements in every state are released when the connections close |
| `large` | S P M | 50 000 rows read in bounded batches; the agent never builds more than one bounded batch |
| `tls_auth` | P M | Verified TLS works; a wrong password, an unknown account, an untrusted CA, a TLS-only account in the clear and a closed port are each refused as what they are |
| `secrets` | P M | No password in a connection's description or a refusal's message; a driver message that quotes the secret is blanked |
| `audit` | S P M | Read-only security posture; the administrator over plain text is the known-bad control |
| `privsep` | P M | A restricted account is refused everything it was not granted; wrong passwords are refused; controls for both |
| `injection` | S P M | Hostile inputs through an application: the version that pastes input is broken (control), the one that binds it is not |
| `perf` | S P M | Query latency, rows per second, transaction throughput, connect latency, error rate, timeout frequency |
| `equivalence` | P-S, M-S, P-M | The same data and queries on two engines give equivalent results |

100 iterations. Every test ends with a TE measurement artifact and a line
graph (rows per batch, anomalies seen, latency samples, findings by
severity, ...), drawn in the HTML log and in Bublik.

### Expected failures

`conf/trc.xml` lists the 24 iterations of `assert_violation` as expected
to fail, each with the verdict of the assertion it provokes. They are
the controls of the database assertions: each one breaks a property on
purpose and shows that its assertion then fails. A normal run reports
them as "failed, as expected".

### Transaction semantics, as observed

`isolation` and `lost_update` assert this table (yes = the anomaly
happens):

| | dirty read | non-repeatable read | phantom | lost update |
|---|---|---|---|---|
| SQLite | no | no | no | no |
| PostgreSQL read uncommitted / committed | no | yes | yes | yes |
| PostgreSQL repeatable read / serializable | no | no | no | no |
| MariaDB read uncommitted | yes | yes | yes | - |
| MariaDB read committed | no | yes | yes | yes |
| MariaDB repeatable read | no | no | no | yes |
| MariaDB serializable | no | no | no | no |

MariaDB with `innodb_snapshot_isolation` on refuses the lost update at
repeatable read; the test asks the server and expects accordingly.

## Running

TE is expected as a sibling directory:

```
git clone https://github.com/interpretica-io/test-environment ../test-environment
./scripts/run.sh docker guess --cfg=localhost
```

The Docker image carries TE's build dependencies, the three client
libraries, a docker client and openssl. `run.sh` mounts the Docker
socket, so the PostgreSQL and MariaDB containers are siblings of the
build container. They publish their ports on loopback: 55433 and 53307,
with the replicas on 55434 and 53308. `TSF_SQL_PG_PORT`,
`TSF_SQL_MY_PORT`, `TSF_SQL_PG_REPLICA_PORT` and
`TSF_SQL_MY_REPLICA_PORT` change the ports; `TSF_SQL_PG_IMAGE` and
`TSF_SQL_MY_IMAGE` pick the server versions.

To build without Docker, run `./scripts/run.sh guess --cfg=localhost`.
The host then needs TE's dependencies, `libsqlite3-dev`, `libpq-dev`,
`libmariadb-dev`, and a docker client for the servers.

`run.sh` exits 0 whenever the run completes. `scripts/check-run.sh
<output of run.sh>` turns the Tester's summary into a verdict: every
iteration passed, or failed as `conf/trc.xml` expects.

Results: `log.txt` (text), `./scripts/html-log.sh` (HTML, with the
graphs).

On the localhost configuration the agent is the build container. Name a
host in `conf/rcf.conf` and the same tests reach the databases from that
host.

## The fixture credentials

The accounts and passwords in `scripts/sql-servers.sh` and
`lib/tsapi_sql.h` belong to containers that live for one run and listen
on loopback, so they protect nothing. The suite needs to know them
because `secrets`, `tls_auth` and `privsep` check that the module does
not show a password, and such a check has to know the password it is
looking for.

## Layout

```
conf/                    TE configuration: builder, RCF, Tester, TRC
scripts/run.sh           build and run (docker | native)
scripts/sql-servers.sh   start / stop the PostgreSQL and MariaDB containers
scripts/check-run.sh     verdict of a run against conf/trc.xml
scripts/lint.sh          static checks, as CI runs them
tsf-sql-ts/prologue.c    starts the servers
tsf-sql-ts/epilogue.c    removes them
tsf-sql-ts/lib/          tsapi_sql: sessions, accounts, charts
tsf-sql-ts/sql/          the tests
```

## CI

`lint` runs on every push: layout, licence headers, XML, shellcheck,
actionlint, and `scripts/lint_suite.py`, which checks that the tests,
the meson list, `package.xml`, the expected results in `conf/trc.xml`
and this README agree. `integration` runs when started by hand and once
a week; it does the full Docker build and run on all three engines and
is judged by `scripts/check-run.sh`.

## Licence

MIT. See `LICENSE`.
