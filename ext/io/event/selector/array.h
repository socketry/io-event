// Released under the MIT License.
// Copyright, 2023, by Samuel Williams.

// Provides a simple implementation of unique pointers to elements of the given size.

#include <ruby.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

struct IO_Event_Array {
	// The array of pointers to elements:
	void **base;
	
	// The allocated size of the array:
	size_t count;
	
	// The biggest item we've seen so far:
	size_t limit;
	
	// The size of each element that is allocated:
	size_t element_size;
	
	void (*element_initialize)(void*);
	void (*element_free)(void*);
};

inline static void IO_Event_Array_allocate(struct IO_Event_Array *array, size_t count, size_t element_size)
{
	if (count) {
		array->base = (void**)calloc(count, sizeof(void*));
		array->count = count;
	} else {
		array->base = NULL;
		array->count = 0;
	}
	
	array->limit = 0;
	array->element_size = element_size;
}

inline static size_t IO_Event_Array_memory_size(const struct IO_Event_Array *array)
{
	// Upper bound.
	return array->count * (sizeof(void*) + array->element_size);
}

inline static void IO_Event_Array_free(struct IO_Event_Array *array)
{
	for (size_t i = 0; i < array->limit; i += 1) {
		void *element = array->base[i];
		if (element) {
			array->element_free(element);
			free(element);
		}
	}
	
	if (array->base)
		free(array->base);
	
	array->base = NULL;
	array->count = 0;
	array->limit = 0;
}

inline static int IO_Event_Array_resize(struct IO_Event_Array *array, size_t count)
{
	if (count <= array->count) {
		return 0;
	}
	
	// Compute the next multiple (ideally a power of 2):
	size_t new_count = array->count;
	while (new_count < count) {
		new_count *= 2;
	}
	
	void **new_base = (void**)realloc(array->base, new_count * sizeof(void*));
	
	if (new_base == NULL) {
		return -1;
	}
	
	// Zero out the new memory:
	memset(new_base + array->count, 0, (new_count - array->count) * sizeof(void*));
	
	array->base = (void**)new_base;
	array->count = new_count;
	
	return 1;
}

inline static void* IO_Event_Array_lookup(struct IO_Event_Array *array, size_t index)
{
	size_t count = index + 1;
	
	// Resize the array if necessary:
	if (count > array->count) {
		if (IO_Event_Array_resize(array, count) == -1) {
			return NULL;
		}
	}
	
	// Get the element:
	void **element = array->base + index;
	
	// Allocate the element if it doesn't exist:
	if (*element == NULL) {
		*element = malloc(array->element_size);
		
		if (array->element_initialize) {
			array->element_initialize(*element);
		}
		
		// Update the limit:
		if (count > array->limit) array->limit = count;
	}
	
	return *element;
}

// Push a new element onto the end of the array.
inline static void* IO_Event_Array_push(struct IO_Event_Array *array)
{
	return IO_Event_Array_lookup(array, array->limit);
}

inline static void IO_Event_Array_each(struct IO_Event_Array *array, void (*callback)(void*))
{
	for (size_t i = 0; i < array->limit; i += 1) {
		void *element = array->base[i];
		if (element) {
			callback(element);
		}
	}
}
