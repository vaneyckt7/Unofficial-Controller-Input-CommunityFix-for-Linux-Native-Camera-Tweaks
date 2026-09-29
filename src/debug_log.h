#pragma once

#include <stddef.h>


#define LNCT_DEBUG_ENV "LNCT_DEBUG"
#define LNCT_DEBUG_LOG_ENV "LNCT_DEBUG_LOG"
#define LNCT_DEFAULT_DEBUG_LOG_PATH "/tmp/lnct-debug.log"

/*
 * A non-empty LNCT_DEBUG environment variable decides: "0" disables, any other
 * value enables.  Without it, `config_enabled` (debug_log in the config file)
 * decides.  Returns non-zero when a log file is open; its path is written to `path`.
 */
int LNCT_DebugInit(int config_enabled, char* path, size_t path_size);
int LNCT_DebugEnabled(void);
void LNCT_DebugLog(const char* format, ...) __attribute__((format(printf, 1, 2)));
