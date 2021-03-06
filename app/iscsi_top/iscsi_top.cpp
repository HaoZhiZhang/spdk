/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/uio.h>

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <algorithm>
#include <map>
#include <vector>

extern "C" {
#include "spdk/trace.h"
#include "iscsi/conn.h"
}

static char *exe_name;
static int g_instance_id = 0;

static void usage(void)
{
	fprintf(stderr, "usage:\n");
	fprintf(stderr, "   %s <option>\n", exe_name);
	fprintf(stderr, "        option = '-i' to specify the instance ID,"
		" (default: 0)\n");
}

static bool
conns_compare(struct spdk_iscsi_conn *first, struct spdk_iscsi_conn *second)
{
	if (first->lcore < second->lcore)
		return true;

	if (first->lcore > second->lcore)
		return false;

	if (first->id < second->id)
		return true;

	return false;
}

static void
print_connections(void)
{
	std::vector<struct spdk_iscsi_conn *>		v;
	std::vector<struct spdk_iscsi_conn *>::iterator	iter;
	size_t			conns_size;
	struct spdk_iscsi_conn	*conns, *conn;
	void			*conns_ptr;
	int			fd, i;
	char			shm_name[64];

	sprintf(shm_name, "spdk_iscsi_conns.%d", g_instance_id);
	fd = shm_open(shm_name, O_RDONLY, 0600);
	if (fd < 0) {
		fprintf(stderr, "Cannot open shared memory: %s\n", shm_name);
		usage();
		exit(1);
	}

	conns_size = sizeof(*conns) * MAX_ISCSI_CONNECTIONS;

	conns_ptr = mmap(NULL, conns_size, PROT_READ, MAP_SHARED, fd, 0);
	if (conns_ptr == NULL) {
		fprintf(stderr, "Cannot mmap shared memory\n");
		exit(1);
	}

	conns = (struct spdk_iscsi_conn *)conns_ptr;

	for (i = 0; i < MAX_ISCSI_CONNECTIONS; i++) {
		if (!conns[i].is_valid) {
			continue;
		}
		v.push_back(&conns[i]);
	}

	stable_sort(v.begin(), v.end(), conns_compare);
	for (iter = v.begin(); iter != v.end(); iter++) {
		conn = *iter;
		printf("lcore %2d conn %3d T:%-8s I:%s (%s)\n",
		       conn->lcore, conn->id,
		       conn->target_short_name, conn->initiator_name,
		       conn->initiator_addr);
	}

	printf("\n");
	munmap(conns, conns_size);
	close(fd);
}

int main(int argc, char **argv)
{
	void			*history_ptr;
	struct spdk_trace_histories *histories;
	struct spdk_trace_history *history;

	uint64_t		tasks_done, last_tasks_done[SPDK_TRACE_MAX_LCORE];
	int			delay, history_fd, i, quit, rc;
	int			tasks_done_delta, tasks_done_per_sec;
	int			total_tasks_done_per_sec;
	struct timeval		timeout;
	fd_set			fds;
	char			ch;
	struct termios		oldt, newt;
	char			spdk_trace_shm_name[64];
	int			op;

	exe_name = argv[0];
	while ((op = getopt(argc, argv, "i:")) != -1) {
		switch (op) {
		case 'i':
			g_instance_id = atoi(optarg);
			break;
		default:
			usage();
			exit(1);
		}
	}

	sprintf(spdk_trace_shm_name, "/iscsi_trace.%d", g_instance_id);
	history_fd = shm_open(spdk_trace_shm_name, O_RDONLY, 0600);
	if (history_fd < 0) {
		fprintf(stderr, "Unable to open history shm %s\n", spdk_trace_shm_name);
		usage();
		exit(1);
	}

	history_ptr = mmap(NULL, sizeof(*histories), PROT_READ, MAP_SHARED, history_fd, 0);
	if (history_ptr == NULL) {
		fprintf(stderr, "Unable to mmap history shm\n");
		exit(1);
	}

	histories = (struct spdk_trace_histories *)history_ptr;

	memset(last_tasks_done, 0, sizeof(last_tasks_done));

	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		history = &histories->per_lcore_history[i];
		last_tasks_done[i] = history->tpoint_count[TRACE_ISCSI_TASK_DONE];
	}

	delay = 1;
	quit = 0;

	tcgetattr(0, &oldt);
	newt = oldt;
	newt.c_lflag &= ~(ICANON);
	tcsetattr(0, TCSANOW, &newt);

	while (1) {

		FD_ZERO(&fds);
		FD_SET(0, &fds);
		timeout.tv_sec = delay;
		timeout.tv_usec = 0;
		rc = select(2, &fds, NULL, NULL, &timeout);

		if (rc > 0) {
			if (read(0, &ch, 1) != 1) {
				fprintf(stderr, "Read error on stdin\n");
				goto cleanup;
			}

			printf("\b");
			switch (ch) {
			case 'd':
				printf("Enter num seconds to delay (1-10): ");
				rc = scanf("%d", &delay);
				if (rc != 1) {
					break;
				} else if (delay < 1 || delay > 10) {
					delay = 1;
				}
				break;
			case 'q':
				quit = 1;
				break;
			default:
				fprintf(stderr, "'%c' not recognized\n", ch);
				break;
			}

			if (quit == 1) {
				break;
			}
		}

		printf("\e[1;1H\e[2J");
		print_connections();
		printf("lcore   tasks\n");
		printf("=============\n");
		total_tasks_done_per_sec = 0;
		for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
			history = &histories->per_lcore_history[i];
			tasks_done = history->tpoint_count[TRACE_ISCSI_TASK_DONE];
			tasks_done_delta = tasks_done - last_tasks_done[i];
			if (tasks_done_delta == 0) {
				continue;
			}
			last_tasks_done[i] = tasks_done;
			tasks_done_per_sec = tasks_done_delta / delay;
			printf("%5d %7d\n", history->lcore, tasks_done_per_sec);
			total_tasks_done_per_sec += tasks_done_per_sec;
		}
		printf("Total %7d\n", total_tasks_done_per_sec);
	}

cleanup:
	tcsetattr(0, TCSANOW, &oldt);

	munmap(history_ptr, sizeof(*histories));
	close(history_fd);

	return (0);
}
