#include <stdio.h>
#include <sys/mman.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>

#include "xmalloc.h"

// Used whether free or in use
// if in use, next and prev of surrounding entries will skip one in use
typedef struct bkt_t {
	size_t size;
	void* mem;
	long used_map[8];
	struct bkt_t* next; 
} bkt_t;

typedef struct large_chunk_t {
	size_t size;
} large_chunk_t;

long NUM_BUCKETS = 19;
size_t bucket_sizes[] = {8, 12, 16, 24, 32, 48, 64, 96, 128, 192,
						 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096};

bkt_t* bkt_list = NULL;
void* BKT_MEM = NULL;
size_t BKT_MEM_OFF = 0;
size_t BKT_MEM_LEN = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
const size_t PAGE_SIZE = 4096;

long
bucket(size_t size)
{
	long b_idx = 0;
	size_t minSize = bucket_sizes[b_idx];

	while (minSize < size) {
		if (b_idx < NUM_BUCKETS)
			minSize = bucket_sizes[++b_idx];
	}

	return bucket_sizes[b_idx];
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

/*
Returns bkt_t* if mem is within that bucket
Returns NULL if mem was a large allocation
@params: void* mem
*/
bkt_t*
bucket_memfind(void* ptr)
{
	bkt_t* tmp = bkt_list;

	while (tmp) {
		if ((uintptr_t)tmp->mem <= (uintptr_t)ptr && 
			(uintptr_t)ptr <= (uintptr_t)tmp->mem + PAGE_SIZE)
			break;

		tmp = tmp->next;
	}

	return tmp;
}

/*
Returns bucket of size if it is in bkt_list and has free entries
Returns NULL otherwise
Assumes size is one of potential bucket sizes
@params: size_t size
*/
bkt_t*
bucket_find(size_t size)
{
	bkt_t* tmp = bkt_list;
	long bm_max = PAGE_SIZE / size;
	long chk = 0;

	while (tmp) {

		if (tmp->size == size) {
			
			for (int i = 0; i < bm_max; i++) {
				
				if (((tmp->used_map[chk >> 6] >> (chk & 0x3F)) & 0x1) == 0)
					break;

				if (++chk == bm_max)
					goto next;
			}

			return tmp;
		}

		next:
		tmp = tmp->next;
	}

	return NULL;
}

/*
Returns address of free chunk from bkt
Assumes bkt has free entries
Modifies bitmap to reflect fact that new entry is being used
@params: bkt_t* bkt
*/
void*
bucket_get(bkt_t* bkt)
{
	bkt_t* tmp = bkt_list;

	void* ptr = NULL;
	long bm_max = PAGE_SIZE / bkt->size;
	long off = 0;
	
	for (int i = 0; i < bm_max; i++) {
				
		if (((tmp->used_map[off >> 6] >> (off & 0x3F)) & 0x1) == 0) {
			ptr = (void*)((uintptr_t)bkt->mem + (uintptr_t)(off * bkt->size));
			tmp->used_map[off >> 6] |= 1 << (off & 0x3F);
			break;
		}

		off += 1;
	}

	return ptr;
}

/*
Inserts bucket of size b_size into bkt_list
Constructs bucket
Assumes b_size is one of sizes in bucket_sizes
Orders bucket list in size order from least to greatest
New buckets go at the end of the run of buckets of their size
@params: bkt_t* bkt
*/
void
bucket_insert(long b_size)
{
	if (!BKT_MEM) {
		BKT_MEM = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
		BKT_MEM_LEN = PAGE_SIZE;
	}

	if (BKT_MEM_LEN <= BKT_MEM_OFF + sizeof(bkt_t)) {

		void* bmem_tmp = mmap(NULL, BKT_MEM_LEN + PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
		memcpy(bmem_tmp, BKT_MEM, BKT_MEM_OFF);

		int err = munmap(BKT_MEM, BKT_MEM_LEN);
		if (err == -1)
			perror("bucket_insert: munmap() failed");

		BKT_MEM = bmem_tmp;
		BKT_MEM_LEN += PAGE_SIZE;
	}

	bkt_t* tmp = bkt_list;
	bkt_t* tmp_prev = NULL;

	bkt_t* bkt = (bkt_t*)((uintptr_t)BKT_MEM + (uintptr_t)BKT_MEM_OFF);
	bkt->size = b_size;

	void* addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	bkt->mem = addr;

	for (int i = 0; i < 8; i++)
		bkt->used_map[i] = 0;

	long idx = 0;
	while (tmp && tmp->size <= bkt->size) {
		tmp_prev = tmp;
		tmp = tmp->next;
		idx += 1;
	}

	if (idx == 0)
		bkt_list = bkt;
	else if (tmp_prev)
		tmp_prev->next = bkt;

	bkt->next = tmp;

	BKT_MEM_OFF += sizeof(bkt_t);
}

/*
Finds spot mem belongs at and sets it used bit to 0,
putting it back in the bucket for later use
Assumes mem is within bkt
@params: void* mem, bkt_t* bkt
*/
void
bucket_put(void* mem, bkt_t* bkt)
{
	uintptr_t mem_off = (uintptr_t)mem - (uintptr_t)bkt;
	long idx = (long)mem_off / (long)bkt->size;
	long inner_idx = idx >> 6;

	bkt->used_map[inner_idx] &= ~(1 << (idx & 0x3F));
}

void*
xmalloc(size_t bytes)
{
	void* ptr = NULL;

	if (bytes <= PAGE_SIZE) {
		pthread_mutex_lock(&mutex);

		long b_size = bucket(bytes);
		bkt_t* bkt = bucket_find(b_size);

		if (bkt)
			ptr = bucket_get(bkt);
		else {
			bucket_insert(b_size);
			pthread_mutex_unlock(&mutex);
			return xmalloc(bytes);
		}

		pthread_mutex_unlock(&mutex);	
		return ptr;
	}
	else {
		bytes += sizeof(large_chunk_t);
		size_t pages = div_up(bytes, PAGE_SIZE);

		ptr = mmap(NULL, pages * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (ptr == (void*)(-1))
			perror("xmalloc: mmap() failed");

		large_chunk_t* cPtr = (large_chunk_t*)(ptr);
		cPtr->size = pages * PAGE_SIZE;
		
		return (void*)cPtr + sizeof(large_chunk_t);
	}

	return (void*)(-1);
}

void
xfree(void* ptr)
{
	bkt_t* bkt = bucket_memfind(ptr);

	// ptr was small allocation
	if (bkt) {
		pthread_mutex_lock(&mutex);
		bucket_put(ptr, bkt);
		pthread_mutex_unlock(&mutex);
	}
	// ptr was large allocation
	else {
		ptr -= sizeof(large_chunk_t);

		int err = munmap(ptr, ((large_chunk_t*)(ptr))->size);
		if (err == -1)
			perror("xfree: munmap() failed");
	}

}

void*
xrealloc(void* prev, size_t bytes)
{
	void* ptr = NULL;
	bkt_t* bkt = bucket_memfind(prev);

	if (bkt && bytes <= PAGE_SIZE) {

		if (bytes == bkt->size)
			ptr = prev;
		else {
			ptr = xmalloc(bytes);
			memcpy(ptr, prev, bkt->size);
			xfree(prev);
		}

	}
	else if (bytes <= PAGE_SIZE) {
		prev -= sizeof(large_chunk_t);

		ptr = xmalloc(bytes);
		memcpy(ptr, prev, bkt->size);

		int err = munmap(prev, ((large_chunk_t*)(prev))->size);
		if (err == -1)
			perror("xrealloc: munmap() failed");

	}
	else if (bkt) {
		bytes += sizeof(large_chunk_t);

		long pages = div_up(bytes, PAGE_SIZE);

		ptr = mmap(NULL, pages * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (ptr == (void*)(-1))
			perror("xrealloc: mmap() failed");

		memcpy(ptr, prev, bkt->size);
		xfree(prev);

		((large_chunk_t*)(ptr))->size = bytes;

		ptr += sizeof(large_chunk_t);
	}
	else {
		prev -= sizeof(large_chunk_t);
		bytes += sizeof(large_chunk_t);

		long pages = div_up(bytes, PAGE_SIZE);

		ptr = mmap(NULL, pages * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (ptr == (void*)(-1))
			perror("xrealloc: mmap() failed");

		memcpy(ptr, prev, ((large_chunk_t*)(prev))->size);
		xfree(prev + sizeof(large_chunk_t));

		((large_chunk_t*)(ptr))->size = bytes;
		ptr += sizeof(large_chunk_t);
	}

	return ptr;
}

void
dump_buckets()
{
	bkt_t* tmp = bkt_list;

	if (!tmp)
		printf("Buckets Empty\n\n");

	while (tmp) {
		
		printf("%lu Bytes:\n", tmp->size);
		printf("Chunk loc: %p\n", tmp->mem);

		printf("Used Map: ");
		for (int i = 7; i >= 0; i--)
			printf("%lx", tmp->used_map[i]);

		fflush(stdout);
		printf("\n");

	}
}

