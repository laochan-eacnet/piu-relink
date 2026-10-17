#pragma once

#include <stdint.h>

struct phoenix_resource_record {
    uint32_t word[8];
};

struct phoenix_resource_node {
    uint32_t unused0;
    uint32_t unused1;
    uint32_t unused2;
    struct phoenix_resource_record *records;
};

struct phoenix_resource_block {
    struct phoenix_resource_node node;
    struct phoenix_resource_record record;
    char path[];
};

struct phoenix_resource_handle {
    struct phoenix_resource_node *node;
    uint32_t index;
    void *key_and_owner;
};

_Static_assert(sizeof(struct phoenix_resource_record) == 32,
               "Phoenix resource record ABI changed");
_Static_assert(sizeof(struct phoenix_resource_node) == 16,
               "Phoenix resource node ABI changed");
_Static_assert(sizeof(struct phoenix_resource_handle) == 12,
               "Phoenix resource handle ABI changed");

