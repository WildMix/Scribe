-- Scribe PostgreSQL Audit Triggers
-- This script sets up trigger-based change data capture

-- Create audit table to store changes
CREATE TABLE IF NOT EXISTS scribe_audit (
    id BIGSERIAL PRIMARY KEY,
    table_name TEXT NOT NULL,
    operation TEXT NOT NULL CHECK (operation IN ('INSERT', 'UPDATE', 'DELETE')),
    row_pk JSONB NOT NULL,
    old_data JSONB,
    new_data JSONB,
    changed_at TIMESTAMPTZ DEFAULT now(),
    transaction_id BIGINT DEFAULT txid_current(),
    processed BOOLEAN DEFAULT FALSE
);

-- Index for efficient polling of unprocessed changes
CREATE INDEX IF NOT EXISTS idx_scribe_audit_unprocessed
ON scribe_audit(processed) WHERE NOT processed;

-- Index for timestamp-based queries
CREATE INDEX IF NOT EXISTS idx_scribe_audit_changed_at
ON scribe_audit(changed_at);

-- Generic audit trigger function
CREATE OR REPLACE FUNCTION scribe_audit_trigger()
RETURNS TRIGGER AS $$
DECLARE
    pk_columns TEXT[];
    pk_values JSONB;
BEGIN
    -- Get primary key columns for this table
    SELECT array_agg(a.attname) INTO pk_columns
    FROM pg_index i
    JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = ANY(i.indkey)
    WHERE i.indrelid = TG_RELID AND i.indisprimary;

    -- Default to 'id' if no primary key found
    IF pk_columns IS NULL THEN
        pk_columns := ARRAY['id'];
    END IF;

    -- Build primary key JSON from the appropriate row
    IF TG_OP = 'DELETE' THEN
        pk_values := to_jsonb(OLD);
    ELSE
        pk_values := to_jsonb(NEW);
    END IF;

    -- Insert audit record
    INSERT INTO scribe_audit (table_name, operation, row_pk, old_data, new_data)
    VALUES (
        TG_TABLE_NAME,
        TG_OP,
        pk_values,
        CASE WHEN TG_OP IN ('UPDATE', 'DELETE') THEN to_jsonb(OLD) END,
        CASE WHEN TG_OP IN ('INSERT', 'UPDATE') THEN to_jsonb(NEW) END
    );

    RETURN COALESCE(NEW, OLD);
END;
$$ LANGUAGE plpgsql;

-- Helper function to add audit trigger to a table
-- Usage: SELECT scribe_watch_table('my_table');
CREATE OR REPLACE FUNCTION scribe_watch_table(target_table TEXT)
RETURNS VOID AS $$
BEGIN
    -- Drop existing trigger if any
    EXECUTE format(
        'DROP TRIGGER IF EXISTS scribe_audit_%I ON %I',
        target_table, target_table
    );

    -- Create new trigger
    EXECUTE format(
        'CREATE TRIGGER scribe_audit_%I
         AFTER INSERT OR UPDATE OR DELETE ON %I
         FOR EACH ROW EXECUTE FUNCTION scribe_audit_trigger()',
        target_table, target_table
    );

    RAISE NOTICE 'Added scribe audit trigger to table: %', target_table;
END;
$$ LANGUAGE plpgsql;

-- Helper function to remove audit trigger from a table
-- Usage: SELECT scribe_unwatch_table('my_table');
CREATE OR REPLACE FUNCTION scribe_unwatch_table(target_table TEXT)
RETURNS VOID AS $$
BEGIN
    EXECUTE format(
        'DROP TRIGGER IF EXISTS scribe_audit_%I ON %I',
        target_table, target_table
    );

    RAISE NOTICE 'Removed scribe audit trigger from table: %', target_table;
END;
$$ LANGUAGE plpgsql;

-- View for recent unprocessed changes
CREATE OR REPLACE VIEW scribe_pending_changes AS
SELECT
    id,
    table_name,
    operation,
    row_pk,
    changed_at,
    transaction_id
FROM scribe_audit
WHERE NOT processed
ORDER BY id;

-- Function to mark changes as processed
-- Usage: SELECT scribe_mark_processed(ARRAY[1, 2, 3]);
CREATE OR REPLACE FUNCTION scribe_mark_processed(change_ids BIGINT[])
RETURNS INTEGER AS $$
DECLARE
    updated_count INTEGER;
BEGIN
    UPDATE scribe_audit
    SET processed = TRUE
    WHERE id = ANY(change_ids);

    GET DIAGNOSTICS updated_count = ROW_COUNT;
    RETURN updated_count;
END;
$$ LANGUAGE plpgsql;

-- Comment the objects
COMMENT ON TABLE scribe_audit IS 'Scribe audit log for change data capture';
COMMENT ON FUNCTION scribe_audit_trigger() IS 'Trigger function for Scribe CDC';
COMMENT ON FUNCTION scribe_watch_table(TEXT) IS 'Add Scribe audit trigger to a table';
COMMENT ON FUNCTION scribe_unwatch_table(TEXT) IS 'Remove Scribe audit trigger from a table';
