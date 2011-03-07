#include "utils.h"

#include <stdlib.h>

void *xcalloc(size_t nmemb, size_t size)
{
	void *p = calloc(nmemb, size);
	if (p == NULL) {
		fprintf(stderr, "Fatal error: out of memory\n");
		exit(1);
	}
	return p;
}
