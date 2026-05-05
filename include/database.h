


#include "ht.h"
#include <stdint.h>



typedef struct node{
  ht_entry entry;
  bool is_file;
  uint32_t ttl;
  TAILQ_ENTRY(node) lru_link;  // Generates *prev and *next for the LRU list
  TAILQ_ENTRY(node) ttl_link;
}node ;


//db_init to intialize hash table and ttl and lru linkedlist
// the return value is a pointer to hash table
ht* db_init();
void db_destroy(ht* table);

// get_v to get the value by key from table
void* get_v(ht* table, const char* key);

// set_e to set an entry in table 
const char* set_n(ht* table, const char* key, void* value, bool is_file);

//delete_e to delete entry from table
bool delete_n(ht* table, const char* key);


//set_ttl to set time to live for specific entry in table
bool set_ttl(ht* table, const char* key, uint32_t ttl);


// search_t to search in table key's by s_str (Not implemented yet)
char** search_t(ht* table, const char* s_str);





