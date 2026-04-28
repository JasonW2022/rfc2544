#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <net/ethernet.h>
#include "rfc2544.h"

static int open_packet_socket(const config_t *cfg, int ring_type)
{
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        perror("socket(AF_PACKET)");
        return -1;
    }

    /* Enable TPACKET_V3 */
    int version = TPACKET_V3;
    if (setsockopt(fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)) < 0) {
        perror("setsockopt PACKET_VERSION");
        close(fd);
        return -1;
    }

    /* Set up the ring */
    struct tpacket_req3 req;
    memset(&req, 0, sizeof(req));

    if (ring_type == PACKET_TX_RING) {
        req.tp_block_size  = TP_BLOCK_SIZE;
        req.tp_frame_size  = TP_FRAME_SIZE;
        req.tp_block_nr    = TP_BLOCK_NR;
        req.tp_frame_nr    = TP_FRAME_NR;
        req.tp_retire_blk_tov = 0;
        req.tp_feature_req_word = 0;
    } else {
        req.tp_block_size  = RX_BLOCK_SIZE;
        req.tp_frame_size  = RX_FRAME_SIZE;
        req.tp_block_nr    = RX_BLOCK_NR;
        req.tp_frame_nr    = RX_FRAME_NR;
        req.tp_retire_blk_tov = 1; /* retire blocks after 1ms timeout */
        req.tp_feature_req_word = 0;
    }

    if (setsockopt(fd, SOL_PACKET, ring_type, &req, sizeof(req)) < 0) {
        perror("setsockopt PACKET_*_RING");
        close(fd);
        return -1;
    }

    /* Bind to interface */
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family   = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex  = cfg->ifindex;
    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    return fd;
}

int setup_rings(const config_t *cfg, ring_ctx_t *ring)
{
    memset(ring, 0, sizeof(*ring));

    /* TX ring */
    ring->tx_fd = open_packet_socket(cfg, PACKET_TX_RING);
    if (ring->tx_fd < 0)
        return -1;

    ring->tx_req.tp_block_size = TP_BLOCK_SIZE;
    ring->tx_req.tp_frame_size = TP_FRAME_SIZE;
    ring->tx_req.tp_block_nr   = TP_BLOCK_NR;
    ring->tx_req.tp_frame_nr   = TP_FRAME_NR;
    ring->tx_ring_size = (size_t)TP_BLOCK_SIZE * TP_BLOCK_NR;
    ring->tx_ring = mmap(NULL, ring->tx_ring_size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_LOCKED,
                         ring->tx_fd, 0);
    if (ring->tx_ring == MAP_FAILED) {
        perror("mmap tx ring");
        close(ring->tx_fd);
        return -1;
    }

    /* RX ring — separate socket so we can poll/read independently */
    ring->rx_fd = open_packet_socket(cfg, PACKET_RX_RING);
    if (ring->rx_fd < 0) {
        munmap(ring->tx_ring, ring->tx_ring_size);
        close(ring->tx_fd);
        return -1;
    }

    ring->rx_req.tp_block_size = RX_BLOCK_SIZE;
    ring->rx_req.tp_frame_size = RX_FRAME_SIZE;
    ring->rx_req.tp_block_nr   = RX_BLOCK_NR;
    ring->rx_req.tp_frame_nr   = RX_FRAME_NR;
    ring->rx_ring_size = (size_t)RX_BLOCK_SIZE * RX_BLOCK_NR;
    ring->rx_ring = mmap(NULL, ring->rx_ring_size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_LOCKED,
                         ring->rx_fd, 0);
    if (ring->rx_ring == MAP_FAILED) {
        perror("mmap rx ring");
        close(ring->rx_fd);
        munmap(ring->tx_ring, ring->tx_ring_size);
        close(ring->tx_fd);
        return -1;
    }

    ring->tx_block_idx = 0;
    ring->tx_frame_idx = 0;
    return 0;
}

void teardown_rings(ring_ctx_t *ring)
{
    if (ring->tx_ring && ring->tx_ring != MAP_FAILED)
        munmap(ring->tx_ring, ring->tx_ring_size);
    if (ring->rx_ring && ring->rx_ring != MAP_FAILED)
        munmap(ring->rx_ring, ring->rx_ring_size);
    if (ring->tx_fd >= 0)
        close(ring->tx_fd);
    if (ring->rx_fd >= 0)
        close(ring->rx_fd);
}
