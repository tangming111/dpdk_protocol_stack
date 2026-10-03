/* Portable Reno and RFC 6298-style RTO arithmetic; independent of DPDK. */
#ifndef DPDKNET_TCP_CC_H
#define DPDKNET_TCP_CC_H
#include <stdint.h>
#include <string.h>
struct tcp_cc {
    uint32_t send_unacked_seq, max_segment_size, congestion_window, slow_start_threshold,
        duplicate_ack_count, recovery_end_seq, avoidance_acked_bytes;
    uint32_t smoothed_rtt_ms, rtt_variation_ms, retransmit_timeout_ms, retransmit_count;
    int in_fast_recovery;
};
static inline int tcp_seq_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static inline int tcp_seq_after(uint32_t a, uint32_t b)
{
    return tcp_seq_before(b, a);
}

static inline void tcp_cc_init(struct tcp_cc *congestion_control, uint32_t seq, uint32_t mss)
{
    memset(congestion_control, 0, sizeof(*congestion_control));
    congestion_control->send_unacked_seq = seq;
    congestion_control->max_segment_size = mss;
    congestion_control->congestion_window = 10 * mss;
    congestion_control->slow_start_threshold = 65535;
    congestion_control->retransmit_timeout_ms = 1000;
}

static inline void tcp_cc_rtt(struct tcp_cc *congestion_control, uint32_t sample_rtt_ms)
{
    if (!sample_rtt_ms)
        sample_rtt_ms = 1;
    if (!congestion_control->smoothed_rtt_ms) {
        congestion_control->smoothed_rtt_ms = sample_rtt_ms;
        congestion_control->rtt_variation_ms = sample_rtt_ms / 2;
    } else {
        uint32_t rtt_delta_ms = congestion_control->smoothed_rtt_ms > sample_rtt_ms
                                    ? congestion_control->smoothed_rtt_ms - sample_rtt_ms
                                    : sample_rtt_ms - congestion_control->smoothed_rtt_ms;
        congestion_control->rtt_variation_ms =
            (3 * congestion_control->rtt_variation_ms + rtt_delta_ms) / 4;
        congestion_control->smoothed_rtt_ms =
            (7 * congestion_control->smoothed_rtt_ms + sample_rtt_ms) / 8;
    }
    congestion_control->retransmit_timeout_ms =
        congestion_control->smoothed_rtt_ms + 4 * congestion_control->rtt_variation_ms;
    if (congestion_control->retransmit_timeout_ms < 1000)
        congestion_control->retransmit_timeout_ms = 1000;
    if (congestion_control->retransmit_timeout_ms > 60000)
        congestion_control->retransmit_timeout_ms = 60000;
}

static inline int tcp_cc_dupack(struct tcp_cc *congestion_control, uint32_t send_next_seq)
{
    ++congestion_control->duplicate_ack_count;
    if (congestion_control->duplicate_ack_count == 3) {
        uint32_t half_flight_bytes = (send_next_seq - congestion_control->send_unacked_seq) / 2;
        congestion_control->slow_start_threshold =
            half_flight_bytes > 2 * congestion_control->max_segment_size
                ? half_flight_bytes
                : 2 * congestion_control->max_segment_size;
        congestion_control->congestion_window =
            congestion_control->slow_start_threshold + 3 * congestion_control->max_segment_size;
        congestion_control->recovery_end_seq = send_next_seq;
        congestion_control->in_fast_recovery = 1;
        ++congestion_control->retransmit_count;
        return 1;
    }
    if (congestion_control->in_fast_recovery &&
        congestion_control->congestion_window < 65535 - congestion_control->max_segment_size)
        congestion_control->congestion_window += congestion_control->max_segment_size;
    return 0;
}

static inline void tcp_cc_ack(struct tcp_cc *congestion_control, uint32_t ack, uint32_t acked_bytes)
{
    congestion_control->send_unacked_seq = ack;
    congestion_control->duplicate_ack_count = 0;
    if (congestion_control->in_fast_recovery) {
        /* Reno exits fast recovery at the first advancing ACK. */
        congestion_control->congestion_window = congestion_control->slow_start_threshold;
        congestion_control->in_fast_recovery = 0;
    } else if (congestion_control->congestion_window < congestion_control->slow_start_threshold)
        congestion_control->congestion_window += acked_bytes < congestion_control->max_segment_size
                                                     ? acked_bytes
                                                     : congestion_control->max_segment_size;
    else {
        congestion_control->avoidance_acked_bytes += acked_bytes;
        if (congestion_control->avoidance_acked_bytes >= congestion_control->congestion_window) {
            congestion_control->avoidance_acked_bytes -= congestion_control->congestion_window;
            congestion_control->congestion_window += congestion_control->max_segment_size;
        }
    }
    if (congestion_control->congestion_window > 65535)
        congestion_control->congestion_window = 65535;
}

static inline void tcp_cc_timeout(struct tcp_cc *congestion_control, uint32_t send_next_seq)
{
    uint32_t half_flight_bytes = (send_next_seq - congestion_control->send_unacked_seq) / 2;
    congestion_control->slow_start_threshold =
        half_flight_bytes > 2 * congestion_control->max_segment_size
            ? half_flight_bytes
            : 2 * congestion_control->max_segment_size;
    congestion_control->congestion_window = congestion_control->max_segment_size;
    congestion_control->avoidance_acked_bytes = 0;
    congestion_control->duplicate_ack_count = 0;
    congestion_control->in_fast_recovery = 0;
    ++congestion_control->retransmit_count;
    congestion_control->retransmit_timeout_ms = congestion_control->retransmit_timeout_ms >= 30000
                                                    ? 60000
                                                    : 2 * congestion_control->retransmit_timeout_ms;
}

#endif
