/* SPDX-License-Identifier: GPL-2.0 */

static const char *__doc__ = "An In-Kernel IDS based on XDP\n";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <pthread.h>
#include <stdbool.h>

#include <locale.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>

#include <sys/resource.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <bpf/xsk.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if_link.h> /* depend on kernel-headers installed */
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/icmpv6.h>
#include <linux/udp.h>
#include <linux/tcp.h>

#include "common/common_params.h"
#include "common/common_user_bpf_xdp.h"
#include "common/common_libbpf.h"
#include "common/common_xsk.h"

#include "common/xdp_stats_kern_user.h"

/* re2dfa and str2dfa library */
#include "common/re2dfa.h"
#include "common/str2dfa.h"

#include "common_kern_user.h"

#define LINE_BUFFER_MAX 160

static const char *ids_inspect_map_name = "ids_inspect_map";
static const char *xsks_map_name = "xsks_map";
static const char *pattern_file_name = \
		// "./patterns/snort2-community-rules-content.txt";
		"./patterns/patterns.txt";

static const struct option_wrapper long_options[] = {

	{{"help",        no_argument,		NULL, 'h' },
	 "Show help", false},

	{{"dev",         required_argument,	NULL, 'd' },
	 "Operate on device <ifname>", "<ifname>", true},

	{{"queue",	 required_argument,	NULL, 'Q' },
	 "Configure interface receive queue for AF_XDP, default=0"},

	{{"poll-mode",	 no_argument,		NULL, 'p' },
	 "Use the poll() API waiting for packets to arrive"},

	{{"quiet",       no_argument,		NULL, 'q' },
	 "Quiet mode (no output)"},

	{{0, 0, NULL,  0 }, NULL, false}
};

static bool global_exit;

/* Follow struct declaration is for fixing the bug of bpf_map_update_elem */
struct ids_inspect_map_update_value {
	struct ids_inspect_map_value value;
	uint8_t padding[8 - sizeof(struct ids_inspect_map_value)];
};

/*
 * static int re2dfa2map(char *re_string, int map_fd)
 * {
 *     struct DFA_state *dfa;
 *     struct generic_list state_list;
 *     struct DFA_state **state, *next_state;
 *     struct ids_inspect_map_key map_key;
 *     struct ids_inspect_map_value map_value;
 *     int i_state, n_state;
 * 
 *     // Convert the RE string to DFA first
 *     dfa = re2dfa(re_string);
 *     if (!dfa) {
 *         fprintf(stderr, "ERR: can't convert the RE to DFA\n");
 *         return EXIT_FAIL_RE2DFA;
 *     }
 * 
 *     // Save all state in DFA into a generic list
 *     create_generic_list(struct DFA_state *, &state_list);
 *     generic_list_push_back(&state_list, &dfa);
 *     DFA_traverse(dfa, &state_list);
 * 
 *     // Encode each state
 *     n_state = state_list.length;
 *     state = (struct DFA_state **) state_list.p_dat;
 *     for (i_state = 0; i_state < n_state; i_state++, state++) {
 *         (*state)->state_id = i_state;
 *     }
 * 
 *     // Convert dfa to map
 *     state = (struct DFA_state **) state_list.p_dat;
 *     map_key.padding = 0;
 *     map_value.padding = 0;
 *     for (i_state = 0; i_state < n_state; i_state++, state++) {
 *         int i_trans, n_trans = (*state)->n_transitions;
 *         for (i_trans = 0; i_trans < n_trans; i_trans++) {
 *             next_state = (*state)->trans[i_trans].to;
 *             map_key.state = (*state)->state_id;
 *             map_key.unit = (*state)->trans[i_trans].trans_char;
 *             map_value.state = next_state->state_id;
 *             map_value.flag = next_state->flag;
 *             if (bpf_map_update_elem(map_fd, &map_key, &map_value, 0) < 0) {
 *                 fprintf(stderr,
 *                     "WARN: Failed to update bpf map file: err(%d):%s\n",
 *                     errno, strerror(errno));
 *                 return -1;
 *             } else {
 *                 printf("---------------------------------------------------\n");
 *                 printf(
 *                     "New element is added in to map (%s)\n",
 *                     ids_inspect_map_name);
 *                 printf(
 *                     "Key - state: %d, unit: %c\n",
 *                     map_key.state, map_key.unit);
 *                 printf(
 *                     "Value - flag: %d, state: %d\n",
 *                     map_value.flag, map_value.state);
 *                 printf("---------------------------------------------------\n");
 *             }
 *             printf("Insert match (src_state: %d, chars: %d) and action (dst_state: %d)\n",
 *					  map_key.state, map_key.unit, map_value.state);
 *         }
 *     }
 * 
 *     return 0;
 * }
 * 
 * static int get_number_of_nonblank_lines(const char *source_file) {
 *     FILE *fp;
 *     char buf[LINE_BUFFER_MAX];
 *     int count = 0;
 *     if ((fp = fopen(source_file, "r")) == NULL) {
 *         fprintf(stderr, "ERR: can not open the source file\n");
 *         return 0;
 *     } else {
 *         while (fgets(buf, sizeof(buf), fp)) {
 *             // Skip blank line (only '\n')
 *             if (strlen(buf) > 1) {
 *                 count += 1;
 *             }
 *         }
 *     }
 *     fclose(fp);
 *     return count;
 * }
 * 
 * static int get_pattern_list(const char *source_file, char ***pattern_list) {
 *     FILE *fp;
 *     char buf[LINE_BUFFER_MAX];
 *     char *pattern;
 *     int pattern_len = 0;
 *     int pattern_count = 0;
 * 
 *     if ((fp = fopen(source_file, "r")) == NULL) {
 *         fprintf(stderr, "ERR: can not open pattern source file\n");
 *         return -1;
 *     } else {
 *         memset(buf, 0, LINE_BUFFER_MAX);
 *         while (fgets(buf, sizeof(buf), fp)) {
 *             pattern_len = strchr(buf, '\n') - buf;
 *             if (pattern_len == 0) {
 *                 // Skip blank line (only '\n')
 *                 continue;
 *             }
 *             pattern = (char *)malloc(sizeof(char) * pattern_len);
 *             memset(pattern, 0, pattern_len);
 *             memcpy(pattern, buf, pattern_len);
 *             memset(buf, 0, LINE_BUFFER_MAX);
 *             printf("Get pattern with length %d: %s\n", pattern_len, pattern);
 *             (*pattern_list)[pattern_count++] = pattern;
 *         };
 *     }
 *     printf("Total %d patterns fetched\n", pattern_count);
 *     fclose(fp);
 *     return 0;
 * };
 * 
 * static int str2dfa2map(char **pattern_list, int pattern_number, int map_fd) {
 *     struct str2dfa_kv *map_entries;
 *     int i_entry, n_entry;
 *     struct ids_inspect_map_key map_key;
 *     struct ids_inspect_map_value map_value;
 * 
 *     // Convert string to DFA first
 *     n_entry = str2dfa(pattern_list, pattern_number, &map_entries);
 *     if (n_entry < 0) {
 *         fprintf(stderr, "ERR: can't convert the String to DFA/Map\n");
 *         return -1;
 *     } else {
 *         printf("Totol %d entries generated from pattern list\n", n_entry);
 *     }
 * 
 *     // Convert dfa to map
 *     map_key.padding = 0;
 *     map_value.padding = 0;
 *     for (i_entry = 0; i_entry < n_entry; i_entry++) {
 *         map_key.state = map_entries[i_entry].key_state;
 *         map_key.unit = map_entries[i_entry].key_unit;
 *         map_value.state = map_entries[i_entry].value_state;
 *         map_value.flag = map_entries[i_entry].value_flag;
 *         if (bpf_map_update_elem(map_fd, &map_key, &map_value, 0) < 0) {
 *             fprintf(stderr,
 *                 "WARN: Failed to update bpf map file: err(%d):%s\n",
 *                 errno, strerror(errno));
 *             return -1;
 *         } else {
 *             printf("---------------------------------------------------\n");
 *             printf(
 *                 "New element is added in to map (%s)\n",
 *                 ids_inspect_map_name);
 *             printf(
 *                 "Key - state: %d, unit: %c\n",
 *                 map_key.state, map_key.unit);
 *             printf(
 *                 "Value - flag: %d, state: %d\n",
 *                 map_value.flag, map_value.state);
 *             printf("---------------------------------------------------\n");
 *         }
 *     }
 *     printf("Total entries are inserted: %d\n", n_entry);
 *     return 0;
 * }
 */

static int str2dfa2map_fromfile(const char *pattern_file, int ids_map_fd) {
	struct str2dfa_kv *map_entries;
	int i_entry, n_entry;
	int i_cpu, n_cpu = libbpf_num_possible_cpus();
	struct ids_inspect_map_key ids_map_key;
	struct ids_inspect_map_update_value ids_map_values[n_cpu];
	ids_inspect_state value_state;
	accept_state_flag value_flag;

	printf("Number of CPUs: %d\n", n_cpu);

	/* Convert string to DFA first */
	n_entry = str2dfa_fromfile(pattern_file, &map_entries);
	if (n_entry < 0) {
		fprintf(stderr, "ERR: can't convert the String to DFA/Map\n");
		return -1;
	} else {
		printf("Totol %d entries generated from pattern list\n", n_entry);
	}

	/* Initial */
	ids_map_key.padding = 0;
	memset(ids_map_values, 0, sizeof(ids_map_values));
	/* Convert dfa to map */
	for (i_entry = 0; i_entry < n_entry; i_entry++) {
		ids_map_key.state = map_entries[i_entry].key_state;
		ids_map_key.unit = map_entries[i_entry].key_unit;
		value_state = map_entries[i_entry].value_state;
		value_flag = map_entries[i_entry].value_flag;
		for (i_cpu = 0; i_cpu < n_cpu; i_cpu++) {
			ids_map_values[i_cpu].value.state = value_state;
			ids_map_values[i_cpu].value.flag = value_flag;
		}
		if (bpf_map_update_elem(ids_map_fd,
								&ids_map_key, ids_map_values, 0) < 0) {
			fprintf(stderr,
				"WARN: Failed to update bpf map file: err(%d):%s\n",
				errno, strerror(errno));
			return -1;
		} else {
			printf("---------------------------------------------------\n");
			printf(
				"New element is added in to map (%s)\n",
				ids_inspect_map_name);
			printf(
				"Key - state: %d, unit: %c\n",
				ids_map_key.state, ids_map_key.unit);
			printf("Value - state: %d, flag: %d\n", value_state, value_flag);
			printf("---------------------------------------------------\n");
		}
	}
	printf("\nTotal entries are inserted: %d\n\n", n_entry);
	return 0;
}

static void *stats_poll(void *arg)
{
	unsigned int interval = 2;
	struct xsk_socket_info *xsk = arg;
	static struct stats_record previous_stats = { 0 };

	previous_stats.timestamp = xsk_gettime();

	/* Trick to pretty printf with thousands separators use %' */
	setlocale(LC_NUMERIC, "en_US");

	while (!global_exit) {
		sleep(interval);
		xsk->stats.timestamp = xsk_gettime();
		stats_print(&xsk->stats, &previous_stats);
		previous_stats = xsk->stats;
	}
	return NULL;
}

static bool proc_pkt(struct xsk_socket_info *xsk, uint64_t addr, uint32_t len)
{
	int ret, ip_type;
	uint32_t hdr_len = 0;
	uint8_t *pkt = xsk_umem__get_data(xsk->umem->buffer, addr);
	struct ethhdr *eth = (struct ethhdr *) pkt;
	ids_inspect_unit *ids_unit;
	uint32_t tx_idx = 0;


	hdr_len += sizeof(*eth);

	if (len < hdr_len) {
		return false;
	}

	if (ntohs(eth->h_proto) == ETH_P_IP) {
		struct iphdr *iph = (struct iphdr *)(eth + 1);
		ip_type = iph->protocol;
		hdr_len += iph->ihl * 4;
	} else if (ntohs(eth->h_proto) == ETH_P_IPV6) {
		struct ipv6hdr *ip6h = (struct ipv6hdr *)(eth + 1);
		ip_type = ip6h->nexthdr;
		hdr_len += sizeof(*ip6h);
	} else {
		/* Ignore vlan here currently */
		goto sendpkt;
	}

	if (len < hdr_len) {
		return false;
	}

	if (ip_type == IPPROTO_TCP) {
		struct tcphdr *tcph = (struct tcphdr *)(pkt + hdr_len);
		hdr_len += sizeof(*tcph);
	} else if (ip_type == IPPROTO_UDP) {
		struct udphdr *udph = (struct udphdr *)(pkt + hdr_len);
		hdr_len += sizeof(*udph);
	} else {
		goto sendpkt;
	}

	if (len < hdr_len) {
		return false;
	}

	ids_unit = (ids_inspect_unit *)(pkt + hdr_len);

sendpkt:
	/* Here we sent the packet out of the receive port. Note that
	 * we allocate one entry and schedule it. Your design would be
	 * faster if you do batch processing/transmission */

	ret = xsk_ring_prod__reserve(&xsk->tx, 1, &tx_idx);
	if (ret != 1) {
		/* No more transmit slots, drop the packet */
		return false;
	}

	xsk_ring_prod__tx_desc(&xsk->tx, tx_idx)->addr = addr;
	xsk_ring_prod__tx_desc(&xsk->tx, tx_idx)->len = len;
	xsk_ring_prod__submit(&xsk->tx, 1);
	xsk->outstanding_tx++;

	xsk->stats.tx_bytes += len;
	xsk->stats.tx_packets++;
	return true;
}

static void rx_and_process(struct config *cfg,
						   struct xsk_socket_info *xsk_socket)
{
	struct pollfd fds[2];
	int ret, nfds = 1;

	memset(fds, 0, sizeof(fds));
	fds[0].fd = xsk_socket__fd(xsk_socket->xsk);
	fds[0].events = POLLIN;

	while(!global_exit) {
		if (cfg->xsk_poll_mode) {
			ret = poll(fds, nfds, -1);
			if (ret <= 0 || ret > 1)
				continue;
		}
		handle_receive_packets(xsk_socket, proc_pkt);
	}
}

static void exit_application(int signal)
{
	signal = signal;
	global_exit = true;
}

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

const char *pin_basedir = "/sys/fs/bpf";

int main(int argc, char **argv)
{
	int ret, len;
	int ids_map_fd, xsks_map_fd;
	char pin_dir[PATH_MAX];
	struct xsk_umem_info *umem;
	struct xsk_socket_info *xsk_socket;
	pthread_t stats_poll_thread;

	struct config cfg = {
		.ifindex = -1,
		.redirect_ifindex = -1,
	};

	/* Global shutdown handler */
	signal(SIGINT, exit_application);

	/* Cmdline options can change progsec */
	parse_cmdline_args(argc, argv, long_options, &cfg, __doc__);
	if (cfg.redirect_ifindex > 0 && cfg.ifindex == -1) {
		fprintf(stderr, "ERR: required option --dev missing\n\n");
		usage(argv[0], __doc__, long_options, (argc == 1));
		return EXIT_FAIL_OPTION;
	}

	len = snprintf(pin_dir, PATH_MAX, "%s/%s", pin_basedir, cfg.ifname);
	if (len < 0) {
		fprintf(stderr, "ERR: creating pin dirname\n");
		return EXIT_FAIL_OPTION;
	}

	printf("map dir: %s\n", pin_dir);

	/* Open the maps corresponding to the cfg.ifname interface */
	ids_map_fd = open_bpf_map_file(pin_dir, ids_inspect_map_name, NULL);
	if (ids_map_fd < 0) {
		return EXIT_FAIL_BPF;
	}
	xsks_map_fd = open_bpf_map_file(pin_dir, xsks_map_name, NULL);
	if (xsks_map_fd < 0) {
		return EXIT_FAIL_BPF;
	}

	/* Convert the string to DFA and map */
	if (str2dfa2map_fromfile(pattern_file_name, ids_map_fd) < 0) {
		fprintf(stderr, "ERR: can't convert the string to DFA/Map\n");
		return EXIT_FAIL_RE2DFA;
	}

	af_xdp_init(&cfg, xsks_map_fd, &umem, &xsk_socket);
	if (!umem || !xsk_socket) {
		fprintf(stderr, "ERR: can't initialize for AF_XDP\n");
		return EXIT_FAIL_BPF;
	}

    printf("Start to receive and process packets from the data plane...\n\n");
	/* Start thread to do statistics display */
	if (verbose) {
		ret = pthread_create(&stats_poll_thread, NULL, stats_poll,
				     xsk_socket);
		if (ret) {
			fprintf(stderr, "ERROR: Failed creating statistics thread "
				"\"%s\"\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
	}
	/* Receive and count packets than drop them */
	rx_and_process(&cfg, xsk_socket);

	/* Cleanup */
	xsk_socket__delete(xsk_socket->xsk);
	xsk_umem__delete(umem->umem);
	// xdp_link_detach(cfg.ifindex, cfg.xdp_flags, 0);

	return EXIT_OK;
}
