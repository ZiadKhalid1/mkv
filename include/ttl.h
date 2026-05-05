#include "ht.h"
#include "database.h"



// Initialize the TTL subsystem
void ttl_init(void);

// Add a node and sort it by expiration time
void ttl_add(node *node, long long ttl_ms);

// Instantly remove a node (if deleted manually or by LRU)
void ttl_remove(node *node);

// Get the milliseconds until the next expiration (for epoll)
int ttl_get_next_timeout(void);

// Find and destroy all expired nodes
void ttl_process_expirations(void);

