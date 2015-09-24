#ifndef THIN_LOG
#define THIN_LOG

/*
 * This header file provides a logging API for thin provisioning.
 *
 * Things to note:
 *
 *     1) Messages over 1024 characters will be truncated by syslogd.
 *
 *     2) New line characters, '\n', DO NOT break lines; the octal ASCII
 *        code of the char is printed instead. For every new line you want,
 *        you have to call a variant of 'thin_log_X()'
 */

enum THIN_LOG_LEVEL
{
    THIN_LOG_ERR,
    THIN_LOG_INFO,
    THIN_LOG_DBG
};

void thin_openlog (char *logname);
void thin_closelog(void);

/*
 * Use this function to specify the LOWEST logging level.
 * Everything from this level and above will be logged.
 */
void thin_log_upto(enum THIN_LOG_LEVEL tpd_log_level);

void thin_log_err (const char *message, ...);
void thin_log_info(const char *message, ...);
void thin_log_dbg (const char *message, ...);

#endif /* THIN_LOG */
