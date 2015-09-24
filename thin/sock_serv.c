#include <stdlib.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <netinet/in.h> /* TCP accept client info */
#include <arpa/inet.h> /* TCP accept client info */
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/queue.h> /* non POSIX */
#include <sys/eventfd.h> /* non POSIX */
#include <poll.h>
#include <stdbool.h>
#include "blktap.h"
#include "payload.h"
#include "kpr_util.h"

#define BACKLOG 5
#define PORT_NO 7777

static inline int process_payload(int, struct payload *);
static inline int req_reply(int, struct payload *);
static int handle_request(struct payload * buf);
static int handle_query(struct payload * buf);
static void * worker_thread(void *);
static void * worker_thread_net(void *);
static int slave_worker_hook(struct payload *);
static int reject_hook(struct payload *);
static int dispatch_hook(struct payload *);
static int slave_net_hook(struct payload *);
static int master_net_hook(struct payload *);
static int increase_size(off64_t size, const char * path);
static int refresh_lvm(const char * path);
static void parse_cmdline(int, char **);
static int do_daemon(void);
static int handle_cli(struct payload *);
static void split_command(char *, char **);
static int add_vg(char *vg);
static int del_vg(char *vg);
static int slave_mode(char *ip);
static int master_mode(void);

bool master; /* no need to be mutex-ed: main writes, workers read */
char master_ip[IP_MAX_LEN]; /*
			      Used only in slave mode, ensure it is
			      NULL terminated. This variable is used
			      only in handle_request but, as long as
			      this function is used in the network thread,
			      it must be mutex protected
			    */
pthread_mutex_t ip_mtx = PTHREAD_MUTEX_INITIALIZER; /* see above */


/* queue structures */
SIMPLEQ_HEAD(sqhead, sq_entry);
struct kpr_queue {
	struct sqhead qhead;
	pthread_mutex_t mtx;
	pthread_cond_t cnd;
	int efd; /* some queues are notified by eventfd */
} *net_queue, *out_queue;
struct sq_entry {
	struct payload data;
	SIMPLEQ_ENTRY(sq_entry) entries;
};

static struct sq_entry * find_and_remove(struct sqhead *, pid_t);
static struct kpr_queue * get_out_queue(struct payload *);

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
		if (pthread_cond_init(&sqp->cnd, NULL) != 0)
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

int
main(int argc, char *argv[]) {

	/* default is master mode */
	master = true;

	/* Init pool */
	LIST_INIT(&vg_pool.head);
	if (pthread_mutex_init(&vg_pool.mtx, NULL) != 0)
		return 1;	

	/* Init default queues */
	net_queue = alloc_init_queue();
	if(!net_queue)
		return 1; /*no free: return from main */
	out_queue = alloc_init_queue();
	if(!out_queue)
		return 1; /*no free: return from main */

	daemonize = 0;

	/* accept command line opts */
	parse_cmdline(argc, argv);

	/* daemonize if required */
	if (do_daemon() == -1)
		return 1; /* can do better */

	/* prepare and spawn default thread: use vg_entry even if not VG */
	struct vg_entry net_thr;
	net_thr.thr.r_queue = net_queue;
	net_thr.thr.hook = dispatch_hook;
	if (master)
		net_thr.thr.net_hook = master_net_hook;
	else
		net_thr.thr.net_hook = slave_net_hook;
	net_thr.thr.net = true;
	if (pthread_create(&net_thr.thr.thr_id, NULL, worker_thread_net,
			   &net_thr.thr)) {
		printf("failed worker thread creation\n");
		return 1;
	}


	struct sockaddr_un addr;
	int sfd, cfd;
	ssize_t numRead;
	struct payload buf;

	sfd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
	if (sfd == -1)
		return -errno;

	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, THIN_CONTROL_SOCKET, sizeof(addr.sun_path) - 1);

	if (bind(sfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) == -1)
		return -errno;

	if (listen(sfd, BACKLOG) == -1)
		return -errno;

	for (;;) {

		cfd = accept4(sfd, NULL, NULL, SOCK_CLOEXEC);
		if (cfd == -1)
			return -errno;

		while ((numRead = read(cfd, &buf, sizeof(buf))) > 0) {
			/* temporary: ensure ipaddr is NULL if coming from
			   socket. Remove if network thread is sending packets
			   through the socket
			*/
			buf.ipaddr[0] = '\0';
			process_payload(cfd, &buf);
		}

		if (numRead == -1)
			return -errno;

		if (close(cfd) == -1)
			return -errno;
	}
}

static inline int
process_payload(int fd, struct payload * buf)
{
	int err;

	print_payload(buf);
	err = req_reply(fd, buf);
	print_payload(buf);
	printf("EOM\n\n");

	return err;
}

static int
req_reply(int fd, struct payload * buf)
{
	switch (buf->reply) {
	case PAYLOAD_REQUEST:
		handle_request(buf);
		break;
	case PAYLOAD_QUERY:
		handle_query(buf);
		break;
	case PAYLOAD_CLI:
		handle_cli(buf);
		break;
	default:
		buf->reply = PAYLOAD_UNDEF;
		print_payload(buf);
	}

	/* TBD: very basic write, need a while loop */
	if (write(fd, buf, sizeof(*buf)) != sizeof(*buf))
		return -errno;

	return 0;
}

static int
handle_request(struct payload * buf)
{
	struct sq_entry *req;
	struct vg_entry *vgentry;
	struct kpr_queue *in_queue;
	char vgname[PAYLOAD_MAX_PATH_LENGTH];
	char lvname[PAYLOAD_MAX_PATH_LENGTH];

	if( kpr_split_lvm_path(buf->path, vgname, lvname) )
		return 1;

	/* search we have a queue for it */
	vgentry = vg_pool_find(vgname, true);
	if (vgentry) /* we do */
		in_queue = vgentry->r_queue; /* no lock (sure?) */
	else {
		/* In master mode this means rejected */
		if (master) {
			/* hack to reuse it in net_thread */
			if (buf->ipaddr[0] != '\0')
				return 1;
			in_queue = out_queue;
			buf->reply = PAYLOAD_REJECTED;
		}
		else {
			/* write master address */
			pthread_mutex_lock(&ip_mtx);
			strncpy(buf->ipaddr, master_ip, IP_MAX_LEN);
			pthread_mutex_unlock(&ip_mtx);
			in_queue = net_queue;
		}
	}

	req = malloc(sizeof(struct sq_entry));
	if(!req)
		return 1;

	req->data = *buf;
	buf->reply = PAYLOAD_ACCEPTED;
	pthread_mutex_lock(&in_queue->mtx);
	
	SIMPLEQ_INSERT_TAIL(&in_queue->qhead, req, entries);

	/* Temporary hack for the new event mechanism used by default queue */
	if ( in_queue == net_queue )
		eventfd_write(in_queue->efd, 1);
	else if ( in_queue == out_queue )
		/* no need to signal for out_queue */
		;
	else
		pthread_cond_signal(&in_queue->cnd);
	pthread_mutex_unlock(&in_queue->mtx);

	return 0;
}

static int
handle_query(struct payload * buf)
{
	struct sq_entry * req;

	/* Check we have something ready */
	pthread_mutex_lock(&out_queue->mtx);
	if (SIMPLEQ_EMPTY(&out_queue->qhead)) {
		pthread_mutex_unlock(&out_queue->mtx);
		buf->reply = PAYLOAD_WAIT;
		return 0;
	}

	/* check if we have a served request for this query */
	req = find_and_remove(&out_queue->qhead, buf->id);
	if (req) {
		pthread_mutex_unlock(&out_queue->mtx);
		buf->reply = req->data.reply;
		free(req);
	} else { /* wait */
		pthread_mutex_unlock(&out_queue->mtx);
		buf->reply = PAYLOAD_WAIT;		
	}

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
	else if (!strcmp("slave", cmd[0])) {
		if(!cmd[1])
			return 1;
		ret = slave_mode(cmd[1]);
	}
	else if (!strcmp("master", cmd[0])) {
		ret = master_mode();
	}
	else
		ret = 1;

	if (ret)
		strcpy(buf->path, "fail");
	else
		strcpy(buf->path, "ok");

	return 0;
}


/* This function must be invoked with the corresponding mutex locked */
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


static void *
worker_thread(void * ap)
{
	struct sq_entry * req;
	struct payload * data;
	struct kpr_thread_info *thr_arg;
	struct kpr_queue *r_queue, *o_queue;
	int (*hook)(struct payload *);

	/* We must guarantee this structure is properly polulated or
	   check it and fail in case it is not. In the latter case
	   we need to check if the thread has returned.
	*/
	thr_arg = (struct kpr_thread_info *) ap;
	r_queue = thr_arg->r_queue;
	hook = thr_arg->hook;

	for(;;) {
		pthread_mutex_lock(&r_queue->mtx);

		while (SIMPLEQ_EMPTY(&r_queue->qhead)) {
			pthread_cond_wait(&r_queue->cnd, &r_queue->mtx);
		}

		/* pop from requests queue and unlock */
		req = SIMPLEQ_FIRST(&r_queue->qhead);
		SIMPLEQ_REMOVE_HEAD(&r_queue->qhead, entries);
		pthread_mutex_unlock(&r_queue->mtx);

		data = &req->data;
		/* For the time being we use PAYLOAD_UNDEF as a way
		   to notify threads to exit
		*/
		if (data->reply == PAYLOAD_UNDEF) {
			free(req);
			fprintf(stderr, "Thread cancellation received\n");
			return NULL;
		}

		/* Execute worker-thread specific hook */
		hook(data);

		/* push to out queue */
		o_queue = get_out_queue(data);
		pthread_mutex_lock(&o_queue->mtx);
		SIMPLEQ_INSERT_TAIL(&o_queue->qhead, req, entries);
		pthread_mutex_unlock(&o_queue->mtx);
	}
	return NULL;
}


static void *
worker_thread_net(void * ap)
{
	struct sq_entry * req;
	struct payload * data;
	struct kpr_thread_info *thr_arg;
	struct kpr_queue *r_queue;

	struct pollfd fds[2];
	int maxfds = 2;
	int i;
	int (*hook)(struct payload *);
	int (*net_hook)(struct payload *);

	int sfd, cfd;
	struct payload buf;
	static int len = sizeof(buf);
	struct sockaddr_in c_addr;
	socklen_t c_len;
	char *c;

	uint64_t ebuf;
	int ret;

	/* We must guarantee this structure is properly polulated or
	   check it and fail in case it is not. In the latter case
	   we need to check if the thread has returned.
	*/
	thr_arg = (struct kpr_thread_info *) ap;
	r_queue = thr_arg->r_queue;
	hook = thr_arg->hook;
	net_hook = thr_arg->net_hook;

	/*
	 * Network specific block
	 */
	if (thr_arg->net) {
		/* create tcp socket and listen */
		sfd = kpr_tcp_create(PORT_NO);
		if (sfd < 0)
			return NULL;
	} else {
		sfd = -1;
		maxfds = 1; /* no need to loop more */
	}

	/* register events for poll */
	fds[0].fd = r_queue->efd;
	fds[0].events = POLLIN;
	fds[1].fd = sfd; /* if net=false it is ok to be negative */
	fds[1].events = POLLIN;

	for(;;) {
		ret = poll(fds, maxfds, -1); /* wait for ever */
		if ( ret < 1 ) { /* 0 not expected */
			fprintf(stderr, "poll returned %d\n", ret);
			continue;
		}

		for( i = 0; i < maxfds; ++i) {
			if ( !fds[i].revents )
				continue;
			switch(i) {
			case 0: /* queue request */
				pthread_mutex_lock(&r_queue->mtx);
				/* others using this queue..? */
				if (SIMPLEQ_EMPTY(&r_queue->qhead)) {
					pthread_mutex_unlock(&r_queue->mtx);
					continue;
				}

				/* pop from requests queue and unlock */
				req = SIMPLEQ_FIRST(&r_queue->qhead);
				SIMPLEQ_REMOVE_HEAD(&r_queue->qhead, entries);
				/* notify back we read one el of the queue */
				eventfd_read(r_queue->efd, &ebuf);
				pthread_mutex_unlock(&r_queue->mtx);

				data = &req->data;
				/* For the time being we use PAYLOAD_UNDEF as a way
				   to notify threads to exit
				*/
				if (data->reply == PAYLOAD_UNDEF) {
					fprintf(stderr, "Thread cancellation received\n");
					free(req);
					if(sfd >= 0)
						close(sfd);
					return NULL;
				}

				/* Execute worker-thread specific hook */
				if ( hook(data) ) {
					free(req);
					continue;
				}

				/* push to served queue */
				pthread_mutex_lock(&out_queue->mtx);
				SIMPLEQ_INSERT_TAIL(&out_queue->qhead, req, entries);
				pthread_mutex_unlock(&out_queue->mtx);
				break;
			case 1: /* TCP socket */
				c_len = sizeof(c_addr);
				cfd = accept4(sfd, &c_addr, &c_len, SOCK_CLOEXEC);
				if (cfd == -1) {
					fprintf(stderr, "Accept error\n");
					continue;
				}
				if ( read(cfd, &buf, len) != len ) {
					fprintf(stderr, "TCP read error\n");
					continue;
				}

				req = malloc(sizeof(struct sq_entry));
				if(!req) {
					fprintf(stderr, "Cannot allocate"
						"for TCP packet\n");
					continue;
				}

				req->data = buf;
				buf.reply = PAYLOAD_ACCEPTED;

				/* Always acknowledge we got it */
				/* TBD: very basic write, need a while loop */
				if (write(cfd, &buf, len) != len)
					fprintf(stderr, "TCP not "
						"acknowledged\n");

				/* store sender ipaddr */
				c = inet_ntoa(c_addr.sin_addr);
				strncpy(req->data.ipaddr, c, IP_MAX_LEN);

				/* process payload */
				if ( net_hook(&req->data) ) {
					free(req);
					continue;
				}

				/* push to served queue */
				pthread_mutex_lock(&out_queue->mtx);
				SIMPLEQ_INSERT_TAIL(&out_queue->qhead, req, entries);
				pthread_mutex_unlock(&out_queue->mtx);
				break;
			default: /* it should not happen */
				fprintf(stderr, "what?!?!\n");
			}
		}
	}
	return NULL;
}


static int
slave_worker_hook(struct payload *data)
{
	int ret;

	/* Fulfil request */
	ret = increase_size(data->curr, data->path);
	if (ret == 0 || ret == 3) /* 3 means big enough */
		data->reply = PAYLOAD_DONE;
	else
		data->reply = PAYLOAD_REJECTED;
	printf("worker_thread: completed %u %s (%d)\n\n",
	       (unsigned)data->id, data->path, ret);

	return 0;
}


static int
reject_hook(struct payload *data)
{
	/* Reject request */
	data->reply = PAYLOAD_REJECTED;
	printf("default_thread: No registered VG!\n\n");

	return 0;
}

/**
 * Send packet to specified destination. If send is successful and
 * packet is accepted it returns 1 because there is nothing more
 * to be done. Reply will come on the TCP socket.
 * In master mode packets not sent are discarded, while in slave
 * mode they are queued as rejected.
 *
 * @param[in,out] data to be processed
 * @return 0 if packet is not sent and marked rejected.
 *   1 if sent or to be discarded anyway
 */
static int
dispatch_hook(struct payload *data)
{
	/* Send */
	if ( !kpr_tcp_conn_tx_rx(data->ipaddr, PORT_NO, data ) ) {
		fprintf(stderr, "Dispatch failed\n");
		goto fail;
	}
	/* Check reply */
	if ( data->reply != PAYLOAD_ACCEPTED ) {
		fprintf(stderr, "Payload rejected\n");
		goto fail;
	}

	return 1;
fail:
	return master ? 1 : reject_hook(data);
}


/**
 * Packet can be either DONE or REJECTED, in any other case packet
 * is discarded.
 *
 * @param[in,out] data to be processed
 * @return 0 if packet can be pushed in the "served" queue, 1 otherwise
 */
static int
slave_net_hook(struct payload *data)
{
	switch(data->reply) {
	case PAYLOAD_REJECTED:
		break;
	case PAYLOAD_DONE:
		refresh_lvm(data->path);
		break;
	default:
		fprintf(stderr, "Spurious payload\n");
		return 1;
	}

	return 0;
}


/**
 * Packet can be only a REQUEST, in any other case packet
 * is discarded. If it is a request, we always return 1 because
 * it is either pushed in the proper queue here or discarded.
 *
 * @param[in,out] data to be processed
 * @return 1
 */
static int
master_net_hook(struct payload *data)
{
	/* Check reply */
	if ( data->reply != PAYLOAD_REQUEST ) {
		fprintf(stderr, "Spurious payload\n");
		return 1;
	}

	/* Either way we need to return 1 to avoid further push in queue */
	if ( handle_request(data) )
		fprintf(stderr, "Packet discarded\n");
	return 1;
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
	size += 104857600; /* add 100 MB */

	/* prepare size for command line */
	num_read = snprintf(ssize, NCHARS, "-L""%"PRIu64"b", size);
	if (num_read >= NCHARS)
		return -1; /* size too big */

	switch (pid = fork()) {
	case -1:
		return -1;
	case 0: /* child */
		execl("/opt/xensource/sm/lvhdutil.py", "lvhdutil.py", "extend", ssize,
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
		execl("/sbin/lvchange", "lvchange", "--refresh", path,
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


static void
parse_cmdline(int argc, char ** argv)
{
	int arg, fd_open = 0;

	while ((arg = getopt(argc, argv, "df")) != EOF ) {
		switch(arg) {
		case 'd': /* daemonize and close fd */
			daemonize = 1;
			break;
		case 'f': /* if daemonized leave fd open */
			fd_open = 1;
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

	printf("CLI: add_vg\n");

	/* check we already have it */
	if(vg_pool_find(vg, true)) {
		printf("%s already added\n", vg);
		return 0;
	}

	/* allocate and init vg_entry */
	p_vg = malloc(sizeof(*p_vg));
	if (!p_vg)
		return 1;

	/* We rely on CLI to avoid truncated name or non-NULL terminated
	   strings. Moreover, by dest is not smaller then src */
	strcpy(p_vg->name, vg);

	/* VG and thread specific thread allocated */
	p_vg->r_queue = alloc_init_queue();
	if(!p_vg->r_queue)
		goto out;

	/* Prepare and start VG specific thread */
	p_vg->thr.r_queue = p_vg->r_queue;
	p_vg->thr.hook = slave_worker_hook;
	p_vg->thr.net_hook = NULL;
	if (pthread_create(&p_vg->thr.thr_id, NULL, worker_thread, &p_vg->thr)) {
		fprintf(stderr, "Failed worker thread creation for %s\n",
			p_vg->name);
		goto out2;
	}

	/* Everything ok. Add vg to pool */
	LIST_INSERT_HEAD(&vg_pool.head, p_vg, entries);

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

	printf("CLI: del_vg\n");

	/* Once removed from the pool no new requests can be served
	   any more
	*/
	p_vg = vg_pool_find_and_remove(vg);
	if(!p_vg) {
		fprintf(stderr, "Nothing removed\n");
		return 1;
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
		fprintf(stderr, "Error with malloc!! Thread still running\n"
			"and memory leaked\n");
		return 1;
	}
	init_payload(&req->data);
	req->data.reply = PAYLOAD_UNDEF;
	/* Insert in queue */
	pthread_mutex_lock(&p_vg->r_queue->mtx);
	SIMPLEQ_INSERT_TAIL(&p_vg->r_queue->qhead, req, entries);
	pthread_cond_signal(&p_vg->r_queue->cnd); /* Wake thread if needed */
	pthread_mutex_unlock(&p_vg->r_queue->mtx);

	/* Wait for thread to complete */
	ret = pthread_join(p_vg->thr.thr_id, NULL);
	if (ret != 0)
		fprintf(stderr, "Problem joining thread..FIXME\n");

	/*
	 * Thread is dead, let's free resources
	 */
	/* By design the queue must be empty but we check */
	if (!SIMPLEQ_EMPTY(&p_vg->r_queue->qhead))
		fprintf(stderr, "queue not empty, memory leak! FIXME\n");
	free(p_vg->r_queue);
	free(p_vg);

	return 0;
}


int
slave_mode(char *ipaddr)
{
	fprintf(stderr, "CLI slave %s received\n", ipaddr);
	if (master) {
		fprintf(stderr, "Fake: switching master to slave\n");
	} else {
		fprintf(stderr, "Already in slave mode: checking ip addr\n");
		pthread_mutex_lock(&ip_mtx); /* not really needed.. */
		if ( !strcmp(master_ip, ipaddr) ) {
			fprintf(stderr, "nothing to be done\n");
			goto done;
		}
		strncpy(master_ip, ipaddr, IP_MAX_LEN);
		pthread_mutex_unlock(&ip_mtx);
	}

	return 0;
done:
	pthread_mutex_unlock(&ip_mtx);
	return 0;
}


int
master_mode(void)
{
	fprintf(stderr, "CLI master received\n");
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
	if(!entry)
		return NULL;
	LIST_REMOVE(entry, entries);
	pthread_mutex_unlock(&vg_pool.mtx);

	return entry;
}


static struct kpr_queue *
get_out_queue(struct payload *data)
{
	if ( master && (data->ipaddr[0] != '\0') )
		return net_queue;

	return out_queue;
}
