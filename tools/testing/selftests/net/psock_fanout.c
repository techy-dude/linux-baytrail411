/*
 * Copyright 2013 Google Inc.
 * Author: Willem de Bruijn (willemb@google.com)
 *
 * A basic test of packet socket fanout behavior.
 *
 * Control:
 * - create fanout fails as expected with illegal flag combinations
 * - join   fanout fails as expected with diverging types or flags
 *
 * Datapath:
 *   Open a pair of packet sockets and a pair of INET sockets, send a known
 *   number of packets across the two INET sockets and count the number of
 *   packets enqueued onto the two packet sockets.
 *
 *   The test currently runs for
 *   - PACKET_FANOUT_HASH
 *   - PACKET_FANOUT_HASH with PACKET_FANOUT_FLAG_ROLLOVER
 *   - PACKET_FANOUT_LB
 *   - PACKET_FANOUT_CPU
 *   - PACKET_FANOUT_ROLLOVER
 *   - PACKET_FANOUT_CBPF
 *   - PACKET_FANOUT_EBPF
 *
 * Todo:
 * - functionality: PACKET_FANOUT_FLAG_DEFRAG
 *
 * License (GPLv2):
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. * See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _GNU_SOURCE		/* for sched_setaffinity */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/unistd.h>	/* for __NR_bpf */
#include <linux/filter.h>
#include <linux/bpf.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "psock_lib.h"

#define RING_NUM_FRAMES			20

/* Open a socket in a given fanout mode.
 * @return -1 if mode is bad, a valid socket otherwise */
static int sock_fanout_open(uint16_t typeflags, int num_packets)
{
	int fd, val;

	fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_IP));
	if (fd < 0) {
		perror("socket packet");
		exit(1);
	}

	/* fanout group ID is always 0: tests whether old groups are deleted */
	val = ((int) typeflags) << 16;
	if (setsockopt(fd, SOL_PACKET, PACKET_FANOUT, &val, sizeof(val))) {
		if (close(fd)) {
			perror("close packet");
			exit(1);
		}
		return -1;
	}

	pair_udp_setfilter(fd);
	return fd;
}

static void sock_fanout_set_cbpf(int fd)
{
	struct sock_filter bpf_filter[] = {
		BPF_STMT(BPF_LD+BPF_B+BPF_ABS, 80),	      /* ldb [80] */
		BPF_STMT(BPF_RET+BPF_A, 0),		      /* ret A */
	};
	struct sock_fprog bpf_prog;

	bpf_prog.filter = bpf_filter;
	bpf_prog.len = sizeof(bpf_filter) / sizeof(struct sock_filter);

	if (setsockopt(fd, SOL_PACKET, PACKET_FANOUT_DATA, &bpf_prog,
		       sizeof(bpf_prog))) {
		perror("fanout data cbpf");
		exit(1);
	}
}

static void sock_fanout_set_ebpf(int fd)
{
	const int len_off = __builtin_offsetof(struct __sk_buff, len);
	struct bpf_insn prog[] = {
		{ BPF_ALU64 | BPF_MOV | BPF_X,   6, 1, 0, 0 },
		{ BPF_LDX   | BPF_W   | BPF_MEM, 0, 6, len_off, 0 },
		{ BPF_JMP   | BPF_JGE | BPF_K,   0, 0, 1, DATA_LEN },
		{ BPF_JMP   | BPF_JA  | BPF_K,   0, 0, 4, 0 },
		{ BPF_LD    | BPF_B   | BPF_ABS, 0, 0, 0, 0x50 },
		{ BPF_JMP   | BPF_JEQ | BPF_K,   0, 0, 2, DATA_CHAR },
		{ BPF_JMP   | BPF_JEQ | BPF_K,   0, 0, 1, DATA_CHAR_1 },
		{ BPF_ALU   | BPF_MOV | BPF_K,   0, 0, 0, 0 },
		{ BPF_JMP   | BPF_EXIT,          0, 0, 0, 0 }
	};
	char log_buf[512];
	union bpf_attr attr;
	int pfd;

	memset(&attr, 0, sizeof(attr));
	attr.prog_type = BPF_PROG_TYPE_SOCKET_FILTER;
	attr.insns = (unsigned long) prog;
	attr.insn_cnt = sizeof(prog) / sizeof(prog[0]);
	attr.license = (unsigned long) "GPL";
	attr.log_buf = (unsigned long) log_buf,
	attr.log_size = sizeof(log_buf),
	attr.log_level = 1,

	pfd = syscall(__NR_bpf, BPF_PROG_LOAD, &attr, sizeof(attr));
	if (pfd < 0) {
		perror("bpf");
		fprintf(stderr, "bpf verifier:\n%s\n", log_buf);
		exit(1);
	}

	if (setsockopt(fd, SOL_PACKET, PACKET_FANOUT_DATA, &pfd, sizeof(pfd))) {
		perror("fanout data ebpf");
		exit(1);
	}

	if (close(pfd)) {
		perror("close ebpf");
		exit(1);
	}
}

static char *sock_fanout_open_ring(int fd)
{
	struct tpacket_req req = {
		.tp_block_size = getpagesize(),
		.tp_frame_size = getpagesize(),
		.tp_block_nr   = RING_NUM_FRAMES,
		.tp_frame_nr   = RING_NUM_FRAMES,
	};
	char *ring;
	int val = TPACKET_V2;

	if (setsockopt(fd, SOL_PACKET, PACKET_VERSION, (void *) &val,
		       sizeof(val))) {
		perror("packetsock ring setsockopt version");
		exit(1);
	}
	if (setsockopt(fd, SOL_PACKET, PACKET_RX_RING, (void *) &req,
		       sizeof(req))) {
		perror("packetsock ring setsockopt");
		exit(1);
	}

	ring = mmap(0, req.tp_block_size * req.tp_block_nr,
		    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ring == MAP_FAILED) {
		perror("packetsock ring mmap");
		exit(1);
	}

	return ring;
}

static int sock_fanout_read_ring(int fd, void *ring)
{
	struct tpacket2_hdr *header = ring;
	int count = 0;

	while (count < RING_NUM_FRAMES && header->tp_status & TP_STATUS_USER) {
		count++;
		header = ring + (count * getpagesize());
	}

	return count;
}

static int sock_fanout_read(int fds[], char *rings[], const int expect[])
{
	int ret[2];

	ret[0] = sock_fanout_read_ring(fds[0], rings[0]);
	ret[1] = sock_fanout_read_ring(fds[1], rings[1]);

	fprintf(stderr, "info: count=%d,%d, expect=%d,%d\n",
			ret[0], ret[1], expect[0], expect[1]);

	if ((!(ret[0] == expect[0] && ret[1] == expect[1])) &&
	    (!(ret[0] == expect[1] && ret[1] == expect[0]))) {
		fprintf(stderr, "ERROR: incorrect queue lengths\n");
		return 1;
	}

	return 0;
}

/* Test illegal mode + flag combination */
static void test_control_single(void)
{
	fprintf(stderr, "test: control single socket\n");

	if (sock_fanout_open(PACKET_FANOUT_ROLLOVER |
			       PACKET_FANOUT_FLAG_ROLLOVER, 0) != -1) {
		fprintf(stderr, "ERROR: opened socket with dual rollover\n");
		exit(1);
	}
}

/* Test illegal group with different modes or flags */
static void test_control_group(void)
{
	int fds[2];

	fprintf(stderr, "test: control multiple sockets\n");

	fds[0] = sock_fanout_open(PACKET_FANOUT_HASH, 20);
	if (fds[0] == -1) {
		fprintf(stderr, "ERROR: failed to open HASH socket\n");
		exit(1);
	}
	if (sock_fanout_open(PACKET_FANOUT_HASH |
			       PACKET_FANOUT_FLAG_DEFRAG, 10) != -1) {
		fprintf(stderr, "ERROR: joined group with wrong flag defrag\n");
		exit(1);
	}
	if (sock_fanout_open(PACKET_FANOUT_HASH |
			       PACKET_FANOUT_FLAG_ROLLOVER, 10) != -1) {
		fprintf(stderr, "ERROR: joined group with wrong flag ro\n");
		exit(1);
	}
	if (sock_fanout_open(PACKET_FANOUT_CPU, 10) != -1) {
		fprintf(stderr, "ERROR: joined group with wrong mode\n");
		exit(1);
	}
	fds[1] = sock_fanout_open(PACKET_FANOUT_HASH, 20);
	if (fds[1] == -1) {
		fprintf(stderr, "ERROR: failed to join group\n");
		exit(1);
	}
	if (close(fds[1]) || close(fds[0])) {
		fprintf(stderr, "ERROR: closing sockets\n");
		exit(1);
	}
}

static int test_datapath(uint16_t typeflags, int port_off,
			 const int expect1[], const int expect2[])
{
	const int expect0[] = { 0, 0 };
	char *rings[2];
	uint8_t type = typeflags & 0xFF;
	int fds[2], fds_udp[2][2], ret;

	fprintf(stderr, "test: datapath 0x%hx\n", typeflags);

	fds[0] = sock_fanout_open(typeflags, 20);
	fds[1] = sock_fanout_open(typeflags, 20);
	if (fds[0] == -1 || fds[1] == -1) {
		fprintf(stderr, "ERROR: failed open\n");
		exit(1);
	}
	if (type == PACKET_FANOUT_CBPF)
		sock_fanout_set_cbpf(fds[0]);
	else if (type == PACKET_FANOUT_EBPF)
		sock_fanout_set_ebpf(fds[0]);

	rings[0] = sock_fanout_open_ring(fds[0]);
	rings[1] = sock_fanout_open_ring(fds[1]);
	pair_udp_open(fds_udp[0], PORT_BASE);
	pair_udp_open(fds_udp[1], PORT_BASE + port_off);
	sock_fanout_read(fds, rings, expect0);

	/* Send data, but not enough to overflow a queue */
	pair_udp_send(fds_udp[0], 15);
	pair_udp_send_char(fds_udp[1], 5, DATA_CHAR_1);
	ret = sock_fanout_read(fds, rings, expect1);

	/* Send more data, overflow the queue */
	pair_udp_send_char(fds_udp[0], 15, DATA_CHAR_1);
	/* TODO: ensure consistent order between expect1 and expect2 */
	ret |= sock_fanout_read(fds, rings, expect2);

	if (munmap(rings[1], RING_NUM_FRAMES * getpagesize()) ||
	    munmap(rings[0], RING_NUM_FRAMES * getpagesize())) {
		fprintf(stderr, "close rings\n");
		exit(1);
	}
	if (close(fds_udp[1][1]) || close(fds_udp[1][0]) ||
	    close(fds_udp[0][1]) || close(fds_udp[0][0]) ||
	    close(fds[1]) || close(fds[0])) {
		fprintf(stderr, "close datapath\n");
		exit(1);
	}

	return ret;
}

static int set_cpuaffinity(int cpuid)
{
	cpu_set_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpuid, &mask);
	if (sched_setaffinity(0, sizeof(mask), &mask)) {
		if (errno != EINVAL) {
			fprintf(stderr, "setaffinity %d\n", cpuid);
			exit(1);
		}
		return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	const int expect_hash[2][2]	= { { 15, 5 },  { 20, 5 } };
	const int expect_hash_rb[2][2]	= { { 15, 5 },  { 20, 15 } };
	const int expect_lb[2][2]	= { { 10, 10 }, { 18, 17 } };
	const int expect_rb[2][2]	= { { 15, 5 },  { 20, 15 } };
	const int expect_cpu0[2][2]	= { { 20, 0 },  { 20, 0 } };
	const int expect_cpu1[2][2]	= { { 0, 20 },  { 0, 20 } };
	const int expect_bpf[2][2]	= { { 15, 5 },  { 15, 20 } };
	int port_off = 2, tries = 5, ret;

	test_control_single();
	test_control_group();

	/* find a set of ports that do not collide onto the same socket */
	ret = test_datapath(PACKET_FANOUT_HASH, port_off,
			    expect_hash[0], expect_hash[1]);
	while (ret && tries--) {
		fprintf(stderr, "info: trying alternate ports (%d)\n", tries);
		ret = test_datapath(PACKET_FANOUT_HASH, ++port_off,
				    expect_hash[0], expect_hash[1]);
	}

	ret |= test_datapath(PACKET_FANOUT_HASH | PACKET_FANOUT_FLAG_ROLLOVER,
			     port_off, expect_hash_rb[0], expect_hash_rb[1]);
	ret |= test_datapath(PACKET_FANOUT_LB,
			     port_off, expect_lb[0], expect_lb[1]);
	ret |= test_datapath(PACKET_FANOUT_ROLLOVER,
			     port_off, expect_rb[0], expect_rb[1]);

	ret |= test_datapath(PACKET_FANOUT_CBPF,
			     port_off, expect_bpf[0], expect_bpf[1]);
	ret |= test_datapath(PACKET_FANOUT_EBPF,
			     port_off, expect_bpf[0], expect_bpf[1]);

	set_cpuaffinity(0);
	ret |= test_datapath(PACKET_FANOUT_CPU, port_off,
			     expect_cpu0[0], expect_cpu0[1]);
	if (!set_cpuaffinity(1))
		/* TODO: test that choice alternates with previous */
		ret |= test_datapath(PACKET_FANOUT_CPU, port_off,
				     expect_cpu1[0], expect_cpu1[1]);

	if (ret)
		return 1;

	printf("OK. All tests passed\n");
	return 0;
}
