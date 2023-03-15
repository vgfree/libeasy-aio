#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

#include "array.h"
#include "etask.h"
#include "eaio_logger.h"
#include "eaio_api.h"

#define EAIO_INFLIGHT_MAX 512

#define DATA_FROM_TASK(t) ((void *)(t))
#define TASK_FROM_DATA(d) ((struct eaio_task *)(d))

struct eaio_task {
	struct list_node node;
	int efd;

	int result;
	int qnum;
	int prio;

	struct iocb iocb;
};

static int eaio_queue_init(struct eaio_queue *qaio)
{
	qaio->i_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (qaio->i_efd < 0) {
		return -1;
	}
	qaio->o_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (qaio->o_efd < 0) {
		close(qaio->i_efd);
		return -1;
	}

	memset(&qaio->context, 0, sizeof(qaio->context));	/*不能少*/
	int ret = io_setup(EAIO_INFLIGHT_MAX, &qaio->context);
	if (ret < 0) {
		close(qaio->i_efd);
		close(qaio->o_efd);
		return -1;
	}

	for (int i = 0; i < EAIO_PRIO_MAX; i++) {
		INIT_LIST_HEAD(&qaio->waiting[i]);
	}
	pthread_mutex_init(&qaio->mutex, NULL);

	qaio->inflight = 0;
	return 0;
}

static int eaio_queue_free(struct eaio_queue *qaio)
{
	close(qaio->i_efd);
	close(qaio->o_efd);
	io_destroy(qaio->context);
	pthread_mutex_destroy(&qaio->mutex);
	return 0;
}

static bool eaio_queue_have_waiting(struct eaio_queue *qaio)
{
	bool have = false;
	pthread_mutex_lock(&qaio->mutex);
	for (int i = 0; i < EAIO_PRIO_MAX; i ++) {
		if (!list_empty(&qaio->waiting[i])) {
			have = true;
			break;
		}
	}
	pthread_mutex_unlock(&qaio->mutex);
	return have;
}

static long eaio_queue_getevents(struct eaio_queue *qaio, long need)
{
	struct io_event events[need];
	//memset(events, 0, sizeof(struct io_event) * need);

	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = 0,
	};

	int nr_events = io_getevents(qaio->context, 1, need, events, &ts);

	for (int i = 0; i < nr_events; i++) {
		struct io_event *ev = &events[i];
		struct eaio_task *task = TASK_FROM_DATA(ev->data);

		task->result = ev->res;
		eventfd_xsend(task->efd, 1);
		qaio->inflight --;
	}
	return nr_events;
}

static int eaio_queue_try_inflight_and_submit(struct eaio_queue *qaio)
{
	struct iocb *iocbp[EAIO_INFLIGHT_MAX];

	int done = 0;
	int todo = EAIO_INFLIGHT_MAX - qaio->inflight;
	pthread_mutex_lock(&qaio->mutex);
	for (int i = 0; todo && (i < EAIO_PRIO_MAX); i ++) {
		while (todo && !list_empty(&qaio->waiting[i])) {
			struct eaio_task *task = list_first_entry(&qaio->waiting[i], struct eaio_task, node);

			io_set_eventfd(&task->iocb, qaio->o_efd);
			iocbp[done] = &task->iocb;

			list_del(&task->node);
			done ++;
			todo --;
		}
	}
	pthread_mutex_unlock(&qaio->mutex);

	if (done) {
		int first = 0;
		do {
			int ret = io_submit(qaio->context, done - first, &iocbp[first]);
			if (ret < 0) {
				eaio_printf(LOG_INFO, "io %d submited ret %d: %s", done - first, ret, strerror(-ret));
				struct eaio_task *task = TASK_FROM_DATA(iocbp[first]->data);
				task->result = ret;
				eventfd_xsend(task->efd, 1);
				first ++;
			} else {
				//eaio_printf(LOG_DEBUG, "io %d submited ret %d", done - first, ret);
				qaio->inflight += ret;
				first += ret;
			}
		} while (first < done);
	}

	return done;
}

int eaio_context_init(struct eaio_context *aio_ctx, int qmax)
{
	if (qmax <= 0) {
		return -1;
	}
	aio_ctx->qslot = calloc(qmax, sizeof(struct eaio_queue));
	aio_ctx->qcnts = qmax;

	int idx = 0;
	for (; idx < qmax; idx++) {
		struct eaio_queue *qaio = &aio_ctx->qslot[idx];
		int ret = eaio_queue_init(qaio);
		if (ret < 0) {
			for (int i = 0; i < idx; i++) {
				eaio_queue_free(&aio_ctx->qslot[i]);
			}
			free(aio_ctx->qslot);
			aio_ctx->qslot = NULL;
			aio_ctx->qcnts = 0;
			return -1;
		}
	}
	return 0;
}

int eaio_context_exit(struct eaio_context *aio_ctx)
{
	for (int i = 0; i < aio_ctx->qcnts; i++) {
		struct eaio_queue *qaio = &aio_ctx->qslot[i];
		eaio_queue_free(qaio);
	}
	free(aio_ctx->qslot);
	aio_ctx->qslot = NULL;
	aio_ctx->qcnts = 0;
	return 0;
}


static int efd_cmp(const void *efd1, const void *efd2)
{
	return bcmp(efd1, efd2, sizeof(int));
}

int eaio_context_exec(struct eaio_context *aio_ctx)
{
	int efds[aio_ctx->qcnts * 2];
	for (int i = 0; i < aio_ctx->qcnts; i++) {
		struct eaio_queue *qaio = &aio_ctx->qslot[i];
		efds[i*2 + 0] = qaio->i_efd;
		efds[i*2 + 1] = qaio->o_efd;
	}

	int evts = eventfd_xwait(efds, aio_ctx->qcnts * 2, -1);
	assert(evts > 0);

	xqsort(efds, evts, efd_cmp);
	for (int j = 0; j < aio_ctx->qcnts; j++) {
		bool follow = false;
		struct eaio_queue *qaio = &aio_ctx->qslot[j];
		if (xbsearch(&qaio->o_efd, efds, evts, efd_cmp)) {
			eventfd_t cnt = 0;
			int ret = eventfd_xrecv(qaio->o_efd, &cnt);
			assert(ret == 0);
			if (cnt > 0) {
				follow = true;
				int get = eaio_queue_getevents(qaio, cnt);
				assert(get == cnt);
			}
		}
		if (xbsearch(&qaio->i_efd, efds, evts, efd_cmp)) {
			eventfd_t cnt = 0;
			int ret = eventfd_xrecv(qaio->i_efd, &cnt);
			assert(ret == 0);
			if (cnt > 0) {
				follow = true;
			}
		}
		if (follow) {
			do {
				eaio_queue_try_inflight_and_submit(qaio);
			} while ((qaio->inflight != EAIO_INFLIGHT_MAX) && eaio_queue_have_waiting(qaio));
		}
	}
	return 0;
}

static void _default_watch_fcb(int efd, void *usr)
{
	eventfd_t val = 0;
	eventfd_xrecv(efd, &val);
}

int eaio_context_rdwt(struct eaio_context *aio_ctx, enum eaio_opt opt, int qnum, int prio,
		int fd, void *buf, size_t count, off_t offset,
		eaio_watch_fcb_t fcb, void *usr)
{
	struct eaio_task task = {};

	INIT_LIST_NODE(&task.node);
	int efd = eventfd(0, 0);
	if (efd < 0) {
		return efd;
	}
	task.efd = efd;

	task.result = 0;
	task.qnum = qnum % aio_ctx->qcnts;
	task.prio = prio % EAIO_PRIO_MAX;

	switch (opt) {
		case EAIO_OPT_PWRITE:
			io_prep_pwrite(&task.iocb, fd, buf, count, offset);
			break;
		case EAIO_OPT_PREAD:
			io_prep_pread(&task.iocb, fd, buf, count, offset);
			break;
		case EAIO_OPT_POLL:
			io_prep_poll(&task.iocb, fd, *(int *)buf);
			break;
		default:
			assert(0);
	}
	task.iocb.data = DATA_FROM_TASK(&task);

	struct eaio_queue *qaio = &aio_ctx->qslot[task.qnum];

retry:
	pthread_mutex_lock(&qaio->mutex);
	list_add_tail(&task.node, &qaio->waiting[task.prio]);
	pthread_mutex_unlock(&qaio->mutex);


	eventfd_xsend(qaio->i_efd, 1);
	if (fcb) {
		fcb(task.efd, usr);
	} else {
		_default_watch_fcb(task.efd, usr);
	}

	if (task.result < 0) {
		errno = -task.result;
		task.result = -1;

		if ((errno == EINTR) || (errno == EAGAIN)) {
			goto retry;
		}
		fprintf(stderr, "failed: %s\n", strerror(errno));
	}

	close(task.efd);

	return task.result;
}
