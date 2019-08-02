#include <stdio.h>
#include <unistd.h>
#include "eaio_logger.h"

static EAIO_LOGGER_IMPL g_logger_cb = NULL;

int eaio_logger_printf(short syslv, const char *func, const char *file, int line, const char *format, ...)
{
	int ret = 0;

	if (g_logger_cb) {
		va_list ap;

		va_start(ap, format);
		ret = g_logger_cb(syslv, func, file, line, format, &ap);
		va_end(ap);
	} else {
		char _logfmt[1024] = {0};
		snprintf(_logfmt, sizeof(_logfmt), "%s|%s:%d|%s\n", func, file, line, format);

		va_list ap;

		va_start(ap, format);
		vdprintf(STDOUT_FILENO, _logfmt, ap);
		va_end(ap);
	}

	return ret;
}

int eaio_logger_setup(EAIO_LOGGER_IMPL lcb)
{
	g_logger_cb = lcb;
	return 0;
}

