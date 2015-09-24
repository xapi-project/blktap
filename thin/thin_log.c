/* This define is needed for vsyslog(). */
#define _BSD_SOURCE

#include "thin_log.h"

#include <syslog.h>
#include <stdarg.h>

static int
thin_match_log_level(enum THIN_LOG_LEVEL tpd_log_level)
{
    int log_level;

    switch (tpd_log_level) {
    case THIN_LOG_ERR:
        log_level = LOG_EMERG;
        break;
    case THIN_LOG_INFO:
        log_level = LOG_INFO;
        break;
    case THIN_LOG_DBG:
        log_level = LOG_DEBUG;
        break;
    default:
        log_level = LOG_INFO;
    }

    return log_level;
}

inline void
thin_openlog(char *logname)
{
    openlog(logname, LOG_CONS | LOG_PID, LOG_DAEMON);
}

inline void
thin_closelog(void)
{
    closelog();
}

inline void
thin_log_upto(enum THIN_LOG_LEVEL tpd_log_level)
{
    setlogmask(LOG_UPTO(thin_match_log_level(tpd_log_level)));
}

void
thin_log_err(const char *message, ...)
{
    va_list args;

    va_start(args, message);

    /* THIN_LOG_ERR == LOG_EMERG */
    vsyslog(LOG_EMERG, message, args);

    va_end(args);
}

void
thin_log_info(const char *message, ...)

{
    va_list args;

    va_start(args, message);

    /* THIN_LOG_INFO == LOG_INFO */
    vsyslog(LOG_INFO, message, args);

    va_end(args);
}

void
thin_log_dbg(const char *message, ...)
{
    va_list args;

    va_start(args, message);

    /* THIN_LOG_DBG == LOG_DEBUG */
    vsyslog(LOG_DEBUG, message, args);

    va_end(args);
}
