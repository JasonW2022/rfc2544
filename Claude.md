# CLAUDE.md
**Project Instructions for RFC 2544 Traffic Generator**

## Project Overview (WHY)
This project implements a high-performance, Linux-native C-language traffic generator and analyzer for RFC 2544 throughput testing. It acts as the initiator/sender while a remote Juniper device operates in RFC 2544 reflect mode (`services rpm rfc2544-benchmarking` with `mode reflect`, `ip-swap`, etc.). 

The goal is accurate, zero-loss throughput measurement (pps and Mbps) across standard frame sizes without introducing artifacts from the generator itself.

## Core Requirements (WHAT)
- **Single interface topology**: TX and RX occur on the **same** network interface (e.g., eth0 or enpXs0). The program must open the interface once and correctly handle both outbound generated packets and inbound reflected packets.
- **Traffic types**: IPv4 only. Optional UDP (Layer 4). Optional single 802.1Q VLAN tag (with VID and PCP/priority). User-configurable IPv4 ToS.
- **Juniper reflector compatibility**: 
  - Use exact source/destination MACs from Juniper config.
  - Support IP swap (and optional UDP port swap).
  - Match reflected packets using swapped headers + 8-byte sequence number in payload.
- **Test methodology**: RFC 2544 compliant throughput test (binary search or stepped rate search until zero frame loss). Support multiple frame sizes (64, 128, ..., 1518 bytes, etc.).
- **Flow control**: Ethernet 802.3x PAUSE frames **must be disabled** on the test interface for accurate results. Always remind/document this (e.g., via `ethtool -A <iface> rx off tx off`).

## Technical Stack & Implementation Rules (HOW)
- **Language**: Pure C (C99 standard). No C++.
- **Networking**: Native Linux AF_PACKET sockets with TPACKET_V3 rings for high-performance zero-copy TX/RX. Prefer memory-mapped buffers.
- **Concurrency**: Recommended — one thread for precise TX pacing (high-resolution timers via `clock_gettime(CLOCK_MONOTONIC)`), one thread for RX and matching.
- **Dependencies**: Minimal. Standard library + Linux headers only (`<linux/if_packet.h>`, `<netinet/ip.h>`, `<net/ethernet.h>`, `<sys/socket.h>`, etc.). `ethtool` command may be used for optional pause checks (via system() or parsing).
- **Performance**: Must sustain wire-rate up to at least 10 Gbps on modern hardware without self-induced loss. Use efficient pacing (microsecond granularity).
- **CLI**: Use `getopt_long` for arguments (`--interface`, `--src-mac`, `--dst-mac`, `--src-ip`, `--dst-ip`, `--tos`, `--vlan-id`, `--vlan-prio`, `--l4 none|udp`, `--frame-sizes`, `--duration`, etc.). Support both CLI and optional JSON config file.
- **Error handling & robustness**: Validate all inputs, interface MTU, root privileges. Clear, actionable error messages. Verbose mode with per-second stats.
- **Output**: Console results + optional CSV/JSON report. Include offered rate, measured throughput, frame loss %, etc.

## Coding Style & Conventions
- Use consistent indentation (4 spaces).
- Descriptive variable/function names (e.g., `build_ipv4_packet()`, `match_reflected_frame()`).
- Add comments for complex header construction and reflection matching logic.
- Keep functions small and focused. Separate packet crafting, transmission pacing, reception, and statistics.
- Always handle byte-order correctly (`htons`, `htonl`).
- Prefer `uint8_t`, `uint16_t`, etc. from `<stdint.h>`.
- No global variables unless strictly necessary (use structs for context).

## Critical Rules (MUST Follow)
1. **Never introduce packet loss** in the generator itself — measure and report only true wire loss.
2. **Always respect Juniper reflection behavior**: Compute expected received MAC/IP/ports explicitly.
3. **Disable flow control reminder**: In help text, verbose output, and any generated README, clearly state that flow control must be turned off on the Linux interface.
4. **Thread safety**: Protect shared counters (Tx/Rx packets, loss stats) with appropriate synchronization (mutexes or atomics).
5. **Frame size accuracy**: Payload + headers must exactly match the requested frame size (including FCS — kernel usually adds it).
6. **Sequence number**: Embed an incrementing 64-bit sequence in the payload for reliable matching (ignore non-matching packets as background traffic).

## Project Structure (Expected)


Following is example of reflect config on Juniper device:

set services rpm rfc2544-benchmarking tests test-name reflect_elan_l3 source-mac-address 40:b4:f0:01:01:01

set services rpm rfc2544-benchmarking tests test-name reflect_elan_l3 destination-mac-address 40:b4:f0:02:02:02

set services rpm rfc2544-benchmarking tests test-name reflect_elan_l3 service-type elan

set services rpm rfc2544-benchmarking tests test-name reflect_elan_l3 ip-swap

set services rpm rfc2544-benchmarking tests test-name reflect_elan_l3 ignore-test-interface-state

set services rpm rfc2544-benchmarking tests test-name reflect_elan_l3 check-test-interface-mtu

set services rpm rfc2544-benchmarking tests test-name reflect_elan_l3 disable-signature-check

set services rpm rfc2544-benchmarking tests test-name reflect_elan_l3 mode reflect

set services rpm rfc2544-benchmarking tests test-name reflect_elan_l3 family bridge

set services rpm rfc2544-benchmarking tests test-name reflect_elan_l3 direction egress

set services rpm rfc2544-benchmarking tests test-name reflect_elan_l3 test-interface ge-0/0/0.0

set services rpm rfc2544-benchmarking tests test-name reflect_elan_l3 destination-ipv4-address 192.168.101.1
