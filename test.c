#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <inttypes.h>
#include "rfc2544.h"

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/*
 * Run a single rate trial.
 * Returns frame loss percentage (0.0 = no loss).
 */
double run_trial(config_t *cfg, ring_ctx_t *ring, uint32_t frame_size,
                 double target_pps, uint32_t duration, stats_t *stats)
{
    /* Reset stats */
    atomic_store(&stats->tx_packets, 0);
    atomic_store(&stats->rx_packets, 0);
    atomic_store(&stats->rx_matched, 0);
    atomic_store(&stats->tx_bytes, 0);
    atomic_store(&stats->rx_bytes, 0);

    thread_ctx_t ctx = {
        .cfg        = cfg,
        .ring       = ring,
        .stats      = stats,
        .frame_size = frame_size,
        .target_pps = target_pps,
        .stop       = 0,
    };

    pthread_t tx_tid, rx_tid;
    pthread_create(&rx_tid, NULL, rx_thread, &ctx);
    pthread_create(&tx_tid, NULL, tx_thread, &ctx);

    /* Per-second verbose stats */
    uint64_t start = now_ns();
    for (uint32_t s = 0; s < duration; s++) {
        uint64_t wake = start + (uint64_t)(s + 1) * 1000000000ULL;
        struct timespec ts = {
            .tv_sec  = (time_t)(wake / 1000000000ULL),
            .tv_nsec = (long)(wake % 1000000000ULL),
        };
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);

        if (cfg->verbose) {
            printf("  [%us] tx=%" PRIu64 " rx_matched=%" PRIu64 "\n",
                   s + 1,
                   (uint64_t)atomic_load(&stats->tx_packets),
                   (uint64_t)atomic_load(&stats->rx_matched));
        }
    }

    ctx.stop = 1;
    pthread_join(tx_tid, NULL);
    pthread_join(rx_tid, NULL);

    uint64_t tx_total = atomic_load(&stats->tx_packets);
    uint64_t rx_total = atomic_load(&stats->rx_matched);

    if (tx_total == 0)
        return 100.0;

    double loss = (double)(tx_total > rx_total ? tx_total - rx_total : 0)
                  / (double)tx_total * 100.0;
    return loss;
}

static double frame_to_line_rate_pps(uint32_t frame_size)
{
    /* 10 Gbps line rate, frame_size bytes + 20 bytes IFG+preamble */
    double bits_per_frame = (frame_size + 20) * 8.0;
    return 10e9 / bits_per_frame;
}

/*
 * Binary search for zero-loss throughput.
 */
void run_throughput_test(config_t *cfg, ring_ctx_t *ring, uint32_t frame_size)
{
    stats_t stats;
    memset(&stats, 0, sizeof(stats));

    double max_pps = frame_to_line_rate_pps(frame_size);
    double lo = 0.0, hi = max_pps;
    double best_pps = 0.0;
    int iterations = cfg->binary_search ? 10 : (int)(100.0 / cfg->step_pct + 1);

    printf("\n=== Frame size: %u bytes ===\n", frame_size);
    printf("  Line rate: %.0f pps\n", max_pps);

    if (cfg->binary_search) {
        for (int i = 0; i < iterations; i++) {
            double mid = (lo + hi) / 2.0;
            printf("  Trial %d: %.0f pps (%.2f%% line rate)\n",
                   i + 1, mid, mid / max_pps * 100.0);
            double loss = run_trial(cfg, ring, frame_size, mid, cfg->duration, &stats);
            printf("  -> loss=%.4f%%\n", loss);
            if (loss < 0.0001) {
                best_pps = mid;
                lo = mid;
            } else {
                hi = mid;
            }
        }
    } else {
        /* Stepped search from step_pct% to 100% */
        for (double pct = cfg->step_pct; pct <= 100.0; pct += cfg->step_pct) {
            double trial_pps = max_pps * pct / 100.0;
            printf("  Trial: %.1f%% = %.0f pps\n", pct, trial_pps);
            double loss = run_trial(cfg, ring, frame_size, trial_pps, cfg->duration, &stats);
            printf("  -> loss=%.4f%%\n", loss);
            if (loss < 0.0001)
                best_pps = trial_pps;
            else
                break;
        }
    }

    double best_mbps = best_pps * frame_size * 8.0 / 1e6;
    printf("\n  Zero-loss throughput: %.0f pps = %.2f Mbps (%.2f%% line rate)\n",
           best_pps, best_mbps, best_pps / max_pps * 100.0);

    uint64_t tx_total = atomic_load(&stats.tx_packets);
    uint64_t rx_total = atomic_load(&stats.rx_matched);
    double final_loss = tx_total > 0
        ? (double)(tx_total > rx_total ? tx_total - rx_total : 0) / tx_total * 100.0
        : 100.0;

    print_results(cfg, frame_size, best_pps, best_pps, final_loss, best_mbps);
    if (cfg->csv_output[0])
        write_csv(cfg, frame_size, best_pps, best_pps, final_loss, best_mbps);
}

void print_results(const config_t *cfg, uint32_t frame_size,
                   double offered_pps, double measured_pps,
                   double loss_pct, double mbps)
{
    (void)cfg;
    printf("RESULT | frame=%u offered_pps=%.0f measured_pps=%.0f "
           "loss=%.4f%% throughput=%.2fMbps\n",
           frame_size, offered_pps, measured_pps, loss_pct, mbps);
}

void write_csv(const config_t *cfg, uint32_t frame_size,
               double offered_pps, double measured_pps,
               double loss_pct, double mbps)
{
    FILE *f = fopen(cfg->csv_output, "a");
    if (!f) {
        perror("fopen csv");
        return;
    }
    fprintf(f, "%u,%.0f,%.0f,%.4f,%.2f\n",
            frame_size, offered_pps, measured_pps, loss_pct, mbps);
    fclose(f);
}
