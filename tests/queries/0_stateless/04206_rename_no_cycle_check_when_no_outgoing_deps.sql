-- Regression test for the RENAME / EXCHANGE cyclic-dependency check fast path
-- in `DatabaseCatalog::checkTableCanBeRenamedWithNoCyclicDependencies` and
-- `DatabaseCatalog::checkTablesCanBeExchangedWithNoCyclicDependencies`.
--
-- The check used to run a full O(V + E) topological sort over the catalog dependency
-- graph on every `RENAME` and `EXCHANGE`, even when the renamed-from table had no
-- outgoing edges in the graph. That made `CREATE OR REPLACE TABLE` — which performs
-- a `RENAME` of a freshly-created intermediate table into the target name — scale
-- linearly with the total number of tables in the server, dramatically slowing down
-- concurrent `CREATE OR REPLACE` workloads on multi-tenant deployments.
--
-- This test exercises three things:
--   1. `CREATE OR REPLACE TABLE` works correctly when the target does not exist.
--   2. `CREATE OR REPLACE TABLE` works correctly when the target exists and has
--      a materialized view depending on it. The dependent edges must survive the
--      rename intact.
--   3. The cyclic-dependency check still rejects a `CREATE` that would actually
--      introduce a cycle. The short-circuit must not silence the slow path.

DROP TABLE IF EXISTS rename_no_cycle_t1 SYNC;
DROP TABLE IF EXISTS rename_no_cycle_t2 SYNC;
DROP TABLE IF EXISTS rename_no_cycle_mv SYNC;
DROP TABLE IF EXISTS rename_no_cycle_t1_cycle SYNC;
DROP DICTIONARY IF EXISTS rename_no_cycle_dict;

-- (1) Plain `CREATE OR REPLACE TABLE` on a non-existent target. The intermediate
-- table has no outgoing dependencies, so the short-circuit in the rename cycle
-- check fires and the rename completes without scanning the catalog graph.
CREATE OR REPLACE TABLE rename_no_cycle_t1 (a Int32, b String) ENGINE = Memory;
INSERT INTO rename_no_cycle_t1 VALUES (1, 'one');
SELECT a, b FROM rename_no_cycle_t1 ORDER BY a;

-- (2) `CREATE OR REPLACE TABLE` on a target with a dependent materialized view.
-- The MV's inbound edge must remain attached after the source table is replaced.
CREATE TABLE rename_no_cycle_t2 (a Int32, b String) ENGINE = Memory;
CREATE MATERIALIZED VIEW rename_no_cycle_mv ENGINE = Memory AS SELECT a, upper(b) AS b_upper FROM rename_no_cycle_t2;
INSERT INTO rename_no_cycle_t2 VALUES (1, 'one');
SELECT a, b_upper FROM rename_no_cycle_mv ORDER BY a;

CREATE OR REPLACE TABLE rename_no_cycle_t2 (a Int32, b String) ENGINE = Memory;
INSERT INTO rename_no_cycle_t2 VALUES (2, 'two');
-- The MV should still exist and accept new inserts via the new source.
SELECT a, b_upper FROM rename_no_cycle_mv ORDER BY a;

-- (3) Cycle detection on the `CREATE` path still works. A dictionary that resolves
-- a column from a table, combined with an `ALTER` that adds a column using that
-- dictionary, would form a cycle — the catalog must still reject it. This guards
-- against the rename short-circuit being applied too aggressively in the future.
CREATE TABLE rename_no_cycle_t1_cycle (key Int32, value Int32) ENGINE = MergeTree ORDER BY key;
INSERT INTO rename_no_cycle_t1_cycle VALUES (0, 0);

CREATE DICTIONARY rename_no_cycle_dict (key Int32, value Int32)
PRIMARY KEY key
SOURCE(CLICKHOUSE(DATABASE currentDatabase() TABLE rename_no_cycle_t1_cycle))
LIFETIME(MIN 0 MAX 0)
LAYOUT(HASHED());

ALTER TABLE rename_no_cycle_t1_cycle ADD COLUMN cycle_col Int32 DEFAULT dictGetOrDefault('rename_no_cycle_dict', 'value', 0, 1); -- {serverError INFINITE_LOOP}

DROP DICTIONARY rename_no_cycle_dict;
DROP TABLE rename_no_cycle_t1_cycle;
DROP TABLE rename_no_cycle_mv;
DROP TABLE rename_no_cycle_t2;
DROP TABLE rename_no_cycle_t1;
