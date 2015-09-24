#include <stdlib.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <netinet/in.h> /* TCP accept client info */
#include <arpa/inet.h> /* TCP accept client info */
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/queue.h> /* non POSIX */
#include <sys/eventfd.h> /* non POSIX */
#include <poll.h>
#include <stdbool.h>
#include <signal.h>
#include "blktap.h"
#include "payload.h"
#include "thin_log.h"
#include "kpr_util.h"

#define BACKLOG 5
#define PORT_NO 7777

static inline int process_payload(struct payload *);
static void process_out_queue(void);
static inline int req_reply(struct payload *);
static int handle_resize(struct payload * buf);
static int handle_status(struct payload * buf);
static int handle_cli(struct payload *);
static void * worker_thread(void *);
static int slave_worker_hook(struct payload *);
static int increase_size(off64_t size, const char * path);
static void parse_cmdline(int, char **);
static int do_daemon(void);
static void split_command(char *, char **);
static int add_vg(char *vg);
static int del_vg(char *vg);
static int signal_set(int signo, void (*func) (int));
static void clean_handler(int signo);

#ifdef THIN_REFRESH_LVM
static int refresh_lvm(const char * path);
#endif /* THIN_REFRESH_LVM */

bool master; /* no need to be mutex-ed: main writes, workers read */
pthread_mutex_t ip_mtx = PTHREAD_MUTEX_INITIALIZER; /* see above */

/* queue structures */
SIMPLEQ_HEAD(sqhead, sq_entry);
struct kpr_queue {
	struct sqhead qhead;
	pthread_mutex_t mtx;
	int efd; /* some queues are notified by eventfd */
} *out_queue;
struct sq_entry {
	struct payload data;
	SIMPLEQ_ENTRY(sq_entry) entries;
};

/* thread structures */
struct kpr_thread_info {
	pthread_t thr_id;
	struct kpr_queue *r_queue;
	int (*hook)(struct payload *);
	int (*net_hook)(struct payload *);
	bool net;
};

/* list structures */
LIST_HEAD(vg_list_head, vg_entry);
struct kpr_vg_list {
	struct vg_list_head head;
	pthread_mutex_t mtx;
} vg_pool;
struct vg_entry {
	char name[PAYLOAD_MAX_PATH_LENGTH];
	struct kpr_thread_info thr;
	struct kpr_queue *r_queue;
	LIST_ENTRY(vg_entry) entries;
};

static struct vg_entry * vg_pool_find(char *, bool);
static struct vg_entry * vg_pool_find_and_remove(char *);


int daemonize;


static struct kpr_queue *
alloc_init_queue(void)
{
	struct kpr_queue *sqp;

	sqp = malloc(sizeof(*sqp));
	if (sqp) {
		SIMPLEQ_INIT(&sqp->qhead);
		if (pthread_mutex_init(&sqp->mtx, NULL) != 0)
			goto out;
		if ( (sqp->efd = eventfd(0, EFD_CLOEXEC|EFD_NONBLOCK|
				    EFD_SEMAPHORE)) == -1 )
			goto out;
	}
	return sqp;

out:
	free(sqp);
	return NULL;
}

static void
free_queue(struct kpr_queue *sqp)
{
	if (sqp) {
		close(sqp->efd);
		free(sqp);
	}
}

static inline struct sq_entry *
get_req_from_queue(struct kpr_queue *q)
{
	struct sq_entry *req = NULL;
	uint64_t ebuf;

	pthread_mutex_lock(&q->mtx);
	if (!SIMPLEQ_EMPTY(&q->qhead)) {
		/* pop from requests queue */
		req = SIMPLEQ_FIRST(&q->qhead);
		SIMPLEQ_REMOVE_HEAD(&q->qhead, entries);

	} else {
		/* clear the fd so we can go back to poll */
		eventfd_read(q->efd, &ebuf);
	}
	pthread_mutex_unlock(&q->mtx);
	return req;
}

static inline void
put_req_into_queue(struct kpr_queue *q, struct sq_entry *req )
{
	bool notify_consumer;

	pthread_mutex_lock(&q->mtx);
	if (SIMPLEQ_EMPTY(&q->qhead)) {
		notify_consumer = true;
	} else {
		notify_consumer = false;
	}
	SIMPLEQ_INSERT_TAIL(&q->qhead, req, entries);
	pthread_mutex_unlock(&q->mtx);

	/* Notify after releasing the mutex, since the receiver 
	 * is probably in a poll, so when he gets notified the mutex
	 * is probably available */
	if (notify_consumer == true) {
		eventfd_write(q->efd, 1);
	}
}

/**
 * Signal handler to clean-up socket file on exit
 *
 * This is very basic and it assumes it is registered once the socket file
 * is created, so no checks.
 * FIXME: did not give much thought to its behaviour in multi-threaded env
 *
 * @param[in] signo the signal to handle
 */
static void clean_handler(int signo)
{
	unlink(THIN_CONTROL_SOCKET);
	_exit(0);
}


/**
 * Set a new signal handler for the specified signal
 *
 * @param[in] signo the signal to handle
 * @return the same as sigaction
 */
static int
signal_set(int signo, void (*func) (int))
{
	struct sigaction new_act;
	struct sigaction old_act;
	int r;

	new_act.sa_handler = func;
	sigemptyset(&new_act.sa_mask);
	new_act.sa_flags = 0;	

	r = sigaction(signo, &new_act, &old_act);
	if (r < 0)
		thin_log_err("Signal %d: handler registration failed",
			     signo);
	return r;
}

static int
add_previously_added_vgs(void)
{
	DIR *dir;
	struct dirent *ent;
	int ret;

	if ((dir = opendir(THINPROVD_DIR)) != NULL) {
		/* Wen need to call add_vg for every file in this
		 * directory excluding '.' and '..' since these files
		 * were added as a consequence of a successfull
		 * 'thin-cli --add <VG>' command
		 */
		while ((ent = readdir(dir)) != NULL) {
			if (ent->d_name[0] != '.') {
				thin_log_info("adding VG %s\n", ent->d_name);
				ret = add_vg(ent->d_name);
				if (ret != 0) {
					thin_log_info("failed to add VG %s\n",
						ent->d_name);
				}
			}
		}
		closedir(dir);
	} else {
		/* could not open directory */
		thin_log_err("could not open %s\n", THINPROVD_DIR);
		return errno;
	}
	return 0;
}

int
main(int argc, char *argv[]) {

	struct pollfd fds[2];
	nfds_t maxfds = 2;
	struct sockaddr_un sv_addr, cl_addr;
	int sfd;
	socklen_t len;
	ssize_t ret;
	int poll_ret;
	struct payload buf;
	mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

	thin_openlog("THINPROVD");
	thin_log_upto(THIN_LOG_INFO);

	/* Init pool */
	LIST_INIT(&vg_pool.head);
	if (pthread_mutex_init(&vg_pool.mtx, NULL) != 0)
		return 1;	

	/* Init default queues */
	out_queue = alloc_init_queue();
	if(!out_queue)
		return 1; /*no free: return from main */

	daemonize = 0;

	/* accept command line opts */
	parse_cmdline(argc, argv);

	/* daemonize if required */
	if (do_daemon() == -1)
		return 1; /* can do better */

	ret = mkdir(THINPROVD_DIR, mode);
	if (ret == -1) {
		if (errno == EEXIST) {
			/* If there are some volume groups files in
			 * this directory, we need to add the
			 * corresponding VGs back.  This is because
			 * some logic was able to successfully add
			 * them and is relying on that, so it is not
			 * going to do an other "add" to the newly
			 * started thinprovd.
			 */
			thin_log_info("adding previously added vgs\n");
			ret = add_previously_added_vgs();
			if (ret != 0) {
				thin_log_info(
					"failed to add previously added vgs\n");
			}
		} else {
			thin_log_err("failed to create %s errno=%d\n", 
				     THINPROVD_DIR, errno);
			return errno;
		}
	}

	sfd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC, 0);
	if (sfd == -1)
		return -errno;

	memset(&sv_addr, 0, sizeof(struct sockaddr_un));
	sv_addr.sun_family = AF_UNIX;
	strncpy(sv_addr.sun_path, THIN_CONTROL_SOCKET, sizeof(sv_addr.sun_path) - 1);

	if (bind(sfd, (struct sockaddr *) &sv_addr, sizeof(struct sockaddr_un)) == -1) {
		thin_log_err("bind failed, %s", strerror(errno));
		return -errno;
	}

	signal_set(SIGINT, clean_handler);
	signal_set(SIGTERM, clean_handler);

	fds[0].fd = out_queue->efd;
	fds[0].events = POLLIN;
	fds[1].fd = sfd;
	fds[1].events = POLLIN;

	for(;;) {
		poll_ret = poll(fds, maxfds, -1); /* wait for ever */
		if ( poll_ret < 1 ) { /* 0 not expected */
			thin_log_info("poll returned %d, %s\n", 
				      poll_ret, strerror(errno));
			continue;
		}

		if (fds[0].revents) {
			/* process out_queue until empty*/
			process_out_queue();
		}

		if (fds[1].revents) {
recv:
			len = sizeof(struct sockaddr_un);
			/* read from the control socket */
			ret = recvfrom(sfd, &buf, sizeof(buf), 0, 
					&cl_addr, &len);
			if (ret == -1 && errno == EINTR)
				goto recv;
			if (ret != sizeof(buf)) {
				thin_log_err("recvfrom returned %ld, %s\n",
					     (long)ret, strerror(errno));
				continue;
			}
			/* Packet of expected len arrived, process it*/
			process_payload(&buf);

			/* Send the acknowledge packet */
send:
			ret = sendto(sfd, &buf, ret, 0, &cl_addr, len);
			if (ret == -1 && errno == EINTR)
				goto send;
			if(ret != sizeof(buf)) {
				thin_log_err("sendto returned %ld, %s\n",
					     (long)ret, strerror(errno));
			}
		}
	}
}

static void
process_out_queue(void)
{
	struct payload buf;
	struct sq_entry *req;

	for (;;) {
		req = get_req_from_queue(out_queue);
		if (req == NULL)
			break;

		/* Process the req */
		buf = req->data;
		switch (buf.cb_type) {
		case PAYLOAD_CB_NONE:
			/* Just free the req, no async response 
			 * was needed */
			thin_log_info("Processed CB_NONE req\n");
			break;
		case PAYLOAD_CB_SOCK:
			/* FIXME:
			 * We do not expect somebody to use this 
			 * for now */
			thin_log_err("CB_SOCK not implemented yet\n");
			break;
		default:
			thin_log_err("cb_type unknown\n");
		}
		free(req);
	}
}

static inline int
process_payload(struct payload * buf)
{
	int err;

	print_payload(buf);
	err = req_reply(buf);
	print_payload(buf);
	thin_log_info("EOM\n\n");

	return err;
}

static int
req_reply(struct payload * buf)
{
	switch (buf->type) {
	case PAYLOAD_RESIZE:
		handle_resize(buf);
		break;
	case PAYLOAD_CLI:
		handle_cli(buf);
		break;
	case PAYLOAD_STATUS:
		handle_status(buf);
		break;
	default:
		buf->type = PAYLOAD_UNDEF;
		print_payload(buf);
	}
	return 0;
}

static int
handle_resize(struct payload * buf)
{
	struct sq_entry *req;
	struct vg_entry *vgentry;
	struct kpr_queue *in_queue;
	char vgname[PAYLOAD_MAX_PATH_LENGTH];
	char lvname[PAYLOAD_MAX_PATH_LENGTH];

	if( kpr_split_lvm_path(buf->path, vgname, lvname) ) {
		/* Fail request, malformed path */
		buf->err_code = THIN_ERR_CODE_FAILURE;
		return 1;
	}

	/* search we have a queue for it */
	vgentry = vg_pool_find(vgname, true);
	if (vgentry) /* we do */
		in_queue = vgentry->r_queue; /* no lock (sure?) */
	else {
		/* Fail request, vg unknown */ 
		buf->err_code = THIN_ERR_CODE_FAILURE;
		return 1;
	}

	req = malloc(sizeof(struct sq_entry));
	if(!req)
		return 1;

	req->data = *buf;
	buf->err_code = THIN_ERR_CODE_SUCCESS;

	put_req_into_queue(in_queue, req);
	return 0;
}

static int
handle_status(struct payload * buf)
{
	/* This is just returning SUCCESS for now */
	buf->err_code = THIN_ERR_CODE_SUCCESS;
	return 0;
}

static int
handle_cli(struct payload * buf)
{
	char command[PAYLOAD_MAX_PATH_LENGTH];
	char *cmd[2];
	int ret;

	/* we reuse the path field for CLI */
	strcpy(command, buf->path);

	split_command(command, cmd);
	if(!cmd[0])
		return 1;

	if (!strcmp("add", cmd[0])) {
		if(!cmd[1])
			return 1;
		ret = add_vg(cmd[1]);
		
	}
	else if (!strcmp("del", cmd[0])) {
		if(!cmd[1])
			return 1;
		ret = del_vg(cmd[1]);
	}
	else
		ret = 1;

	if (ret)
		buf->err_code = THIN_ERR_CODE_FAILURE;
	else
		buf->err_code = THIN_ERR_CODE_SUCCESS;

	return 0;
}

static void *
worker_thread(void * ap)
{
	struct sq_entry * req;
	struct payload * data;
	struct kpr_thread_info *thr_arg;
	struct kpr_queue *r_queue;
	struct pollfd fds[1];
	int maxfds = 1;
	int poll_ret;
	int (*hook)(struct payload *);

	/* We must guarantee this structure is properly polulated or
	   check it and fail in case it is not. In the latter case
	   we need to check if the thread has returned.
	*/
	thr_arg = (struct kpr_thread_info *) ap;
	r_queue = thr_arg->r_queue;
	hook = thr_arg->hook;

	/* Register events for poll */
	fds[0].fd = r_queue->efd;
	fds[0].events = POLLIN;

	for(;;) {
		req = get_req_from_queue(r_queue);
		if (req == NULL) {
			/* wait until there is something in the queue */
			for (;;) {
				/* wait for ever */
				poll_ret = poll(fds, maxfds, -1);
				if ( poll_ret < 1 ) { /* 0 not expected */
					thin_log_info("poll returned %d, %s\n", 
						      poll_ret, strerror(errno));
					continue;
				}
				if (fds[0].revents)
					break;
			}
			/* try again to get a req after the poll */
			continue;
		}

		data = &req->data;
		/* For the time being we use PAYLOAD_UNDEF as a way
		   to notify threads to exit
		*/
		if (data->type == PAYLOAD_UNDEF) {
			free(req);
			thin_log_info("Thread cancellation received\n");
			return NULL;
		}

		/* Execute worker-thread specific hook */
		hook(data);

		/* push to out queue */
		put_req_into_queue(out_queue, req);
	}
	return NULL;
}

static int
slave_worker_hook(struct payload *data)
{
	int ret;

	/* Fulfil request */
	ret = increase_size(data->req_size, data->path);
	if (ret == 0 || ret == 3) /* 3 means big enough */
		data->err_code = THIN_ERR_CODE_SUCCESS;
	else
		data->err_code = THIN_ERR_CODE_FAILURE;
	thin_log_info("worker_thread: completed %s (%d)\n\n",
	       data->path, ret);
	/* FIXME:
	 * Probably we do not need to call refresh_lvm, leaving the
	 * code here commented so we do not forget that before it was
	 * called from the slave as a result of a resize from the 
	 * done from master */
#ifdef THIN_REFRESH_LVM
	refresh_lvm(data->path);
#endif /* THIN_REFRESH_LVM */
	return 0;
}

/**
 * @param size: current size to increase in bytes
 * @param path: device full path
 * @return command return code if command returned properly, -1 otherwise
 */
static int
increase_size(off64_t size, const char * path)
{
#define NCHARS 16
	pid_t pid;
	int status, num_read;
	char ssize[NCHARS]; /* enough for G bytes */

	/* prepare size for command line */
	num_read = snprintf(ssize, NCHARS, "%"PRIu64"b", size);
	if (num_read >= NCHARS)
		return -1; /* size too big */

	switch (pid = fork()) {
	case -1:
		return -1;
	case 0: /* child */
		execl("/usr/sbin/xlvhd-resize", "xlvhd-resize", ssize,
		      path, (char *)NULL);
		_exit(127); /* TBD */
	default: /* parent */
		if (waitpid(pid, &status, 0) == -1)
			return -1;
		else if (WIFEXITED(status)) /* normal exit? */
			status = WEXITSTATUS(status);
		else
			return -1;
		return status;
	}
}

#ifdef THIN_REFRESH_LVM
/**
 * @param path: device full path
 * @return command return code if command returned properly, -1 otherwise
 */
static int
refresh_lvm(const char * path)
{
	pid_t pid;
	int status;

	switch (pid = fork()) {
	case -1:
		return -1;
	case 0: /* child */
		execl("/usr/sbin/xlvhd-refresh", "xlvhd-refresh", path,
		      (char *)NULL);
		_exit(127); /* TBD */
	default: /* parent */
		if (waitpid(pid, &status, 0) == -1)
			return -1;
		else if (WIFEXITED(status)) /* normal exit? */
			status = WEXITSTATUS(status);
		else
			return -1;
		return status;
	}
}
#endif /* THIN_REFRESH_LVM */

static void
parse_cmdline(int argc, char ** argv)
{
	int arg, fd_open = 0;

	while ((arg = getopt(argc, argv, "dfs:")) != EOF ) {
		switch(arg) {
		case 'd': /* daemonize and close fd */
			daemonize = 1;
			break;
		case 'f': /* if daemonized leave fd open */
			fd_open = 1;
			break;
		default:
			break;
		}
	}
	daemonize += daemonize ? fd_open : 0;
	return;
}


static int
do_daemon()
{
	if (!daemonize)
		return 0;

	return daemon(0, daemonize - 1); /* root dir and close if needed */
}


static void
split_command(char *command, char **cmd_vec)
{
	char *token;
	int i;

	token = strtok(command, " ");
	for(i = 0; token && (i < 2); ++i) {
		cmd_vec[i] = token;
		token = strtok(NULL, " ");
	}

	if (i < 2)
		cmd_vec[1] = '\0';

	return;
}


static int
add_vg(char *vg)
{
	struct vg_entry *p_vg;

	thin_log_info("CLI: add_vg for %s\n",vg);

	/* check we already have it */
	if(vg_pool_find(vg, true)) {
		thin_log_info("%s already added\n", vg);
		return 0;
	}

	/* allocate and init vg_entry */
	p_vg = malloc(sizeof(*p_vg));
	if (!p_vg) {
		thin_log_err("Failed to allocate vg_entry struct\n");
		return 1;
	}

	/* We rely on CLI to avoid truncated name or non-NULL terminated
	   strings. Moreover, by dest is not smaller then src */
	strcpy(p_vg->name, vg);

	/* VG and thread specific thread allocated */
	p_vg->r_queue = alloc_init_queue();
	if(!p_vg->r_queue) {
		thin_log_err("Failed worker queue creation for %s\n",
			     p_vg->name);
		goto out;
	}

	/* Prepare and start VG specific thread */
	p_vg->thr.r_queue = p_vg->r_queue;
	p_vg->thr.hook = slave_worker_hook;
	p_vg->thr.net_hook = NULL;
	if (pthread_create(&p_vg->thr.thr_id, NULL, worker_thread, &p_vg->thr)) {
		thin_log_err("Failed worker thread creation for %s\n",
			     p_vg->name);
		goto out2;
	}

	/* Everything ok. Add vg to pool */
	LIST_INSERT_HEAD(&vg_pool.head, p_vg, entries);

	thin_log_info("Successfully registered VG %s\n", p_vg->name);
	return 0;
out2:
	free(p_vg->r_queue);
out:
	free(p_vg);
	return 1;
}


static int
del_vg(char *vg)
{
	struct vg_entry *p_vg;
	struct sq_entry *req;
	int ret;

	thin_log_info("CLI: del_vg\n");

	/* Once removed from the pool no new requests can be served
	   any more
	*/
	p_vg = vg_pool_find_and_remove(vg);
	if(!p_vg) {
		thin_log_info("Nothing removed\n");
		return 0;
	}

	/* The thread is still able to crunch requests in its queue
	   so we "poison" the queue to stop the thread
	 */
	req = malloc(sizeof(*req));
	if(!req) {
		/* FIXME: we are going to return but the vg is no more in the
		 pool while the thread is still running.
		 We are returning with a runnig thread, not able to receive new
		 requests and 2 memory leaks..
		*/
		thin_log_err("Error with malloc!! Thread still running\n"
			     "and memory leaked\n");
		return 1;
	}
	init_payload(&req->data);
	req->data.type = PAYLOAD_UNDEF;
	/* Insert in queue */
	put_req_into_queue(p_vg->r_queue, req);

	/* Wait for thread to complete */
	ret = pthread_join(p_vg->thr.thr_id, NULL);
	if (ret != 0)
		thin_log_err("Problem joining thread..FIXME\n");

	/*
	 * Thread is dead, let's free resources
	 */
	/* By design the queue must be empty but we check */
	if (!SIMPLEQ_EMPTY(&p_vg->r_queue->qhead))
		thin_log_err("queue not empty, memory leak! FIXME\n");
	free_queue(p_vg->r_queue);
	free(p_vg);

	return 0;
}

/**
 * This function searches the vg_pool for an entry with a given VG name.
 * If invoked with locking no mutexes must be hold
 *
 * @param vg_name name of the volume group to search for
 * @param lock ask for function to take care of locking
 * @return NULL if not in the pool or a pointer to the entry
*/
static struct vg_entry *
vg_pool_find(char *vg_name, bool lock)
{
	struct vg_entry *entry, *ret;
	ret = NULL;

	if(lock)
		pthread_mutex_lock(&vg_pool.mtx);
	LIST_FOREACH(entry, &vg_pool.head, entries) {
		/* looking for exact match */
		if (strcmp(entry->name, vg_name) == 0) {
			ret = entry;
			break;
		}
	}
	if(lock)
		pthread_mutex_unlock(&vg_pool.mtx);

	return ret;
}


/**
 * This function removes from vg_pool the entry named vg_name.
 * The pool lock is automatic so make sure you are not holding
 * any mutex
 *
 * @param vg_name name of the volume group to remove
 * @return NULL if nothing is removed or a pointer to removed item
*/
static struct vg_entry *
vg_pool_find_and_remove(char *vg_name)
{
	struct vg_entry *entry;

	pthread_mutex_lock(&vg_pool.mtx);
	entry = vg_pool_find(vg_name, false);
	if(!entry) {
		pthread_mutex_unlock(&vg_pool.mtx);
		return NULL;
	}
	LIST_REMOVE(entry, entries);
	pthread_mutex_unlock(&vg_pool.mtx);

	return entry;
}

#if 0
/* Leaving this here in case will be usefull later */
static struct sq_entry *
find_and_remove(struct sqhead * head, pid_t id) 
{
	struct sq_entry * entry;
	SIMPLEQ_FOREACH(entry, head, entries) {
		if (entry->data.id == id) {
			SIMPLEQ_REMOVE(head, entry, sq_entry, entries);
			return entry;
		}
	}
	/* No matches */
	return NULL;
}
#endif
