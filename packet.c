#include <string.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/ethernet.h>
#include "rfc2544.h"

/* Ethernet II + optional 802.1Q VLAN header sizes */
#define ETH_HDR_LEN     14
#define VLAN_HDR_LEN    4
#define IP_HDR_LEN      20
#define UDP_HDR_LEN     8
#define FCS_LEN         4   /* kernel appends FCS; we size payload accordingly */

uint16_t checksum(const void *data, int len)
{
    const uint16_t *ptr = data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len)
        sum += *(const uint8_t *)ptr;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t udp_checksum(const struct iphdr *iph, const struct udphdr *udph,
                      const uint8_t *payload, int payload_len)
{
    struct {
        uint32_t src;
        uint32_t dst;
        uint8_t  zero;
        uint8_t  proto;
        uint16_t udp_len;
    } pseudo;
    pseudo.src     = iph->saddr;
    pseudo.dst     = iph->daddr;
    pseudo.zero    = 0;
    pseudo.proto   = IPPROTO_UDP;
    pseudo.udp_len = udph->len;

    int total = sizeof(pseudo) + sizeof(struct udphdr) + payload_len;
    uint8_t buf[total];
    memcpy(buf, &pseudo, sizeof(pseudo));
    memcpy(buf + sizeof(pseudo), udph, sizeof(struct udphdr));
    memcpy(buf + sizeof(pseudo) + sizeof(struct udphdr), payload, payload_len);
    return checksum(buf, total);
}

/*
 * Build a complete Ethernet frame into buf.
 * frame_size is the desired on-wire size including FCS.
 * Returns number of bytes written (frame_size - FCS_LEN = bytes to send).
 */
int build_packet(const config_t *cfg, uint8_t *buf, uint32_t frame_size,
                 uint64_t seq)
{
    memset(buf, 0, frame_size);

    int offset = 0;

    /* Ethernet header */
    struct ethhdr *eth = (struct ethhdr *)buf;
    memcpy(eth->h_dest,   cfg->dst_mac, 6);
    memcpy(eth->h_source, cfg->src_mac, 6);
    offset += ETH_HDR_LEN;

    /* Optional 802.1Q VLAN tag */
    if (cfg->use_vlan) {
        /* Insert 802.1Q header: TPID=0x8100, TCI=prio<<13|vlan_id */
        uint16_t *tpid = (uint16_t *)(buf + ETH_HDR_LEN - 2);
        /* Shift ethertype out, insert VLAN tag */
        eth->h_proto = htons(0x8100);
        uint16_t tci = htons(((uint16_t)cfg->vlan_prio << 13) | (cfg->vlan_id & 0x0fff));
        memcpy(buf + offset, &tci, 2);
        offset += 2;
        /* inner ethertype placeholder — filled below */
        offset += 2;
        (void)tpid;
    }

    /* Mark where ethertype/inner-ethertype goes */
    int ethertype_offset = cfg->use_vlan ? (ETH_HDR_LEN + 2) : (ETH_HDR_LEN - 2);
    uint16_t ethertype = htons(ETH_P_IP);
    memcpy(buf + ethertype_offset, &ethertype, 2);
    if (!cfg->use_vlan)
        offset = ETH_HDR_LEN;
    else
        offset = ETH_HDR_LEN + VLAN_HDR_LEN;

    int l3_offset = offset;

    /* Compute payload length:
     * frame_size = l3_offset_bytes + IP_HDR + [UDP_HDR] + payload + FCS
     */
    int overhead = l3_offset + IP_HDR_LEN + FCS_LEN;
    if (cfg->use_udp)
        overhead += UDP_HDR_LEN;
    int payload_len = (int)frame_size - overhead;
    if (payload_len < 8)
        payload_len = 8; /* minimum: 8-byte sequence number */

    /* IPv4 header */
    struct iphdr *iph = (struct iphdr *)(buf + l3_offset);
    iph->ihl      = 5;
    iph->version  = 4;
    iph->tos      = cfg->tos;
    int ip_total  = IP_HDR_LEN + (cfg->use_udp ? UDP_HDR_LEN : 0) + payload_len;
    iph->tot_len  = htons(ip_total);
    iph->id       = htons((uint16_t)(seq & 0xffff));
    iph->frag_off = 0;
    iph->ttl      = 64;
    iph->protocol = cfg->use_udp ? IPPROTO_UDP : IPPROTO_RAW;
    iph->saddr    = cfg->src_ip;
    iph->daddr    = cfg->dst_ip;
    iph->check    = 0;
    iph->check    = checksum(iph, IP_HDR_LEN);
    offset += IP_HDR_LEN;

    uint8_t *payload_ptr;

    if (cfg->use_udp) {
        struct udphdr *udph = (struct udphdr *)(buf + offset);
        udph->source = htons(cfg->src_port);
        udph->dest   = htons(cfg->dst_port);
        udph->len    = htons(UDP_HDR_LEN + payload_len);
        udph->check  = 0;
        offset += UDP_HDR_LEN;
        payload_ptr = buf + offset;

        /* Embed 64-bit sequence number */
        uint64_t seq_be = htobe64(seq);
        memcpy(payload_ptr, &seq_be, 8);
        /* Fill rest of payload with pattern */
        for (int i = 8; i < payload_len; i++)
            payload_ptr[i] = (uint8_t)(i & 0xff);

        udph->check = udp_checksum(iph, udph, payload_ptr, payload_len);
    } else {
        payload_ptr = buf + offset;
        uint64_t seq_be = htobe64(seq);
        memcpy(payload_ptr, &seq_be, 8);
        for (int i = 8; i < payload_len; i++)
            payload_ptr[i] = (uint8_t)(i & 0xff);
    }

    /* Return bytes to write (excluding FCS which kernel appends) */
    return (int)frame_size - FCS_LEN;
}

/*
 * Match an inbound packet against expected reflected headers.
 * Juniper ip-swap: src_ip/dst_ip are swapped in reflection.
 * Returns 1 if matched, 0 otherwise. Sets *seq_out if matched.
 */
int match_reflected(const config_t *cfg, const uint8_t *pkt, uint32_t pkt_len,
                    uint64_t *seq_out)
{
    int offset = 0;

    if (pkt_len < ETH_HDR_LEN)
        return 0;

    const struct ethhdr *eth = (const struct ethhdr *)pkt;

    /* Check MACs: reflector swaps src/dst MACs */
    /* Reflected: src=cfg->dst_mac, dst=cfg->src_mac */
    if (memcmp(eth->h_dest,   cfg->src_mac, 6) != 0)
        return 0;
    if (memcmp(eth->h_source, cfg->dst_mac, 6) != 0)
        return 0;

    offset = ETH_HDR_LEN;
    uint16_t ethertype = ntohs(eth->h_proto);

    /* Skip 802.1Q VLAN tag if present */
    if (ethertype == 0x8100) {
        if (pkt_len < (uint32_t)(offset + 4))
            return 0;
        offset += 2; /* TCI */
        ethertype = ntohs(*(const uint16_t *)(pkt + offset));
        offset += 2;
    }

    if (ethertype != ETH_P_IP)
        return 0;

    if (pkt_len < (uint32_t)(offset + IP_HDR_LEN))
        return 0;

    const struct iphdr *iph = (const struct iphdr *)(pkt + offset);
    int ihl = iph->ihl * 4;

    /* ip-swap: reflected src_ip = original dst_ip, reflected dst_ip = original src_ip */
    if (iph->saddr != cfg->dst_ip)
        return 0;
    if (iph->daddr != cfg->src_ip)
        return 0;

    offset += ihl;

    const uint8_t *payload_ptr;
    int payload_len;

    if (cfg->use_udp) {
        if (iph->protocol != IPPROTO_UDP)
            return 0;
        if (pkt_len < (uint32_t)(offset + UDP_HDR_LEN))
            return 0;
        const struct udphdr *udph = (const struct udphdr *)(pkt + offset);
        /* UDP port swap on reflection */
        if (ntohs(udph->source) != cfg->dst_port)
            return 0;
        if (ntohs(udph->dest) != cfg->src_port)
            return 0;
        offset += UDP_HDR_LEN;
        payload_ptr = pkt + offset;
        payload_len = (int)pkt_len - offset;
    } else {
        payload_ptr = pkt + offset;
        payload_len = (int)pkt_len - offset;
    }

    if (payload_len < 8)
        return 0;

    uint64_t seq_be;
    memcpy(&seq_be, payload_ptr, 8);
    *seq_out = be64toh(seq_be);
    return 1;
}
