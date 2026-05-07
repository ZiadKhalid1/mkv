#include "../include/ttl.h"
#include "../include/LRU_linked_list.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
LRU_list* LRU;

ht *db_init() {
  ttl_init();
  // inialize lru linkedlist
  LRU =  LRU_linked_list_create();
  //

  return ht_create();
}

void db_destroy(ht *table) {
  // 1. Create an iterator to loop through the entire hash table
  hti it = ht_iterator(table);

  // 2. Loop through every single active item
  while (ht_next(&it)) {
    // Grab the node pointer from the value field
    node *n = (node *)it.value;

    // 3. Safely free all the memory owned by the node
    if (n->is_file) {
      remove((char *)
                 n->entry.value); // Delete file from disk if needed on shutdown
    }
    free(n->entry.value);       // Free the value string / filepath
    free((void *)n->entry.key); // Free the key string
    free(n);                    // Free the node struct itself
  }

  // 4. Now that the table is empty, it is safe to destroy the container!
  ht_destroy(table);

    // destroy linkedlist
  LRU_linked_list_deleteLRU(LRU);
}

const char *set_n(ht *table, const char *key, void *value, bool is_file) {
  // 1. If the key already exists, we should delete the old node first!
  if (ht_get(table, key) != NULL) {
    delete_n(table, key);
  }

  // 2. CREATE THE NODE: Allocate memory for it on the Heap
  node *new_node = calloc(1, sizeof(node));

  // Fill in the data based on your struct
  new_node->entry.key = strdup(key); // We own the key string
  new_node->entry.value = value;     // We own the value
  new_node->is_file = is_file;
  new_node->ttl = 0; // Default to 0 (no expiration)

  // 3. STORE IT: Hand the pointer to the Hash Table
  // Notice we pass `new_node` as the third parameter!
  ht_set(table, new_node->entry.key, new_node);

  // 4. STORE IT: Put it at the front of the LRU List (Most Recently Used)
  new_node->LRU_node = LRU_linked_list_createNode(LRU,new_node);

  return new_node->entry.key;
}

void *get_v(ht *table, const char *key) {
  // 1. Fetch the node from the hash table
  node *n = (node *)ht_get(table, key);

  // If it doesn't exist, return NULL
  if (n == NULL) {
    return NULL;
  }

  // 2. Change place in LRU Linked List!
  LRU_linked_list_deleteNode(n->LRU_node); // remove
  n->LRU_node = LRU_linked_list_createNode(LRU,n); // add at front

  // 3. Return the actual string/file path to the user
  return n->entry.value;
}

bool set_ttl(ht *table, const char *key, uint32_t ttl_ms) {
  // Fetch the whole node
  node *n = (node *)ht_get(table, key);

  if (n == NULL) {
    return false; // Key doesn't exist!
  }


  // Send it to your TTL subsystem (which you defined in ttl.h)
  ttl_add(n, ttl_ms);

  return true;
}

bool is_file(ht *table, const char *key) {
  node *n = (node *)ht_get(table, key);
  if (n == NULL)
    return false;

  return n->is_file;
}

bool delete_n(ht *table, const char *key) {
  // 1. Find the node in the Hash Table
  node *n = (node *)ht_get(table, key);

  if (n == NULL) {
    return false; // Key doesn't exist, nothing to delete
  }

  // 2. Unhook it from the Hash Table
  ht_delete(table, key);

  // 3. Unhook it from the Linked Lists!
  LRU_linked_list_deleteNode(n->LRU_node);
  //
  //

  if (n->ttl > 0) {
    ttl_remove(n);
  }

  // 4. Clean up the hard drive (If it was a file)
  if (n->is_file) {
    // n->entry.value holds the filepath string (e.g., "store/files/abc.bin")
    // We must delete the physical file from the hard drive!
    remove((char *)n->entry.value);
  }

  // 5. FREE THE RAM (This is where the server frees the value!)
  free(n->entry.value);       // Frees the string text OR the filepath text
  free((void *)n->entry.key); // Frees the key string we duplicated earlier
  free(n);                    // Frees the node struct itself

  return true;
}
