/*
 * Scribe - A protocol for Verifiable Data Lineage
 * core/merkle.h - Merkle tree operations
 */

#ifndef SCRIBE_CORE_MERKLE_H
#define SCRIBE_CORE_MERKLE_H

#include "scribe/types.h"
#include "scribe/error.h"

/* Merkle tree node */
typedef struct scribe_merkle_node {
    scribe_hash_t hash;
    struct scribe_merkle_node *left;
    struct scribe_merkle_node *right;
    char *field_name;     /* For leaf nodes: field/column name */
    void *data;           /* For leaf nodes: raw data */
    size_t data_size;
    int is_leaf;
} scribe_merkle_node_t;

/* Merkle tree builder */
typedef struct {
    scribe_merkle_node_t **leaves;
    size_t leaf_count;
    size_t leaf_capacity;
    scribe_merkle_node_t *root;
    int built;
} scribe_merkle_tree_t;

/* Tree lifecycle */
scribe_merkle_tree_t *scribe_merkle_tree_new(void);
void scribe_merkle_tree_free(scribe_merkle_tree_t *tree);

/* Building the tree */
scribe_error_t scribe_merkle_add_field(scribe_merkle_tree_t *tree,
                                       const char *field_name,
                                       const void *data,
                                       size_t data_size);

scribe_error_t scribe_merkle_add_hash(scribe_merkle_tree_t *tree,
                                      const char *field_name,
                                      const scribe_hash_t *hash);

scribe_error_t scribe_merkle_build(scribe_merkle_tree_t *tree);

/* Accessing results */
const scribe_hash_t *scribe_merkle_root(const scribe_merkle_tree_t *tree);
size_t scribe_merkle_leaf_count(const scribe_merkle_tree_t *tree);

/* Verification */
scribe_error_t scribe_merkle_verify(const scribe_merkle_tree_t *tree);

/* Merkle proof for inclusion verification */
typedef struct {
    scribe_hash_t *hashes;  /* Sibling hashes from leaf to root */
    int *positions;         /* 0=left, 1=right for each level */
    size_t depth;
} scribe_merkle_proof_t;

scribe_merkle_proof_t *scribe_merkle_proof_create(const scribe_merkle_tree_t *tree,
                                                   size_t leaf_index);
void scribe_merkle_proof_free(scribe_merkle_proof_t *proof);

int scribe_merkle_proof_verify(const scribe_merkle_proof_t *proof,
                               const scribe_hash_t *leaf_hash,
                               const scribe_hash_t *root_hash);

#endif /* SCRIBE_CORE_MERKLE_H */
