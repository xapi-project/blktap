/* Unix domain socket fd receiver */

typedef void (*fd_cb_t) (int fd, char *msg, void *data);

struct td_fdreceiver *td_fdreceiver_start(char *path, fd_cb_t, void *data);
void td_fdreceiver_stop(struct td_fdreceiver *);

struct td_fdreceiver {
  char *path;

  int fd;
  int fd_event_id;

  int client_fd;
  int client_event_id;

  fd_cb_t callback;
  void *callback_data;
};
