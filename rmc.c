#include "utils.h"

#include <uade/uade.h>
#include <getopt.h>

int main(int argc, char *argv[])
{
	int ret;
	int i;
	struct uade_state *state = NULL;

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
		struct uade_file *f = uade_file_load(argv[i]);
		if (f == NULL) {
			fprintf(stderr, "Can not open %s\n", argv[i]);
			continue;
		}
		debug("Process %s\n", argv[i]);

		if (state == NULL)
			state = uade_new_state(NULL, NULL);
		if (state == NULL)
			die("Can not initialize uade state\n");

		ret = uade_play_from_buffer(f->name, f->data, f->size, -1, state);
		if (ret < 0) {
			uade_cleanup_state(state);
			state = NULL;
			warning("Fatal error in uade state when initializing %s\n", argv[i]);
			goto nextfile;
		} else if (ret == 0) {
			debug("%s is not playable\n", argv[i]);
			goto nextfile;
		}

	nextfile:
		uade_file_free(f);
	}

	/* state can be NULL */
	uade_cleanup_state(state);
	return 0;
}
