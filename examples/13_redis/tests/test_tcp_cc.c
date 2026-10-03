#include <assert.h>
#include <stdio.h>
#include "../tcp_cc.h"
int main(void)
{
    struct tcp_cc congestion_control;
    tcp_cc_init(&congestion_control, 0xfffffff0u, 1460);
    assert(tcp_seq_after(10, 0xfffffff0u));
    assert(tcp_seq_before(0xfffffff0u, 10));
    uint32_t old = congestion_control.congestion_window;
    tcp_cc_ack(&congestion_control, 1444, 1460);
    assert(congestion_control.congestion_window == old + 1460);
    congestion_control.slow_start_threshold = congestion_control.congestion_window;
    old = congestion_control.congestion_window;
    for (int i = 0; i < 11; ++i)
        tcp_cc_ack(&congestion_control, congestion_control.send_unacked_seq + 1460, 1460);
    assert(congestion_control.congestion_window == old + 1460);
    assert(!tcp_cc_dupack(&congestion_control, congestion_control.send_unacked_seq + 12000));
    assert(!tcp_cc_dupack(&congestion_control, congestion_control.send_unacked_seq + 12000));
    assert(tcp_cc_dupack(&congestion_control, congestion_control.send_unacked_seq + 12000));
    assert(congestion_control.in_fast_recovery && congestion_control.slow_start_threshold == 6000);
    tcp_cc_ack(&congestion_control, congestion_control.send_unacked_seq + 1460, 1460);
    assert(!congestion_control.in_fast_recovery && congestion_control.congestion_window == 6000);
    tcp_cc_timeout(&congestion_control, congestion_control.send_unacked_seq + 12000);
    assert(congestion_control.congestion_window == 1460 &&
           congestion_control.retransmit_timeout_ms == 2000);
    tcp_cc_rtt(&congestion_control, 100);
    assert(congestion_control.smoothed_rtt_ms == 100 &&
           congestion_control.retransmit_timeout_ms == 1000);
    tcp_cc_rtt(&congestion_control, 200);
    assert(congestion_control.smoothed_rtt_ms == 112 && congestion_control.rtt_variation_ms == 62);
    for (int i = 0; i < 20; ++i)
        tcp_cc_timeout(&congestion_control, congestion_control.send_unacked_seq + 1460);
    assert(congestion_control.retransmit_timeout_ms == 60000 &&
           congestion_control.slow_start_threshold >= 2920);
    puts("tcp_cc: wraparound, slow start, additive increase, fast recovery, RTO passed");
    return 0;
}
