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

static size_t xfwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
        size_t ret;
        size_t written = 0;
        const char *writeptr = ptr;

        while (written < nmemb) {
                ret = fwrite(writeptr, size, nmemb - written, stream);
                if (ret == 0)
                        break;
                written += ret;
                writeptr += size * ret;
        }

        return written;
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

static int write_rmc(const char *targetfname, const struct bencode *container)
{
	char *data;
	size_t len;
	struct bencode *files = ben_list_get(container, 2);
	size_t pos;
	struct bencode *key;
	struct bencode *value;
	FILE *f;
	int ret = -1;

	fprintf(stderr, "meta: %s files: ", ben_print(ben_list_get(container, 1)));
	ben_dict_for_each(key, value, pos, files)
		fprintf(stderr, "%s ", ben_str_val(key));
	fprintf(stderr, "\n");

	data = ben_encode(&len, container);
	if (data == NULL)
		die("Can not serialize\n");

	f = fopen(targetfname, "wb");
	if (f != NULL) {
		if (xfwrite(data, 1, len, f) == len) {
			ret = 0;
		} else {
			fprintf(stderr, "rmc: Can not write all data to %s\n", targetfname);
			unlink(targetfname);
		}
		fclose(f);
	} else {
		fprintf(stderr, "rmc: Can not create file %s\n", targetfname);
	}

	free(data);
	return ret;
}

static void xbasename(char *bname, size_t maxlen, const char *fname)
{
	char path[PATH_MAX];
	snprintf(path, sizeof path, "%s", fname);
	snprintf(bname, maxlen, "%s", basename(path));
}

static void xdirname(char *dname, size_t maxlen, const char *fname)
{
	char path[PATH_MAX];
	snprintf(path, sizeof path, "%s", fname);
	snprintf(dname, maxlen, "%s", dirname(path));
}

static struct bencode *get_basename(const char *fname)
{
	char path[PATH_MAX];
	struct bencode *bname;
	xbasename(path, sizeof path, fname);
	bname = ben_str(path);
	if (bname == NULL)
		die("Can not get basename from %s\n", fname);
	return bname;
}

#if 0
static void set_player_name(struct bencode *meta, const char *fname)
{
	struct bencode *bfname;
	char path[PATH_MAX];
	snprintf(path, sizeof path, "%s", fname);
	assert(0);
	bfname = ben_str(path);
	assert(bfname != NULL);
	if (ben_dict_set_by_str(meta, "player", bfname))
		die("Can not set player name: %s\n", fname);
}
#endif

static void set_strstr(struct bencode *d, const char *key, const char *value)
{
	if (ben_dict_set_str_by_str(d, key, value))
		die("Can not set %s to %s\n", value, key);
}

static void set_info(struct bencode *meta, struct uade_state *state)
{
	const struct uade_song_info *info = uade_get_song_info(state);
	if (info->iscustom)
		set_strstr(meta, "format", "custom");
}

static void record_file(struct bencode *container, struct uade_file *f)
{
	struct bencode *files = ben_list_get(container, 2);
	struct bencode *file = ben_blob(f->data, f->size);
	if (file == NULL || files == NULL)
		die("Unable to get container or create a blob: %s\n", f->name);
	ben_dict_set(files, get_basename(f->name), file);
}

static void get_targetname(char *name, size_t maxlen, struct uade_state *state)
{
	char dname[PATH_MAX];
	char bname[PATH_MAX];
	char newbname[PATH_MAX];
	const struct uade_song_info *info = uade_get_song_info(state);
	int isprefix = 0;
	int ispostfix = 0;
	char *t = NULL;

	xdirname(dname, sizeof dname, info->modulefname);
	xbasename(bname, sizeof bname, info->modulefname);

	if (info->ext[0]) {
		size_t extlen = strlen(info->ext);
		isprefix = (strncasecmp(bname, info->ext, extlen) == 0) && (bname[extlen] == '.');
		t = strrchr(bname, '.');
		ispostfix = (t != NULL) && (strcasecmp(t + 1, info->ext) == 0);
	}

	if (ispostfix) {
		t = strrchr(bname, '.');
		assert(t != NULL);
		*t = 0;
		snprintf(newbname, sizeof newbname, "%s.rmc", bname);
	} else if (isprefix) {
		t = strchr(bname, '.');
		assert(t != NULL);
		snprintf(newbname, sizeof newbname, "%s.rmc", t + 1);
	} else {
		snprintf(newbname, sizeof newbname, "%s.rmc", bname);
	}

	snprintf(name, maxlen, "%s/%s", dname, newbname);
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
	char targetname[PATH_MAX];

	assert(nsubsongs > 0);
	debug("Converting %s (%d subsongs)\n", f->name, nsubsongs);

	uade_stop(state);

	record_file(container, f);

	starttime = getmstime();

	for (cur = min; cur <= max; cur++) {
		ret = uade_play_from_buffer(f->name, f->data, f->size, cur, state);
		if (ret < 0) {
			uade_cleanup_state(state);
			warning("Fatal error in uade state when initializing %s\n", f->name);
			goto error;
		} else if (ret == 0) {
			debug("%s is not playable\n", f->name);
			goto error;
		}

		set_info(meta, state);

		subsongbytes = simulate(state);
		if (subsongbytes == -1)
			goto error;

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

	get_targetname(targetname, sizeof targetname, state);

	ret = write_rmc(targetname, container);

	ben_free(container);

	return ret;

error:
	ben_free(container);
	return -1;
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
