#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/param.h>	/*for roundup*/
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "array.h"
#include "eaio_api.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#if GCC_VERSION
  #define likely(x)     __builtin_expect(!!(x), 1)
  #define unlikely(x)   __builtin_expect(!!(x), 0)
#else
  #define likely(x)     (!!(x))
  #define unlikely(x)   (!!(x))
#endif

void *xvalloc(size_t size)
{
        void    *ret = NULL;
        int     err = posix_memalign((void **)&ret, getpagesize(), size);

        if (unlikely(err)) {
                assert(0);
        }

        memset(ret, 0, size);
        return ret;
}


static ssize_t _pread(int fd, void *buf, size_t len, off_t offset)
{
	ssize_t nr;
	while (true) {
		nr = pread(fd, buf, len, offset);
		if (unlikely(nr < 0) && (errno == EAGAIN || errno == EINTR))
			continue;
		return nr;
	}
}

ssize_t xpread(int fd, void *buf, size_t count, off_t offset)
{
	char *p = buf;
	ssize_t total = 0;

	while (count > 0) {
		ssize_t loaded = _pread(fd, p, count, offset);
		if (unlikely(loaded < 0))
			return -1;
		if (unlikely(loaded == 0))
			return total;
		count -= loaded;
		p += loaded;
		total += loaded;
		offset += loaded;
	}

	return total;
}

static ssize_t _pwrite(int fd, const void *buf, size_t len, off_t offset)
{
	ssize_t nr;
	while (true) {
		nr = pwrite(fd, buf, len, offset);
		if (unlikely(nr < 0) && (errno == EAGAIN || errno == EINTR))
			continue;
		return nr;
	}
}

ssize_t xpwrite(int fd, const void *buf, size_t count, off_t offset)
{
	const char *p = buf;
	ssize_t total = 0;

	while (count > 0) {
		ssize_t written = _pwrite(fd, p, count, offset);
		if (unlikely(written < 0))
			return -1;
		if (unlikely(!written)) {
			errno = ENOSPC;
			return -1;
		}
		count -= written;
		p += written;
		total += written;
		offset += written;
	}

	return total;
}

mode_t _def_fmode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;

int option_parse_size(const char *value, uint64_t *ret)
{
	char    *postfix;
	double  sizef;

	sizef = strtod(value, &postfix);

	if ((postfix[0] != '\0') && (postfix[1] != '\0')) {
		goto err;
	}

	switch (*postfix)
	{
		case 'P':
		case 'p':
			sizef *= 1024;

		case 'T':
		case 't':
			sizef *= 1024;

		case 'G':
		case 'g':
			sizef *= 1024;

		case 'M':
		case 'm':
			sizef *= 1024;

		case 'K':
		case 'k':
			sizef *= 1024;

		case 'b':
		case 'B':
		case '\0':
			*ret = (uint64_t)sizef;
			break;

		default:
err:
			fprintf(stderr, "Invalid size '%s'", value);
			fprintf(stderr, "You may use B, K, M, G, T or P suffixes for "
					"bytes, kilobytes, megabytes, gigabytes, terabytes and"
					" petabytes.");
			return -1;
	}

	return 0;
}
/****************************************************************/
bool opt_async = false;
bool opt_random = false;
uint64_t opt_bs = (4 << 10);
uint64_t opt_ibs = (4 << 10);
uint64_t opt_obs = (4 << 10);
int opt_count = 0;
int opt_skip = 0;
int opt_seek = 0;
bool opt_direct = false;
int opt_thread = 1;
char *opt_if = NULL;
char *opt_of = NULL;

static struct option longopts[] = {
	{ "help", no_argument,       NULL, 'h' },
	{ "async", no_argument, NULL, 'a' },
	{ "bs", required_argument, NULL, 'b' },
	{ "direct", no_argument, NULL, 'd' },
	{ "if", required_argument, NULL, 'i' },
	{ "of", required_argument, NULL, 'o' },
	{ "random", no_argument, NULL, 'r' },
	{ "thread", required_argument, NULL, 't' },
	{ "count", required_argument, NULL, 'c' },
	{ "ibs", required_argument, NULL, 'I' },
	{ "obs", required_argument, NULL, 'O' },
	{ "skip", required_argument, NULL, 'p' },
	{ "seek", required_argument, NULL, 'k' },
	{ NULL,   0,                 NULL, 0   }
};

static void usage(const char *execfile)
{
	printf("Usage: %s [OPTION...]\n\n", execfile);
	printf("  -a, --async                 enable async, default is sync\n");
	printf("  -b, --bs=size               set bs size, default is 4k\n");
	printf("  -d, --direct                enable direct, default is indirect\n");
	printf("  -i, --if=file               set the input file\n");
	printf("  -o, --of=file               set the output file\n");
	printf("  -r, --random                enable random, default is order\n");
	printf("  -t, --thread=num            set thread io count\n");
	printf("  -c, --count=num             set bs op count, default is file size\n");
	printf("  -I, --ibs=size              set ibs size, default is 4k\n");
	printf("  -O, --obs=size              set obs size, default is 4k\n");
	printf("  -p, --skip=num              set ibs op count, default is 0\n");
	printf("  -k, --seek=num              set obs op count, default is 0\n");
	printf("  -h, --help                  show this message\n\n");
}

int parse(int argc, char *argv[])
{
	int             c;

	while ((c = getopt_long(argc, argv, "ab:di:o:r:t:c:I:O:p:k:h", longopts, NULL)) != EOF) {
		switch (c) {
			case 'a':
				opt_async = true;
				break;

			case 'b':
				option_parse_size(optarg, &opt_bs);
				break;

			case 'd':
				opt_direct = true;
				break;

			case 'i':
				opt_if = optarg;
				break;

			case 'o':
				opt_of = optarg;
				break;

			case 'r':
				opt_random = true;
				break;

			case 't':
				opt_thread = atoi(optarg);
				break;

			case 'c':
				opt_count = atoi(optarg);
				break;

			case 'I':
				option_parse_size(optarg, &opt_ibs);
				break;

			case 'O':
				option_parse_size(optarg, &opt_obs);
				break;

			case 'p':
				opt_skip = atoi(optarg);
				break;

			case 'k':
				opt_seek = atoi(optarg);
				break;

			default:
				usage(argv[0]);
				return 1;
		}
	}
	if (!opt_if || !opt_of) {
		usage(argv[0]);
		return -1;
	}
	return 0;
}
/****************************************************************/
void *aio_manager(void *arg)
{
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

	struct eaio_context *ctx = arg;
	while (1) {
		eaio_context_exec(ctx);

		pthread_testcancel();
	}
	return NULL;
}


uint64_t clock_get_abso_time(void)
{
	struct timespec ts;

	int ret = clock_gettime(CLOCK_MONOTONIC, &ts);

	if (ret < 0) {
		fprintf(stderr, "clock_gettime failure: %m\n");
	}

	return (uint64_t)ts.tv_sec * 1000LL + (uint64_t)ts.tv_nsec / 1000000;
}


int _do_rw(struct eaio_context *aio_ctx, enum eaio_opt opt, int qnum, int prio,
		int fd, void *buf, size_t count, off_t offset)
{
	int ret = 0;

	if (opt_async) {
		ret = eaio_context_rdwt(aio_ctx, opt, qnum, prio,
				fd, buf, count, offset,
				NULL, NULL);
	} else {
		if (opt == EAIO_OPT_PWRITE) {
			ret = xpwrite(fd, buf, count, offset);
		} else {
			ret = xpread(fd, buf, count, offset);
		}
	}

	return ret;
}


off_t g_data_size = 0;
struct eaio_context *g_ctx = NULL;

static inline bool is_aligned_to_pagesize(void *p)
{
	return ((uintptr_t)p & (getpagesize() - 1)) == 0;
}

int do_test_rw(void *data, off_t offset, size_t length)
{
	struct eaio_context *ctx = g_ctx;
	uint64_t if_skip_offset = opt_ibs * opt_skip;
	uint64_t of_seek_offset = opt_obs * opt_seek;

	/*direct适合libaio*/
#define SECTOR_SIZE (1U << 9)
#define sector_algined(x) ({ ((x) & (SECTOR_SIZE - 1)) == 0; })
	int flags = 0;
	if (sector_algined(offset) && sector_algined(length)) {
		if (is_aligned_to_pagesize(data)) {
			flags = opt_direct ? O_DIRECT : 0;
		}
	}

	int rfd = open(opt_if, O_RDONLY | flags);
	if (rfd < 0) {
		fprintf(stderr, "test: Unable to open file \"%s\": %s.\n", opt_if, strerror(errno));
		return -1;
	}
	int ret = _do_rw(ctx, EAIO_OPT_PREAD, 0, 0, rfd, data, length, if_skip_offset + offset);
	assert(ret == length);
	close(rfd);

	int wfd = open(opt_of, O_WRONLY | flags, _def_fmode);
	if (wfd < 0) {
		fprintf(stderr, "test: Unable to open file \"%s\": %s.\n", opt_of, strerror(errno));
		return -1;
	}
	ret = _do_rw(ctx, EAIO_OPT_PWRITE, 1, 0, wfd, data, length, of_seek_offset + offset);
	assert(ret == length);
	close(wfd);
	return 0;
}

void do_test_sequ(long idx)
{
	int i = 0;
	char *data = xvalloc(opt_bs);
	do {
		off_t offset = (i * opt_thread + idx) * opt_bs;
		if (offset >= g_data_size) {
			break;
		}
		size_t length = ((offset + opt_bs) > g_data_size) ? (g_data_size - offset) : opt_bs;

		do_test_rw(data, offset, length);
	} while (++i);
	free(data);
}

void do_test_rand(long idx)
{
	uint64_t max = roundup(g_data_size, opt_bs) / opt_bs;
	uint64_t *map = calloc(max, sizeof(uint64_t));
	uint64_t n = 0;
	for (n = 0; n < max; n++) {
		uint64_t j = n * opt_thread + idx;
		if (j >= max) {
			break;
		}
		map[n] = j;
	}
	xshuffle(map, n, sizeof(uint64_t));

	char *data = xvalloc(opt_bs);
	for (uint64_t i = 0; i < n; i ++) {
		off_t offset = map[i] * opt_bs;
		assert(offset < g_data_size);
		size_t length = ((offset + opt_bs) > g_data_size) ? (g_data_size - offset) : opt_bs;

		do_test_rw(data, offset, length);
	}
	free(data);

	free(map);
}

void *do_test(void *arg)
{
	long idx = (long)arg;

	if (opt_random) {
		/*模拟随机读写*/
		do_test_rand(idx);
	} else {
		/*模拟顺序读写*/
		do_test_sequ(idx);
	}
	return NULL;
}

int go_test(struct eaio_context *aio_ctx)
{
	g_ctx = aio_ctx;

	struct stat stat_buf;
	if (lstat(opt_if, &stat_buf) < 0) {
		fprintf(stderr, "test: Unable to get file \"%s\" type: %s.\n", opt_if, strerror(errno));
		return(-errno);
	}
	if (!S_ISREG(stat_buf.st_mode) && !S_ISBLK(stat_buf.st_mode)) {
		fprintf(stderr, "test: Not support: \"%s\".\n", opt_if);
		return -1;
	}

	int rfd = open(opt_if, O_RDONLY);
	if (rfd < 0) {
		fprintf(stderr, "test: Unable to open file \"%s\": %s.\n", opt_if, strerror(errno));
		return(-errno);
	}
	if (S_ISREG(stat_buf.st_mode)) {
		int rc = fstat(rfd, &stat_buf);
		if (rc != 0) {
			fprintf(stderr, "test: Unable to guess size of file \"%s\": %s.\n", opt_if, strerror(errno));
			return(-errno);
		}

		g_data_size = stat_buf.st_size;
	} else {
		int rc = ioctl(rfd, BLKGETSIZE64, &g_data_size);
		if (rc < 0) {
			fprintf(stderr, "test: Unable to guess size of block \"%s\": %s.\n", opt_if, strerror(errno));
			return(-errno);
		}
	}
	close(rfd);
	if ((opt_ibs * opt_skip) >= g_data_size) {
		fprintf(stderr, "test: Unable to skip %ld size of \"%s\".\n", opt_ibs * opt_skip, opt_if);
		return -1;
	}
	g_data_size -= opt_ibs * opt_skip;
	if (opt_count) {
		g_data_size = MIN(opt_count * opt_bs, g_data_size);
	}


	int wfd = open(opt_of, O_WRONLY | O_TRUNC | O_CREAT, 0644);
	if (wfd < 0) {
		fprintf(stderr, "test: Unable to open file \"%s\": %s.\n", opt_of, strerror(errno));
		return(-errno);
	}
	close(wfd);


	pthread_t tid[opt_thread];
	for (long i = 0; i < opt_thread; i++) {
		int ret = pthread_create(&tid[i], NULL, do_test, (void *)i);
		assert(ret == 0);
	}

	for (int i = 0; i < opt_thread; i++) {
		pthread_join(tid[i], NULL);
	}

	return 0;
}

void start_poll(struct eaio_context *ctx, int fd, int flags)
{
	int ret = eaio_context_rdwt(ctx, EAIO_OPT_POLL, 0, 0,
			fd, (void *)&flags, 0, 0,
			NULL, NULL);
	if (ret < 0) {
		return;
	}

	int events = ret;
	if (events & POLLIN) {
		char buf[256] = {0};
		int rc = read(fd, buf, sizeof(buf));
		if (rc < 0) {
			fprintf(stderr, "poll_test: Error reading from fd: %s\n", strerror(errno));
		} else if (rc == 0) {
			fprintf(stdout, "poll_test: End of File\n");
		} else {
			fprintf(stdout, "poll_test: Read %d bytes :%s\n", rc, buf);
		}
	} else if (events & POLLERR) {
		fprintf(stderr, "poll_test: Error on fd!\n");
	}
}


int main(int argc, char *argv[])
{
	int ret = parse(argc, argv);
	if (ret) {
		return ret;
	}

	/*start*/
	struct eaio_context ctx = {0};
	ret = eaio_context_init(&ctx, 2);
	assert(ret == 0);

	pthread_t tid;
	ret = pthread_create(&tid, NULL, aio_manager, &ctx);
	assert(ret == 0);

	//start_poll(&ctx, STDIN_FILENO, POLLIN | POLLHUP | POLLERR);

	uint64_t beg = clock_get_abso_time();

	go_test(&ctx);

	uint64_t end = clock_get_abso_time();
	printf("Use %ld\n", end - beg);

	pthread_cancel(tid);
	pthread_join(tid, NULL);

	ret = eaio_context_exit(&ctx);
	assert(ret == 0);

	return 0;
}
