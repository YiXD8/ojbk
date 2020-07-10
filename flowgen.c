/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2013 Tilera Corporation. All rights reserved.
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
 *     * Neither the name of Tilera Corporation nor the names of its
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
 *
 */

#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>

#include <sys/queue.h>
#include <sys/stat.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_cycles.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_memory.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_string_fns.h>
#include <rte_timer.h>
#include <rte_malloc.h>

#include "testpmd.h"

#define min(X,Y) ((X) < (Y) ? (X) : (Y))
#define max(X,Y) ((X) > (Y) ? (X) : (Y))

#define ENABLE_AEOLUS 0

#define SERVERNUM 9 // including one warm-up server
#define MAX_TIME 10 // in second
static struct ether_addr eth_addr_array[SERVERNUM];
static uint32_t ip_addr_array[SERVERNUM];

/* Output level:
    verbose = 0; stage level; guarantees best performance
    verbose = 1; flow level;
    verbose = 2; packet level;
    verbose = 3; all.           
*/
int verbose        = 0; 
int total_flow_num = 79; // total flows among all servers 
int this_server_id = 1;

/* Configuration files to be placed in app/test-pmd/config/ */
/* The first line (server_id=0) is used for warm-up receiver */
static const char ethaddr_filename[] = "app/test-pmd/config/eth_addr_info.txt";
static const char ipaddr_filename[] = "app/test-pmd/config/ip_addr_info.txt";
/* The first few lines are used for warm-up flows */
static const char flow_filename[] = "app/test-pmd/config/flow_info_incast.txt";

#define DEFAULT_PKT_SIZE 1500
#define L2_LEN sizeof(struct ether_hdr)
#define L3_LEN sizeof(struct ipv4_hdr)
#define L4_LEN sizeof(struct tcp_hdr)
#define HDR_ONLY_SIZE (L2_LEN + L3_LEN + L4_LEN) 

/* Define ip header info */
#define IP_DEFTTL  64   /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN  0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)

/* Define ECN bit */
#define NON_ECT 0x00
#define ECT_0   0x01
#define ECT_1   0x02
#define CE      0x03


/* RPO packet type */
#define RPO_SYNC               0Xff
#define RPO_NORM_PKT           0X10
#define RPO_OPPO_PKT           0X11
#define RPO_GRANT              0X12
#define RPO_GRANT_REQUEST      0x13

/* Redefine TCP header fields for Homa */
#define PKT_TYPE_8BITS tcp_flags
#define FLOW_ID_16BITS rx_win
// Homa grant request header ONLY
#define FLOW_SIZE_LOW_16BITS tcp_urp
#define FLOW_SIZE_HIGH_16BITS cksum
// Homa grant header ONLY
#define PRIORITY_GRANTED_8BITS data_off 
#define SEQ_GRANTED_LOW_16BITS tcp_urp
#define SEQ_GRANTED_HIGH_16BITS cksum
// Homa data header ONLY
#define DATA_LEN_16BITS tcp_urp
// Homa resend request header ONLY
#define DATA_RESEND_16BITS tcp_urp


/* RPO sates */
#define RPO_SEND_READY               0x00
#define RPO_SEND_GRANT_REQUSET       0x01
#define RPO_SEND_SENDING             0x02
#define RPO_SEND_WAITING             0x03
#define RPO_SEND_SENDING_CONGESTION  0x04
#define RPO_SEND_CLOASED             0x05
#define RPO_RECV_READY               0x06
#define RPO_RECV_SENDING_GRANT       0x07
#define RPO_RECV_CLOSED              0x08

/* Homa transport configuration (parameters and variables) */
#define RTT_BYTES (13*((DEFAULT_PKT_SIZE)-(HDR_ONLY_SIZE))) 
#define MAX_GRANT_TRANSMIT_ONE_TIME 32
#define MAX_REQUEST_RETRANSMIT_ONE_TIME 16
#define TIMEOUT 0.01
#define GRANT_INTERVAL 0.000005
#define BURST_THRESHOLD 64

#define SCHEDULED_PRIORITY 1 // Scheduled priority levels
#define UNSCHEDULED_PRIORITY 7 // Unscheduled priority levels
/* Map message size to divided n+1 unscheduled priorities */
static const int prio_cut_off_bytes[] = {2000, 4000, 6000, 8000, 10000, 12000}; 
/* 0-n from low to high priority map to DSCP field (TOS_8BIT=DSCP_6BIT+ECN_2BIT) */
static const uint8_t prio_dscp_map[] = {0x00, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20}; 
//static const uint8_t prio_dscp_map[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; 

double start_cycle, elapsed_cycle;
double flowgen_start_time;
double warm_up_time = 5.0; // in sec
double sync_start_time = 3.0; // within warm_up_time
int    sync_done = 0;
double hz;
struct fwd_stream *global_fs;

struct flow_info {
    uint32_t dst_ip;
    uint32_t src_ip;
    uint16_t dst_port;
    uint16_t src_port;

    uint8_t  flow_state;
    uint32_t flow_size; /* flow total size */
    uint32_t remain_size; /* flow remain size */
    double   start_time;
    double   finish_time;
    int      fct_printed;
    int      flow_finished;

    uint32_t data_seqnum;
    uint32_t data_recv_next; 
    uint32_t granted_seqnum;
    uint8_t  granted_priority;

    /* Used to detect grant request timeout */
    double last_grant_request_sent_time;

    /* Used to avoid duplicate grants and detect timeout */
    double   last_grant_sent_time;
    uint32_t last_grant_granted_seq;
    uint8_t  last_grant_granted_prio;

    /* Stage info */
    double first_request_access_time;
    double first_grant_access_time;
    double wait_close_start_time;
    int    resend_request_counter;

    int receiver_could_echo;
};
struct flow_info *sender_flows;
struct flow_info *receiver_flows;

struct rte_mbuf *sender_pkts_burst[MAX_PKT_BURST];
struct rte_mbuf *receiver_pkts_burst[MAX_PKT_BURST];
int    sender_current_burst_size;
int    receiver_current_burst_size;

/* Sender task states */
int sender_total_flow_num              = 0;
int sender_grant_request_sent_flow_num = 0;
int sender_finished_flow_num           = 0;
int sender_next_unstart_flow_id        = -1;
int sender_current_burst_size          = 0;

/* Receiver task states */
int receiver_total_flow_num      = 0;
int receiver_active_flow_num     = 0; 
int max_receiver_active_flow_num = 0;
int receiver_finished_flow_num   = 0;
int receiver_current_burst_size  = 0;


#define MAX_CONCURRENT_FLOW 10000
int sender_request_sent_flow_array[MAX_CONCURRENT_FLOW];
int sender_active_flow_array[MAX_CONCURRENT_FLOW];
int receiver_active_flow_array[MAX_CONCURRENT_FLOW];

/* Declaration of functions */
static void
main_flowgen(struct fwd_stream *fs);

static void
start_new_flow(void);

static void
start_warm_up_flow(void);

static void
sender_send_pkt(void);

static void
receiver_send_pkt(void);

static void
recv_pkt(struct fwd_stream *fs);

static void
recv_grant_request(struct tcp_hdr *transport_recv_hdr, struct ipv4_hdr *ipv4_hdr);

static void
recv_grant(struct tcp_hdr *transport_recv_hdr);

static void
recv_data(struct tcp_hdr *transport_recv_hdr);

static void
recv_oppo_pkt(struct tcp_hdr *transport_recv_hdr);

static void
recv_resend_request(struct tcp_hdr *transport_recv_hdr);

static void
process_ack(struct tcp_hdr* transport_recv_hdr);

static void
construct_sync(int dst_server_id);

static void
construct_grant_request(uint32_t flow_id);

static void
construct_grant(uint32_t flow_id, uint32_t seq_granted, uint8_t ecn);

static void
construct_data(uint32_t flow_id, int data_type);

static void
construct_resend_request(uint32_t flow_id, uint32_t resend_size);

static void
init(void);

static void
read_config(void);

static inline uint16_t
ip_sum(const unaligned_uint16_t *hdr, int hdr_len);

static inline void
print_ether_addr(const char *what, struct ether_addr *eth_addr);

static inline void
add_sender_grant_request_sent_flow(int flow_id);

static inline void
add_receiver_active_flow(int flow_id);

static inline int
find_next_sender_grant_request_sent_flow(int start_index);

static inline void
remove_sender_grant_request_sent_flow(int flow_id);

static inline void
remove_receiver_active_flow(int flow_id);

static inline void
sort_receiver_active_flow_by_remaining_size(void);

static inline int
find_next_unstart_flow_id(void);

static inline void 
remove_newline(char *str);

static inline uint8_t
map_to_unscheduled_priority(int flow_size);

static inline int
get_src_server_id(uint32_t flow_id, struct flow_info *flows);

static inline int
get_dst_server_id(uint32_t flow_id, struct flow_info *flows);

static inline void
print_elapsed_time(void);

static void
print_fct(void);


static inline uint16_t
ip_sum(const unaligned_uint16_t *hdr, int hdr_len)
{
    uint32_t sum = 0;

    while (hdr_len > 1) {
        sum += *hdr++;
        if (sum & 0x80000000)
            sum = (sum & 0xFFFF) + (sum >> 16);
        hdr_len -= 2;
    }

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return ~sum;
}

static void 
remove_newline(char *str)
{
    for (uint32_t i = 0; i < strlen(str); i++) {
        if (str[i] == '\r' || str[i] == '\n')
            str[i] = '\0';
    }
}

static inline void
print_ether_addr(const char *what, struct ether_addr *eth_addr)
{
    char buf[ETHER_ADDR_FMT_SIZE];
    ether_format_addr(buf, ETHER_ADDR_FMT_SIZE, eth_addr);
    printf("%s%s", what, buf);
}

/* Map flow size to unscheduled priority */
static inline uint8_t
map_to_unscheduled_priority(int flow_size)
{
    for (int i=0; i<UNSCHEDULED_PRIORITY-1; i++) {
        if (flow_size<prio_cut_off_bytes[i])
            return (SCHEDULED_PRIORITY + UNSCHEDULED_PRIORITY - 1 - i);
    }
    return SCHEDULED_PRIORITY;
}


/* Map flow src ip to server id */
static inline int
get_src_server_id(uint32_t flow_id, struct flow_info *flows) 
{
    for (int server_index =0; server_index < SERVERNUM; server_index++) {
        if (flows[flow_id].src_ip == ip_addr_array[server_index]) {
            return server_index; 
        }
    }
    return -1;
}

/* Map flow dst ip to server id */
static inline int
get_dst_server_id(uint32_t flow_id, struct flow_info *flows) 
{
    for (int server_index =0; server_index < SERVERNUM; server_index++) {
        if (flows[flow_id].dst_ip == ip_addr_array[server_index]) {
            return server_index; 
        }
    }
    return -1;
}

static inline void
add_sender_grant_request_sent_flow(int flow_id)
{
    int i;
    for (i=0; i<MAX_CONCURRENT_FLOW; i++) {
        if (sender_request_sent_flow_array[i] < 0) {
            sender_request_sent_flow_array[i] = flow_id;
            break;
        }
    }
    if (i == MAX_CONCURRENT_FLOW) {
        printf("error: run out of memory for add_sender_grant_request_sent_flow\n");
    }
    sender_grant_request_sent_flow_num++;
    sender_next_unstart_flow_id = find_next_unstart_flow_id();
    sender_flows[flow_id].flow_state = RPO_SEND_GRANT_REQUSET; 
    sender_flows[flow_id].first_request_access_time = rte_rdtsc() / (double)hz;
    sender_flows[flow_id].last_grant_request_sent_time = rte_rdtsc() / (double)hz;
}

static inline void
add_receiver_active_flow(int flow_id)
{
    int i;
    for (i=0; i<MAX_CONCURRENT_FLOW; i++) {
        if (receiver_active_flow_array[i] < 0) {
            receiver_active_flow_array[i] = flow_id;
            break;
        }
    }
    if (i == MAX_CONCURRENT_FLOW) {
        printf("error: run out of memory for add_receiver_active_flow\n");
    }
    receiver_active_flow_num++;
    if (receiver_active_flow_num > max_receiver_active_flow_num)
        max_receiver_active_flow_num = receiver_active_flow_num;
    receiver_total_flow_num++;
    receiver_flows[flow_id].flow_state = RPO_RECV_SENDING_GRANT;
}

static inline int
find_next_sender_grant_request_sent_flow(int start_index)
{
    for (int i=start_index; i<MAX_CONCURRENT_FLOW; i++) {
        if (sender_request_sent_flow_array[i] != -1) {
            return i;
        }
    }
    return -1;
}

static inline void
remove_receiver_active_flow(int flow_id)
{
    int i;
    for (i=0; i<MAX_CONCURRENT_FLOW; i++) {
        if (receiver_active_flow_array[i] == flow_id) {
            receiver_active_flow_array[i] = -1;
            break;
        }
    }
    if (i == MAX_CONCURRENT_FLOW) {
        printf("error: cannot find the node for remove_receiver_active_flow\n");
    }
    receiver_active_flow_num--;
    receiver_flows[flow_id].flow_state = RPO_RECV_CLOSED;
    receiver_flows[flow_id].flow_finished = 1;
    receiver_flows[flow_id].finish_time = rte_rdtsc() / (double)hz;
    receiver_finished_flow_num++;
}

static inline void
remove_sender_grant_request_sent_flow(int flow_id)
{
    int i;
    for (i=0; i<MAX_CONCURRENT_FLOW; i++) {
        if (sender_request_sent_flow_array[i] == flow_id) {
            sender_request_sent_flow_array[i] = -1;
            break;
        }
    }
    if (i == MAX_CONCURRENT_FLOW) {
        printf("error: cannot find the node for remove_sender_grant_request_sent_flow\n");
    }
    sender_grant_request_sent_flow_num--;
    sender_flows[flow_id].flow_state = RPO_SEND_WAITING;
    //sender_flows[flow_id].flow_state = HOMA_SEND_GRANT_RECEIVING; 
}

static inline void
sort_receiver_active_flow_by_remaining_size(void)
{
    /* Selection sorting */
    int sort_k = SCHEDULED_PRIORITY; // Sort top SCHEDULED_PRIORITY only for efficiency
    int temp_id;
    for (int i=0; i<sort_k; i++) {
        for (int j=i+1; j<max_receiver_active_flow_num; j++) {
            if (receiver_active_flow_array[j] >= 0) {
                if (receiver_active_flow_array[i] >= 0) {
                    if (receiver_flows[receiver_active_flow_array[i]].remain_size >
                        receiver_flows[receiver_active_flow_array[j]].remain_size) {
                            temp_id = receiver_active_flow_array[i];
                            receiver_active_flow_array[i] = receiver_active_flow_array[j];
                            receiver_active_flow_array[j] = temp_id;
                        }
                }
                else {
                    receiver_active_flow_array[i] = receiver_active_flow_array[j];
                    receiver_active_flow_array[j] = -1;
                }
            }
        }
    }
}

static inline int
find_next_unstart_flow_id(void)
{
    int i;
    for (i=sender_next_unstart_flow_id+1; i<total_flow_num; i++) {
        if (get_src_server_id(i, sender_flows) == this_server_id)
            return i;
    }
    return i;
}

/* Update and print global elapsed time */
static inline void
print_elapsed_time(void)
{
    elapsed_cycle = rte_rdtsc() - start_cycle;
    printf("Time: %lf", elapsed_cycle/(double)hz);
}

static void
print_fct(void)
{
    printf("Summary:\ntotal_flow_num = %d (including %d warm up flows)\n"
        "sender_total_flow_num = %d, sender_finished_flow_num = %d\n"
        "receiver_total_flow_num = %d, receiver_finished_flow_num = %d\n"
        "max_receiver_active_flow_num = %d\n",
        total_flow_num, SERVERNUM-1, sender_total_flow_num, sender_finished_flow_num,
        receiver_total_flow_num, receiver_finished_flow_num, max_receiver_active_flow_num);

    /* print_option: 0 - fct only
                     1 - unfinished flows as well */
    int print_option = 1;
    printf("Sender FCT:\n");
    for (int i=0; i<total_flow_num; i++) {
        if (sender_flows[i].fct_printed == 0 &&
            sender_flows[i].src_ip == ip_addr_array[this_server_id]) {
            if (sender_flows[i].flow_finished == 1)
                printf("flow %d - fct = %lf, start_time = %lf, first_grant_recv_time = %lf\n", i, 
                    sender_flows[i].finish_time - sender_flows[i].first_request_access_time,
                    sender_flows[i].first_request_access_time, sender_flows[i].first_grant_access_time);
            else if (print_option == 1)
                printf("flow %d - flow_size = %u, remain_size = %u, start_time = %lf, "
                    "first_grant_recv_time = %lf\n", i, sender_flows[i].flow_size, 
                    sender_flows[i].remain_size, sender_flows[i].first_request_access_time, 
                    sender_flows[i].first_grant_access_time);
            sender_flows[i].fct_printed = 1;
        }
    }
    printf("Receiver FCT:\n");
    for (int i=0; i<total_flow_num; i++) {
        if (receiver_flows[i].fct_printed == 0 &&
            receiver_flows[i].src_ip == ip_addr_array[this_server_id]) {
            if (receiver_flows[i].flow_finished == 1)
                printf("flow %d - fct = %lf, start_time = %lf, first_grant_send_time = %lf, "
                    "resend_request_counter = %d\n", i, 
                    receiver_flows[i].finish_time - receiver_flows[i].start_time,
                    receiver_flows[i].start_time, receiver_flows[i].first_grant_access_time,
                    receiver_flows[i].resend_request_counter);
            else if (print_option == 1)
                printf("flow %d - flow_size = %u, remain_size = %u, start_time = %lf, "
                    "first_grant_send_time = %lf, resend_request_counter = %d\n", 
                    i, receiver_flows[i].flow_size, receiver_flows[i].remain_size,
                    receiver_flows[i].start_time, receiver_flows[i].first_grant_access_time,
                    receiver_flows[i].resend_request_counter);
            receiver_flows[i].fct_printed = 1;
        }
    }
}

/* Read basic info of server id, mac and ip */
static void
read_config(void)
{
    FILE *fd = NULL;
    char line[256] = {0};
    int  server_id;
    uint32_t src_ip_segment1 = 0, src_ip_segment2 = 0, src_ip_segment3 = 0, src_ip_segment4 = 0;

    /* Read ethernet address info */
    server_id = 0;
    fd = fopen(ethaddr_filename, "r");
    if (!fd)
        printf("%s: no such file\n", ethaddr_filename);
    while (fgets(line, sizeof(line), fd) != NULL) {
        remove_newline(line);
        sscanf(line, "%hhu %hhu %hhu %hhu %hhu %hhu", &eth_addr_array[server_id].addr_bytes[0], 
            &eth_addr_array[server_id].addr_bytes[1], &eth_addr_array[server_id].addr_bytes[2],
            &eth_addr_array[server_id].addr_bytes[3], &eth_addr_array[server_id].addr_bytes[4], 
            &eth_addr_array[server_id].addr_bytes[5]);
        if (verbose > 0) {
           printf("Server id = %d   ", server_id);
           print_ether_addr("mac = ", &eth_addr_array[server_id]);
           printf("\n");
        }
        server_id++;
    }
    fclose(fd);

    /* Read ip address info */
    server_id = 0;
    fd = fopen(ipaddr_filename, "r");
    if (!fd)
        printf("%s: no such file\n", ipaddr_filename);
    while (fgets(line, sizeof(line), fd) != NULL) {
        remove_newline(line);
        sscanf(line, "%u %u %u %u", &src_ip_segment1, &src_ip_segment2, &src_ip_segment3, &src_ip_segment4);
        ip_addr_array[server_id] = IPv4(src_ip_segment1, src_ip_segment2, src_ip_segment3, src_ip_segment4);
        if (verbose > 0) {
            printf("Server id = %d   ", server_id);
            printf("ip = %u.%u.%u.%u (%u)\n", src_ip_segment1, src_ip_segment2, 
                src_ip_segment3, src_ip_segment4, ip_addr_array[server_id]);
        }
        server_id++;
    }
    fclose(fd);
}

/* Init flow info */
static void
init(void)
{
    char     line[256] = {0};
    uint32_t flow_id = 0;
    uint32_t src_ip_segment1 = 0, src_ip_segment2 = 0, src_ip_segment3 = 0, src_ip_segment4 = 0;
    uint32_t dst_ip_segment1 = 0, dst_ip_segment2 = 0, dst_ip_segment3 = 0, dst_ip_segment4 = 0;
    uint16_t udp_src_port = 0, udp_dst_port = 0;
    uint32_t flow_size = 0;
    double   start_time;
    
    FILE *fd = fopen(flow_filename, "r");
    if (!fd)
        printf("%s: no such file\n", flow_filename);

    while (fgets(line, sizeof(line), fd) != NULL) {
        remove_newline(line);
        sscanf(line, "%u %u %u %u %u %u %u %u %u %hu %hu %u %lf", &flow_id, 
            &src_ip_segment1, &src_ip_segment2, &src_ip_segment3, &src_ip_segment4, 
            &dst_ip_segment1, &dst_ip_segment2, &dst_ip_segment3, &dst_ip_segment4, 
            &udp_src_port, &udp_dst_port, &flow_size, &start_time);
        sender_flows[flow_id].src_ip                    = IPv4(src_ip_segment1, src_ip_segment2, 
                                                          src_ip_segment3, src_ip_segment4);
        sender_flows[flow_id].dst_ip                    = IPv4(dst_ip_segment1, dst_ip_segment2, 
                                                          dst_ip_segment3, dst_ip_segment4);
        sender_flows[flow_id].src_port                  = udp_src_port;
        sender_flows[flow_id].dst_port                  = udp_dst_port;
        sender_flows[flow_id].flow_size                 = flow_size;
        sender_flows[flow_id].remain_size               = flow_size;
        sender_flows[flow_id].data_seqnum               = 1;
        sender_flows[flow_id].start_time                = start_time;
        sender_flows[flow_id].first_request_access_time = -1.0;
        sender_flows[flow_id].last_grant_sent_time      = 0;
        sender_flows[flow_id].flow_state                = RPO_SEND_READY;
        sender_flows[flow_id].first_grant_access_time   = -1.0;
        sender_flows[flow_id].wait_close_start_time     = -1.0;

        receiver_flows[flow_id].flow_state = RPO_RECV_READY;

        if (get_src_server_id(flow_id, sender_flows) == this_server_id)
            sender_total_flow_num++;
        
        if (verbose > 0) {
            printf("Flow info: flow_id=%u, src_ip=%u, dst_ip=%u, "
                "src_port=%hu, dst_port=%hu, flow_size=%u, start_time=%lf\n",  
                flow_id, sender_flows[flow_id].src_ip, sender_flows[flow_id].dst_ip, 
                sender_flows[flow_id].src_port, sender_flows[flow_id].dst_port, 
                sender_flows[flow_id].flow_size, sender_flows[flow_id].start_time); 
        }

        if (flow_id == (uint32_t)total_flow_num-2)
            break;
    }
    /* find the first flow to start for this server */
    sender_next_unstart_flow_id = -1;
    sender_next_unstart_flow_id = find_next_unstart_flow_id();
    fclose(fd);

    for (int i=0; i<MAX_CONCURRENT_FLOW; i++) {
        sender_request_sent_flow_array[i] = -1;
        sender_active_flow_array[i] = -1;
        receiver_active_flow_array[i] = -1;
    }

    if (verbose > 0)
        printf("Flow info summary: total_flow_num = %d, sender_total_flow_num = %d\n",
            total_flow_num, sender_total_flow_num);
}

static void
construct_sync(int dst_server_id){
    struct   ether_hdr *eth_hdr;
    struct   ipv4_hdr *ip_hdr;
    struct   tcp_hdr *transport_hdr;
    uint64_t ol_flags, tx_offloads;
    unsigned pkt_size = HDR_ONLY_SIZE;
    
    struct rte_mempool *mbp = current_fwd_lcore()->mbp;
    struct rte_mbuf *pkt = rte_mbuf_raw_alloc(mbp);
    if (!pkt) {
        printf("sync server %d: allocation pkt error", dst_server_id);
    }

    pkt->data_len = pkt_size;
    pkt->next = NULL;

    /* Initialize Ethernet header. */
    eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
    ether_addr_copy(&eth_addr_array[dst_server_id], &eth_hdr->d_addr);
    ether_addr_copy(&eth_addr_array[this_server_id], &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

    /* Initialize IP header. */
    ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
    memset(ip_hdr, 0, L3_LEN);
    ip_hdr->version_ihl     = IP_VHL_DEF;
    ip_hdr->type_of_service = ECT_1; 
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live    = IP_DEFTTL;
    ip_hdr->next_proto_id   = IPPROTO_TCP;
    ip_hdr->packet_id       = 0;
    ip_hdr->src_addr        = rte_cpu_to_be_32(ip_addr_array[this_server_id]);
    ip_hdr->dst_addr        = rte_cpu_to_be_32(ip_addr_array[dst_server_id]);
    ip_hdr->total_length    = RTE_CPU_TO_BE_16(pkt_size - L2_LEN);
    ip_hdr->hdr_checksum    = ip_sum((unaligned_uint16_t *)ip_hdr, L3_LEN);

    /* Initialize transport header. */
    transport_hdr = (struct tcp_hdr *)(ip_hdr + 1);
    transport_hdr->src_port       = 55555;
    transport_hdr->dst_port       = 55555;
    transport_hdr->sent_seq       = 0;
    transport_hdr->recv_ack       = 0;
    transport_hdr->PKT_TYPE_8BITS = RPO_SYNC;
    
    tx_offloads = ports[global_fs->tx_port].dev_conf.txmode.offloads;
    if (tx_offloads & DEV_TX_OFFLOAD_VLAN_INSERT)
        ol_flags = PKT_TX_VLAN_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_QINQ_INSERT)
        ol_flags |= PKT_TX_QINQ_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_MACSEC_INSERT)
        ol_flags |= PKT_TX_MACSEC;

    pkt->nb_segs        = 1;
    pkt->data_len       = pkt_size;
    pkt->pkt_len        = pkt_size;
    pkt->ol_flags       = ol_flags;
    pkt->vlan_tci       = ports[global_fs->tx_port].tx_vlan_id;
    pkt->vlan_tci_outer = ports[global_fs->tx_port].tx_vlan_id_outer;
    pkt->l2_len         = L2_LEN;
    pkt->l3_len         = L3_LEN;

    sender_pkts_burst[sender_current_burst_size] = pkt;
    sender_current_burst_size++;
    if (sender_current_burst_size >= BURST_THRESHOLD) {
        sender_send_pkt();
        sender_current_burst_size = 0;
    }
}

static void
construct_grant_request(uint32_t flow_id)
{
    struct   ether_hdr *eth_hdr;
    struct   ipv4_hdr *ip_hdr;
    struct   tcp_hdr *transport_hdr;
    uint64_t ol_flags, tx_offloads;
    int      dst_server_id;
    unsigned pkt_size = HDR_ONLY_SIZE;
    
    struct rte_mempool *mbp = current_fwd_lcore()->mbp;
    struct rte_mbuf *pkt = rte_mbuf_raw_alloc(mbp);
    if (!pkt) {
        printf("flow_id = %d: allocation pkt error", flow_id);
    }

    pkt->data_len = pkt_size;
    pkt->next = NULL;

    /* Initialize Ethernet header. */
    eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
    dst_server_id = get_dst_server_id(flow_id, sender_flows);
    if (dst_server_id == -1) {
        printf("server error: cannot find server id\n");
    }
    ether_addr_copy(&eth_addr_array[dst_server_id], &eth_hdr->d_addr);
    ether_addr_copy(&eth_addr_array[this_server_id], &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

    /* Initialize IP header. */
    ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
    memset(ip_hdr, 0, L3_LEN);
    ip_hdr->version_ihl     = IP_VHL_DEF;
    ip_hdr->type_of_service = ECT_1; 
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live    = IP_DEFTTL;
    ip_hdr->next_proto_id   = IPPROTO_TCP;
    ip_hdr->packet_id       = 0;
    ip_hdr->src_addr        = rte_cpu_to_be_32(ip_addr_array[this_server_id]);
    ip_hdr->dst_addr        = rte_cpu_to_be_32(sender_flows[flow_id].dst_ip);
    ip_hdr->total_length    = RTE_CPU_TO_BE_16(pkt_size - L2_LEN);
    ip_hdr->hdr_checksum    = ip_sum((unaligned_uint16_t *)ip_hdr, L3_LEN);

    /* Initialize transport header. */
    transport_hdr = (struct tcp_hdr *)(ip_hdr + 1);
    transport_hdr->src_port              = rte_cpu_to_be_16(sender_flows[flow_id].src_port);
    transport_hdr->dst_port              = rte_cpu_to_be_16(sender_flows[flow_id].dst_port);
    transport_hdr->sent_seq              = rte_cpu_to_be_32(sender_flows[flow_id].data_seqnum);
    transport_hdr->recv_ack              = 0;
    transport_hdr->PKT_TYPE_8BITS        = RPO_GRANT_REQUEST;
    transport_hdr->FLOW_ID_16BITS        = rte_cpu_to_be_16((uint16_t)(flow_id & 0xffff));
    transport_hdr->FLOW_SIZE_LOW_16BITS  = rte_cpu_to_be_16((uint16_t)(sender_flows[flow_id].flow_size & 0xffff));
    transport_hdr->FLOW_SIZE_HIGH_16BITS = (uint16_t)((sender_flows[flow_id].flow_size >> 16) & 0xffff);
    
    tx_offloads = ports[global_fs->tx_port].dev_conf.txmode.offloads;
    if (tx_offloads & DEV_TX_OFFLOAD_VLAN_INSERT)
        ol_flags = PKT_TX_VLAN_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_QINQ_INSERT)
        ol_flags |= PKT_TX_QINQ_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_MACSEC_INSERT)
        ol_flags |= PKT_TX_MACSEC;

    pkt->nb_segs        = 1;
    pkt->data_len       = pkt_size;
    pkt->pkt_len        = pkt_size;
    pkt->ol_flags       = ol_flags;
    pkt->vlan_tci       = ports[global_fs->tx_port].tx_vlan_id;
    pkt->vlan_tci_outer = ports[global_fs->tx_port].tx_vlan_id_outer;
    pkt->l2_len         = L2_LEN;
    pkt->l3_len         = L3_LEN;

    sender_pkts_burst[sender_current_burst_size] = pkt;
    sender_current_burst_size++;
    if (sender_current_burst_size >= BURST_THRESHOLD) {
        sender_send_pkt();
        sender_current_burst_size = 0;
    }
}

static void
construct_grant(uint32_t flow_id, uint32_t seq_granted, uint8_t ecn)
{
    struct   ether_hdr *eth_hdr;
    struct   ipv4_hdr *ip_hdr;
    struct   tcp_hdr *transport_hdr;
    uint64_t ol_flags, tx_offloads;
    int      dst_server_id;
    unsigned pkt_size = HDR_ONLY_SIZE;

    struct rte_mempool *mbp = current_fwd_lcore()->mbp;
    struct rte_mbuf *pkt = rte_mbuf_raw_alloc(mbp);
    if (!pkt) {
        printf("flow_id = %d: allocation pkt error", flow_id);
    }

    pkt->data_len = pkt_size;
    pkt->next = NULL;

    /* Initialize Ethernet header. */
    eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
    dst_server_id = get_dst_server_id(flow_id, receiver_flows);
    if (dst_server_id == -1) {
        printf("server error: cannot find server id\n");
    }
    ether_addr_copy(&eth_addr_array[dst_server_id], &eth_hdr->d_addr);
    ether_addr_copy(&eth_addr_array[this_server_id], &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

    /* Initialize IP header. */
    ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
    memset(ip_hdr, 0, L3_LEN);
    ip_hdr->version_ihl     = IP_VHL_DEF;
    ip_hdr->type_of_service = ecn;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live    = IP_DEFTTL;
    ip_hdr->next_proto_id   = IPPROTO_TCP;
    ip_hdr->packet_id       = 0;
    ip_hdr->src_addr        = rte_cpu_to_be_32(ip_addr_array[this_server_id]);
    ip_hdr->dst_addr        = rte_cpu_to_be_32(receiver_flows[flow_id].dst_ip);
    ip_hdr->total_length    = RTE_CPU_TO_BE_16(pkt_size - L2_LEN);
    ip_hdr->hdr_checksum    = ip_sum((unaligned_uint16_t *)ip_hdr, L3_LEN);

    /* Initialize transport header. */
    transport_hdr = (struct tcp_hdr *)(ip_hdr + 1);
    transport_hdr->src_port                = rte_cpu_to_be_16(receiver_flows[flow_id].src_port);
    transport_hdr->dst_port                = rte_cpu_to_be_16(receiver_flows[flow_id].dst_port);
    transport_hdr->sent_seq                = rte_cpu_to_be_32(receiver_flows[flow_id].data_seqnum);
    transport_hdr->recv_ack                = rte_cpu_to_be_32(receiver_flows[flow_id].data_recv_next);
    transport_hdr->PKT_TYPE_8BITS          = RPO_GRANT;
    transport_hdr->FLOW_ID_16BITS          = rte_cpu_to_be_16((uint16_t)(flow_id & 0xffff));
    transport_hdr->PRIORITY_GRANTED_8BITS  = 0;
    transport_hdr->SEQ_GRANTED_LOW_16BITS  = rte_cpu_to_be_16((uint16_t)(seq_granted & 0xffff));
    transport_hdr->SEQ_GRANTED_HIGH_16BITS = (uint16_t)((seq_granted >> 16) & 0xffff);

    tx_offloads = ports[global_fs->tx_port].dev_conf.txmode.offloads;
    if (tx_offloads & DEV_TX_OFFLOAD_VLAN_INSERT)
        ol_flags = PKT_TX_VLAN_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_QINQ_INSERT)
        ol_flags |= PKT_TX_QINQ_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_MACSEC_INSERT)
        ol_flags |= PKT_TX_MACSEC;

    pkt->nb_segs        = 1;
    pkt->data_len       = pkt_size;
    pkt->pkt_len        = pkt_size;
    pkt->ol_flags       = ol_flags;
    pkt->vlan_tci       = ports[global_fs->tx_port].tx_vlan_id;
    pkt->vlan_tci_outer = ports[global_fs->tx_port].tx_vlan_id_outer;
    pkt->l2_len         = L2_LEN;
    pkt->l3_len         = L3_LEN;

    receiver_pkts_burst[receiver_current_burst_size] = pkt;
    receiver_current_burst_size++;
    if (receiver_current_burst_size >= BURST_THRESHOLD) {
        receiver_send_pkt();
        receiver_current_burst_size = 0;
    }
}

static void
construct_data(uint32_t flow_id, int data_type)
{
    struct   ether_hdr *eth_hdr;
    struct   ipv4_hdr *ip_hdr;
    struct   tcp_hdr *transport_hdr;
    uint64_t ol_flags, tx_offloads;
    uint16_t data_len;
    int      dst_server_id;
    unsigned pkt_size;

    /* Data type (data_type): 
        0 normal data pkt
        2 opportunistic pkt/
    if (data_type < 2)
        pkt_size = DEFAULT_PKT_SIZE;
    else
        pkt_size = HDR_ONLY_SIZE;

    struct rte_mempool *mbp = current_fwd_lcore()->mbp;
    struct rte_mbuf *pkt = rte_mbuf_raw_alloc(mbp);
    if (!pkt) {
        printf("flow_id = %d: allocation pkt error", flow_id);
    }

    pkt->data_len = pkt_size;
    pkt->next = NULL;

    /* Initialize Ethernet header. */
    eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
    dst_server_id = get_dst_server_id(flow_id, sender_flows);
    if (dst_server_id == -1) {
        printf("server error: cannot find server id\n");
    }
    ether_addr_copy(&eth_addr_array[dst_server_id], &eth_hdr->d_addr);
    ether_addr_copy(&eth_addr_array[this_server_id], &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

    /* Initialize IP header. */
    ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
    memset(ip_hdr, 0, L3_LEN);
    ip_hdr->version_ihl     = IP_VHL_DEF;
    ip_hdr->type_of_service = ECT_1;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live    = IP_DEFTTL;
    ip_hdr->next_proto_id   = IPPROTO_TCP;
    ip_hdr->packet_id       = 0;
    ip_hdr->src_addr        = rte_cpu_to_be_32(ip_addr_array[this_server_id]);
    ip_hdr->dst_addr        = rte_cpu_to_be_32(sender_flows[flow_id].dst_ip);
    ip_hdr->total_length    = RTE_CPU_TO_BE_16(pkt_size - L2_LEN);
    ip_hdr->hdr_checksum    = ip_sum((unaligned_uint16_t *)ip_hdr, L3_LEN);
    
    /* Initialize transport header. */
    transport_hdr = (struct tcp_hdr *)(ip_hdr + 1);
    transport_hdr->src_port       = rte_cpu_to_be_16(sender_flows[flow_id].src_port);
    transport_hdr->dst_port       = rte_cpu_to_be_16(sender_flows[flow_id].dst_port);
    transport_hdr->sent_seq       = rte_cpu_to_be_32(sender_flows[flow_id].data_seqnum);
    transport_hdr->recv_ack       = rte_cpu_to_be_32(sender_flows[flow_id].data_recv_next);
    transport_hdr->PKT_TYPE_8BITS = RPO_NORM_PKT;
    transport_hdr->FLOW_ID_16BITS = rte_cpu_to_be_16((uint16_t)(flow_id & 0xffff));
    data_len = (pkt_size - HDR_ONLY_SIZE);
    if (data_len > sender_flows[flow_id].remain_size) {
        data_len = sender_flows[flow_id].remain_size;
        pkt_size = HDR_ONLY_SIZE + data_len;
    }
    transport_hdr->DATA_LEN_16BITS = RTE_CPU_TO_BE_16(data_len);
    sender_flows[flow_id].data_seqnum += data_len; 
    sender_flows[flow_id].remain_size -= data_len;

    tx_offloads = ports[global_fs->tx_port].dev_conf.txmode.offloads;
    if (tx_offloads & DEV_TX_OFFLOAD_VLAN_INSERT)
        ol_flags = PKT_TX_VLAN_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_QINQ_INSERT)
        ol_flags |= PKT_TX_QINQ_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_MACSEC_INSERT)
        ol_flags |= PKT_TX_MACSEC;

    pkt->nb_segs        = 1;
    pkt->data_len       = pkt_size;
    pkt->pkt_len        = pkt_size;
    pkt->ol_flags       = ol_flags;
    pkt->vlan_tci       = ports[global_fs->tx_port].tx_vlan_id;
    pkt->vlan_tci_outer = ports[global_fs->tx_port].tx_vlan_id_outer;
    pkt->l2_len         = L2_LEN;
    pkt->l3_len         = L3_LEN;

    sender_pkts_burst[sender_current_burst_size] = pkt;
    sender_current_burst_size++;
    if (sender_current_burst_size >= BURST_THRESHOLD) {
        sender_send_pkt();
        sender_current_burst_size = 0;
    }
}

static void
construct_resend_request(uint32_t flow_id, uint32_t resend_size)
{
    struct   ether_hdr *eth_hdr;
    struct   ipv4_hdr *ip_hdr;
    struct   tcp_hdr *transport_hdr;
    uint64_t ol_flags, tx_offloads;
    int      dst_server_id;
    unsigned pkt_size = HDR_ONLY_SIZE;

    struct rte_mempool *mbp = current_fwd_lcore()->mbp;
    struct rte_mbuf *pkt = rte_mbuf_raw_alloc(mbp);
    if (!pkt) {
        printf("flow_id = %d: allocation pkt error", flow_id);
    }

    pkt->data_len = pkt_size;
    pkt->next = NULL;

    /* Initialize Ethernet header. */
    eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
    dst_server_id = get_dst_server_id(flow_id, receiver_flows);
    if (dst_server_id == -1) {
        printf("server error: cannot find server id\n");
    }
    ether_addr_copy(&eth_addr_array[dst_server_id], &eth_hdr->d_addr);
    ether_addr_copy(&eth_addr_array[this_server_id], &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

    /* Initialize IP header. */
    ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
    memset(ip_hdr, 0, L3_LEN);
    ip_hdr->version_ihl     = IP_VHL_DEF;
    ip_hdr->type_of_service = ECT_1;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live    = IP_DEFTTL;
    ip_hdr->next_proto_id   = IPPROTO_TCP;
    ip_hdr->packet_id       = 0;
    ip_hdr->src_addr        = rte_cpu_to_be_32(ip_addr_array[this_server_id]);
    ip_hdr->dst_addr        = rte_cpu_to_be_32(receiver_flows[flow_id].dst_ip);
    ip_hdr->total_length    = RTE_CPU_TO_BE_16(pkt_size - L2_LEN);
    ip_hdr->hdr_checksum    = ip_sum((unaligned_uint16_t *)ip_hdr, L3_LEN);

    /* Initialize transport header. */
    transport_hdr = (struct tcp_hdr *)(ip_hdr + 1);
    transport_hdr->src_port           = rte_cpu_to_be_16(receiver_flows[flow_id].src_port);
    transport_hdr->dst_port           = rte_cpu_to_be_16(receiver_flows[flow_id].dst_port);
    transport_hdr->sent_seq           = rte_cpu_to_be_32(receiver_flows[flow_id].data_seqnum);
    transport_hdr->recv_ack           = 0;
    transport_hdr->PKT_TYPE_8BITS     = PT_HOMA_RESEND_REQUEST;
    transport_hdr->FLOW_ID_16BITS     = rte_cpu_to_be_16((uint16_t)(flow_id & 0xffff));
    transport_hdr->DATA_RESEND_16BITS = rte_cpu_to_be_16((uint16_t)(resend_size & 0xffff));

    tx_offloads = ports[global_fs->tx_port].dev_conf.txmode.offloads;
    if (tx_offloads & DEV_TX_OFFLOAD_VLAN_INSERT)
        ol_flags = PKT_TX_VLAN_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_QINQ_INSERT)
        ol_flags |= PKT_TX_QINQ_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_MACSEC_INSERT)
        ol_flags |= PKT_TX_MACSEC;

    pkt->nb_segs        = 1;
    pkt->data_len       = pkt_size;
    pkt->pkt_len        = pkt_size;
    pkt->ol_flags       = ol_flags;
    pkt->vlan_tci       = ports[global_fs->tx_port].tx_vlan_id;
    pkt->vlan_tci_outer = ports[global_fs->tx_port].tx_vlan_id_outer;
    pkt->l2_len         = L2_LEN;
    pkt->l3_len         = L3_LEN;

    receiver_pkts_burst[receiver_current_burst_size] = pkt;
    receiver_current_burst_size++;
    if (receiver_current_burst_size >= BURST_THRESHOLD) {
        receiver_send_pkt();
        receiver_current_burst_size = 0;
    }
}

static void
process_ack(struct tcp_hdr* transport_recv_hdr, struct ipv4_hdr *ipv4_hdr)
{
    uint16_t datalen = rte_be_to_cpu_16(transport_recv_hdr->DATA_LEN_16BITS);
    uint16_t flow_id = (uint16_t)rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);

    if (verbose > 2 && 
        rte_be_to_cpu_32(transport_recv_hdr->sent_seq) != receiver_flows[flow_id].data_recv_next) {
        print_elapsed_time();
        printf(" - flow %d: data reordering detected. (expected = %u, received = %u)\n", flow_id, 
            receiver_flows[flow_id].data_recv_next, rte_be_to_cpu_32(transport_recv_hdr->sent_seq));
    }
    if (receiver_flows[flow_id].remain_size > datalen) {
        receiver_flows[flow_id].remain_size -= datalen;
        receiver_flows[flow_id].data_recv_next += datalen;
    }
    else {
        receiver_flows[flow_id].remain_size = 0;
        receiver_flows[flow_id].data_recv_next = receiver_flows[flow_id].flow_size + 1;
    }
    
    static int print_counter = 300;
    if (verbose > 1 && print_counter > 0) {
        print_elapsed_time();
        printf(" - process_ack of flow %u, datalen=%u, data_recv_next=%u, remain_size=%u\n", 
            flow_id, datalen, receiver_flows[flow_id].data_recv_next, 
            receiver_flows[flow_id].remain_size);
        print_counter--;
    }

    if (receiver_flows[flow_id].remain_size == 0) {
        if (receiver_flows[flow_id].flow_state != RPO_RECV_CLOSED) {    
            /* This is to tell sender to close */
            construct_grant(flow_id, receiver_flows[flow_id].flow_size+DEFAULT_PKT_SIZE+1, 
                0);
            remove_receiver_active_flow(flow_id);
            if (verbose > 0) {
                print_elapsed_time();
                printf(" - receiver flow %d finished\n", flow_id);
            }
        }
        return;
    }
    receiver_flows[flow_id].receiver_could_echo++;
    construct_grant(flow_id,
                        receiver_flows[flow_id].flow_size+receiver_flows[flow_id].receiver_could_echo*DEFAULT_PKT_SIZE,
                        ipv4_hdr->type_of_service);
}

static void
recv_grant_request(struct tcp_hdr *transport_recv_hdr, struct ipv4_hdr *ipv4_hdr)
{
    uint16_t flow_id = rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);
    uint32_t flow_size_lowpart = (uint32_t)rte_be_to_cpu_16(transport_recv_hdr->FLOW_SIZE_LOW_16BITS);
    uint32_t flow_size_highpart = ((uint32_t)transport_recv_hdr->FLOW_SIZE_HIGH_16BITS << 16) & 0xffff0000;
    uint32_t flow_size = flow_size_highpart + flow_size_lowpart;

    //if (receiver_flows[flow_id].flow_state != HOMA_RECV_UNSTARTED)
    if (receiver_flows[flow_id].flow_state != RPO_RECV_READY)
        return; 
    
    if (verbose > 1) {
        print_elapsed_time();
        printf(" - recv_grant_request of flow %u\n", flow_id);
    }

    add_receiver_active_flow(flow_id);

    receiver_flows[flow_id].flow_size               = flow_size;
    receiver_flows[flow_id].remain_size             = flow_size;
    receiver_flows[flow_id].src_port                = RTE_BE_TO_CPU_16(transport_recv_hdr->dst_port);
    receiver_flows[flow_id].dst_port                = RTE_BE_TO_CPU_16(transport_recv_hdr->src_port);
    receiver_flows[flow_id].src_ip                  = rte_be_to_cpu_32(ipv4_hdr->dst_addr);
    receiver_flows[flow_id].dst_ip                  = rte_be_to_cpu_32(ipv4_hdr->src_addr);
    receiver_flows[flow_id].start_time              = rte_rdtsc() / (double)hz;
    receiver_flows[flow_id].fct_printed             = 0;
    receiver_flows[flow_id].flow_finished           = 0;
    receiver_flows[flow_id].data_recv_next          = 1;
    receiver_flows[flow_id].data_seqnum             = 1;
    receiver_flows[flow_id].last_grant_sent_time    = rte_rdtsc() / (double)hz;
    receiver_flows[flow_id].last_grant_granted_seq  = DEFAULT_PKT_SIZE+1;
    receiver_flows[flow_id].last_grant_granted_prio = 0;
    receiver_flows[flow_id].resend_request_counter  = 0;
    receiver_flows[flow_id].first_grant_access_time = -1.0;
    receiver_flows[flow_id].receiver_could_echo = 0;

    if (verbose > 0) {
        print_elapsed_time();
        printf(" - receiver flow %d started\n", flow_id);
    }
}

static void
recv_oppo_pkt(struct tcp_hdr *transport_recv_hdr)
{
    uint16_t flow_id = rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);
    receiver_flows[flow_id].receiver_could_echo++;
    construct_grant(flow_id,
                        receiver_flows[flow_id].flow_size+receiver_flows[flow_id].receiver_could_echo*DEFAULT_PKT_SIZE,
                        ipv4_hdr->type_of_service);
}

static void
recv_grant(struct tcp_hdr *transport_recv_hdr, struct ipv4_hdr *ipv4_hdr)
{
    uint16_t flow_id = rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);
    uint32_t seq_granted_lowpart = (uint32_t)rte_be_to_cpu_16(transport_recv_hdr->SEQ_GRANTED_LOW_16BITS);
    uint32_t seq_granted_highpart = ((uint32_t)transport_recv_hdr->SEQ_GRANTED_HIGH_16BITS << 16) & 0xffff0000;
    uint32_t seq_granted = seq_granted_highpart + seq_granted_lowpart;
    uint8_t  priority_granted = transport_recv_hdr->PRIORITY_GRANTED_8BITS;
    uint8_t ecn_granted = ipv4_hdr->type_of_service && CE;
    struct   rte_mbuf *queued_pkt;
    struct   tcp_hdr *transport_hdr;
    int      queued_pkt_type;
    uint16_t queued_flow_id;

    if (verbose > 1) {
        print_elapsed_time();
        printf(" - recv_grant of flow %u, seq_granted=%u, priority_granted=%u\n", 
            flow_id, seq_granted, priority_granted);
    }

    switch (sender_flows[flow_id].flow_state) {
        //case HOMA_SEND_GRANT_REQUEST_SENT:
        case RPO_SEND_GRANT_REQUSET:
            remove_sender_grant_request_sent_flow(flow_id);
            sender_flows[flow_id].first_grant_access_time = rte_rdtsc() / (double)hz;
        case RPO_SEND_WAITING:
            sender_flows[flow_id].granted_seqnum = seq_granted;
            sender_flows[flow_id].data_recv_next = transport_recv_hdr->sent_seq+1;
            while (sender_flows[flow_id].remain_size > 0 &&
                        sender_flows[flow_id].granted_seqnum > sender_flows[flow_id].data_seqnum) {
                        construct_data(flow_id, 0);
                        if (ecn_granted != CE){
                            construct_data(flow_id, 2);
                        }
                    }
            if (sender_flows[flow_id].remain_size == 0) { 
                double now = rte_rdtsc() / (double)hz;
                if (sender_flows[flow_id].wait_close_start_time < 0)
                    sender_flows[flow_id].wait_close_start_time = now;
                if (seq_granted >= sender_flows[flow_id].flow_size + DEFAULT_PKT_SIZE ||
                    now - sender_flows[flow_id].wait_close_start_time >= 100*TIMEOUT) {
                    sender_finished_flow_num++;
                    sender_flows[flow_id].flow_state = RPO_SEND_CLOASED;
                    sender_flows[flow_id].flow_finished = 1;
                    sender_flows[flow_id].finish_time = rte_rdtsc() / (double)hz;
                    if (verbose > 0) {
                        print_elapsed_time();
                        printf(" - sender flow %d finished\n", flow_id);
                    }
                }
            }
            break;
        default:
            break;
    }
}

static void
recv_data(struct tcp_hdr *transport_recv_hdr, struct ipv4_hdr *ipv4_hdr)
{
    uint16_t flow_id = rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);

    if (verbose > 1) {
        print_elapsed_time();
        printf(" - recv_data of flow %u\n", flow_id);
    }

    /* Drop all data packets if received before the grant requests */
    //if (receiver_flows[flow_id].flow_state < HOMA_RECV_GRANT_SENDING) {
    if (receiver_flows[flow_id].flow_state < RPO_RECV_SENDING_GRANT) 
        if (verbose > 1) {
            print_elapsed_time();
            printf(" - recv_data of flow %u and dropped due to no grant request\n", flow_id);
        }
        return;
    }

    process_ack(transport_recv_hdr, ipv4_hdr);
}

static void
recv_resend_request(struct tcp_hdr *transport_recv_hdr)
{
    uint16_t flow_id = rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);
    uint16_t resend_size = rte_be_to_cpu_16(transport_recv_hdr->DATA_RESEND_16BITS);

    /* Roll back resend_size data pkts for resend if grants allowed. */
    uint16_t rollback = min(resend_size, sender_flows[flow_id].data_seqnum-1);
    sender_flows[flow_id].data_seqnum -= rollback; 
    sender_flows[flow_id].remain_size += rollback;

    if (verbose > 1) {
        print_elapsed_time();
        printf(" - recv_resend_request of flow %u, data_seqnum=%u, remain_size=%u, resend_size=%u\n", 
            flow_id, sender_flows[flow_id].data_seqnum, sender_flows[flow_id].remain_size, resend_size);
    }
}

/* Receive and process a burst of packets. */
static void
recv_pkt(struct fwd_stream *fs)
{
    struct   rte_mbuf *pkts_burst[MAX_PKT_BURST];
    struct   rte_mbuf *mb;
    uint16_t nb_rx;
    struct   ipv4_hdr *ipv4_hdr;
    struct   tcp_hdr *transport_recv_hdr;
    uint8_t  l4_proto;
    uint8_t  pkt_type;

    /* Receive a burst of packets. */
    nb_rx = rte_eth_rx_burst(fs->rx_port, fs->rx_queue, pkts_burst, nb_pkt_per_burst);

    if (unlikely(nb_rx == 0))
        return;

#ifdef RTE_TEST_PMD_RECORD_BURST_STATS
    fs->rx_burst_stats.pkt_burst_spread[nb_rx]++;
#endif
    fs->rx_packets += nb_rx;

    /* Process a burst of packets. */
    for (int i = 0; i < nb_rx; i++) {
        mb = pkts_burst[i]; 
        ipv4_hdr = rte_pktmbuf_mtod_offset(mb, struct ipv4_hdr *, L2_LEN);
        if (ipv4_hdr->dst_addr != rte_cpu_to_be_32(ip_addr_array[this_server_id]))
            continue;
        l4_proto = ipv4_hdr->next_proto_id;
        if (l4_proto == IPPROTO_TCP) {
            transport_recv_hdr = rte_pktmbuf_mtod_offset(mb, struct tcp_hdr *, L2_LEN + L3_LEN);
            pkt_type = transport_recv_hdr->PKT_TYPE_8BITS;
            switch (pkt_type) {
                case RPO_SYNC:
                    sync_done = 1;
                    break;
                case RPO_NORM_PKT:
                    recv_data(transport_recv_hdr, ipv4_hdr);
                    break;
                case RPO_GRANT_REQUEST:
                    recv_grant_request(transport_recv_hdr, ipv4_hdr);
                    break;
                case RPO_OPPO_PKT:
                    recv_oppo_pkt(transport_recv_hdr);
                    break;
                case RPO_GRANT:
                    recv_grant(transport_recv_hdr, ipv4_hdr);
                    break;
                default:
                    break;
            }
        }
        rte_pktmbuf_free(mb);
    }
}

static void
send_grant(void)
{
    if (receiver_current_burst_size > 0) {
        receiver_send_pkt();
    }
    for (int i=0; i<max_receiver_active_flow_num; i++) {
        if (receiver_active_flow_array[i] >= 0) {
            if ((now - receiver_flows[flow_id].last_grant_sent_time) > TIMEOUT) {
                construct_grant(flow_id, seq_granted, 0);
                receiver_flows[flow_id].last_grant_sent_time = now;
                receiver_flows[flow_id].last_grant_granted_seq = seq_granted;
                receiver_flows[flow_id].last_grant_granted_prio = 0;
                receiver_flows[flow_id].resend_request_counter++;
                if (receiver_flows[flow_id].first_grant_access_time < 0)
                    receiver_flows[flow_id].first_grant_access_time = now;
            }
        }
    }
}

/* Sender send a burst of packets */
static void
sender_send_pkt(void)
{
    uint16_t nb_pkt = sender_current_burst_size;
    uint16_t nb_tx = rte_eth_tx_burst(global_fs->tx_port, global_fs->tx_queue, sender_pkts_burst, nb_pkt);

    if (unlikely(nb_tx < nb_pkt) && global_fs->retry_enabled) {
        uint32_t retry = 0;
        while (nb_tx < nb_pkt && retry++ < burst_tx_retry_num) {
            if (verbose > 1) {
                print_elapsed_time();
                printf(" - sender_send_pkt, retry = %u\n", retry);
            }
            rte_delay_us(burst_tx_delay_time);
            nb_tx += rte_eth_tx_burst(global_fs->tx_port, global_fs->tx_queue, 
                                        &sender_pkts_burst[nb_tx], nb_pkt - nb_tx);
        }
    }

    global_fs->tx_packets += nb_tx;
    sender_current_burst_size = 0;
}

/* Receiver send a burst of packets */
static void
receiver_send_pkt(void)
{
    uint16_t nb_pkt = receiver_current_burst_size;
    uint16_t nb_tx = rte_eth_tx_burst(global_fs->tx_port, global_fs->tx_queue, receiver_pkts_burst, nb_pkt);

    if (unlikely(nb_tx < nb_pkt) && global_fs->retry_enabled) {
        uint32_t retry = 0;
        while (nb_tx < nb_pkt && retry++ < burst_tx_retry_num) {
            if (verbose > 1) {
                print_elapsed_time();
                printf(" - receiver_send_pkt, retry = %u\n", retry);
            }
            rte_delay_us(burst_tx_delay_time);
            nb_tx += rte_eth_tx_burst(global_fs->tx_port, global_fs->tx_queue, 
                                        &receiver_pkts_burst[nb_tx], nb_pkt - nb_tx);
        }
    }

    global_fs->tx_packets += nb_tx;
    receiver_current_burst_size = 0;
}

/* Start one warm up flow - send data without receive */
static void
start_warm_up_flow(void)
{
    int flow_id = sender_next_unstart_flow_id;
    sender_next_unstart_flow_id = find_next_unstart_flow_id();
    
    while (1) {
        construct_data(flow_id, 0);
        if (sender_flows[flow_id].remain_size == 0)
            break;
    }

    sender_send_pkt();
    sender_finished_flow_num++;

    if (verbose > 0) {
        print_elapsed_time();
        printf(" - start_warm_up_flow %d done\n", flow_id);
    }
}

static void
start_new_flow(void)
{
    double now;
    int flow_id, request_retrans_check, retransmit_flow_index, max_request_retrans_check;

    if (sender_current_burst_size > 0) {
        sender_send_pkt();
    }

    /* Retransmit timeout grant request */
    max_request_retrans_check = min(MAX_REQUEST_RETRANSMIT_ONE_TIME, sender_grant_request_sent_flow_num);
    if (sender_grant_request_sent_flow_num > 0) {
        retransmit_flow_index = find_next_sender_grant_request_sent_flow(0);
        request_retrans_check = 0;
        while (request_retrans_check < max_request_retrans_check) {
            now =  rte_rdtsc() / (double)hz; 
            flow_id = sender_request_sent_flow_array[retransmit_flow_index];
            if ((now - sender_flows[flow_id].last_grant_request_sent_time) > TIMEOUT) {
                if (verbose > 1) {
                    print_elapsed_time();
                    printf(" - construct_grant_request %d due to TIMEOUT\n"
                        "                 last_grant_request_sent_time = %lf\n", flow_id, 
                        sender_flows[flow_id].last_grant_request_sent_time - flowgen_start_time);
                }
                construct_grant_request(flow_id);
                sender_flows[flow_id].last_grant_request_sent_time = now;
            }
            retransmit_flow_index = find_next_sender_grant_request_sent_flow(retransmit_flow_index+1);
            request_retrans_check++;
        }
    }

    /* Start new flow */
    if (sender_next_unstart_flow_id < total_flow_num) {
        flow_id = sender_next_unstart_flow_id;
        now = rte_rdtsc() / (double)hz;
        while ((sender_flows[flow_id].start_time + flowgen_start_time + warm_up_time) <= now) {
            if (verbose > 0) {
                print_elapsed_time();
                printf(" - start_new_flow %d\n", flow_id);
            }

            /* Send grant requests for new flows */
            construct_grant_request(flow_id);
            add_sender_grant_request_sent_flow(flow_id);

            /* Send normal pkt */
            construct_data(flow_id, 0);

            /* Send opportunistic pkt */
            construct_data(flow_id, 2);

            if (sender_next_unstart_flow_id < total_flow_num) {
                flow_id = sender_next_unstart_flow_id;
            } else {
                break;
            }
        }
    }
}

/* flowgen packet_fwd main function */
static void
main_flowgen(struct fwd_stream *fs)
{
    /* Initialize global variables */
    rte_delay_ms(1000);
    global_fs = fs;
    hz = rte_get_timer_hz(); 
    sender_flows = rte_zmalloc("testpmd: struct flow_info",
            total_flow_num*sizeof(struct flow_info), RTE_CACHE_LINE_SIZE);
    receiver_flows = rte_zmalloc("testpmd: struct flow_info",
            total_flow_num*sizeof(struct flow_info), RTE_CACHE_LINE_SIZE);

    /* Read basic info of server number, mac and ip */
    printf("\nEnter read_config...\n\n");
    read_config();
    printf("\nExit read_config...\n\n");
    
    /* Init flow info */
    printf("\nEnter init...\n\n");
    init();
    printf("\nExit init...\n\n");

    /* Warm up and sync all servers */
    printf("\nEnter warm up and sync loop...\n\n");
    start_cycle = rte_rdtsc();
    elapsed_cycle = 0;
    flowgen_start_time = start_cycle / (double)hz;
    start_warm_up_flow();
    while (elapsed_cycle/(double)hz < warm_up_time) {
        recv_pkt(fs);
        send_grant();
        elapsed_cycle = rte_rdtsc() - start_cycle;
        if (this_server_id == 1 && elapsed_cycle/(double)hz > sync_start_time) {
            for (int i=2; i<SERVERNUM; i++) {
                construct_sync(i);
                if (verbose > 0 || 1) {
                    print_elapsed_time();
                    printf(" - sync pkt sent of server_id %d\n", i);
                }
            }
            sender_send_pkt();
            sync_done = 1;
        }
        if (sync_done) {
            warm_up_time = elapsed_cycle/(double)hz;
            break;
        }
    }
    printf("Warm up and sync delay = %lf sec\n", warm_up_time);
    printf("\nExit warm up and sync loop...\n\n");

    /* Main flowgen loop */
    printf("\nEnter main_flowgen loop...\n\n");
    printf("MAX_TIME = %d sec after warm up\n", MAX_TIME);
    double loop_time = warm_up_time + MAX_TIME;
    int    main_flowgen_loop = 1;
    double total_time_phase1 = 0;
    double total_time_phase2 = 0;
    double total_time_phase3 = 0;
    double now;
    long int loop_num = 0;
    while (elapsed_cycle < loop_time*hz) {
        loop_num++;
        if (verbose > 2) {
            printf("main_flowgen_loop = %d\n", main_flowgen_loop++);
            print_elapsed_time();
            printf(" - enter start_new_flow...\n");
        }

        now = rte_rdtsc() / (double)hz;
        start_new_flow();
        total_time_phase1 += rte_rdtsc() / (double)hz - now;

        if (verbose > 2) {
            print_elapsed_time();
            printf(" - enter recv_pkt...\n");
        }
        
        now = rte_rdtsc() / (double)hz;
        recv_pkt(fs);
        total_time_phase2 += rte_rdtsc() / (double)hz - now;

        if (verbose > 2) {
            print_elapsed_time();
            printf(" - enter send_grant...\n");
        }

        now = rte_rdtsc() / (double)hz;
        send_grant();
        total_time_phase3 += rte_rdtsc() / (double)hz - now;

        if (verbose > 2) {
            print_elapsed_time();
            printf(" - exit  send_grant...\n\n");
        }

        elapsed_cycle = rte_rdtsc() - start_cycle;

        if (loop_num == 1 || loop_num == 10 || loop_num == 100 || loop_num == 1000 || 
            loop_num == 10000 || loop_num == 100000 || loop_num == 1000000 || loop_num == 10000000 ||
            loop_num == 100000000 || loop_num == 1000000000 || loop_num == 10000000000)
            printf("loop_num=%ld, total/average time in phase 1/2/3: %lf/%lf/%lf %lf/%lf/%lf\n",  
                loop_num, total_time_phase1, total_time_phase2, total_time_phase3,
                total_time_phase1/(double)loop_num, total_time_phase2/(double)loop_num, 
                total_time_phase3/(double)loop_num);
    }
    printf("\nExit main_flowgen loop...\n\n");

    printf("\nEnter print_fct...\n\n");
    print_fct();
    printf("\nExit print_fct...\n\n");

    exit(0);
}

struct fwd_engine flow_gen_engine = {
    .fwd_mode_name  = "flowgen",
    .port_fwd_begin = NULL,
    .port_fwd_end   = NULL,
    .packet_fwd     = main_flowgen,
};
