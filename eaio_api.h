#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libaio.h>

#include "list.h"

#define EAIO_PRIO_MAX     2

enum eaio_opt {
	EAIO_OPT_PREAD = 0,
	EAIO_OPT_PWRITE = 1,
	/* Jeff Moyer says io_prep_poll was implemented in Red Hat AS2.1 and RHEL3.
	 * AFAICT, it was never in mainline, and should not be used. --RR */
	EAIO_OPT_POLL = 2
};

struct eaio_queue {
	int i_efd;
	int o_efd;
	struct list_head waiting[EAIO_PRIO_MAX];
	pthread_mutex_t mutex;

	int inflight;
	io_context_t context;
};

struct eaio_context {
	int qcnts;
	struct eaio_queue *qslot;
};


int eaio_context_init(struct eaio_context *aio_ctx, int qmax);

/*confirm no task left before call this function*/
int eaio_context_exit(struct eaio_context *aio_ctx);

int eaio_context_exec(struct eaio_context *aio_ctx);

typedef void (*eaio_watch_fcb_t)(int efd, void *usr);

int eaio_context_rdwt(struct eaio_context *aio_ctx, enum eaio_opt opt, int qnum, int prio,
		int fd, void *buf, size_t count, off_t offset,
		eaio_watch_fcb_t fcb, void *usr);
