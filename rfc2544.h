#ifndef RFC2544_H
#define RFC2544_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#define MAX_FRAME_SIZES     16
#define DEFAULT_DURATION    10      /* seconds per trial */
#define DEFAULT_SRC_PORT    1234
#define DEFAULT_DST_PORT    5678
#define PAYLOAD_SEQ_OFFSET  0       /* seq is first 8 bytes of payload */

/* TPACKET_V3 ring parameters */
#define TP_BLOCK_SIZE       (1 << 22)   /* 4 MB */
#define TP_FRAME_SIZE       2048
#define TP_BLOCK_NR         64
#define TP_FRAME_NR         ((TP_BLOCK_SIZE / TP_FRAME_SIZE) * TP_BLOCK_NR)

/* RX ring */
#define RX_BLOCK_SIZE       (1 << 22)
#define RX_FRAME_SIZE       2048
#define RX_BLOCK_NR         64
#define RX_FRAME_NR         ((RX_BLOCK_SIZE / RX_FRAME_SIZE) * RX_BLOCK_NR)

typedef struct {
    char        iface[64];
    char        gateway[64];        /* optional: resolve dst_mac via ARP */
    uint8_t     src_mac[6];
    uint8_t     dst_mac[6];
    uint32_t    src_ip;             /* network byte order */
    uint32_t    dst_ip;             /* network byte order */
    uint8_t     tos;
    int         use_vlan;
    uint16_t    vlan_id;
    uint8_t     vlan_prio;
    int         use_udp;
    uint16_t    src_port;
    uint16_t    dst_port;
    uint32_t    frame_sizes[MAX_FRAME_SIZES];
    int         n_frame_sizes;
    uint32_t    duration;           /* seconds per rate trial */
    double      rate_bps;           /* offered rate bits/sec (0 = line rate) */
    int         verbose;
    char        csv_output[256];
    char        json_output[256];
    int         binary_search;      /* 1 = binary search, 0 = stepped */
    double      step_pct;           /* step percentage for stepped search */
    int         ifindex;
    int         mtu;
} config_t;

typedef struct {
    _Atomic uint64_t    tx_packets;
    _Atomic uint64_t    rx_packets;
    _Atomic uint64_t    rx_matched;
    _Atomic uint64_t    rx_seq_errors;
    _Atomic uint64_t    tx_bytes;
    _Atomic uint64_t    rx_bytes;
} stats_t;

typedef struct {
    int         tx_fd;
    int         rx_fd;
    void       *tx_ring;
    size_t      tx_ring_size;
    void       *rx_ring;
    size_t      rx_ring_size;
    struct tpacket_req3 tx_req;
    struct tpacket_req3 rx_req;
    int         tx_block_idx;
    int         tx_frame_idx;
} ring_ctx_t;

typedef struct {
    config_t   *cfg;
    ring_ctx_t *ring;
    stats_t    *stats;
    uint32_t    frame_size;
    double      target_pps;
    volatile int stop;
} thread_ctx_t;

/* Function prototypes */
int  parse_args(int argc, char **argv, config_t *cfg);
int  parse_config_json(const char *path, config_t *cfg);
int  setup_interface(config_t *cfg);
int  setup_rings(const config_t *cfg, ring_ctx_t *ring);
void teardown_rings(ring_ctx_t *ring);
int  build_packet(const config_t *cfg, uint8_t *buf, uint32_t frame_size,
                  uint64_t seq);
int  match_reflected(const config_t *cfg, const uint8_t *pkt, uint32_t pkt_len,
                     uint64_t *seq_out);
void *tx_thread(void *arg);
void *rx_thread(void *arg);
double run_trial(config_t *cfg, ring_ctx_t *ring, uint32_t frame_size,
                 double target_pps, uint32_t duration, stats_t *stats);
void run_throughput_test(config_t *cfg, ring_ctx_t *ring, uint32_t frame_size);
void print_results(const config_t *cfg, uint32_t frame_size,
                   double offered_pps, double measured_pps,
                   double loss_pct, double mbps);
void write_csv(const config_t *cfg, uint32_t frame_size,
               double offered_pps, double measured_pps,
               double loss_pct, double mbps);
uint16_t checksum(const void *data, int len);
uint16_t udp_checksum(const struct iphdr *iph, const struct udphdr *udph,
                      const uint8_t *payload, int payload_len);

#endif /* RFC2544_H */
