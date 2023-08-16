// Released under the MIT License.
// Copyright, 2023, by Samuel Williams.

#include <stdio.h>
#include <assert.h>

struct IO_Event_List {
	struct IO_Event_List *head, *tail;
	
	// We could consider introducing this to deal with non-node types in the list:
	// void *type;
};

inline static void IO_Event_List_initialize(struct IO_Event_List *list)
{
	list->head = list->tail = list;
}

// Append an item to the end of the list.
inline static void IO_Event_List_append(struct IO_Event_List *list, struct IO_Event_List *node)
{
	assert(node->head == NULL);
	assert(node->tail == NULL);
	
	node->tail = list;
	list->head->tail = node;
	node->head = list->head;
	list->head = node;
}

inline static void IO_Event_List_prepend(struct IO_Event_List *list, struct IO_Event_List *node)
{
	assert(node->head == NULL);
	assert(node->tail == NULL);
	
	node->head = list;
	list->tail->head = node;
	node->tail = list->tail;
	list->tail = node;
}

// Pop an item from the list.
inline static void IO_Event_List_pop(struct IO_Event_List *node)
{
	assert(node->head != NULL);
	assert(node->tail != NULL);
	
	struct IO_Event_List *head = node->head;
	struct IO_Event_List *tail = node->tail;
	
	head->tail = tail;
	tail->head = head;
	node->head = node->tail = NULL;
}

inline static void IO_Event_List_free(struct IO_Event_List *node)
{
	if (node->head != node->tail) {
		IO_Event_List_pop(node);
	}
}

inline static int IO_Event_List_empty(struct IO_Event_List *list)
{
	return list->head == list->tail;
}
