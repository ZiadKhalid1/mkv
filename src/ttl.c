#include "../include/ttl.h"

#include <stddef.h>
#include <sys/queue.h>
#include <time.h>

// This list head is static! No other file can see or touch it.
static TAILQ_HEAD(TtlList, node) hidden_ttl_head;

void ttl_init(void) { TAILQ_INIT(&hidden_ttl_head); }

long long get_now_ms(void) {
  struct timespec ts;
  // CLOCK_MONOTONIC is critical! It ensures the timer never jumps
  // backwards even if the Linux system clock syncs with the internet.
  clock_gettime(CLOCK_MONOTONIC, &ts);

  // Convert seconds to milliseconds, and nanoseconds to milliseconds
  return (long long)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
}

void ttl_add(node *entry, long long ttl_ms) {
  // If the node already had a TTL, remove it from the list before re-adding it
  // This prevents corrupting the TAILQ if we call EXPIRE twice on the same key.
  ttl_remove(entry);

  entry->ttl = get_now_ms() + ttl_ms;

  // Scan and insert in sorted order
  node *current;
  TAILQ_FOREACH(current, &hidden_ttl_head, ttl_link) {
    if (current->ttl > entry->ttl) {
      TAILQ_INSERT_BEFORE(current, entry, ttl_link);
      return;
    }
  }
  TAILQ_INSERT_TAIL(&hidden_ttl_head, entry, ttl_link);
}

void ttl_remove(node *entry) {
  // Only remove if it's actually in the list.
  // TAILQ_ENTRY uses internal pointers; if they are NULL, it's not in the list.
  if (entry->ttl_link.tqe_next != NULL || entry->ttl_link.tqe_prev != NULL) {
    TAILQ_REMOVE(&hidden_ttl_head, entry, ttl_link);
    // Zero the links so we don't try to remove it again.
    entry->ttl_link.tqe_next = NULL;
    entry->ttl_link.tqe_prev = NULL;
  }
  entry->ttl = 0;
}

int ttl_get_next_timeout(void) {
  node *first = TAILQ_FIRST(&hidden_ttl_head);
  if (!first)
    return -1; // Infinite timeout

  int timeout = (int)(first->ttl - get_now_ms());
  return (timeout < 0) ? 0 : timeout;
}

// CRITICAL UPDATE: We must pass the hash table so we can call delete_n properly
void ttl_process_expirations(ht *table) {
  long long now = get_now_ms();

  // 1. Get the very first node
  node *current = TAILQ_FIRST(&hidden_ttl_head);

  while (current != NULL) {
    // 2. CRITICAL: Save the next pointer BEFORE we do anything!
    node *next_node = TAILQ_NEXT(current, ttl_link);

    if (current->ttl <= now) {
      // It expired! We safely delete it using our Master Manager function.
      delete_n(table, current->entry.key);
    } else {
      // It hasn't expired yet. Since the list is sorted, stop looking.
      break;
    }

    // 3. Move to the next node using our safely saved pointer
    current = next_node;
  }
}
