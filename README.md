# MemoryAllocator
This project is a working thread-safe memory allocator writting in C. It was created for CS3650, Computer Systems, at Northeastern University.
The files list_main.c, frag_main.c, and ivec_main.c are example uses for the allocator. The allocator itself is contained in hwx_malloc.c.
The file opt_malloc.c is an incomplete attempt at beating the system allocator (in terms of time) at the three given examples. It uses bucket-based memory allocation.
