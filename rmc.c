#include "utils.h"

#include <uade/uade.h>
#include <bencodetools/bencode.h>

#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#define FREQUENCY 44100

static int subsongtimeout = 512;

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

static void set_str_by_str(struct bencode *d, const char *key, const char *value)
{
	if (ben_dict_set_str_by_str(d, key, value))
		die("Can not set %s to %s\n", value, key);
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
	magic = ben_blob(RMC_MAGIC, RMC_MAGIC_LEN);
	meta = ben_dict();
	subsongs = ben_dict();
	files = ben_dict();

	if (list == NULL || magic == NULL || meta == NULL || subsongs == NULL ||
	    files == NULL)
		die("Can not allocate memory for bencode\n");

	if (ben_list_append(list, magic) || ben_list_append(list, meta) || ben_list_append(list, files))
		die("Can not append to list\n");

	set_str_by_str(meta, "platform", "amiga");

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
	fprintf(stderr, "Subsong %d: %.3fs\n", sub, playtime / 1000.0);
}

static void print_dict_keys(const struct bencode *files, const char *oldprefix)
{
	size_t pos;
	struct bencode *key;
	struct bencode *value;
	char prefix[PATH_MAX];

	ben_dict_for_each(key, value, pos, files) {
		if (ben_is_dict(value)) {
			snprintf(prefix, sizeof prefix, "%s%s/", oldprefix, ben_str_val(key));
			print_dict_keys(value, prefix);
		} else {
			fprintf(stderr, "%s%s ", oldprefix, ben_str_val(key));
		}
	}
}

static int write_rmc(const char *targetfname, const struct bencode *container)
{
	char *data;
	size_t len;
	struct bencode *files = ben_list_get(container, 2);
	FILE *f;
	int ret = -1;

	fprintf(stderr, "meta: %s files: ", ben_print(ben_list_get(container, 1)));
	print_dict_keys(files, "");
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

static void set_info(struct bencode *meta, struct uade_state *state)
{
	const struct uade_song_info *info = uade_get_song_info(state);
	if (info->detectioninfo.custom)
		set_str_by_str(meta, "format", "custom");
}

struct uade_file *collect_files(const char *name, const char *playerdir,
				void *context, struct uade_state *state)
{
	char dirname[PATH_MAX];
	char path[PATH_MAX];
	char *separator;
	size_t pos;
	const struct uade_song_info *info = uade_get_song_info(state);
	struct bencode *container = context;
	struct uade_file *oldfile;
	struct uade_file *f = uade_load_amiga_file(name, playerdir, state);
	if (f == NULL)
		return NULL;

	fprintf(stderr, "Trying to collect %s\n", name);

	/* Do not collect file names with ':' (for example, ENV:Foo) */
	separator = strchr(name, ':');
	if (separator != NULL)
		return f;

	if (strchr(info->modulefname, '/')) {
		xdirname(dirname, sizeof dirname, info->modulefname);
	} else {
		dirname[0] = 0;
	}

	if (memcmp(dirname, name, strlen(dirname)) != 0) {
		fprintf(stderr, "Ignoring file which does not have the same path prefix as the song file. File to be loaded: %s Song file: %s\n", name, info->modulefname);
		return f;
	}

	pos = strlen(dirname);
	while (name[pos] == '/')
		pos++;
	assert(name[pos] != '/');

	snprintf(path, sizeof path, "%s", name + pos);
	/* path is now relative to the song file */
	assert(strlen(path) > 0);
	fprintf(stderr, "Shortened path name is %s\n", path);

	oldfile = uade_rmc_get_file(container, path);
	if (oldfile != NULL) {
		fprintf(stderr, "File already exists, not recording: %s\n", path);
		return f;
	}

	if (uade_rmc_record_file(container, path, f->data, f->size))
		die("Failed to record %s\n", name);

	return f;
}

static void record_file(struct bencode *container, struct uade_file *f)
{
	struct bencode *files = ben_list_get(container, 2);
	struct bencode *file = ben_blob(f->data, f->size);
	if (file == NULL || files == NULL)
		die("Unable to get container or create a blob: %s\n", f->name);
	if (ben_dict_set(files, get_basename(f->name), file))
		die("Unable to insert file: %s\n", f->name);
}

static void get_targetname(char *name, size_t maxlen, struct uade_state *state)
{
	char dname[PATH_MAX];
	char bname[PATH_MAX];
	char newbname[PATH_MAX];
	const struct uade_song_info *info = uade_get_song_info(state);
	const char *ext = info->detectioninfo.ext;
	int isprefix = 0;
	int ispostfix = 0;
	char *t = NULL;

	xdirname(dname, sizeof dname, info->modulefname);
	xbasename(bname, sizeof bname, info->modulefname);

	if (ext[0]) {
		size_t extlen = strlen(ext);
		isprefix = (strncasecmp(bname, ext, extlen) == 0) && (bname[extlen] == '.');
		t = strrchr(bname, '.');
		ispostfix = (t != NULL) && (strcasecmp(t + 1, ext) == 0);
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

static void finalize(struct bencode *container, struct uade_file *f)
{
	struct bencode *meta = ben_list_get(container, 1);
	if (ben_dict_len(meta) == 1)
		return;
	if (ben_dict_set_by_str(meta, "song", get_basename(f->name)))
		die("Can not set song name to be played\n");
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

	get_targetname(targetname, sizeof targetname, state);

	debug("Converting %s to %s (%d subsongs)\n", f->name, targetname, nsubsongs);

	uade_stop(state);

	record_file(container, f);

	uade_set_amiga_loader(collect_files, container, state);

	starttime = getmstime();

	for (cur = min; cur <= max; cur++) {
		if (nsubsongs > 1)
			fprintf(stderr, "Converting subsong %d / %d\n", cur, max);

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

	finalize(container, f);

	ret = write_rmc(targetname, container);
	goto exit;

error:
	ret = -1;
exit:
	uade_set_amiga_loader(NULL, NULL, state);
	ben_free(container);
	return ret;
}

static void initialize_config(struct uade_config *config)
{
	char buf[16];
	uade_config_set_defaults(config);
	snprintf(buf, sizeof buf, "%d", FREQUENCY);
	uade_config_set_option(config, UC_FREQUENCY, buf);
	uade_config_set_option(config, UC_ENABLE_TIMEOUTS, NULL);
	uade_config_set_option(config, UC_SILENCE_TIMEOUT_VALUE, "20");

	snprintf(buf, sizeof buf, "%d", subsongtimeout);
	uade_config_set_option(config, UC_SUBSONG_TIMEOUT_VALUE, buf);

	uade_config_set_option(config, UC_TIMEOUT_VALUE, "-1");
}

static void print_usage(void)
{
	printf(
"Usage:\n"
"\n"
"-h      Print help\n"
"-u      Unpack mode: unpack RMC files into meta and song files\n"
"-w t    Set subsong timeout to be t seconds\n"
		);
}

static int put_files_into_container(int i, int argc, char *argv[])
{
	int ret;
	struct uade_state *state = NULL;
	int exitval = 0;
	struct uade_config config;

	initialize_config(&config);

	for (; i < argc; i++) {
		struct uade_file *f = uade_file_load(argv[i]);
		if (f == NULL) {
			fprintf(stderr, "Can not open %s\n", argv[i]);
			continue;
		}

		if (uade_is_rmc(f->data, f->size)) {
			fprintf(stderr, "Won't convert RMC again: %s\n", f->name);
			uade_file_free(f);
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
			warning("Error in uade state when initializing %s\n", argv[i]);
			goto nextfile;
		} else if (ret == 0) {
			debug("%s is not playable (convertable)\n", argv[i]);
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


static struct bencode *get_container(struct uade_file *f)
{
	struct bencode *container = ben_decode(f->data, f->size);
	if (container == NULL) {
		fprintf(stderr, "Unable to decode %s\n", f->name);
		return NULL;
	}

	if (!ben_is_list(container) || ben_list_len(container) < 3) {
		fprintf(stderr, "Invalid container format: no main list: %s\n", f->name);
		goto err;
	}

	if (!ben_is_dict(ben_list_get(container, 1)) ||
	    !ben_is_dict(ben_list_get(container, 2))) {
		fprintf(stderr, "Either meta or files is not a dictionary: %s\n", f->name);
		goto err;
	}

	return container;

err:
	ben_free(container);
	return NULL;
}

static int unpack_meta(const char *dirname, struct bencode *container)
{
	char metaname[PATH_MAX];
	char *metastring = NULL;
	FILE *f;

	snprintf(metaname, sizeof metaname, "%s/meta", dirname);

	f = fopen(metaname, "wb");
	if (f == NULL) {
		fprintf(stderr, "Can not open meta file for writing: %s (%s)\n", metaname, strerror(errno));
		goto err;
	}

	metastring = ben_print(ben_list_get(container, 1));
	if (metastring == NULL) {
		fprintf(stderr, "Can not generate meta string\n");
		goto err;
	}

	if (xfwrite(metastring, strlen(metastring), 1, f) != 1) {
		fprintf(stderr, "Write error to meta file %s (%s)\n", metaname, strerror(errno));
		goto err;
	}

	free(metastring);
	fclose(f);
	return 0;

err:
	if (f != NULL)
		fclose(f);
	free(metastring);
	return -1;

}

static int scan_and_write_files(const struct bencode *files, const char *oldprefix)
{
	size_t pos;
	struct bencode *key;
	struct bencode *value;
	char prefix[PATH_MAX];
	FILE *f;

	ben_dict_for_each(key, value, pos, files) {
		if (!ben_is_str(key)) {
			fprintf(stderr, "File name must be a string\n");
			return 1;
		}
		if (strcmp(ben_str_val(key), "..") == 0 ||
		    strcmp(ben_str_val(key), ".") == 0 ||
		    strstr(ben_str_val(key), "/") != NULL) {
			fprintf(stderr, "Invalid name: %s\n", ben_str_val(key));
			return 1;
		}

		if (ben_is_dict(value)) {
			snprintf(prefix, sizeof prefix, "%s%s/", oldprefix, ben_str_val(key));
			if (mkdir(prefix, 0700)) {
				fprintf(stderr, "Unable to create directory %s (%s)\n", prefix, strerror(errno));
				return 1;
			}
			if (scan_and_write_files(value, prefix))
				return 1;
			continue;
		}

		if (!ben_is_str(value)) {
			fprintf(stderr, "Invalid file content: %s\n", ben_str_val(key));
			return 1;
		}
		snprintf(prefix, sizeof prefix, "%s%s", oldprefix, ben_str_val(key));
		f = fopen(prefix, "wb");
		if (f == NULL) {
			fprintf(stderr, "Can not write file");
			return 1;
		}
		if (xfwrite(ben_str_val(value), ben_str_len(value), 1, f) != 1) {
			fprintf(stderr, "Unable to write to file: %s (%s)\n", prefix, strerror(errno));
			fclose(f);
			return 1;
		}
		fclose(f);
		f = NULL;
	}
	return 0;
}

static int unpack_files(const char *dirname, struct bencode *container)
{
	struct bencode *files = ben_list_get(container, 2);
	char prefix[PATH_MAX];

	/* Note, trailing / is important in prefix string */
	snprintf(prefix, sizeof prefix, "%s/files/", dirname);

	if (mkdir(prefix, 0700)) {
		fprintf(stderr, "Unable to create directory %s (%s)\n", prefix, strerror(errno));
		return -1;
	}
	if (scan_and_write_files(files, prefix)) {
		fprintf(stderr, "Can not unpack RMC to %s\n", dirname);
		return -1;
	}
	return 0;
}

static int unpack_file(struct uade_file *f)
{
	char dirname[PATH_MAX];
	struct bencode *container;

	dirname[0] = 0;

	if (!uade_is_rmc(f->data, f->size)) {
		fprintf(stderr, "%s is not an RMC file\n", f->name);
		return 1;
	}

	snprintf(dirname, sizeof dirname, "%s.unpacked", f->name);
	if (mkdir(dirname, 0700)) {
		fprintf(stderr, "Unable to create directory %s: %s\n", dirname, strerror(errno));
		return 1;
	}

	container = get_container(f);
	if (container == NULL)
		goto cleanup;

	if (unpack_meta(dirname, container))
		goto cleanup;

	if (unpack_files(dirname, container))
		goto cleanup;

	ben_free(container);
	fprintf(stderr, "Unpacked %s to directory %s (OK)\n", f->name, dirname);
	return 0;

cleanup:
	if (dirname[0] && rmdir(dirname)) {
		fprintf(stderr, "Directory %s has been left in invalid state.\nPlease fix or clean it.\n", dirname);
	}
	return 1;
}

static int unpack_containers(int i, int argc, char *argv[])
{
	int exitval = 0;
	struct uade_file *f;

	for (; i < argc; i++) {
		f = uade_file_load(argv[i]);
		if (f == NULL) {
			fprintf(stderr, "Can not open file %s\n", argv[i]);
			exitval = 1;
			continue;
		}
		if (unpack_file(f))
			exitval = 1;
		uade_file_free(f);
	}

	return exitval;
}

int main(int argc, char *argv[])
{
	char *end;
	int ret;
	int (*operation)(int i, int argc, char *argv[]);

	operation = put_files_into_container;

	while (1) {
		ret = getopt(argc, argv, "huw:");
		if (ret  < 0)
			break;
		switch (ret) {
		case 'h':
			print_usage();
			exit(0);
		case 'u':
			/* Unpack rmc file */
			operation = unpack_containers;
			break;
		case 'w':
			/* Set subsong timeout */
			subsongtimeout = strtol(optarg, &end, 10);
			if (*end != 0)
				die("Invalid timeout: %s\n", optarg);
			break;
		default:
			die("Unknown option: %c\n", optopt);
		}
	}

	return operation(optind, argc, argv);
}
