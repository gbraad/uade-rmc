#ifndef _RMC_UTILS_H_
#define _RMC_UTILS_H_

#include <stdlib.h>
#include <stdio.h>

#define UNUSED(x) do { (void) (x); } while (0)

#define debug(fmt, args...) do { fprintf(stderr, fmt, ## args); } while (0)
#define die(fmt, args...) do { fprintf(stderr, "rmc fatal error: " fmt, ## args); abort(); } while(0)
#define error(fmt, args...) do { fprintf(stderr, "rmc error: " fmt, ## args); } while(0)
#define info(fmt, args...) do { fprintf(stderr, fmt, ## args); } while (0)
#define warning(fmt, args...) do { fprintf(stderr, "rmc warning: " fmt, ## args); } while(0)

#define free_and_null(x) do { free(x); (x) = NULL; } while (0)
#define free_and_poison(x) do { free(x); (x) = (void *) -1; } while (0)

void *xcalloc(size_t nmemb, size_t size);


#endif
