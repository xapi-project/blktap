#ifndef THIN_THINPROVD_LOG
#define THIN_THINPROVD_LOG

/*
 * This header file provides a logging API for thinprovd.
 *
 * Things to note:
 *
 *     1) Messages over 1024 characters will be truncated by syslogd.
 *
 *     2) New line characters, '\n', DO NOT break lines; the octal ASCII
 *        code of the char is printed instead. For every new line you want,
 *        you have to call a variant of 'thinpd_log_X()'
 */

enum THINPD_LOG_LEVEL
{
    THINPD_LOG_ERR,
    THINPD_LOG_INFO,
    THINPD_LOG_DBG
};

void thinpd_openlog (void);
void thinpd_closelog(void);

/*
 * Use this function to specify the LOWEST logging level.
 * Everything from this level and above will be logged.
 */
void thinpd_log_upto(enum THINPD_LOG_LEVEL tpd_log_level);

void thinpd_log_err (const char *message, ...);
void thinpd_log_info(const char *message, ...);
void thinpd_log_dbg (const char *message, ...);

#endif /* THIN_THINPROVD_LOG */
