#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <time.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if_arp.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include "rfc2544.h"

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "RFC 2544 Throughput Tester (Linux AF_PACKET / TPACKET_V3)\n"
        "\n"
        "IMPORTANT: Disable flow control on the test interface before running:\n"
        "  ethtool -A <iface> rx off tx off\n"
        "\n"
        "Options:\n"
        "  --interface <iface>       Network interface (required)\n"
        "  --src-mac <MAC>           Source MAC (default: interface MAC)\n"
        "  --dst-mac <MAC>           Destination MAC (required unless --gateway)\n"
        "  --gateway <IP>            Resolve dst-mac via ARP for this gateway IP\n"
        "  --src-ip <IP>             Source IPv4 address (required)\n"
        "  --dst-ip <IP>             Destination IPv4 address (required)\n"
        "  --tos <0-255>             IPv4 ToS/DSCP byte (default: 0)\n"
        "  --vlan-id <1-4094>        802.1Q VLAN ID (default: none)\n"
        "  --vlan-prio <0-7>         802.1Q PCP priority (default: 0)\n"
        "  --l4 <none|udp>           Layer 4 protocol (default: none)\n"
        "  --src-port <port>         UDP source port (default: 1234)\n"
        "  --dst-port <port>         UDP destination port (default: 5678)\n"
        "  --frame-sizes <list>      Comma-separated frame sizes in bytes\n"
        "                            (default: 64,128,256,512,1024,1280,1518)\n"
        "  --duration <seconds>      Trial duration (default: 10)\n"
        "  --binary-search           Use binary search (default)\n"
        "  --stepped-search <pct>    Use stepped search with given step %%\n"
        "  --rate <Mbps>             Fixed offered rate (skip search)\n"
        "  --csv <file>              Write results to CSV file\n"
        "  --json <file>             Write results to JSON file\n"
        "  --verbose                 Print per-second statistics\n"
        "  --help                    Show this help\n"
        "\n"
        "Juniper reflect config example:\n"
        "  set services rpm rfc2544-benchmarking tests <name> mode reflect\n"
        "  set services rpm rfc2544-benchmarking tests <name> ip-swap\n"
        "\n",
        prog);
}

static int parse_mac(const char *str, uint8_t *mac)
{
    unsigned int b[6];
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6 &&
        sscanf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++)
        mac[i] = (uint8_t)b[i];
    return 0;
}

static void set_default_frame_sizes(config_t *cfg)
{
    static const uint32_t defaults[] = {64, 128, 256, 512, 1024, 1280, 1518};
    cfg->n_frame_sizes = (int)(sizeof(defaults) / sizeof(defaults[0]));
    for (int i = 0; i < cfg->n_frame_sizes; i++)
        cfg->frame_sizes[i] = defaults[i];
}

/* Read the hardware MAC address of a network interface. */
static int get_iface_mac(const char *iface, uint8_t *mac)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return -1;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    memcpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        perror("ioctl SIOCGIFHWADDR");
        close(sock);
        return -1;
    }
    close(sock);
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}

/*
 * Resolve the MAC address of gateway_ip on iface via ARP.
 *
 * Strategy:
 *   1. Send a single UDP datagram to gateway_ip to force the kernel to
 *      perform ARP (or use a cached entry).
 *   2. Poll /proc/net/arp up to ~2 s for the entry to appear as complete.
 *   3. Fall back to SIOCGARP ioctl query.
 */
static int resolve_gateway_mac(const char *iface, const char *gateway_ip,
                                uint8_t *mac)
{
    struct in_addr gw_addr;
    if (inet_pton(AF_INET, gateway_ip, &gw_addr) != 1) {
        fprintf(stderr, "Invalid gateway IP: %s\n", gateway_ip);
        return -1;
    }

    /* Trigger ARP by connecting+sending a tiny UDP datagram. */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) {
        /* Bind to the right interface so ARP goes out the correct port. */
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        memcpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
        setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE,
                   ifr.ifr_name, (socklen_t)strlen(ifr.ifr_name));

        struct sockaddr_in peer;
        memset(&peer, 0, sizeof(peer));
        peer.sin_family      = AF_INET;
        peer.sin_addr        = gw_addr;
        peer.sin_port        = htons(33434); /* traceroute-style probe port */
        sendto(sock, "", 1, 0, (struct sockaddr *)&peer, sizeof(peer));
        close(sock);
    }

    /* Poll /proc/net/arp for up to 2 s (20 × 100 ms) */
    for (int attempt = 0; attempt < 20; attempt++) {
        struct timespec ts = {0, 100000000L}; /* 100 ms */
        nanosleep(&ts, NULL);

        FILE *f = fopen("/proc/net/arp", "r");
        if (!f)
            break;

        char line[256];
        /* Skip header */
        if (!fgets(line, sizeof(line), f)) {
            fclose(f);
            break;
        }

        while (fgets(line, sizeof(line), f)) {
            char ip_str[32], hw_type[8], flags_str[8], mac_str[32], mask[8], dev[32];
            if (sscanf(line, "%31s %7s %7s %31s %7s %31s",
                       ip_str, hw_type, flags_str, mac_str, mask, dev) != 6)
                continue;
            if (strcmp(ip_str, gateway_ip) != 0)
                continue;
            /* flags=0x0 means incomplete ARP */
            unsigned long flags = strtoul(flags_str, NULL, 16);
            if (flags == 0)
                continue;
            if (strcmp(mac_str, "00:00:00:00:00:00") == 0)
                continue;
            fclose(f);
            if (parse_mac(mac_str, mac) < 0)
                return -1;
            return 0;
        }
        fclose(f);
    }

    /* Last resort: SIOCGARP ioctl */
    int asock = socket(AF_INET, SOCK_DGRAM, 0);
    if (asock < 0) {
        perror("socket for SIOCGARP");
        return -1;
    }
    struct arpreq arpreq;
    memset(&arpreq, 0, sizeof(arpreq));
    struct sockaddr_in *sin = (struct sockaddr_in *)&arpreq.arp_pa;
    sin->sin_family      = AF_INET;
    sin->sin_addr        = gw_addr;
    memcpy(arpreq.arp_dev, iface, sizeof(arpreq.arp_dev) - 1);
    if (ioctl(asock, SIOCGARP, &arpreq) < 0) {
        perror("ioctl SIOCGARP");
        close(asock);
        fprintf(stderr,
                "Could not resolve ARP for gateway %s on %s.\n"
                "Try: arping -c 1 -I %s %s\n",
                gateway_ip, iface, iface, gateway_ip);
        return -1;
    }
    close(asock);
    if (!(arpreq.arp_flags & ATF_COM)) {
        fprintf(stderr, "ARP entry for %s is incomplete\n", gateway_ip);
        return -1;
    }
    memcpy(mac, arpreq.arp_ha.sa_data, 6);
    return 0;
}

int parse_args(int argc, char **argv, config_t *cfg)
{
    static struct option opts[] = {
        {"interface",      required_argument, NULL, 'i'},
        {"src-mac",        required_argument, NULL, 'S'},
        {"dst-mac",        required_argument, NULL, 'D'},
        {"gateway",        required_argument, NULL, 'g'},
        {"src-ip",         required_argument, NULL, 's'},
        {"dst-ip",         required_argument, NULL, 'd'},
        {"tos",            required_argument, NULL, 't'},
        {"vlan-id",        required_argument, NULL, 'V'},
        {"vlan-prio",      required_argument, NULL, 'P'},
        {"l4",             required_argument, NULL, 'L'},
        {"src-port",       required_argument, NULL, 'p'},
        {"dst-port",       required_argument, NULL, 'q'},
        {"frame-sizes",    required_argument, NULL, 'f'},
        {"duration",       required_argument, NULL, 'u'},
        {"binary-search",  no_argument,       NULL, 'b'},
        {"stepped-search", required_argument, NULL, 'e'},
        {"rate",           required_argument, NULL, 'r'},
        {"csv",            required_argument, NULL, 'c'},
        {"json",           required_argument, NULL, 'j'},
        {"verbose",        no_argument,       NULL, 'v'},
        {"help",           no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    memset(cfg, 0, sizeof(*cfg));
    cfg->duration      = DEFAULT_DURATION;
    cfg->src_port      = DEFAULT_SRC_PORT;
    cfg->dst_port      = DEFAULT_DST_PORT;
    cfg->binary_search = 1;
    cfg->step_pct      = 10.0;
    set_default_frame_sizes(cfg);

    int got_src_mac = 0, got_dst_mac = 0, got_src_ip = 0, got_dst_ip = 0;

    int c;
    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (c) {
        case 'i':
            strncpy(cfg->iface, optarg, sizeof(cfg->iface) - 1);
            break;
        case 'S':
            if (parse_mac(optarg, cfg->src_mac) < 0) {
                fprintf(stderr, "Invalid src-mac: %s\n", optarg);
                return -1;
            }
            got_src_mac = 1;
            break;
        case 'D':
            if (parse_mac(optarg, cfg->dst_mac) < 0) {
                fprintf(stderr, "Invalid dst-mac: %s\n", optarg);
                return -1;
            }
            got_dst_mac = 1;
            break;
        case 'g':
            strncpy(cfg->gateway, optarg, sizeof(cfg->gateway) - 1);
            break;
        case 's':
            if (inet_pton(AF_INET, optarg, &cfg->src_ip) != 1) {
                fprintf(stderr, "Invalid src-ip: %s\n", optarg);
                return -1;
            }
            got_src_ip = 1;
            break;
        case 'd':
            if (inet_pton(AF_INET, optarg, &cfg->dst_ip) != 1) {
                fprintf(stderr, "Invalid dst-ip: %s\n", optarg);
                return -1;
            }
            got_dst_ip = 1;
            break;
        case 't':
            cfg->tos = (uint8_t)strtoul(optarg, NULL, 0);
            break;
        case 'V':
            cfg->vlan_id  = (uint16_t)strtoul(optarg, NULL, 0);
            cfg->use_vlan = 1;
            break;
        case 'P':
            cfg->vlan_prio = (uint8_t)strtoul(optarg, NULL, 0);
            break;
        case 'L':
            if (strcmp(optarg, "udp") == 0)
                cfg->use_udp = 1;
            else if (strcmp(optarg, "none") != 0) {
                fprintf(stderr, "Unknown l4 type: %s (use none|udp)\n", optarg);
                return -1;
            }
            break;
        case 'p':
            cfg->src_port = (uint16_t)strtoul(optarg, NULL, 0);
            break;
        case 'q':
            cfg->dst_port = (uint16_t)strtoul(optarg, NULL, 0);
            break;
        case 'f': {
            cfg->n_frame_sizes = 0;
            char *tok = strtok(optarg, ",");
            while (tok && cfg->n_frame_sizes < MAX_FRAME_SIZES) {
                cfg->frame_sizes[cfg->n_frame_sizes++] =
                    (uint32_t)strtoul(tok, NULL, 0);
                tok = strtok(NULL, ",");
            }
            break;
        }
        case 'u':
            cfg->duration = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'b':
            cfg->binary_search = 1;
            break;
        case 'e':
            cfg->binary_search = 0;
            cfg->step_pct = strtod(optarg, NULL);
            break;
        case 'r':
            cfg->rate_bps = strtod(optarg, NULL) * 1e6;
            break;
        case 'c':
            strncpy(cfg->csv_output, optarg, sizeof(cfg->csv_output) - 1);
            break;
        case 'j':
            strncpy(cfg->json_output, optarg, sizeof(cfg->json_output) - 1);
            break;
        case 'v':
            cfg->verbose = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 1;
        default:
            usage(argv[0]);
            return -1;
        }
    }

    /* Validate required args */
    if (!cfg->iface[0]) {
        fprintf(stderr, "Error: --interface is required\n");
        return -1;
    }
    if (!got_src_ip) {
        fprintf(stderr, "Error: --src-ip is required\n");
        return -1;
    }
    if (!got_dst_ip) {
        fprintf(stderr, "Error: --dst-ip is required\n");
        return -1;
    }
    if (!got_dst_mac && !cfg->gateway[0]) {
        fprintf(stderr,
                "Error: either --dst-mac or --gateway must be specified\n");
        return -1;
    }

    /* Warn when both --gateway and --dst-mac are given; gateway wins. */
    if (cfg->gateway[0] && got_dst_mac) {
        fprintf(stderr,
                "Warning: both --gateway and --dst-mac specified; "
                "--dst-mac will be ignored and the gateway MAC will be used\n");
    }

    /* src_mac is resolved later in setup_interface() if not provided. */
    (void)got_src_mac;

    return 0;
}

int setup_interface(config_t *cfg)
{
    cfg->ifindex = (int)if_nametoindex(cfg->iface);
    if (cfg->ifindex == 0) {
        fprintf(stderr, "Interface '%s' not found\n", cfg->iface);
        return -1;
    }

    /* Get MTU */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    memcpy(ifr.ifr_name, cfg->iface, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    if (ioctl(sock, SIOCGIFMTU, &ifr) < 0) {
        perror("ioctl SIOCGIFMTU");
        close(sock);
        return -1;
    }
    cfg->mtu = ifr.ifr_mtu;
    close(sock);

    /* Auto-fill src_mac from interface if not specified on CLI. */
    int src_mac_is_zero = 1;
    for (int i = 0; i < 6; i++) {
        if (cfg->src_mac[i]) { src_mac_is_zero = 0; break; }
    }
    if (src_mac_is_zero) {
        if (get_iface_mac(cfg->iface, cfg->src_mac) < 0) {
            fprintf(stderr,
                    "Could not read MAC of interface %s; use --src-mac\n",
                    cfg->iface);
            return -1;
        }
        printf("Info: src-mac not specified, using interface MAC "
               "%02x:%02x:%02x:%02x:%02x:%02x\n",
               cfg->src_mac[0], cfg->src_mac[1], cfg->src_mac[2],
               cfg->src_mac[3], cfg->src_mac[4], cfg->src_mac[5]);
    }

    /* Resolve dst_mac from gateway if requested. */
    if (cfg->gateway[0]) {
        printf("Info: resolving MAC for gateway %s on %s ...\n",
               cfg->gateway, cfg->iface);
        if (resolve_gateway_mac(cfg->iface, cfg->gateway, cfg->dst_mac) < 0)
            return -1;
        printf("Info: gateway %s -> %02x:%02x:%02x:%02x:%02x:%02x\n",
               cfg->gateway,
               cfg->dst_mac[0], cfg->dst_mac[1], cfg->dst_mac[2],
               cfg->dst_mac[3], cfg->dst_mac[4], cfg->dst_mac[5]);
    }

    /* Validate frame sizes against MTU */
    for (int i = 0; i < cfg->n_frame_sizes; i++) {
        int max_frame = cfg->mtu + 14 + (cfg->use_vlan ? 4 : 0) + 4; /* +FCS */
        if ((int)cfg->frame_sizes[i] > max_frame) {
            fprintf(stderr,
                    "Warning: frame size %u exceeds MTU-derived max %d\n",
                    cfg->frame_sizes[i], max_frame);
        }
    }

    return 0;
}

static void print_test_info(const config_t *cfg)
{
    char sip[INET_ADDRSTRLEN], dip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cfg->src_ip, sip, sizeof(sip));
    inet_ntop(AF_INET, &cfg->dst_ip, dip, sizeof(dip));

    printf("RFC 2544 Throughput Test\n");
    printf("egress interface: %s\n", cfg->iface);
    printf("  src-mac:          %02x:%02x:%02x:%02x:%02x:%02x\n",
           cfg->src_mac[0], cfg->src_mac[1], cfg->src_mac[2],
           cfg->src_mac[3], cfg->src_mac[4], cfg->src_mac[5]);
    printf("  dst-mac:          %02x:%02x:%02x:%02x:%02x:%02x%s\n",
           cfg->dst_mac[0], cfg->dst_mac[1], cfg->dst_mac[2],
           cfg->dst_mac[3], cfg->dst_mac[4], cfg->dst_mac[5],
           cfg->gateway[0] ? " (via gateway ARP)" : "");
    printf("  src-ip:           %s\n", sip);
    printf("  dst-ip:           %s\n", dip);
    if (cfg->use_vlan)
        printf("  VLAN:             id=%u prio=%u\n",
               cfg->vlan_id, cfg->vlan_prio);
    if (cfg->use_udp)
        printf("  L4:               UDP (src-port=%u, dst-port=%u)\n",
               cfg->src_port, cfg->dst_port);
    else
        printf("  L4:               none (raw IP)\n");
    printf("  Duration per trial: %us\n", cfg->duration);
    printf("  Search method:    %s",
           cfg->binary_search ? "binary search" : "stepped search");
    if (!cfg->binary_search)
        printf(" (%.0f%% steps)", cfg->step_pct);
    printf("\n");
    printf("  Frame sizes:");
    for (int i = 0; i < cfg->n_frame_sizes; i++)
        printf(" %u", cfg->frame_sizes[i]);
    printf("\n");
    printf("\n");
    printf("REMINDER: Disable flow control before testing:\n");
    printf("  ethtool -A %s rx off tx off\n\n", cfg->iface);
}

static void write_csv_header(const config_t *cfg)
{
    if (!cfg->csv_output[0])
        return;
    FILE *f = fopen(cfg->csv_output, "w");
    if (!f) {
        perror("fopen csv");
        return;
    }
    fprintf(f, "frame_size,offered_pps,measured_pps,loss_pct,throughput_mbps\n");
    fclose(f);
}

int main(int argc, char **argv)
{
    config_t cfg;
    int ret = parse_args(argc, argv, &cfg);
    if (ret != 0)
        return (ret < 0) ? 1 : 0;

    if (getuid() != 0) {
        fprintf(stderr, "Error: must run as root (AF_PACKET requires CAP_NET_RAW)\n");
        return 1;
    }

    if (setup_interface(&cfg) < 0)
        return 1;

    print_test_info(&cfg);

    ring_ctx_t ring;
    if (setup_rings(&cfg, &ring) < 0)
        return 1;

    write_csv_header(&cfg);

    for (int i = 0; i < cfg.n_frame_sizes; i++) {
        if (cfg.rate_bps > 0.0) {
            stats_t stats;
            memset(&stats, 0, sizeof(stats));
            double pps = cfg.rate_bps / (cfg.frame_sizes[i] * 8.0);
            double loss = run_trial(&cfg, &ring, cfg.frame_sizes[i],
                                    pps, cfg.duration, &stats);
            double mbps = (double)atomic_load(&stats.rx_matched)
                          * cfg.frame_sizes[i] * 8.0 / 1e6 / cfg.duration;
            print_results(&cfg, cfg.frame_sizes[i], pps,
                          (double)atomic_load(&stats.rx_matched) / cfg.duration,
                          loss, mbps);
            if (cfg.csv_output[0])
                write_csv(&cfg, cfg.frame_sizes[i], pps,
                          (double)atomic_load(&stats.rx_matched) / cfg.duration,
                          loss, mbps);
        } else {
            run_throughput_test(&cfg, &ring, cfg.frame_sizes[i]);
        }
    }

    teardown_rings(&ring);
    return 0;
}
