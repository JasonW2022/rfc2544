# rfc2544 — Linux RFC 2544 Throughput Tester

A high-performance, Linux-native traffic generator and analyzer for **RFC 2544 throughput testing**. Written in pure C (C11), it uses **AF_PACKET / TPACKET_V3** memory-mapped rings for zero-copy, wire-rate packet transmission and reception on a single network interface.

Designed to work against a remote **Juniper device** configured in RFC 2544 reflect mode (`services rpm rfc2544-benchmarking` with `mode reflect` and `ip-swap`).

---

## Features

- **Wire-rate TX/RX on a single interface** — same port sends generated traffic and receives reflected frames
- **TPACKET_V3 zero-copy rings** — sustains 10 Gbps+ on modern hardware
- **Binary search or stepped-rate search** for zero-loss throughput per frame size
- **Juniper ip-swap compatible** — matches reflected packets with swapped src/dst IP and MAC
- **64-bit sequence number** in payload for reliable frame matching (ignores background traffic)
- **802.1Q VLAN tagging** (single tag, configurable VID and PCP)
- **IPv4 ToS/DSCP** control
- **UDP or raw IP** payload
- **Auto-detect src-mac** from the interface if not specified
- **Gateway ARP resolution** — specify `--gateway` instead of `--dst-mac`
- **CSV output** for results
- **Verbose mode** with per-second TX/RX counters

---

## Prerequisites

| Requirement | Notes |
|---|---|
| Linux kernel ≥ 3.2 | TPACKET_V3 support |
| Root / `CAP_NET_RAW` | Required for AF_PACKET sockets |
| GCC ≥ 4.9 | C11 atomics (`_Atomic`) |
| Standard Linux headers | `linux/if_packet.h`, `netinet/ip.h`, etc. |

> **Flow control must be disabled** on the test interface or results will be incorrect:
> ```
> ethtool -A <iface> rx off tx off
> ```

---

## Build

```bash
make
```

Clean:

```bash
make clean
```

---

## Usage

```
sudo ./rfc2544 [OPTIONS]
```

### Options

| Option | Description |
|---|---|
| `--interface <iface>` | Network interface to use **(required)** |
| `--src-mac <MAC>` | Source MAC address (default: interface MAC) |
| `--dst-mac <MAC>` | Destination MAC address (required unless `--gateway`) |
| `--gateway <IP>` | Resolve dst-mac via ARP for this gateway IP |
| `--src-ip <IP>` | Source IPv4 address **(required)** |
| `--dst-ip <IP>` | Destination IPv4 address **(required)** |
| `--tos <0-255>` | IPv4 ToS/DSCP byte (default: 0) |
| `--vlan-id <1-4094>` | 802.1Q VLAN ID (default: none) |
| `--vlan-prio <0-7>` | 802.1Q PCP priority (default: 0) |
| `--l4 <none\|udp>` | Layer 4 protocol (default: none / raw IP) |
| `--src-port <port>` | UDP source port (default: 1234) |
| `--dst-port <port>` | UDP destination port (default: 5678) |
| `--frame-sizes <list>` | Comma-separated frame sizes in bytes (default: `64,128,256,512,1024,1280,1518`) |
| `--duration <sec>` | Trial duration in seconds (default: 10) |
| `--binary-search` | Binary search for zero-loss rate **(default)** |
| `--stepped-search <pct>` | Stepped search, incrementing by `<pct>`% of line rate |
| `--rate <Mbps>` | Fixed offered rate — skips search, runs a single trial |
| `--csv <file>` | Append results to a CSV file |
| `--json <file>` | Write results to a JSON file |
| `--verbose` | Print per-second TX/RX statistics |
| `--help` | Show help and exit |

---

## Examples

### Basic test — auto src-mac, dst-mac from gateway ARP

```bash
sudo ./rfc2544 \
  --interface eth0 \
  --gateway 192.168.1.1 \
  --src-ip 192.168.100.1 \
  --dst-ip 192.168.101.1
```

### Full Juniper reflect test with VLAN and UDP

```bash
sudo ./rfc2544 \
  --interface eth0 \
  --src-mac 40:b4:f0:01:01:01 \
  --dst-mac 40:b4:f0:02:02:02 \
  --src-ip 192.168.100.1 \
  --dst-ip 192.168.101.1 \
  --vlan-id 100 \
  --vlan-prio 6 \
  --l4 udp \
  --src-port 1234 \
  --dst-port 5678 \
  --duration 30 \
  --binary-search \
  --frame-sizes 64,512,1518 \
  --csv results.csv \
  --verbose
```

### Fixed-rate single trial

```bash
sudo ./rfc2544 \
  --interface eth0 \
  --gateway 10.0.0.1 \
  --src-ip 10.0.1.1 \
  --dst-ip 10.0.2.1 \
  --rate 1000 \
  --frame-sizes 1518
```

---

## Startup Output Format

When the test starts it prints a summary of the configured parameters:

```
RFC 2544 Throughput Test
egress interface: eth0
  src-mac:          40:b4:f0:01:01:01
  dst-mac:          40:b4:f0:02:02:02
  src-ip:           192.168.100.1
  dst-ip:           192.168.101.1
  L4:               UDP (src-port=1234, dst-port=5678)
  Duration per trial: 30s
  Search method:    binary search
  Frame sizes:      64 512 1518

REMINDER: Disable flow control before testing:
  ethtool -A eth0 rx off tx off
```

---

## Juniper Reflector Configuration

The remote Juniper device must be configured in reflect mode. Example:

```
set services rpm rfc2544-benchmarking tests reflect_elan_l3 source-mac-address 40:b4:f0:01:01:01
set services rpm rfc2544-benchmarking tests reflect_elan_l3 destination-mac-address 40:b4:f0:02:02:02
set services rpm rfc2544-benchmarking tests reflect_elan_l3 service-type elan
set services rpm rfc2544-benchmarking tests reflect_elan_l3 ip-swap
set services rpm rfc2544-benchmarking tests reflect_elan_l3 ignore-test-interface-state
set services rpm rfc2544-benchmarking tests reflect_elan_l3 check-test-interface-mtu
set services rpm rfc2544-benchmarking tests reflect_elan_l3 disable-signature-check
set services rpm rfc2544-benchmarking tests reflect_elan_l3 mode reflect
set services rpm rfc2544-benchmarking tests reflect_elan_l3 family bridge
set services rpm rfc2544-benchmarking tests reflect_elan_l3 direction egress
set services rpm rfc2544-benchmarking tests reflect_elan_l3 test-interface ge-0/0/0.0
set services rpm rfc2544-benchmarking tests reflect_elan_l3 destination-ipv4-address 192.168.101.1
```

### How reflection works

| Field | Sent (generator) | Reflected (Juniper) |
|---|---|---|
| Ethernet src | `--src-mac` | `--dst-mac` |
| Ethernet dst | `--dst-mac` | `--src-mac` |
| IPv4 src | `--src-ip` | `--dst-ip` |
| IPv4 dst | `--dst-ip` | `--src-ip` |
| UDP src port | `--src-port` | `--dst-port` |
| UDP dst port | `--dst-port` | `--src-port` |

The 64-bit sequence number embedded at the start of each payload is used to match reflected frames back to transmitted frames, regardless of other traffic on the interface.

---

## CSV Output

When `--csv <file>` is specified, results are appended with a header row on first run:

```
frame_size,offered_pps,measured_pps,loss_pct,throughput_mbps
64,14880952,14880952,0.0000,7619.35
512,2343750,2343750,0.0000,9600.00
1518,812226,812226,0.0000,9876.54
```

---

## Project Structure

```
rfc2544/
├── rfc2544.h    — shared types, constants, function prototypes
├── main.c       — CLI parsing, interface setup, gateway ARP, orchestration
├── packet.c     — frame builder and reflection matcher
├── ring.c       — TPACKET_V3 TX/RX ring setup and teardown
├── threads.c    — TX pacing thread and RX matching thread
├── test.c       — binary/stepped search, trial runner, result output
└── Makefile
```

---

## License

MIT
