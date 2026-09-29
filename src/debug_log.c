#include "debug_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


static FILE* g_debug_file;
static struct timespec g_debug_start;

int LNCT_DebugInit(int config_enabled, char* path, size_t path_size)
{
	const char* enabled = getenv(LNCT_DEBUG_ENV);
	if (enabled && enabled[0])
		config_enabled = strcmp(enabled, "0") != 0;
	if (!config_enabled)
		return 0;

	const char* log_path = getenv(LNCT_DEBUG_LOG_ENV);
	if (!log_path || !log_path[0])
		log_path = LNCT_DEFAULT_DEBUG_LOG_PATH;
	if (path && path_size)
		snprintf(path, path_size, "%s", log_path);

	/*
	 * The default path is in world-writable /tmp: O_NOFOLLOW refuses a planted
	 * symlink, and O_NONBLOCK plus the seek check refuse a planted FIFO, which
	 * would otherwise block BG3 in this open() or in later writes.  (A seek, not
	 * fstat(), which would raise the glibc requirement to 2.33.)
	 */
	int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0644);
	int not_regular = fd >= 0 && lseek(fd, 0, SEEK_END) < 0;
	if (not_regular)
	{
		close(fd);
		fd = -1;
	}
	if (fd >= 0)
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
	FILE* file = fd >= 0 ? fdopen(fd, "a") : NULL;
	if (!file)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Cannot open debug log %s: %s\n", log_path,
			not_regular ? "not a regular file" : strerror(errno));
		if (fd >= 0)
			close(fd);
		return 0;
	}
	/* Line buffering keeps the log useful if BG3 crashes or is killed. */
	setvbuf(file, NULL, _IOLBF, 0);
	clock_gettime(CLOCK_MONOTONIC, &g_debug_start);
	g_debug_file = file;
	return 1;
}

int LNCT_DebugEnabled(void)
{
	return g_debug_file != NULL;
}

void LNCT_DebugLog(const char* format, ...)
{
	if (!g_debug_file)
		return;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double elapsed = (double)(now.tv_sec - g_debug_start.tv_sec)
		+ (double)(now.tv_nsec - g_debug_start.tv_nsec) / 1000000000.0;

	/* Format into one buffer so lines from the input and camera threads never interleave. */
	char line[1024];
	int prefix = snprintf(line, sizeof(line), "%9.3f ", elapsed);
	va_list args;
	va_start(args, format);
	vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, format, args);
	va_end(args);
	fprintf(g_debug_file, "%s\n", line);
}
