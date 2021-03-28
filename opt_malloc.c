#include <stdio.h>
#include <sys/mman.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>

#include "xmalloc.h"

// Used whether free or in use
// if in use, next and prev of surrounding entries will skip one in use
typedef struct chunk {
	size_t size;
	//struct chunk* prev;
	struct chunk* next;
} chunk;

long NUM_BUCKETS = 10;
static chunk* buckets[10];	// initially all NULL
size_t bucket_sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
const size_t PAGE_SIZE = 4096;
const size_t MIN_ALLOCATION = 8;

long
bucket(size_t size)
{
	long bucket = 0;
	size_t minSize = MIN_ALLOCATION;

	while (minSize < size) {
		bucket += 1;
		minSize *= 2;
	}

	return bucket;
}

static
size_t
div_up(size_t xx, size_t yy)
{
	size_t zz = xx / yy;

	if (zz * yy == xx)
		return zz;
	else
		return zz + 1;

}

long
bucket_add(void* addr, long b_idx)
{
	chunk* toAdd = (chunk*)(addr);
	chunk* tmp = buckets[b_idx];
	chunk* tmp_prev = NULL;
	long insIdx = 0;

	if (tmp && (uintptr_t)tmp < (uintptr_t)addr) {

		while (tmp && (uintptr_t)tmp < (uintptr_t)addr) {
			tmp_prev = tmp;
			tmp = tmp->next;
			insIdx += 1;
		}

		toAdd->size = bucket_sizes[b_idx];
		toAdd->next = tmp;
		//tmp->prev = toAdd;
		if (tmp_prev) {
		//	toAdd->prev = tmp_prev;
			tmp_prev->next = toAdd;
		}
	}
	else {
		toAdd->size = bucket_sizes[b_idx];
		toAdd->next = buckets[b_idx];//(buckets + b_idx);
		//toAdd->prev = NULL;
		//(buckets + b_idx)->prev = toAdd;
	}

	buckets[b_idx] = toAdd;

	return insIdx;
}

void
bucket_coalesce()
{
	char del = 0;

	do {
		
		chunk* tmp = buckets[b_idx];
		chunk* tmp_prev = NULL;
		del = 0;

		while (tmp) {

			if (tmp_prev && (void*)(tmp_prev) + tmp_prev->size == (void*)(tmp)) {
				tmp_prev->size = tmp_prev->size + tmp->size;
				tmp_prev->next = tmp_prev->next->next;
				del = 1;
			}

			tmp_prev = tmp;
			tmp = tmp->next;
		}

	} while (del == 1);

}

void
bucket_delete(long b_idx, long idx)
{
	chunk* tmp = buckets[b_idx];

	if (idx == 0) {
		buckets[b_idx] = tmp->next;
		return;
	}

	while ((idx--) - 1)
		tmp = tmp->next;

	tmp->next = tmp->next->next;
}

void*
xmalloc(size_t bytes)
{
	bytes += sizeof(chunk);

	void* ptr = NULL;
	long b_idx = bucket(bytes);

	if (bytes <= PAGE_SIZE) {
		pthread_mutex_lock(&mutex);
		chunk* tmp = buckets[b_idx];

		if (tmp) {
			ptr = (void*)(tmp); // header filled out when added to list
			bucket_delete(b_idx, 0);
		}
		// No entry of that size in list, so look for available larger chunks to split
		else {

			// get b_idx to first list with available chunks
			long off = 0
			while (!buckets[b_idx + off]) {
				
				if (b_idx + off == NUM_BUCKETS - 1) {
					void* addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
					bucket_add(addr, PAGE_SIZE);
					break;
				}

				off += 1;
			}

			// Let memory flow down buckets
			ptr = buckets[b_idx + off];
			bucket_delete(b_idx + off, 0);

			do {
				void* split = (uintptr_t)ptr + bucket_sizes[b_idx + off];
				bucket_add(split, b_idx + off);				
			} while (off--);

			pthread_mutex_unlock(&mutex);
			return xmalloc(bytes - sizeof(chunk));
		}

		pthread_mutex_unlock(&mutex);	
	}
	else {
		size_t pages = div_up(bytes, PAGE_SIZE);

		ptr = mmap(NULL, pages * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (ptr == (void*)(-1))
			perror("xmalloc: mmap() failed");

		chunk* cPtr = (chunk*)(ptr);
		cPtr->size = bytes;
		cPtr->next = NULL;
		//cPtr->prev = NULL;
	}

	return ptr + sizeof(chunk);
}

void
xfree(void* ptr)
{
	chunk* cPtr = (chunk*)(ptr - sizeof(chunk));
	size_t size = cPtr->size;
	
	if (size <= PAGE_SIZE) {
		long b_idx = bucket(cPtr->size);

		pthread_mutex_lock(&mutex);
		bucket_add((void*)cPtr, b_idx);
		bucket_coalesce(b_idx);
		pthread_mutex_unlock(&mutex);
	}
	else {
		int err = munmap(cPtr, size);
		if (err == -1)
			perror("xfree: munmap() failed");
	}

}

void*
xrealloc(void* prev, size_t bytes)
{
	bytes += sizeof(chunk);

	chunk* cPtr = (chunk*)(prev - sizeof(chunk));
	void* ptr = NULL;

	if (bytes <= PAGE_SIZE) {
		long b_idx_old = bucket(cPtr->size);
		long b_idx_new = bucket(bytes);
	
		if (b_idx_new == b_idx_old)
			ptr = prev;
		else {
			pthread_mutex_lock(&mutex);
			ptr = buckets[b_idx_new];
			bucket_delete(b_idx_new, 0);
			memcpy(ptr, (void*)cPtr, bucket_sizes[b_idx_new]);
			pthread_mutex_unlock(&mutex);
			xfree(prev);
		}
	}
	else {
		long pages = div_up(bytes, PAGE_SIZE);

		ptr = mmap(NULL, pages * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (ptr == (void*)(-1))
			perror("xreallox: mmap() failed");

		memcpy(ptr, (void*)cPtr, pages * PAGE_SIZE);
		xfree(prev);
	}

	return ptr + sizeof(chunk);
}

