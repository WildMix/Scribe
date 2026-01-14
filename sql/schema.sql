-- Scribe SQLite Schema
-- Version: 1

-- Object storage (content-addressable)
CREATE TABLE IF NOT EXISTS objects (
    hash TEXT PRIMARY KEY,
    type TEXT NOT NULL CHECK(type IN ('blob', 'tree', 'commit')),
    content BLOB NOT NULL,
    size INTEGER NOT NULL,
    created_at TEXT DEFAULT (datetime('now'))
);

-- Commit metadata for fast queries
CREATE TABLE IF NOT EXISTS commits (
    hash TEXT PRIMARY KEY,
    parent_hash TEXT,
    tree_hash TEXT NOT NULL,
    author_id TEXT NOT NULL,
    author_role TEXT,
    author_email TEXT,
    process_name TEXT NOT NULL,
    process_version TEXT,
    process_params TEXT,
    process_source TEXT,
    message TEXT,
    timestamp INTEGER NOT NULL,
    created_at TEXT DEFAULT (datetime('now'))
);

-- Index for parent chain traversal
CREATE INDEX IF NOT EXISTS idx_commits_parent ON commits(parent_hash);

-- Index for author queries
CREATE INDEX IF NOT EXISTS idx_commits_author ON commits(author_id);

-- Index for process queries
CREATE INDEX IF NOT EXISTS idx_commits_process ON commits(process_name);

-- Index for timestamp queries
CREATE INDEX IF NOT EXISTS idx_commits_timestamp ON commits(timestamp);

-- Changes within each commit
CREATE TABLE IF NOT EXISTS changes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    commit_hash TEXT NOT NULL,
    table_name TEXT NOT NULL,
    operation TEXT NOT NULL CHECK(operation IN ('INSERT', 'UPDATE', 'DELETE')),
    primary_key TEXT NOT NULL,
    before_hash TEXT,
    after_hash TEXT,
    created_at TEXT DEFAULT (datetime('now')),
    FOREIGN KEY (commit_hash) REFERENCES commits(hash)
);

CREATE INDEX IF NOT EXISTS idx_changes_commit ON changes(commit_hash);
CREATE INDEX IF NOT EXISTS idx_changes_table ON changes(table_name);

-- Repository references (HEAD, branches)
CREATE TABLE IF NOT EXISTS refs (
    name TEXT PRIMARY KEY,
    hash TEXT NOT NULL,
    updated_at TEXT DEFAULT (datetime('now'))
);

-- Initialize HEAD reference
INSERT OR IGNORE INTO refs (name, hash) VALUES ('HEAD', '');

-- Configuration key-value store
CREATE TABLE IF NOT EXISTS config (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

-- Schema version tracking
INSERT OR IGNORE INTO config (key, value) VALUES ('schema_version', '1');
