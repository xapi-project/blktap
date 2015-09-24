/* This define is needed for vsyslog(). */
#define _BSD_SOURCE

#include "thinprovd_log.h"

#include <syslog.h>
#include <stdarg.h>

#define THINPROVD "THINPROVD"

static int
thinpd_match_log_level(enum THINPD_LOG_LEVEL tpd_log_level)
{
    int log_level;

    switch (tpd_log_level) {
    case THINPD_LOG_ERR:
        log_level = LOG_EMERG;
        break;
    case THINPD_LOG_INFO:
        log_level = LOG_INFO;
        break;
    case THINPD_LOG_DBG:
        log_level = LOG_DEBUG;
        break;
    default:
        log_level = LOG_INFO;
    }

    return log_level;
}

inline void
thinpd_openlog(void)
{
    openlog(THINPROVD, LOG_CONS | LOG_PID, LOG_DAEMON);
}

inline void
thinpd_closelog(void)
{
    closelog();
}

inline void
thinpd_log_upto(enum THINPD_LOG_LEVEL tpd_log_level)
{
    setlogmask(LOG_UPTO(thinpd_match_log_level(tpd_log_level)));
}

void
thinpd_log_err(const char *message, ...)
{
    va_list args;

    va_start(args, message);

    /* THINPD_LOG_ERR == LOG_EMERG */
    vsyslog(LOG_EMERG, message, args);

    va_end(args);
}

void
thinpd_log_info(const char *message, ...)

{
    va_list args;

    va_start(args, message);

    /* THINPD_LOG_INFO == LOG_INFO */
    vsyslog(LOG_INFO, message, args);

    va_end(args);
}

void
thinpd_log_dbg(const char *message, ...)
{
    va_list args;

    va_start(args, message);

    /* THINPD_LOG_DBG == LOG_DEBUG */
    vsyslog(LOG_DEBUG, message, args);

    va_end(args);
}
