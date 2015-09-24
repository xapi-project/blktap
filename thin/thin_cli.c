#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include "payload.h"

static void usage(char *);

int
main(int argc, char *argv[]) {
	struct payload message;
	struct thin_conn_handle *ch;
	int arg;
	int opt_idx = 0, flag = 1;
	int ret;
	char vg_name[256];
	mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
	const struct option longopts[] = {
		{ "add", required_argument, NULL, 0 },
		{ "del", required_argument, NULL, 0 },
		{ 0, 0, 0, 0 }
	};

	init_payload(&message);
	message.type = PAYLOAD_CLI;

	/* We expect at least one valid option and, if more, the others
	   are discarded
	*/
	while ((arg = getopt_long(argc, argv, "h",
				  longopts, &opt_idx)) != -1 && flag) {
		switch(arg) {
		case 0:
			/* master: it is fine to have a string with trailing spaces */
			ret = snprintf(message.path, PAYLOAD_MAX_PATH_LENGTH,
				       "%s %s", longopts[opt_idx].name, optarg);
			if (ret >= PAYLOAD_MAX_PATH_LENGTH) {
				fprintf(stderr, "input too long\n");
				return 2;
			}
			flag = 0;
			break;
		case 's':
			ret = snprintf(message.path, IP_MAX_LEN, "%s %s",
				       longopts[opt_idx].name, optarg);
			if (ret >= IP_MAX_LEN) {
				fprintf(stderr, "input too long\n");
				return 2;
			}
			flag = 0;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	/* there must be at least one valid option */
	if(flag) {
		usage(argv[0]);
		return 1;
	}

	ch = thin_connection_create();
	if (ch == NULL) {
		fprintf(stderr, "connection initialization failed,"
			" maybe thinprovd is not running?\n");
		return 1;
	}	
	ret = thin_sync_send_and_receive(ch, &message);
	if(ret) {
		fprintf(stderr, "socket error (%d)\n", ret);
		return -ret;
	}

	thin_connection_destroy(ch);

	if(message.err_code == THIN_ERR_CODE_SUCCESS) {
		/* The request has been successful, so we record it
		 * creating or deleting a VG file in THINPROVD_DIR. In
		 * this way if thinprovd will restart it will find all
		 * the VGs that had been added previously inside
		 * THINPROVD_DIR and it will we able to add them
		 * back.
		 */
		sprintf(vg_name, "%s/%s", THINPROVD_DIR, &message.path[4]);
		if (strncmp("add ", message.path, 4) == 0) {
			ret = open(vg_name, O_CREAT|O_RDONLY|O_EXCL, mode);
			if (ret == -1) {
				if (errno == 17) {
					printf("%s already added\n",
					       &message.path[4]);
				} else {
					fprintf(stderr, "failed to create"
						" %s errno=%d\n", 
						vg_name, errno);
				}
			} else {
				printf("%s added\n", &message.path[4]);
			}
			close(ret);
		} else {
			ret = unlink(vg_name);
			if (ret == -1) {
				if (errno == 2) {
					printf("%s already deleted\n",
					       &message.path[4]);
				} else {
					fprintf(stderr, "failed to unlink"
						" %s errno=%d\n", 
						vg_name, errno);
				}
			} else {
				printf("%s deleted\n", &message.path[4]);
			}
		}
		return 0;
	} else {
		fprintf(stderr, "operation failed: err_code=%d\n",
			message.err_code);
		return message.err_code;
	}
}

static void
usage(char *prog_name)
{
	printf("usage: %s -h\n", prog_name);
	printf("usage: %s --add <volume group name>\n", prog_name);
	printf("usage: %s --del <volume group name>\n", prog_name);
}
