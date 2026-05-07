#include <stdio.h>
#include <stdlib.h>
#include "../include/LRU_linked_list.h"
#include "../include/database.h"


// the tail and head contain no data
LRU_list* LRU_linked_list_create()
{
	
LRU_list* lru = (LRU_list*) malloc(sizeof(LRU_list));
lru->head = (LRU_list_Node*)malloc(sizeof(LRU_list_Node));
lru->tail = (LRU_list_Node*)malloc(sizeof(LRU_list_Node));

lru->head->next = lru->tail;
lru->head->prev = NULL;

lru->tail->next = NULL; 
lru->tail->prev = lru->head;


return lru;
}


//after head and return pointer to node
LRU_list_Node* LRU_linked_list_createNode(LRU_list* lru,node* data)
{
    LRU_list_Node* newNode = (LRU_list_Node*)malloc(sizeof(LRU_list_Node));
    newNode->data = data;
	
    newNode->next = lru->head->next;
	
	newNode->next->prev = newNode;
	
	newNode->prev = lru->head;
	
	lru->head->next = newNode;
	
    return newNode;
}



//delete a node using its pointer
void LRU_linked_list_deleteNode(LRU_list_Node* d_Node)
{
d_Node->prev->next=d_Node->next; 
d_Node->next->prev=d_Node->prev;
free(d_Node);
}


// deletes all the LRU_linked_list
void LRU_linked_list_deleteLRU(LRU_list* lru)
{
LRU_list_Node* car1 = lru->head;
LRU_list_Node* car2 = lru->head->next;
while (car2 != NULL )
{
	free(car1); // free head at first loop
	car1 = car2; 
	car2 = car2->next;
}
free(lru->tail); // free tail 
	
free(lru); // delete the whole LRU list
	
}

