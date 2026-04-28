#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <linux/if_packet.h>
#include "rfc2544.h"

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static inline void busy_wait_until(uint64_t target_ns)
{
    while (now_ns() < target_ns)
        __asm__ volatile("pause" ::: "memory");
}

void *tx_thread(void *arg)
{
    thread_ctx_t *ctx = arg;
    const config_t *cfg = ctx->cfg;
    ring_ctx_t *ring = ctx->ring;
    stats_t *stats = ctx->stats;
    uint32_t frame_size = ctx->frame_size;
    double target_pps = ctx->target_pps;

    uint64_t inter_pkt_ns = (target_pps > 0.0)
        ? (uint64_t)(1e9 / target_pps)
        : 0;

    uint8_t pkt_buf[2048];
    uint64_t seq = 0;
    int frames_per_block = ring->tx_req.tp_block_size / ring->tx_req.tp_frame_size;
    int cur_block = 0;
    int cur_frame = 0;

    uint64_t next_tx_ns = now_ns();

    while (!ctx->stop) {
        /* Get pointer to current TX frame slot */
        uint8_t *block_base = (uint8_t *)ring->tx_ring +
                              (size_t)cur_block * ring->tx_req.tp_block_size;
        struct tpacket3_hdr *hdr = (struct tpacket3_hdr *)
            (block_base + (size_t)cur_frame * ring->tx_req.tp_frame_size);

        /* Wait for slot to become available (TP_STATUS_AVAILABLE = 0) */
        while ((hdr->tp_status & TP_STATUS_SEND_REQUEST) && !ctx->stop)
            __asm__ volatile("pause" ::: "memory");

        if (ctx->stop)
            break;

        /* Build packet into slot */
        uint8_t *data = (uint8_t *)hdr + TPACKET_ALIGN(sizeof(struct tpacket3_hdr));
        int pkt_len = build_packet(cfg, pkt_buf, frame_size, seq);
        memcpy(data, pkt_buf, pkt_len);

        hdr->tp_len    = pkt_len;
        hdr->tp_snaplen = pkt_len;
        /* Mark frame ready to send */
        __sync_synchronize();
        hdr->tp_status = TP_STATUS_SEND_REQUEST;

        /* Pacing: wait until scheduled time */
        if (inter_pkt_ns > 0) {
            busy_wait_until(next_tx_ns);
            next_tx_ns += inter_pkt_ns;
        }

        /* Flush every frame (or batch if desired — here we flush per-frame for pacing) */
        if (send(ring->tx_fd, NULL, 0, MSG_DONTWAIT) < 0 && errno != ENOBUFS) {
            /* ENOBUFS is acceptable under high load */
            if (errno != EAGAIN)
                perror("send");
        }

        atomic_fetch_add(&stats->tx_packets, 1);
        atomic_fetch_add(&stats->tx_bytes, frame_size);
        seq++;

        /* Advance frame/block indices */
        cur_frame++;
        if (cur_frame >= frames_per_block) {
            cur_frame = 0;
            cur_block = (cur_block + 1) % ring->tx_req.tp_block_nr;
        }
    }

    /* Flush remaining */
    send(ring->tx_fd, NULL, 0, 0);
    return NULL;
}

void *rx_thread(void *arg)
{
    thread_ctx_t *ctx = arg;
    const config_t *cfg = ctx->cfg;
    ring_ctx_t *ring = ctx->ring;
    stats_t *stats = ctx->stats;

    int cur_block = 0;

    struct pollfd pfd = {
        .fd     = ring->rx_fd,
        .events = POLLIN | POLLERR,
    };

    while (!ctx->stop) {
        struct tpacket_block_desc *block_hdr =
            (struct tpacket_block_desc *)((uint8_t *)ring->rx_ring +
             (size_t)cur_block * ring->rx_req.tp_block_size);

        if (!(block_hdr->hdr.bh1.block_status & TP_STATUS_USER)) {
            /* No data in this block yet — poll briefly */
            poll(&pfd, 1, 1);
            continue;
        }

        /* Walk all frames in this block */
        uint32_t num_pkts = block_hdr->hdr.bh1.num_pkts;
        uint8_t *frame_ptr = (uint8_t *)block_hdr +
                             block_hdr->hdr.bh1.offset_to_first_pkt;

        for (uint32_t i = 0; i < num_pkts; i++) {
            struct tpacket3_hdr *tp3 = (struct tpacket3_hdr *)frame_ptr;
            uint8_t *pkt = frame_ptr + tp3->tp_mac;
            uint32_t pkt_len = tp3->tp_snaplen;

            uint64_t seq;
            if (match_reflected(cfg, pkt, pkt_len, &seq)) {
                atomic_fetch_add(&stats->rx_matched, 1);
            }
            atomic_fetch_add(&stats->rx_packets, 1);
            atomic_fetch_add(&stats->rx_bytes, pkt_len);

            frame_ptr += TPACKET_ALIGN(tp3->tp_next_offset ? tp3->tp_next_offset
                                       : (sizeof(struct tpacket3_hdr) + pkt_len));
        }

        /* Return block to kernel */
        block_hdr->hdr.bh1.block_status = TP_STATUS_KERNEL;
        __sync_synchronize();
        cur_block = (cur_block + 1) % ring->rx_req.tp_block_nr;
    }

    return NULL;
}
