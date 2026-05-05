#ifndef TTL_H
#define TTL_H

#include "../include/ttl.h"
#include <stddef.h>
#include <sys/queue.h>
#include <time.h>

// This list head is static! No other file can see or touch it.
static TAILQ_HEAD(TtlList, ht_entry) hidden_ttl_head;

void ttl_init() { TAILQ_INIT(&hidden_ttl_head); }
long long get_now_ms(void) {
  struct timespec ts;
  // CLOCK_MONOTONIC is critical! It ensures the timer never jumps
  // backwards even if the Linux system clock syncs with the internet.
  clock_gettime(CLOCK_MONOTONIC, &ts);

  // Convert seconds to milliseconds, and nanoseconds to milliseconds
  return (long long)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
}

void ttl_add(ht_entry *entry, long long ttl_ms) {
  entry->expiry = get_now_ms() + ttl_ms;

  // Scan and insert in sorted order
  ht_entry *current;
  TAILQ_FOREACH(current, &hidden_ttl_head, ttl_link) {
    if (current->expiry > entry->expiry) {
      TAILQ_INSERT_BEFORE(current, entry, ttl_link);
      return;
    }
  }
  TAILQ_INSERT_TAIL(&hidden_ttl_head, entry, ttl_link);
}

void ttl_remove(ht_entry *entry) {
  // Because it is doubly-linked, this is an instant O(1) operation
  TAILQ_REMOVE(&hidden_ttl_head, entry, ttl_link);
}

int ttl_get_next_timeout() {
  ht_entry *first = TAILQ_FIRST(&hidden_ttl_head);
  if (!first)
    return -1; // Infinite timeout

  int timeout = (int)(first->expiry - get_now_ms());
  return (timeout < 0) ? 0 : timeout;
}

void ttl_process_expirations() {
  long long now = get_now_ms();

  // 1. Get the very first node
  ht_entry *current = TAILQ_FIRST(&hidden_ttl_head);

  while (current != NULL) {
    // 2. CRITICAL: Save the next pointer BEFORE we do anything!
    ht_entry *next_node = TAILQ_NEXT(current, ttl_link);

    if (current->expiry <= now) {
      // It expired! We can safely delete it.
      engine_delete_node(current);
    } else {
      // It hasn't expired yet. Since the list is sorted, stop looking.
      break;
    }

    // 3. Move to the next node using our safely saved pointer
    current = next_node;
  }
}

#endif
