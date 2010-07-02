#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

struct range {
	off64_t         start;
	off64_t         end;
};

struct random_copy_ctx {
	int             sfd;
	int             dfd;
	int             total_chunks;
	struct range   *chunks;
};

static void
usage(const char *app, int err)
{
	printf("usage: %s <src> <dst>\n", app);
	exit(err);
}

static int
random_copy_carve_source(struct random_copy_ctx *ctx)
{
	int err, i, n;
	struct stat64 st;
	off64_t bytes, start;

	err = fstat64(ctx->sfd, &st);
	if (err) {
		perror("stat source");
		return errno;
	}

	n     = 100;
	start = 0;
	bytes = st.st_size;

	ctx->chunks = calloc(n, sizeof(struct range));
	if (!ctx->chunks) {
		printf("calloc failed\n");
		return ENOMEM;
	}

	for (i = 0; start < st.st_size; i++) {
		int chunk;
		off64_t end;

		if (i == n) {
			struct range *new;

			n  *= 2;
			new = realloc(ctx->chunks, n * sizeof(struct range));
			if (!new) {
				free(ctx->chunks);
				ctx->chunks = NULL;
				printf("realloc failed\n");
				return ENOMEM;
			}

			ctx->chunks = new;
		}

		chunk = (random() % (st.st_size / 10)) + 1;
		end = start + chunk;
		if (end >= st.st_size)
			end = st.st_size - 1;

		ctx->chunks[i].start = start;
		ctx->chunks[i].end   = end;

		bytes -= (end - start);
		start  = end + 1;
	}

	ctx->total_chunks = i;

	return 0;
}

static int
random_copy_permute_source(struct random_copy_ctx *ctx)
{
	int i;

	for (i = 0; i < ctx->total_chunks; i++) {
		int idx          = random() % ctx->total_chunks;
		struct range tmp = ctx->chunks[idx];
		ctx->chunks[idx] = ctx->chunks[i];
		ctx->chunks[i]   = tmp;
	}

	return 0;
}

static int
random_copy_init(struct random_copy_ctx *ctx, const char *src, const char *dst)
{
	int err;

	memset(ctx, 0, sizeof(*ctx));
	ctx->sfd = ctx->dfd = -1;

	ctx->sfd = open(src, O_LARGEFILE | O_RDONLY);
	if (ctx->sfd == -1) {
		err = errno;
		perror("opening source");
		goto fail;
	}

	ctx->dfd = open(dst, O_LARGEFILE | O_WRONLY);
	if (ctx->dfd == -1) {
		err = errno;
		perror("opening destination");
		goto fail;
	}

	err = random_copy_carve_source(ctx);
	if (err) {
		printf("failed to carve source: %d\n", err);
		goto fail;
	}

	err = random_copy_permute_source(ctx);
	if (err) {
		printf("failed to permute source: %d\n", err);
		goto fail;
	}

	return 0;

fail:
	close(ctx->sfd);
	close(ctx->dfd);
	memset(ctx, 0, sizeof(*ctx));
	return err;
}

static int
random_copy(struct random_copy_ctx *ctx)
{
	char *buf;
	int i, err;

	for (i = 0; i < ctx->total_chunks; i++) {
		struct range *r = &ctx->chunks[i];
		size_t count    = r->end - r->start + 1;

		buf = calloc(1, count);
		if (!buf) {
			printf("calloc failed\n");
			return ENOMEM;
		}

		fprintf(stderr, "copying 0x%x from 0x%llx\n", count, r->start);

		err = pread(ctx->sfd, buf, count, r->start);
		if (err != count) {
			printf("pread(0x%x 0x%llx) returned 0x%x (%d)\n",
			       count, r->start, err, errno);
			free(buf);
			return (errno ? : EIO);
		}

		err = pwrite(ctx->dfd, buf, count, r->start);
		if (err != count) {
			printf("pwrite(0x%x 0x%llx) returned 0x%x (%d)\n",
			       count, r->start, err, errno);
			free(buf);
			return (errno ? : EIO);
		}

		free(buf);
	}

	return 0;
}

static void
random_copy_close(struct random_copy_ctx *ctx)
{
	close(ctx->sfd);
	close(ctx->dfd);
	free(ctx->chunks);
}

int
main(int argc, char *argv[])
{
	int err;
	char *src, *dst;
	struct random_copy_ctx ctx;

	if (argc != 3)
		usage(argv[0], EINVAL);

	src = argv[1];
	dst = argv[2];

	err = random_copy_init(&ctx, src, dst);
	if (err) {
		printf("failed to init: %d\n", err);
		exit(err);
	}

	err = random_copy(&ctx);
	if (err)
		printf("copy failed: %d\n", err);

	random_copy_close(&ctx);

	return err;
}
