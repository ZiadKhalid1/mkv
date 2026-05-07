#define struct node node



// LRU_list is for structures
// LRU_linked_list is for functions

typedef struct LRU_list_Node {
    node* data;
    struct LRU_list_Node* next;
    struct LRU_list_Node* prev;
} LRU_list_Node;


typedef struct LRU_list {
LRU_list_Node*  head;
LRU_list_Node*  tail;

} LRU_list;


// the tail and head contain no data
LRU_list* LRU_linked_list_create();


//after head and return pointer to node
LRU_list_Node* LRU_linked_list_createNode(LRU_list* lru,node* data);


//delete a node using its pointer
void LRU_linked_list_deleteNode(LRU_list_Node* d_Node);

// deletes all the LRU_linked_list
void LRU_linked_list_deleteLRU(LRU_list* lru);

