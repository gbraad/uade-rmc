#ifndef _UADE_UNIXSUPPORT_H_
#define _UADE_UNIXSUPPORT_H_

#include <stdlib.h>
#include <stdio.h>

#define debug(fmt, args...) do { fprintf(stderr, fmt, ## args); } while (0)
#define die(fmt, args...) do { fprintf(stderr, "rmc error: " fmt, ## args); exit(1); } while(0)
#define warning(fmt, args...) do { fprintf(stderr, "rmc warning: " fmt, ## args); } while(0)

void *xcalloc(size_t nmemb, size_t size);

#endif
