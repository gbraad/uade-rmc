#include "utils.h"

#include <uade/uade.h>
#include <getopt.h>

int main(int argc, char *argv[])
{
	int ret;
	int i;

	while (1) {
		ret = getopt(argc, argv, "d");
		if (ret  < 0)
			break;
		switch (ret) {
		case 'd':
			die("-d not implemented\n");
		default:
			die("Unknown option: %c\n", optopt);
		}
	}

	for (i = optind; i < argc; i++) {
		debug("Process %s\n", argv[i]);
	}

	return 0;
}
