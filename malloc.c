#include <stddef.h>
#include <stdio.h>

struct chunk
{
	size_t previous_size;
	size_t this_size;
	struct chunk *previous_bin, *next_bin;
};

struct bin
{
	/* TODO: Add locking */
	struct chunk *head, *tail;
};

#define MAX_ALLOC_SIZE		0x400000
#define BYTES_PER_BIN		(MAX_ALLOC_SIZE/64)
struct bin bins[64];
unsigned long bitmap = 0;

#define ilog2(X) ((unsigned) (8*sizeof (unsigned long long) - __builtin_clzll((X)) - 1))

unsigned long size_to_bin(size_t size)
{

}

size_t round_to_pow2(size_t size)
{
	unsigned log = ilog2(size);
	printf("log2: %lu\n", log);
}

void *__malloc(size_t size)
{
	size = round_to_pow2(size);
	unsigned long bin = size_to_bin(size);
	printf("Allocating from bin %lu\n", bin);
}

int main()
{
	printf("__malloc: %p\n", __malloc(11));
}