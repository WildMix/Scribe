-- Scribe PostgreSQL Logical Replication Setup
-- This script sets up logical replication-based change data capture

-- PREREQUISITES:
-- 1. PostgreSQL must have wal_level = logical in postgresql.conf
-- 2. max_replication_slots must be >= 1
-- 3. max_wal_senders must be >= 1
-- 4. User must have REPLICATION privilege or be superuser

-- Check if logical replication is available
DO $$
DECLARE
    wal_level_value TEXT;
BEGIN
    SELECT setting INTO wal_level_value FROM pg_settings WHERE name = 'wal_level';

    IF wal_level_value != 'logical' THEN
        RAISE EXCEPTION 'Logical replication not available. Current wal_level: %. '
                        'Set wal_level = logical in postgresql.conf and restart PostgreSQL.',
                        wal_level_value;
    END IF;

    RAISE NOTICE 'Logical replication is available (wal_level = %)', wal_level_value;
END $$;

-- Create replication slot (if not exists)
-- Note: This must be done by a user with REPLICATION privilege
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_replication_slots WHERE slot_name = 'scribe_slot') THEN
        PERFORM pg_create_logical_replication_slot('scribe_slot', 'pgoutput');
        RAISE NOTICE 'Created replication slot: scribe_slot';
    ELSE
        RAISE NOTICE 'Replication slot already exists: scribe_slot';
    END IF;
END $$;

-- Create publication for all tables (or specify tables)
-- Option 1: All tables
-- CREATE PUBLICATION scribe_pub FOR ALL TABLES;

-- Option 2: Specific tables (recommended)
-- DROP PUBLICATION IF EXISTS scribe_pub;
-- CREATE PUBLICATION scribe_pub FOR TABLE table1, table2, table3;

-- Helper function to create publication for specific tables
CREATE OR REPLACE FUNCTION scribe_create_publication(table_names TEXT[])
RETURNS VOID AS $$
DECLARE
    table_list TEXT;
BEGIN
    -- Drop existing publication
    DROP PUBLICATION IF EXISTS scribe_pub;

    IF array_length(table_names, 1) > 0 THEN
        table_list := array_to_string(table_names, ', ');
        EXECUTE format('CREATE PUBLICATION scribe_pub FOR TABLE %s', table_list);
        RAISE NOTICE 'Created publication scribe_pub for tables: %', table_list;
    ELSE
        CREATE PUBLICATION scribe_pub FOR ALL TABLES;
        RAISE NOTICE 'Created publication scribe_pub for ALL TABLES';
    END IF;
END;
$$ LANGUAGE plpgsql;

-- Set REPLICA IDENTITY FULL for tables to get before values on UPDATE/DELETE
-- This is important for computing accurate change diffs
CREATE OR REPLACE FUNCTION scribe_set_replica_identity_full(table_names TEXT[])
RETURNS VOID AS $$
DECLARE
    tbl TEXT;
BEGIN
    FOREACH tbl IN ARRAY table_names LOOP
        EXECUTE format('ALTER TABLE %I REPLICA IDENTITY FULL', tbl);
        RAISE NOTICE 'Set REPLICA IDENTITY FULL for table: %', tbl;
    END LOOP;
END;
$$ LANGUAGE plpgsql;

-- View current replication slot status
CREATE OR REPLACE VIEW scribe_slot_status AS
SELECT
    slot_name,
    plugin,
    slot_type,
    active,
    restart_lsn,
    confirmed_flush_lsn,
    wal_status,
    pg_size_pretty(pg_wal_lsn_diff(pg_current_wal_lsn(), restart_lsn)) as lag_bytes
FROM pg_replication_slots
WHERE slot_name = 'scribe_slot';

-- View publication info
CREATE OR REPLACE VIEW scribe_publication_info AS
SELECT
    p.pubname as publication_name,
    p.puballtables as all_tables,
    pt.tablename as table_name,
    pt.schemaname as schema_name
FROM pg_publication p
LEFT JOIN pg_publication_tables pt ON p.pubname = pt.pubname
WHERE p.pubname = 'scribe_pub';

-- Helper to check slot health
CREATE OR REPLACE FUNCTION scribe_check_slot_health()
RETURNS TABLE (
    status TEXT,
    message TEXT,
    lag_bytes BIGINT,
    lag_pretty TEXT
) AS $$
DECLARE
    slot_record RECORD;
    max_lag_bytes BIGINT := 1073741824; -- 1GB warning threshold
BEGIN
    SELECT * INTO slot_record
    FROM pg_replication_slots
    WHERE slot_name = 'scribe_slot';

    IF NOT FOUND THEN
        RETURN QUERY SELECT
            'ERROR'::TEXT,
            'Replication slot scribe_slot not found'::TEXT,
            0::BIGINT,
            '0 bytes'::TEXT;
        RETURN;
    END IF;

    -- Calculate lag
    lag_bytes := pg_wal_lsn_diff(pg_current_wal_lsn(), slot_record.restart_lsn);

    IF NOT slot_record.active THEN
        RETURN QUERY SELECT
            'WARNING'::TEXT,
            'Replication slot is not active - consumer may be offline'::TEXT,
            lag_bytes,
            pg_size_pretty(lag_bytes);
    ELSIF lag_bytes > max_lag_bytes THEN
        RETURN QUERY SELECT
            'WARNING'::TEXT,
            format('High replication lag detected: %s', pg_size_pretty(lag_bytes))::TEXT,
            lag_bytes,
            pg_size_pretty(lag_bytes);
    ELSE
        RETURN QUERY SELECT
            'OK'::TEXT,
            'Replication slot is healthy'::TEXT,
            lag_bytes,
            pg_size_pretty(lag_bytes);
    END IF;
END;
$$ LANGUAGE plpgsql;

-- Cleanup function
CREATE OR REPLACE FUNCTION scribe_cleanup_logical()
RETURNS VOID AS $$
BEGIN
    -- Drop publication
    DROP PUBLICATION IF EXISTS scribe_pub;
    RAISE NOTICE 'Dropped publication: scribe_pub';

    -- Drop replication slot
    BEGIN
        PERFORM pg_drop_replication_slot('scribe_slot');
        RAISE NOTICE 'Dropped replication slot: scribe_slot';
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'Could not drop replication slot (may be in use)';
    END;
END;
$$ LANGUAGE plpgsql;

-- Usage instructions
COMMENT ON FUNCTION scribe_create_publication(TEXT[]) IS
    'Create publication for specified tables. Usage: SELECT scribe_create_publication(ARRAY[''users'', ''orders''])';
COMMENT ON FUNCTION scribe_set_replica_identity_full(TEXT[]) IS
    'Set REPLICA IDENTITY FULL for tables. Usage: SELECT scribe_set_replica_identity_full(ARRAY[''users'', ''orders''])';
COMMENT ON FUNCTION scribe_check_slot_health() IS
    'Check health of the scribe replication slot';
COMMENT ON FUNCTION scribe_cleanup_logical() IS
    'Remove scribe logical replication infrastructure';
