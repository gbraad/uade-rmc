#include <uade/uade.h>
#include <bencodetools/bencode.h>
#include <zakalwe/base.h>
#include <zakalwe/file.h>
#include <zakalwe/mem.h>
#include <zakalwe/string.h>

#include <assert.h>
#include <errno.h>
#include <ftw.h>
#include <getopt.h>
#include <iconv.h>
#include <libgen.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define FREQUENCY 44100

static int subsong_timeout = 512;
static int delete_after_packing = 0;
static int recursive_mode = 0;
static int overwrite_mode = 1;
static char pack_dir[PATH_MAX];
static char unpack_dir[PATH_MAX];

static struct bencode *scanner_file_list;

struct collection_context {
	struct bencode *container;
	struct bencode *filelist;
};

iconv_t iconv_cd;


static long long getmstime(void)
{
	struct timeval tv;
	if (gettimeofday(&tv, NULL))
		z_die("gettimeofday() does not work\n");
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

static void set_str_by_str(struct bencode *d, const char *key,
			   const char *value)
{
	char latin1[4096];
	char utf8[4096];
	size_t inbytesleft = strlcpy(latin1, value, sizeof(latin1));
	size_t outbytesleft = sizeof(utf8);
	char *in = latin1;
	char *out = utf8;
	z_assert(inbytesleft < sizeof(latin1));

	size_t ret = iconv(iconv_cd, &in, &inbytesleft, &out, &outbytesleft);
	if (ret == ((size_t) -1))
		z_die("Characted encoding error: %s\n", strerror(errno));

	if (ben_dict_set_str_by_str(d, key, utf8))
		z_die("Can not set %s to %s\n", utf8, key);
}

/* Simulate one subsong, and return the number of bytes simulated */
static size_t simulate(struct uade_state *state)
{
	char buf[4096];
	size_t nbytes = 0;

	while (1) {
		struct uade_notification n;
		ssize_t ret = uade_read(buf, sizeof buf, state);
		if (ret < 0) {
			fprintf(stderr, "Playback error.\n");
			nbytes = -1;
			break;
		} else if (ret == 0) {
			break;
		}

		nbytes += ret;

		while (uade_read_notification(&n, state)) {
			if (n.type == UADE_NOTIFICATION_SONG_END) {
				/*
				 * Note, we might not ever get here, because
				 * the eagleplayer may never signal a song end.
				 * That is why we sum up the bytes read
				 */
				nbytes = n.song_end.subsongbytes;
				uade_cleanup_notification(&n);
				return nbytes;
			}
			uade_cleanup_notification(&n);
		}
	}

	return nbytes;
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

	if (list == NULL || magic == NULL || meta == NULL ||
	    subsongs == NULL || files == NULL)
		z_die("Can not allocate memory for bencode\n");

	if (ben_list_append(list, magic) || ben_list_append(list, meta) ||
	    ben_list_append(list, files)) {
		z_die("Can not append to list\n");
	}

	set_str_by_str(meta, "platform", "amiga");

	if (ben_dict_set_by_str(meta, "subsongs", subsongs))
		z_die("Can not add subsong lengths\n");

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
		z_die("Can not allocate memory for key/value\n");
	if (ben_dict_set(subsongs, key, value))
		z_die("Can not insert %s -> %s to dictionary\n",
		      ben_print(key), ben_print(value));
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
	char *metastring = ben_print(ben_list_get(container, 1));

	fprintf(stderr, "meta: %s files: ", metastring);
	z_free_and_null(metastring);

	print_dict_keys(files, "");
	fprintf(stderr, "\n");

	data = ben_encode(&len, container);
	if (data == NULL)
		z_die("Can not serialize\n");

	f = fopen(targetfname, "wb");
	if (f != NULL) {
		if (xfwrite(data, 1, len, f) == len) {
			ret = 0;
		} else {
			z_log_error("Can not write all data to %s\n", targetfname);
			unlink(targetfname);
		}
		fclose(f);
	} else {
		z_log_error("Can not create file %s\n", targetfname);
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
		z_die("Can not get basename from %s\n", fname);
	return bname;
}

static void set_info(struct bencode *meta, struct uade_state *state)
{
	const struct uade_song_info *info = uade_get_song_info(state);
	const char *formatname = NULL;
	if (info->detectioninfo.custom) {
		formatname = "custom";
	} else {
		formatname = info->formatname;
	}

	// Workaround for PTK-Prowiz
	if (strncmp(formatname, "type: ", 6) == 0)
		formatname += 6;

	if (strlen(formatname) > 0)
		set_str_by_str(meta, "format", formatname);

	if (strlen(info->modulename) > 0)
		set_str_by_str(meta, "title", info->modulename);
}

static void record_file(struct bencode *container, const char *relname,
			void *data, size_t len,
			struct collection_context *context, const char *fname)
{
	if (uade_rmc_record_file(container, relname, data, len))
		z_die("Failed to record %s into container\n", fname);
	if (ben_list_append_str(context->filelist, fname))
		z_die("Failed to append %s to file list\n", fname);
}

struct uade_file *collect_files(const char *amiganame, const char *playerdir,
				void *context, struct uade_state *state)
{
	char dirname[PATH_MAX];
	char path[PATH_MAX];
	char *separator;
	size_t pos;
	const struct uade_song_info *info = uade_get_song_info(state);
	struct collection_context *collection_context = context;
	struct bencode *container = collection_context->container;
	struct uade_file *oldfile;
	struct uade_file *f = uade_load_amiga_file(amiganame, playerdir, state);
	const char *name = f->name;
	if (f == NULL)
		return NULL;

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
		z_log_warning("Ignoring file which does not have the same path "
			"prefix as the song file. File to be loaded: %s "
			"Song file: %s\n", name, info->modulefname);
		return f;
	}

	pos = strlen(dirname);
	while (name[pos] == '/')
		pos++;
	assert(name[pos] != '/');

	z_assert(strlcpy(path, name + pos, sizeof(path)) < sizeof(path));

	/* path is now relative to the song file */
	assert(strlen(path) > 0);

	oldfile = uade_rmc_get_file(container, path);
	if (oldfile != NULL) {
		uade_file_free(oldfile);
		oldfile = NULL;
		return f;
	}

	fprintf(stderr, "Collecting %s\n", name);

	record_file(container, path, f->data, f->size,
		    collection_context, name);

	return f;
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
	int ret;

	xdirname(dname, sizeof dname, info->modulefname);
	xbasename(bname, sizeof bname, info->modulefname);

	if (ext[0]) {
		const size_t extlen = strlen(ext);
		isprefix = (strncasecmp(bname, ext, extlen) == 0) &&
			   (bname[extlen] == '.');
		t = strrchr(bname, '.');
		ispostfix = (t != NULL) && (strcasecmp(t + 1, ext) == 0);
	}

	if (ispostfix) {
		t = strrchr(bname, '.');
		assert(t != NULL);
		*t = 0;
		ret = snprintf(newbname, sizeof newbname, "%s.rmc", bname);
	} else if (isprefix) {
		t = strchr(bname, '.');
		assert(t != NULL);
		ret = snprintf(newbname, sizeof newbname, "%s.rmc", t + 1);
	} else {
		ret = snprintf(newbname, sizeof newbname, "%s.rmc", bname);
	}
	z_assert(ret >=0 && ((size_t) ret) < sizeof(newbname));

	ret = snprintf(name, maxlen, "%s/%s", dname, newbname);
	z_assert(ret >= 0 && ((size_t) ret) < maxlen);
}

static void meta_set_song(struct bencode *container, struct uade_file *f)
{
	struct bencode *meta = ben_list_get(container, 1);
	if (ben_dict_len(meta) <= 1) {
		z_log_error("Container meta was empty. "
			    "Not setting song name\n");
		return;
	}
	if (ben_dict_set_by_str(meta, "song", get_basename(f->name)))
		z_die("Can not set song name to be played\n");
}

static void init_collection_context(struct collection_context *context,
				    struct bencode *container,
				    struct uade_file *f)
{
	char fbasename[PATH_MAX];
	*context = (struct collection_context) {.container = container};
	xbasename(fbasename, sizeof fbasename, f->name);
	context->filelist = ben_list();
	if (context->filelist == NULL)
		z_die("Can not allocate memory for file collection list\n");
	record_file(container, fbasename, f->data, f->size, context, f->name);
}

static int remove_collected_files(struct collection_context *context)
{
	int ret = 0;
	size_t pos;
	struct bencode *str;
	const char *fname;
	ben_list_for_each(str, pos, context->filelist) {
		assert(ben_is_str(str));
		fname = ben_str_val(str);
		if (remove(fname)) {
			z_log_warning("Unable to remove file %s: %s\n",
				fname, strerror(errno));
			ret = -1;
		} else {
			fprintf(stderr, "Removed %s\n", fname);
		}
	}
	return ret;
}

static int convert(struct uade_file *f, struct uade_state *state)
{
	const struct uade_song_info *info = uade_get_song_info(state);
	int min = info->subsongs.min;
	int max = info->subsongs.max;
	int cur;
	int ret = 0;
	size_t subsongbytes;
	long long starttime;
	long long simtime;
	int playtime;
	int sumtime = 0;
	int nsubsongs = max - min + 1;
	struct bencode *container = NULL;
	struct bencode *meta;
	char targetname[PATH_MAX];
	struct collection_context collection_context = {.filelist = NULL};
	struct stat st;

	assert(nsubsongs > 0);

	get_targetname(targetname, sizeof targetname, state);

	if (stat(targetname, &st) == 0 &&
	    overwrite_mode == 0) {
		fprintf(stderr, "Not overwriting file %s. Not converting file %s.\n",
		     targetname, f->name);
		goto exit;
	}
	fprintf(stderr, "Converting %s to %s (%d subsongs)\n",
	      f->name, targetname, nsubsongs);

	container = create_container();
	meta = ben_list_get(container, 1);

	uade_stop(state);

	init_collection_context(&collection_context, container, f);

	uade_set_amiga_loader(collect_files, &collection_context, state);

	starttime = getmstime();

	for (cur = min; cur <= max; cur++) {
		int bytespersecond = uade_get_sampling_rate(state) *
			UADE_BYTES_PER_FRAME;

		if (nsubsongs > 1)
			fprintf(stderr, "Converting subsong %d / %d\n",
				cur, max);

		ret = uade_play_from_buffer(f->name, f->data, f->size, cur,
					    state);
		if (ret < 0) {
			uade_cleanup_state(state);
			z_log_warning("Error in uade state when initializing "
				      "%s\n", f->name);
			goto error;
		} else if (ret == 0) {
			fprintf(stderr, "%s is not playable\n", f->name);
			goto error;
		}

		set_info(meta, state);

		subsongbytes = simulate(state);
		if (subsongbytes == ((size_t) -1))
			goto error;

		playtime = (subsongbytes * 1000) / bytespersecond;
		assert(cur <= max);
		set_playtime(container, cur, playtime);
		sumtime += playtime;

		uade_stop(state);
	}

	simtime = getmstime() - starttime;
	if (simtime < 0)
		simtime = 0;
	fprintf(stderr, "play time %d ms, simulation time %lld ms, "
		"speedup %.1fx\n",
		sumtime, simtime, ((float) sumtime) / simtime);

	meta_set_song(container, f);

	ret = write_rmc(targetname, container);

	if (ret == 0 && delete_after_packing)
		ret = remove_collected_files(&collection_context);

	goto exit;

error:
	ret = -1;
exit:
	uade_set_amiga_loader(NULL, NULL, state);
	ben_free(container);
	ben_free(collection_context.filelist);
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

	snprintf(buf, sizeof buf, "%d", subsong_timeout);
	uade_config_set_option(config, UC_SUBSONG_TIMEOUT_VALUE, buf);

	uade_config_set_option(config, UC_TIMEOUT_VALUE, "-1");
}

static void print_usage(void)
{
	printf(
"Usage: rmc [-d|-h|-n|-r|-u|-w t] [file1 file2 ..]\n"
"\n"
"-d      Delete song after successful packing. This can be reversed with -u,\n"
"        that is, obtain the original song file by unpacking the container.\n"
"-h      Print help.\n"
"-n      Do not overwrite an existing rmc file. This can be used for\n"
"        incremental conversion of directories.\n"
"-r      Scan given directories recursively and process everything.\n"
"-u      Unpack mode: unpack RMC files into meta and song files.\n"
"-w t    Set subsong timeout to be t seconds.\n"
		);
}

static int directory_traverse_fn(const char *fpath, const struct stat *sb,
				 int typeflag)
{
	(void) sb;
	if (typeflag != FTW_F)
		return 0;
	if (ben_list_append_str(scanner_file_list, fpath))
		z_die("No memory to append file %s to scanner list\n", fpath);
	return 0;
}

static int put_files_into_container(int i, int argc, char *argv[])
{
	int ret;
	struct uade_state *state = NULL;
	int exitval = 0;
	struct uade_config *config = uade_new_config();
	size_t pos;
	struct bencode *benarg;

	/*
	 * Use a global variable because ftw() call does not allow an opaque
	 * data parameter for directory scanning. Dirty.
	 */
	assert(scanner_file_list == NULL);
	scanner_file_list = ben_list();
	if (scanner_file_list == NULL)
		z_die("No memory for scanner file list\n");

	if (config == NULL)
		z_die("Could not allocate memory for config\n");

	initialize_config(config);

	for (; i < argc; i++) {
		struct stat st;
		if (stat(argv[i], &st)) {
			fprintf(stderr, "Can not stat %s. Skipping.\n", argv[i]);
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			ret = ftw(argv[i], directory_traverse_fn, 500);
			if (ret != 0)
				z_die("Traversing directory %s failed\n",
				    argv[i]);
		} else {
			ben_list_append_str(scanner_file_list, argv[i]);
		}
	}

	ben_list_for_each(benarg, pos, scanner_file_list) {
		const char *arg = ben_str_val(benarg);
		struct uade_file *f = uade_file_load(arg);
		if (f == NULL) {
			z_log_error("Can not open %s\n", arg);
			continue;
		}

		if (uade_is_rmc(f->data, f->size)) {
			fprintf(stderr, "Won't convert RMC again: %s\n", f->name);
			uade_file_free(f);
			continue;
		}

		if (state == NULL)
			state = uade_new_state(config);
		if (state == NULL)
			z_die("Can not initialize uade state\n");

		ret = uade_play_from_buffer(f->name, f->data, f->size, -1,
					    state);
		if (ret < 0) {
			uade_cleanup_state(state);
			state = NULL;
			z_log_error("Can not convert (play) %s\n", arg);
			goto nextfile;
		} else if (ret == 0) {
			fprintf(stderr, "%s is not playable (convertable)\n", arg);
			goto nextfile;
		}

		if (convert(f, state))
			exitval = 1;

	nextfile:
		uade_file_free(f);
		if (state != NULL)
			uade_stop(state);
	}

	/* state can be NULL */
	uade_cleanup_state(state);

	z_free_and_null(config);

	ben_free(scanner_file_list);
	scanner_file_list = NULL;

	return exitval;
}


static struct bencode *get_container(struct uade_file *f)
{
	struct bencode *container = ben_decode(f->data, f->size);
	if (container == NULL) {
		z_log_error("Unable to decode %s\n", f->name);
		return NULL;
	}

	if (!ben_is_list(container) || ben_list_len(container) < 3) {
		z_log_error("Invalid container format: no main list: %s\n", f->name);
		goto err;
	}

	if (!ben_is_dict(ben_list_get(container, 1)) ||
	    !ben_is_dict(ben_list_get(container, 2))) {
		z_log_error("Either meta or files is not a dictionary: %s\n", f->name);
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
	int ret = snprintf(metaname, sizeof metaname, "%s/meta", dirname);
	z_assert(ret >= 0 && ((size_t) ret) < sizeof(metaname));

	f = fopen(metaname, "wb");
	if (f == NULL) {
		z_log_error("Can not open meta file for writing: %s (%s)\n", metaname, strerror(errno));
		goto err;
	}

	metastring = ben_print(ben_list_get(container, 1));
	if (metastring == NULL) {
		z_log_error("Can not generate meta string\n");
		goto err;
	}

	if (xfwrite(metastring, strlen(metastring), 1, f) != 1) {
		z_log_error("Write error to meta file %s (%s)\n", metaname, strerror(errno));
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
			z_log_error("File name must be a string\n");
			return 1;
		}
		if (strcmp(ben_str_val(key), "..") == 0 ||
		    strcmp(ben_str_val(key), ".") == 0 ||
		    strstr(ben_str_val(key), "/") != NULL) {
			z_log_error("Invalid name: %s\n", ben_str_val(key));
			return 1;
		}

		if (ben_is_dict(value)) {
			snprintf(prefix, sizeof prefix, "%s%s/", oldprefix, ben_str_val(key));
			if (mkdir(prefix, 0700)) {
				z_log_error("Unable to create directory %s (%s)\n", prefix, strerror(errno));
				return 1;
			}
			if (scan_and_write_files(value, prefix))
				return 1;
			continue;
		}

		if (!ben_is_str(value)) {
			z_log_error("Invalid file content: %s\n", ben_str_val(key));
			return 1;
		}
		snprintf(prefix, sizeof prefix, "%s%s", oldprefix, ben_str_val(key));
		f = fopen(prefix, "wb");
		if (f == NULL) {
			z_log_error("Can not write file");
			return 1;
		}
		if (xfwrite(ben_str_val(value), ben_str_len(value), 1, f) != 1) {
			z_log_error("Unable to write to file: %s (%s)\n", prefix, strerror(errno));
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
	int ret;

	/* Note, trailing / is important in prefix string */
	ret = snprintf(prefix, sizeof prefix, "%s/files/", dirname);
	z_assert(ret >= 0 && ((size_t) ret) < sizeof(prefix));

	if (mkdir(prefix, 0700)) {
		z_log_error("Unable to create directory %s (%s)\n",
			    prefix, strerror(errno));
		return -1;
	}
	if (scan_and_write_files(files, prefix)) {
		z_log_error("Can not unpack RMC to %s\n", dirname);
		return -1;
	}
	return 0;
}

static int unpack_file(const char *dir, struct uade_file *f)
{
	struct bencode *container;

	if (!uade_is_rmc(f->data, f->size)) {
		z_log_error("%s is not an RMC file\n", f->name);
		return 1;
	}

	container = get_container(f);
	if (container == NULL)
		goto cleanup;

	if (unpack_meta(dir, container))
		goto cleanup;

	if (unpack_files(dir, container))
		goto cleanup;

	ben_free(container);
	fprintf(stderr, "Unpacked %s to directory %s (OK)\n", f->name, dir);
	return 0;

cleanup:
	return 1;
}

static int pack_container(int i, int argc, char *argv[])
{
	struct bencode *container = create_container();
	struct bencode *meta;
	struct bencode *files;
	char *targetname;
	char files_dir[PATH_MAX];
	char path[PATH_MAX];
	void *metabytes;
	size_t size;

	z_assert(container != NULL);

	if ((optind + 1) != argc) {
		z_log_fatal("Expect rmc name as the only non-option "
			    "argument\n");
	}
	targetname = argv[i];
	z_assert(targetname != NULL);

	z_snprintf_or_die(path, sizeof(path), "%s/meta", pack_dir);
	metabytes = z_file_read(&size, path);
	z_assert(metabytes != NULL);

	meta = ben_decode_printed(metabytes, size);
	z_assert(meta != NULL);
	ben_list_set(container, 1, meta);

	z_snprintf_or_die(files_dir, sizeof(files_dir), "%s/files", pack_dir);
	files = ben_list_get(container, 2);
	struct str_array *names = z_list_dir(files_dir);
	struct str_array_iter iter;
	z_assert(names != NULL);
	z_array_for_each(str_array, names, &iter) {
		z_snprintf_or_die(path, sizeof(path),
				  "%s/%s", files_dir, iter.value);
		void *filebytes = z_file_read(&size, path);
		z_assert(filebytes != NULL);
		struct bencode *benbytes = ben_blob(filebytes, size);
		z_assert(benbytes != NULL);
		z_assert(ben_dict_set_by_str(files, iter.value,
					     benbytes) == 0);
	}
	str_array_free_all(names);

	return write_rmc(targetname, container);
}

static int unpack_container(int i, int argc, char *argv[])
{
	int exitval = 0;
	struct uade_file *f;

	if (recursive_mode)
		z_die("Recursive mode is not yet implemented for unpacking.");

	if ((i + 1) != argc) {
		z_log_fatal("Expect rmc name as the only non-option "
			    "argument\n");
	}

	f = uade_file_load(argv[i]);
	if (f == NULL) {
		z_log_error("Can not open file %s\n", argv[i]);
		return 1;
	}

	if (unpack_file(unpack_dir, f))
		exitval = 1;

	uade_file_free(f);

	return exitval;
}

int main(int argc, char *argv[])
{
	char *end;
	int ret;
	int (*operation)(int i, int argc, char *argv[]);
	size_t size;

	iconv_cd = iconv_open("utf-8", "iso-8859-1");
	z_assert((size_t) iconv_cd != ((size_t) -1));

	operation = put_files_into_container;

	while (1) {
		ret = getopt(argc, argv, "dhnp:ru:w:");
		if (ret  < 0)
			break;
		switch (ret) {
		case 'd':
			delete_after_packing = 1;
			break;
		case 'h':
			print_usage();
			exit(0);
		case 'n':
			overwrite_mode = 0;
			break;
		case 'p':
			operation = pack_container;
			size = strlcpy(pack_dir, optarg, sizeof(pack_dir));
			z_assert(size < sizeof(pack_dir));
			z_assert(strlen(pack_dir) > 0);
			break;
		case 'r':
			recursive_mode = 1;
			break;
		case 'u':
			/* Unpack rmc file */
			operation = unpack_container;
			size = strlcpy(unpack_dir, optarg, sizeof(unpack_dir));
			z_assert(size < sizeof(unpack_dir));
			z_assert(strlen(unpack_dir) > 0);
			break;
		case 'w':
			/* Set subsong timeout */
			subsong_timeout = strtol(optarg, &end, 10);
			if (*end != 0)
				z_die("Invalid timeout: %s\n", optarg);
			break;
		default:
			z_die("Unknown option: %c\n", optopt);
		}
	}

	return operation(optind, argc, argv);
}
