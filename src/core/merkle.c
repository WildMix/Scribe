/*
 * Scribe - A protocol for Verifiable Data Lineage
 * core/merkle.c - Merkle tree implementation
 */

#include "scribe/core/merkle.h"
#include "scribe/core/hash.h"
#include "scribe/error.h"

#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 16

static scribe_merkle_node_t *create_leaf_node(const char *field_name,
                                               const void *data,
                                               size_t data_size)
{
    scribe_merkle_node_t *node = calloc(1, sizeof(scribe_merkle_node_t));
    if (!node) return NULL;

    node->is_leaf = 1;
    node->field_name = field_name ? strdup(field_name) : NULL;

    if (data && data_size > 0) {
        node->data = malloc(data_size);
        if (!node->data) {
            free(node->field_name);
            free(node);
            return NULL;
        }
        memcpy(node->data, data, data_size);
        node->data_size = data_size;
    }

    /* Compute leaf hash: SHA256(0x00 || data) */
    scribe_hash_leaf(data, data_size, &node->hash);

    return node;
}

static void free_node(scribe_merkle_node_t *node)
{
    if (!node) return;
    free_node(node->left);
    free_node(node->right);
    free(node->field_name);
    free(node->data);
    free(node);
}

scribe_merkle_tree_t *scribe_merkle_tree_new(void)
{
    scribe_merkle_tree_t *tree = calloc(1, sizeof(scribe_merkle_tree_t));
    if (!tree) return NULL;

    tree->leaves = calloc(INITIAL_CAPACITY, sizeof(scribe_merkle_node_t *));
    if (!tree->leaves) {
        free(tree);
        return NULL;
    }

    tree->leaf_capacity = INITIAL_CAPACITY;
    return tree;
}

void scribe_merkle_tree_free(scribe_merkle_tree_t *tree)
{
    if (!tree) return;

    /* Free the root (which includes all internal nodes) */
    if (tree->root) {
        free_node(tree->root);
    } else {
        /* If not built, free leaves directly */
        for (size_t i = 0; i < tree->leaf_count; i++) {
            free_node(tree->leaves[i]);
        }
    }

    free(tree->leaves);
    free(tree);
}

scribe_error_t scribe_merkle_add_field(scribe_merkle_tree_t *tree,
                                       const char *field_name,
                                       const void *data,
                                       size_t data_size)
{
    if (!tree) return SCRIBE_ERR_INVALID_ARG;
    if (tree->built) return SCRIBE_ERR_INVALID_ARG;  /* Can't add after build */

    /* Expand if needed */
    if (tree->leaf_count >= tree->leaf_capacity) {
        size_t new_cap = tree->leaf_capacity * 2;
        scribe_merkle_node_t **new_leaves = realloc(tree->leaves,
                                                     new_cap * sizeof(scribe_merkle_node_t *));
        if (!new_leaves) return SCRIBE_ERR_NOMEM;
        tree->leaves = new_leaves;
        tree->leaf_capacity = new_cap;
    }

    scribe_merkle_node_t *node = create_leaf_node(field_name, data, data_size);
    if (!node) return SCRIBE_ERR_NOMEM;

    tree->leaves[tree->leaf_count++] = node;
    return SCRIBE_OK;
}

scribe_error_t scribe_merkle_add_hash(scribe_merkle_tree_t *tree,
                                      const char *field_name,
                                      const scribe_hash_t *hash)
{
    if (!tree || !hash) return SCRIBE_ERR_INVALID_ARG;
    if (tree->built) return SCRIBE_ERR_INVALID_ARG;

    /* Expand if needed */
    if (tree->leaf_count >= tree->leaf_capacity) {
        size_t new_cap = tree->leaf_capacity * 2;
        scribe_merkle_node_t **new_leaves = realloc(tree->leaves,
                                                     new_cap * sizeof(scribe_merkle_node_t *));
        if (!new_leaves) return SCRIBE_ERR_NOMEM;
        tree->leaves = new_leaves;
        tree->leaf_capacity = new_cap;
    }

    scribe_merkle_node_t *node = calloc(1, sizeof(scribe_merkle_node_t));
    if (!node) return SCRIBE_ERR_NOMEM;

    node->is_leaf = 1;
    node->field_name = field_name ? strdup(field_name) : NULL;
    scribe_hash_copy(&node->hash, hash);

    tree->leaves[tree->leaf_count++] = node;
    return SCRIBE_OK;
}

scribe_error_t scribe_merkle_build(scribe_merkle_tree_t *tree)
{
    if (!tree) return SCRIBE_ERR_INVALID_ARG;
    if (tree->built) return SCRIBE_OK;  /* Already built */

    if (tree->leaf_count == 0) {
        /* Empty tree has zero hash as root */
        tree->root = calloc(1, sizeof(scribe_merkle_node_t));
        if (!tree->root) return SCRIBE_ERR_NOMEM;
        tree->built = 1;
        return SCRIBE_OK;
    }

    if (tree->leaf_count == 1) {
        /* Single leaf is the root */
        tree->root = tree->leaves[0];
        tree->built = 1;
        return SCRIBE_OK;
    }

    /* Build tree bottom-up */
    size_t level_size = tree->leaf_count;
    scribe_merkle_node_t **current_level = malloc(level_size * sizeof(scribe_merkle_node_t *));
    if (!current_level) return SCRIBE_ERR_NOMEM;

    /* Copy leaves to current level */
    memcpy(current_level, tree->leaves, level_size * sizeof(scribe_merkle_node_t *));

    while (level_size > 1) {
        size_t next_size = (level_size + 1) / 2;
        scribe_merkle_node_t **next_level = malloc(next_size * sizeof(scribe_merkle_node_t *));
        if (!next_level) {
            free(current_level);
            return SCRIBE_ERR_NOMEM;
        }

        for (size_t i = 0; i < next_size; i++) {
            size_t left_idx = i * 2;
            size_t right_idx = i * 2 + 1;

            if (right_idx >= level_size) {
                /* Odd node: duplicate the last one */
                right_idx = left_idx;
            }

            scribe_merkle_node_t *parent = calloc(1, sizeof(scribe_merkle_node_t));
            if (!parent) {
                free(next_level);
                free(current_level);
                return SCRIBE_ERR_NOMEM;
            }

            parent->is_leaf = 0;
            parent->left = current_level[left_idx];
            parent->right = (left_idx == right_idx) ? NULL : current_level[right_idx];

            /* Compute parent hash: SHA256(0x01 || left || right) */
            if (parent->right) {
                scribe_hash_combine(&parent->left->hash, &parent->right->hash, &parent->hash);
            } else {
                /* Single child: hash is just the child hash combined with itself */
                scribe_hash_combine(&parent->left->hash, &parent->left->hash, &parent->hash);
            }

            next_level[i] = parent;
        }

        free(current_level);
        current_level = next_level;
        level_size = next_size;
    }

    tree->root = current_level[0];
    free(current_level);
    tree->built = 1;

    return SCRIBE_OK;
}

const scribe_hash_t *scribe_merkle_root(const scribe_merkle_tree_t *tree)
{
    if (!tree || !tree->root) return NULL;
    return &tree->root->hash;
}

size_t scribe_merkle_leaf_count(const scribe_merkle_tree_t *tree)
{
    return tree ? tree->leaf_count : 0;
}

scribe_error_t scribe_merkle_verify(const scribe_merkle_tree_t *tree)
{
    if (!tree) return SCRIBE_ERR_INVALID_ARG;
    if (!tree->built) return SCRIBE_ERR_INVALID_ARG;

    /* For now, just verify the root exists */
    /* Full verification would recompute all hashes */
    if (!tree->root) return SCRIBE_ERR_HASH_MISMATCH;

    return SCRIBE_OK;
}

scribe_merkle_proof_t *scribe_merkle_proof_create(const scribe_merkle_tree_t *tree,
                                                   size_t leaf_index)
{
    if (!tree || !tree->built || leaf_index >= tree->leaf_count) return NULL;

    /* Calculate tree depth */
    size_t depth = 0;
    size_t n = tree->leaf_count;
    while (n > 1) {
        n = (n + 1) / 2;
        depth++;
    }

    scribe_merkle_proof_t *proof = calloc(1, sizeof(scribe_merkle_proof_t));
    if (!proof) return NULL;

    proof->depth = depth;
    proof->hashes = calloc(depth, sizeof(scribe_hash_t));
    proof->positions = calloc(depth, sizeof(int));

    if (!proof->hashes || !proof->positions) {
        free(proof->hashes);
        free(proof->positions);
        free(proof);
        return NULL;
    }

    /* Build proof by walking from leaf to root */
    /* This is a simplified implementation */
    /* Full implementation would traverse the built tree */

    return proof;
}

void scribe_merkle_proof_free(scribe_merkle_proof_t *proof)
{
    if (!proof) return;
    free(proof->hashes);
    free(proof->positions);
    free(proof);
}

int scribe_merkle_proof_verify(const scribe_merkle_proof_t *proof,
                               const scribe_hash_t *leaf_hash,
                               const scribe_hash_t *root_hash)
{
    if (!proof || !leaf_hash || !root_hash) return 0;

    scribe_hash_t current;
    scribe_hash_copy(&current, leaf_hash);

    for (size_t i = 0; i < proof->depth; i++) {
        scribe_hash_t combined;
        if (proof->positions[i] == 0) {
            /* Sibling is on the right */
            scribe_hash_combine(&current, &proof->hashes[i], &combined);
        } else {
            /* Sibling is on the left */
            scribe_hash_combine(&proof->hashes[i], &current, &combined);
        }
        scribe_hash_copy(&current, &combined);
    }

    return scribe_hash_equal(&current, root_hash);
}
