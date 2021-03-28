#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

#include "xmalloc.h"

typedef struct block {
	size_t size;
	struct block* next;
} block;

block* fHEAD = NULL;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
const size_t PAGE_SIZE = 4096;

static
size_t
div_up(size_t xx, size_t yy)
{
    // This is useful to calculate # of pages
    // for large allocations.
    size_t zz = xx / yy;

    if (zz * yy == xx) {
        return zz;
    }
    else {
        return zz + 1;
    }
}

long
free_list_add(void* addr, size_t size)
{
	block* toAdd = (block*)(addr);
	block* tmp = fHEAD;
	block* tmp_prev = NULL;
	long insIdx = 0;
	
	// put new cell where prev entry has lower addr and next entry has greater addr
	// addr won't be equal because mmap will create new blocks in unused parts of memory
	if (tmp && (uintptr_t)tmp < (uintptr_t)addr) {

		while (tmp && (uintptr_t)tmp < (uintptr_t)addr) {
			tmp_prev = tmp;
			tmp = tmp->next;
			insIdx += 1;
		}
	
		toAdd->size = size;
		toAdd->next = tmp;
		if (tmp_prev)
			tmp_prev->next = toAdd;
	}
	else {
		toAdd->size = size;
		toAdd->next = fHEAD;
		fHEAD = toAdd;
	}
	
	return insIdx;
}

long
free_list_coalesce()
{
	long n = 0;
	char del = 0;

	do {

		block* tmp = fHEAD;
		block* tmp_prev = NULL;
		del = 0;

		while (tmp) {

			if (tmp_prev && (void*)(tmp_prev) + tmp_prev->size == (void*)(tmp)) {
				tmp_prev->size = tmp_prev->size + tmp->size;	
				tmp_prev->next = tmp_prev->next->next;
				n += 1;
				del = 1;
			}

			tmp_prev = tmp;
			tmp = tmp->next;
		}

	} while (del == 1);

	return n;
}

void
free_list_delete(long idx)
{
	long prev_idx = 0;
	block* tmp = fHEAD;

	if (idx == 0) {
		fHEAD = tmp->next;
		return;
	}

	while (prev_idx < idx - 1) {
		tmp = tmp->next;
		prev_idx += 1;
	}

	tmp->next = tmp->next->next;
}

void*
xmalloc(size_t size)
{
	void* ptr = NULL;

    size += sizeof(size_t);

	if (size < PAGE_SIZE) {

		pthread_mutex_lock(&mutex);
		block* tmp = fHEAD;
		long idx = 0;
		
		// search free list for block
		while (tmp) {

			// give whole free entry
			if (tmp->size - size < sizeof(block) && tmp->size >= size) {
				ptr = (void*)tmp;	
				*((size_t*)(ptr)) = size;
				//printf("Full : Thread %ld taking %p\n\n", pthread_self(), ptr);

				free_list_delete(idx);

				break;
			} // end if
			// split free entry
			else if (tmp->size - size >= sizeof(block) && tmp->size >= size) {
				ptr = (void*)tmp;

				void* new_addr = ptr + size;
				long insIdx = free_list_add(new_addr, tmp->size - size);

				*((size_t*)(ptr)) = size;
				//printf("Split: Thread %ld taking %p\n\n", pthread_self(), ptr);

				// split
				free_list_delete(insIdx - 1);
				free_list_coalesce();

				break;
			} // end else if

			tmp = tmp->next;
			idx += 1;
		} // end while
	
		// if not found
		if (!ptr) {

			ptr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
			if (ptr == (void*)(-1))
				perror("xmalloc: mmap() failed");
			//printf("mmap : Thread %ld taking %p\n", pthread_self(), ptr);

			free_list_add(ptr, PAGE_SIZE);
			free_list_coalesce();

			pthread_mutex_unlock(&mutex);
			return xmalloc(size - sizeof(size_t));
		}
		
	} // end if
	else {
		size_t pages = div_up(size, PAGE_SIZE);

		ptr = mmap(NULL, pages * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (ptr == (void*)(-1))
			perror("xmalloc: mmap() failed");

		*((size_t*)(ptr)) = pages * PAGE_SIZE;
	} // end else
	
	pthread_mutex_unlock(&mutex);
	return ptr + sizeof(size_t);
} // end hmalloc

void
xfree(void* item)
{
	size_t size = *((size_t*)(item - sizeof(size_t)));

	if (size < PAGE_SIZE) {
		pthread_mutex_lock(&mutex);
		free_list_add(item - sizeof(size_t), size);
		free_list_coalesce();
		pthread_mutex_unlock(&mutex);
	}
	else {
		int err = munmap(item - sizeof(size_t), size);
		if (err == -1)
			perror("xfree: munmap() failed");

	}

}

void*
xrealloc(void* prev, size_t bytes)
{
	void* ptr = NULL;
	size_t oldBytes = *((size_t*)(prev - sizeof(size_t)));
	size_t newBytes = bytes + sizeof(size_t);
	long idx = 0;

	pthread_mutex_lock(&mutex);
	block* tmp = fHEAD;

	// Search free list to see if we can extend current allocation to avoid memcpy
	while (tmp) {

		if ((uintptr_t)(prev - sizeof(size_t)) + oldBytes == (uintptr_t)(tmp) && newBytes <= oldBytes + tmp->size) {

			if ((oldBytes + tmp->size) - newBytes < sizeof(block)) {
				ptr = prev;
				*((size_t*)(ptr - sizeof(size_t))) = oldBytes + tmp->size;

				free_list_delete(idx);
				
				pthread_mutex_unlock(&mutex);
				return ptr;
			}
			else {
				ptr = prev;
				*((size_t*)(ptr - sizeof(size_t))) = newBytes;

				void* addr = ptr + bytes;
				long insIdx = free_list_add(addr, (oldBytes + tmp->size) - newBytes);

				free_list_delete(insIdx - 1);
				free_list_coalesce();

				pthread_mutex_unlock(&mutex);
				return ptr;
			}
			
		}

		tmp = tmp->next;
		idx += 1;
	}
	pthread_mutex_unlock(&mutex);

	// If program got here, it didn't return
	// So use xmalloc() to find new memspace and copy prev data over, then free old space
	ptr = xmalloc(bytes);
	memcpy(ptr, prev, *((size_t*)(prev - sizeof(size_t))) - sizeof(block));
	xfree(prev);

	return ptr;
}

void
dump_flist()
{
	block* tmp = fHEAD;

	while (tmp) {
		printf("addr: %p ; size: %ld\n", tmp, tmp->size);
		tmp = tmp->next;
	}
}

