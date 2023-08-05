
struct IO_Event_List {
	struct IO_Event_List *head, *tail;
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
	
	node->head = list;
	node->tail = list->tail;
	list->tail = node;
}

// Pop an item from the list.
inline static void IO_Event_List_pop(struct IO_Event_List *node)
{
	node->head->tail = node->tail;
	node->tail->head = node->head;
	node->head = node->tail = NULL;
}
