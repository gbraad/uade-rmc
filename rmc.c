#include "utils.h"

#include <uade/uade.h>
#include <bencodetools/bencode.h>

#include <getopt.h>
#include <libgen.h>
#include <sys/time.h>
#include <sys/resource.h>

#define FREQUENCY 44100

#define RMC_MAGIC "rmc\x00\xfb\x13\xf6\x1f\xa2"

static long long getmstime(void)
{
	struct timeval tv;
	if (gettimeofday(&tv, NULL))
		die("gettimeofday() does not work\n");
	return ((long long) tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

static size_t simulate(struct uade_state *state)
{
	struct uade_event event;
	size_t nbytes = 0;

	while (1) {
		if (uade_get_event(&event, state)) {
			fprintf(stderr, "uade_get_event(): error!\n");
			break;
		}

		switch (event.type) {
		case UADE_EVENT_EAGAIN:
			break;
		case UADE_EVENT_DATA:
			nbytes += event.data.size;
			break;
		case UADE_EVENT_MESSAGE:
			break;
		case UADE_EVENT_SONG_END:
			if (!event.songend.happy) {
				nbytes = -1;
				fprintf(stderr, "bad song end: %s\n", event.songend.reason);
			}
			return nbytes;
		default:
			die("uade_get_event returned %s which is not handled.\n", uade_event_name(&event));
		}
	}
	return -1;
}

struct bencode *create_container(void)
{
	struct bencode *list;
	struct bencode *magic;
	struct bencode *meta;
	struct bencode *subsongs;
	struct bencode *files;

	list = ben_list();
	magic = ben_blob(RMC_MAGIC, 9);
	meta = ben_dict();
	subsongs = ben_dict();
	files = ben_dict();

	if (list == NULL || magic == NULL || meta == NULL || subsongs == NULL ||
	    files == NULL)
		die("Can not allocate memory for bencode\n");

	if (ben_list_append(list, magic) || ben_list_append(list, meta) || ben_list_append(list, files))
		die("Can not append to list\n");

	if (ben_dict_set_str_by_str(meta, "platform", "amiga"))
		die("Can not set platform\n");

	if (ben_dict_set_by_str(meta, "subsongs", subsongs))
		die("Can not add subsong lengths\n");

	return list;
}

static void set_playtime(struct bencode *container, int sub, int playtime)
{
	struct bencode *key = ben_int(sub);
	struct bencode *value = ben_int(playtime);
	struct bencode *meta = ben_list_get(container, 1);
	struct bencode *subsongs = ben_dict_get_by_str(meta, "subsongs");
	if (playtime == 0)
		return;
	if (key == NULL || value == NULL)
		die("Can not allocate memory for key/value\n");
	if (ben_dict_set(subsongs, key, value))
		die("Can not insert %s -> %s to dictionary\n", ben_print(key), ben_print(value));
	printf("subsong %d: %d\n", sub, playtime);
}

static int write_rmc(const struct bencode *container)

{
	char *data;
	size_t len;
	fprintf(stderr, "%s\n", ben_print(container));
	data = ben_encode(&len, container);
	if (data == NULL)
		die("Can not serialize\n");
	free(data);
	return 0;
}

#if 0
static void set_player_name(struct bencode *meta, const char *fname)
{
	struct bencode *bfname;
	char path[PATH_MAX];
	snprintf(path, sizeof path, "%s", fname);
	bfname = ben_str(path);
	assert(bfname != NULL);
	if (ben_dict_set_by_str(meta, "player", bfname))
		die("Can not set player name: %s\n", fname);
}
#endif

static void set_format_name(struct bencode *meta, const char *formatname)
{
	if (ben_dict_set_str_by_str(meta, "format", formatname))
		die("Can not set format name: %s\n", formatname);
}

static void set_info(struct bencode *meta, struct uade_state *state)
{
	const struct uade_song_info *info = uade_get_song_info(state);
	if (info->iscustom)
		set_format_name(meta, "custom");
}

static int convert(struct uade_file *f, struct uade_state *state)
{
	const struct uade_song_info *info = uade_get_song_info(state);
	int min = info->subsongs.min;
	int max = info->subsongs.max;
	int cur;
	int ret;
	size_t subsongbytes;
	long long starttime;
	long long simtime;
	int playtime;
	int sumtime = 0;
	int nsubsongs = max - min + 1;
	struct bencode *container = create_container();
	struct bencode *meta = ben_list_get(container, 1);
	struct bencode *files = ben_list_get(container, 2);

	assert(nsubsongs > 0);
	debug("Converting %s (%d subsongs)\n", f->name, nsubsongs);

	uade_stop(state);

	starttime = getmstime();

	for (cur = min; cur <= max; cur++) {
		ret = uade_play_from_buffer(f->name, f->data, f->size, cur, state);
		if (ret < 0) {
			uade_cleanup_state(state);
			warning("Fatal error in uade state when initializing %s\n", f->name);
			return -1;
		} else if (ret == 0) {
			debug("%s is not playable\n", f->name);
			return -1;
		}

		set_info(meta, state);

		subsongbytes = simulate(state);
		if (subsongbytes == -1)
			return -1;

		playtime = (subsongbytes * 1000) / info->bytespersecond;
		assert(cur <= max);
		set_playtime(container, cur, playtime);
		sumtime += playtime;

		uade_stop(state);
	}

	simtime = getmstime() - starttime;
	if (simtime < 0)
		simtime = 0;
	fprintf(stderr, "play time %d ms, simulation time %lld ms, speedup %.1fx\n", sumtime, simtime, ((float) sumtime) / simtime);


	return write_rmc(container);
}

static void initialize_config(struct uade_config *config)
{
	char buf[16];
	uade_config_set_defaults(config);
	snprintf(buf, sizeof buf, "%d", FREQUENCY);
	uade_config_set_option(config, UC_FREQUENCY, buf);
	uade_config_set_option(config, UC_ENABLE_TIMEOUTS, NULL);
	uade_config_set_option(config, UC_SILENCE_TIMEOUT_VALUE, "20");
	uade_config_set_option(config, UC_SUBSONG_TIMEOUT_VALUE, "512");
	uade_config_set_option(config, UC_TIMEOUT_VALUE, "-1");
}

int main(int argc, char *argv[])
{
	int ret;
	int i;
	struct uade_state *state = NULL;
	int exitval = 0;
	struct uade_config config;

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

	initialize_config(&config);

	for (i = optind; i < argc; i++) {
		struct uade_file *f = uade_file_load(argv[i]);
		if (f == NULL) {
			fprintf(stderr, "Can not open %s\n", argv[i]);
			continue;
		}

		if (state == NULL)
			state = uade_new_state(&config, NULL);
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

		if (convert(f, state))
			exitval = 1;

	nextfile:
		uade_file_free(f);
		uade_stop(state);
	}

	/* state can be NULL */
	uade_cleanup_state(state);
	return exitval;
}
