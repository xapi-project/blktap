#ifndef _AIO_GETEVENTS_H
#define _AIO_GETEVENTS_H
#include <libaio.h>
int user_io_getevents(io_context_t io_ctx, unsigned int max, struct io_event *uevents);
#endif
