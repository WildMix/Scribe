# Scribe

**A protocol for Verifiable Data Lineage.**

Scribe brings Git-like version control to your data pipelines. It’s an open-source engine that tracks who changed a record, what process they used, and where that data came from.

---

## The Manifesto

Modern infrastructure is a mess. We have databases, microservices, ETL jobs, and manual admin scripts all writing to the same tables. When a metric looks wrong, we end up asking:

* "Why is this value NULL?"
* "Did the batch job run before or after the manual fix?"
* "Who authorized this change?"

Right now, solving this involves digging through bash history, Slack messages, and git logs to piece together a timeline.

**Scribe is built on a single belief:**

> Accountability isn't optional. Every change to a dataset needs a signed "Author" and a "Process."

---

## How It Works

Scribe doesn't replace your database; it sits beside it. We use a **Merkle-DAG** (Directed Acyclic Graph) to chain changes together, making the history of a row unbreakable and traversable.

### 1. The Envelope

Every time data changes, Scribe records a "Commit" (or Envelope). This isn't just a log line—it's a structured object containing the context of the process.

```json
{
  "commit_id": "sha256:7f9a...",
  "parent_id": "sha256:3b1c...",  // Links to the previous state
  "author": {
    "id": "user:alice",           // The human or service account
    "role": "data_engineer"
  },
  "process": {
    "name": "monthly_reconciliation.py", // The script name
    "version": "git:v2.1.0",             // The exact code version
    "params": "--force-update"           // The flags used
  }
}

```

### 2. The Merkle Tree

Inside the Envelope, Scribe stores a Merkle Root of the data. This lets you prove exactly which fields changed without having to duplicate the entire dataset.

---

## Use Cases

Scribe is for engineers who need to trace data changes or map out their system flows.

### Automated Accountability

A junior dev manually updates a production DB to "quick fix" a bug. Scribe records the action, linking that specific database state to their user ID. No more mystery updates.

### Mapping Legacy Systems

You inherit a 10-year-old database. Scribe automatically builds a visual map of "what writes where" as changes happen, helping you reverse-engineer the data flow.

---

## Project Goals

* **Platform Agnostic:** Works with SQL, NoSQL, or in-memory objects.
* **Low Overhead:** Hashing is fast and runs in parallel. We don't slow down the write path.

---

## Getting Involved

Scribe is currently in the **Architectural Phase**. We are looking for builders interested in:

* **Systems Design:** Efficiently storing millions of DAG nodes.
* **DevEx:** Making "committing data" as distinct and easy as `git commit`.
* **Visualization:** Rendering complex history graphs in the browser.
