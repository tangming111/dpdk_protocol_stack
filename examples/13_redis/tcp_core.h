/* TCP engine included by main.c: all TCB/queue mutations hold socket_state_lock. */
#ifndef DPDKNET_TCP_CORE_H
#define DPDKNET_TCP_CORE_H
#include <limits.h>
#include <stdint.h>
#include "tcp_cc.h"
#define TCP_RX_CAP 65535u
#define TCP_TX_CAP (1024u * 1024u)
#include "socket_core.h"
#define NG_TCP_MSS 1460u
#define TCP_MAX_RETRIES 8
#define TCP_TIME_WAIT_MS 60000u
#define TCP_IDLE_CLOSE_MS 120000u
typedef enum {
    NG_TCP_STATUS_CLOSED,
    NG_TCP_STATUS_LISTEN,
    NG_TCP_STATUS_SYN_RCVD,
    NG_TCP_STATUS_SYN_SENT,
    NG_TCP_STATUS_ESTABLISHED,
    NG_TCP_STATUS_FIN_WAIT_1,
    NG_TCP_STATUS_FIN_WAIT_2,
    NG_TCP_STATUS_CLOSING,
    NG_TCP_STATUS_TIME_WAIT,
    NG_TCP_STATUS_CLOSE_WAIT,
    NG_TCP_STATUS_LAST_ACK
} NG_TCP_STATUS;
struct tcp_seg {
    struct tcp_seg *next;
    uint32_t start_seq, payload_len;
    uint8_t flags, was_sent, retransmit_count;
    /* CLOCK_MONOTONIC milliseconds; zero last_sent_ms requests retransmission. */
    uint64_t last_sent_ms, first_sent_ms;
    unsigned char *data;
};
struct ng_tcp_stream {
    int fd;
    /* Endpoint IPv4 addresses and ports are stored in network byte order. */
    uint32_t local_ip;
    uint8_t local_mac[RTE_ETHER_ADDR_LEN];
    uint16_t local_port, protocol, peer_port;
    uint32_t peer_ip, send_next_seq, recv_next_seq, send_queued_end_seq;
    NG_TCP_STATUS status;
    struct rte_ring *sndbuf, *rcvbuf; /* retained for source compatibility */
    struct ng_tcp_stream *prev, *next;
    pthread_cond_t cond;
    pthread_mutex_t mutex;
    struct tcp_seg *send_head, *send_tail;
    struct tcp_cc congestion;
    uint32_t send_queue_bytes, recv_read_offset, recv_buffer_bytes, peer_recv_window,
        last_window_update_seq, last_window_update_ack;
    unsigned char *recv_buffer;
    int flags, error, peer_eof, read_shutdown, write_shutdown, ack_pending, backlog, listener_fd;
    int reuse_address, no_delay, keepalive_enabled, keepalive_probe_limit, keepalive_probes_sent;
    uint32_t keepalive_idle_sec, keepalive_interval_sec;
    uint64_t last_received_ms, last_keepalive_probe_ms;
    uint64_t state_changed_ms, last_window_probe_ms, last_arp_request_ms;
};
struct ng_tcp_table {
    int count;
    struct eventpoll *ep;
    struct ng_tcp_stream *streams;
};
static struct ng_tcp_table tcp_table;
static void tcp_fork_child_reset(void)
{
    tcp_table.streams = NULL;
}

static struct ng_tcp_table *tcpInstance(void)
{
    return &tcp_table;
}

static struct ng_tcp_stream *tcp_find(int fd)
{
    struct ng_tcp_stream *tcp_stream;
    for (tcp_stream = tcp_table.streams; tcp_stream; tcp_stream = tcp_stream->next)
        if (tcp_stream->fd == fd)
            return tcp_stream;
    return NULL;
}

static void tcp_seg_free(struct tcp_seg *segment)
{
    if (segment) {
        rte_free(segment->data);
        rte_free(segment);
    }
}

static void tcp_destroy(struct ng_tcp_stream *tcp_stream)
{
    struct tcp_seg *segment = tcp_stream->send_head;
    while (segment) {
        struct tcp_seg *next_segment = segment->next;
        tcp_seg_free(segment);
        segment = next_segment;
    }
    LL_REMOVE(tcp_stream, tcp_table.streams);
    rte_free(tcp_stream->recv_buffer);
    rte_free(tcp_stream);
}

static struct ng_tcp_stream *tcp_new(int fd)
{
    struct ng_tcp_stream *tcp_stream = rte_zmalloc("tcp_tcb", sizeof(*tcp_stream), 0);
    if (!tcp_stream) {
        errno = ENOMEM;
        return NULL;
    }
    tcp_stream->recv_buffer = rte_malloc("tcp_rx", TCP_RX_CAP, 0);
    if (!tcp_stream->recv_buffer) {
        rte_free(tcp_stream);
        errno = ENOMEM;
        return NULL;
    }
    tcp_stream->fd = fd;
    tcp_stream->listener_fd = -1;
    tcp_stream->protocol = IPPROTO_TCP;
    tcp_stream->peer_recv_window = 65535;
    tcp_stream->state_changed_ms = tcp_stream->last_received_ms = socket_now();
    tcp_stream->keepalive_idle_sec = 300;
    tcp_stream->keepalive_interval_sec = 75;
    tcp_stream->keepalive_probe_limit = 9;
    tcp_stream->send_next_seq =
        (uint32_t)(rte_get_timer_cycles() ^ ((uint64_t)(uintptr_t)tcp_stream >> 4));
    tcp_stream->send_queued_end_seq = tcp_stream->send_next_seq;
    tcp_cc_init(&tcp_stream->congestion, tcp_stream->send_next_seq, NG_TCP_MSS);
    memcpy(tcp_stream->local_mac, g_port_mac_addr, 6);
    LL_ADD(tcp_stream, tcp_table.streams);
    return tcp_stream;
}

static uint32_t tcp_seg_end(const struct tcp_seg *segment)
{
    return segment->start_seq + segment->payload_len + !!(segment->flags & RTE_TCP_SYN_FLAG) +
           !!(segment->flags & RTE_TCP_FIN_FLAG);
}

static int tcp_queue(struct ng_tcp_stream *tcp_stream, const void *buf, uint32_t payload_len,
                     uint8_t flags)
{
    struct tcp_seg *segment = rte_zmalloc("tcp_tx", sizeof(*segment), 0);
    if (!segment) {
        errno = ENOMEM;
        return -1;
    }
    if (payload_len) {
        segment->data = rte_malloc("tcp_payload", payload_len, 0);
        if (!segment->data) {
            rte_free(segment);
            errno = ENOMEM;
            return -1;
        }
        memcpy(segment->data, buf, payload_len);
    }
    segment->start_seq = tcp_stream->send_queued_end_seq;
    segment->payload_len = payload_len;
    segment->flags = flags;
    tcp_stream->send_queued_end_seq = tcp_seg_end(segment);
    tcp_stream->send_queue_bytes += payload_len;
    if (tcp_stream->send_tail)
        tcp_stream->send_tail->next = segment;
    else
        tcp_stream->send_head = segment;
    tcp_stream->send_tail = segment;
    return 0;
}

/* The output ring owns an mbuf only after successful enqueue. */
static int tcp_emit(struct ng_tcp_stream *tcp_stream, struct rte_mempool *packet_pool, uint32_t seq,
                    uint8_t flags, const void *data, uint32_t payload_len)
{
    uint8_t *peer_mac = ng_get_dst_macaddr(tcp_stream->peer_ip);
    struct inout_ring *ring = ring_instance();
    if (!peer_mac) {
        uint64_t now_ms = socket_now();
        if (now_ms - tcp_stream->last_arp_request_ms >= 1000) {
            struct rte_mbuf *arp_packet =
                ng_send_arp(packet_pool, RTE_ARP_OP_REQUEST, g_arp_broadcast_mac,
                            tcp_stream->local_ip, tcp_stream->peer_ip);
            if (arp_packet && rte_ring_mp_enqueue(ring->out, arp_packet) < 0)
                rte_pktmbuf_free(arp_packet);
            tcp_stream->last_arp_request_ms = now_ms;
        }
        return -1;
    }
    unsigned tcp_options_len = (flags & RTE_TCP_SYN_FLAG) ? 4 : 0;
    unsigned frame_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                         sizeof(struct rte_tcp_hdr) + tcp_options_len + payload_len;
    struct rte_mbuf *packet = rte_pktmbuf_alloc(packet_pool);
    if (!packet)
        return -1;
    unsigned char *frame_data = (unsigned char *)rte_pktmbuf_append(packet, frame_len);
    if (!frame_data) {
        rte_pktmbuf_free(packet);
        return -1;
    }
    memset(frame_data, 0, frame_len);
    struct rte_ether_hdr *ethernet_header = (void *)frame_data;
    memcpy(ethernet_header->s_addr.addr_bytes, tcp_stream->local_mac, 6);
    memcpy(ethernet_header->d_addr.addr_bytes, peer_mac, 6);
    ethernet_header->ether_type = htons(RTE_ETHER_TYPE_IPV4);
    struct rte_ipv4_hdr *ipv4_header = (void *)(ethernet_header + 1);
    ipv4_header->version_ihl = 0x45;
    ipv4_header->total_length = htons(frame_len - sizeof(*ethernet_header));
    ipv4_header->time_to_live = 64;
    ipv4_header->next_proto_id = IPPROTO_TCP;
    ipv4_header->src_addr = tcp_stream->local_ip;
    ipv4_header->dst_addr = tcp_stream->peer_ip;
    ipv4_header->hdr_checksum = rte_ipv4_cksum(ipv4_header);
    struct rte_tcp_hdr *tcp_header = (void *)(ipv4_header + 1);
    tcp_header->src_port = tcp_stream->local_port;
    tcp_header->dst_port = tcp_stream->peer_port;
    tcp_header->sent_seq = htonl(seq);
    tcp_header->recv_ack = htonl(tcp_stream->recv_next_seq);
    tcp_header->tcp_flags = flags;
    tcp_header->data_off = (5 + tcp_options_len / 4) << 4;
    tcp_header->rx_win = htons(TCP_RX_CAP - tcp_stream->recv_buffer_bytes);
    if (tcp_options_len) {
        unsigned char *option_bytes = (void *)(tcp_header + 1);
        option_bytes[0] = 2;
        option_bytes[1] = 4;
        option_bytes[2] = NG_TCP_MSS >> 8;
        option_bytes[3] = NG_TCP_MSS & 255;
    }
    if (payload_len)
        memcpy((unsigned char *)(tcp_header + 1) + tcp_options_len, data, payload_len);
    tcp_header->cksum = rte_ipv4_udptcp_cksum(ipv4_header, tcp_header);
    if (rte_ring_mp_enqueue(ring->out, packet) < 0) {
        rte_pktmbuf_free(packet);
        return -1;
    }
    tcp_stream->ack_pending = 0;
    return 0;
}

static void tcp_fail(struct ng_tcp_stream *tcp_stream, int error)
{
    tcp_stream->error = error;
    tcp_stream->status = NG_TCP_STATUS_CLOSED;
    tcp_stream->state_changed_ms = socket_now();
}

static void tcp_ack(struct ng_tcp_stream *tcp_stream, uint32_t ack, int duplicate)
{
    if (tcp_seq_after(ack, tcp_stream->send_next_seq) ||
        tcp_seq_before(ack, tcp_stream->congestion.send_unacked_seq))
        return;
    uint64_t now_ms = socket_now();
    if (ack == tcp_stream->congestion.send_unacked_seq) {
        if (duplicate && tcp_stream->send_head && tcp_stream->send_head->was_sent &&
            tcp_cc_dupack(&tcp_stream->congestion, tcp_stream->send_next_seq))
            tcp_stream->send_head->last_sent_ms = 0;
        return;
    }
    uint32_t acked_bytes = ack - tcp_stream->congestion.send_unacked_seq;
    while (tcp_stream->send_head && !tcp_seq_after(tcp_seg_end(tcp_stream->send_head), ack)) {
        struct tcp_seg *segment = tcp_stream->send_head;
        if (segment->was_sent && !segment->retransmit_count)
            tcp_cc_rtt(&tcp_stream->congestion, (uint32_t)(now_ms - segment->first_sent_ms));
        tcp_stream->send_queue_bytes -= segment->payload_len;
        tcp_stream->send_head = segment->next;
        tcp_seg_free(segment);
    }
    if (!tcp_stream->send_head)
        tcp_stream->send_tail = NULL;
    else if (tcp_seq_after(ack, tcp_stream->send_head->start_seq) &&
             tcp_stream->send_head->payload_len) {
        struct tcp_seg *segment = tcp_stream->send_head;
        uint32_t trim = ack - segment->start_seq;
        if (trim <= segment->payload_len) {
            memmove(segment->data, segment->data + trim, segment->payload_len - trim);
            segment->payload_len -= trim;
            segment->start_seq = ack;
            tcp_stream->send_queue_bytes -= trim;
        }
    }
    tcp_cc_ack(&tcp_stream->congestion, ack, acked_bytes);
    /* ACK progress restarts the retransmission timer on the oldest segment. */
    if (tcp_stream->send_head && tcp_stream->send_head->was_sent)
        tcp_stream->send_head->last_sent_ms = now_ms;
    if (tcp_stream->status == NG_TCP_STATUS_FIN_WAIT_1 && ack == tcp_stream->send_queued_end_seq) {
        tcp_stream->status = NG_TCP_STATUS_FIN_WAIT_2;
        tcp_stream->state_changed_ms = now_ms;
    } else if ((tcp_stream->status == NG_TCP_STATUS_LAST_ACK ||
                tcp_stream->status == NG_TCP_STATUS_CLOSING) &&
               ack == tcp_stream->send_queued_end_seq) {
        tcp_stream->status = tcp_stream->status == NG_TCP_STATUS_CLOSING ? NG_TCP_STATUS_TIME_WAIT
                                                                         : NG_TCP_STATUS_CLOSED;
        tcp_stream->state_changed_ms = now_ms;
    }
}

static void tcp_mss(struct ng_tcp_stream *tcp_stream, struct rte_tcp_hdr *tcp_header,
                    unsigned tcp_header_len)
{
    unsigned char *option_bytes = (void *)(tcp_header + 1);
    unsigned options_len = tcp_header_len - sizeof(*tcp_header), i = 0;
    while (i < options_len) {
        unsigned kind = option_bytes[i];
        if (kind == 0)
            break;
        if (kind == 1) {
            ++i;
            continue;
        }
        if (i + 1 >= options_len || option_bytes[i + 1] < 2 ||
            i + option_bytes[i + 1] > options_len)
            break;
        if (kind == 2 && option_bytes[i + 1] == 4) {
            unsigned peer_mss = ((unsigned)option_bytes[i + 2] << 8) | option_bytes[i + 3];
            if (peer_mss >= 64 && peer_mss < tcp_stream->congestion.max_segment_size)
                tcp_stream->congestion.max_segment_size = peer_mss;
        }
        i += option_bytes[i + 1];
    }
}

static int ng_tcp_process(struct rte_mbuf *packet)
{
    unsigned l2 = sizeof(struct rte_ether_hdr), available_bytes = rte_pktmbuf_data_len(packet);
    if (available_bytes < l2 + 40) {
        rte_pktmbuf_free(packet);
        return -1;
    }
    struct rte_ipv4_hdr *ipv4_header = rte_pktmbuf_mtod_offset(packet, struct rte_ipv4_hdr *, l2);
    unsigned ipv4_header_len = (ipv4_header->version_ihl & 15) * 4,
             ipv4_packet_len = ntohs(ipv4_header->total_length);
    if ((ipv4_header->version_ihl >> 4) != 4 || ipv4_header_len != 20 ||
        ipv4_packet_len < ipv4_header_len + 20 || ipv4_packet_len > available_bytes - l2 ||
        (ntohs(ipv4_header->fragment_offset) & 0x3fff) ||
        ipv4_header->dst_addr != g_stack_ipv4_addr || !socket_ipv4_valid(ipv4_header)) {
        rte_pktmbuf_free(packet);
        return -1;
    }
    struct rte_tcp_hdr *tcp_header = (void *)((unsigned char *)ipv4_header + ipv4_header_len);
    unsigned tcp_header_len = (tcp_header->data_off >> 4) * 4;
    if (tcp_header_len < 20 || tcp_header_len > ipv4_packet_len - ipv4_header_len) {
        rte_pktmbuf_free(packet);
        return -1;
    }
    uint16_t check = tcp_header->cksum;
    tcp_header->cksum = 0;
    uint16_t expected = rte_ipv4_udptcp_cksum(ipv4_header, tcp_header);
    tcp_header->cksum = check;
    if (check != expected) {
        rte_pktmbuf_free(packet);
        return -1;
    }
    unsigned payload_len = ipv4_packet_len - ipv4_header_len - tcp_header_len;
    uint32_t segment_seq = ntohl(tcp_header->sent_seq), ack_seq = ntohl(tcp_header->recv_ack),
             advertised_window = ntohs(tcp_header->rx_win);
    pthread_mutex_lock(&socket_state_lock);
    struct ng_tcp_stream *tcp_stream, *listener = NULL;
    for (tcp_stream = tcp_table.streams; tcp_stream; tcp_stream = tcp_stream->next) {
        if (tcp_stream->status == NG_TCP_STATUS_LISTEN &&
            tcp_stream->local_port == tcp_header->dst_port &&
            (!tcp_stream->local_ip || tcp_stream->local_ip == ipv4_header->dst_addr))
            listener = tcp_stream;
        if (tcp_stream->status != NG_TCP_STATUS_LISTEN &&
            tcp_stream->peer_ip == ipv4_header->src_addr &&
            tcp_stream->local_ip == ipv4_header->dst_addr &&
            tcp_stream->peer_port == tcp_header->src_port &&
            tcp_stream->local_port == tcp_header->dst_port)
            break;
    }
    if (!tcp_stream && listener &&
        (tcp_header->tcp_flags & (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG | RTE_TCP_RST_FLAG)) ==
            RTE_TCP_SYN_FLAG) {
        unsigned pending = 0;
        struct ng_tcp_stream *candidate_stream;
        for (candidate_stream = tcp_table.streams; candidate_stream;
             candidate_stream = candidate_stream->next)
            if (candidate_stream->listener_fd == listener->fd && candidate_stream->fd < 0)
                ++pending;
        if (pending < (unsigned)listener->backlog && (tcp_stream = tcp_new(-1))) {
            tcp_stream->local_ip = ipv4_header->dst_addr;
            tcp_stream->peer_ip = ipv4_header->src_addr;
            tcp_stream->local_port = tcp_header->dst_port;
            tcp_stream->peer_port = tcp_header->src_port;
            tcp_stream->listener_fd = listener->fd;
            tcp_stream->recv_next_seq = segment_seq + 1;
            tcp_stream->peer_recv_window = advertised_window;
            tcp_stream->last_window_update_seq = segment_seq;
            tcp_stream->last_window_update_ack = ack_seq;
            tcp_mss(tcp_stream, tcp_header, tcp_header_len);
            tcp_stream->status = NG_TCP_STATUS_SYN_RCVD;
            if (tcp_queue(tcp_stream, NULL, 0, RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG) < 0)
                tcp_fail(tcp_stream, ENOMEM);
        }
        goto done;
    }
    if (!tcp_stream) {
        if (!(tcp_header->tcp_flags & RTE_TCP_RST_FLAG)) {
            struct ng_tcp_stream reset;
            memset(&reset, 0, sizeof(reset));
            reset.local_ip = ipv4_header->dst_addr;
            reset.peer_ip = ipv4_header->src_addr;
            reset.local_port = tcp_header->dst_port;
            reset.peer_port = tcp_header->src_port;
            memcpy(reset.local_mac, g_port_mac_addr, 6);
            reset.recv_next_seq = segment_seq + payload_len +
                                  !!(tcp_header->tcp_flags & RTE_TCP_SYN_FLAG) +
                                  !!(tcp_header->tcp_flags & RTE_TCP_FIN_FLAG);
            tcp_emit(&reset, packet->pool, (tcp_header->tcp_flags & RTE_TCP_ACK_FLAG) ? ack_seq : 0,
                     (tcp_header->tcp_flags & RTE_TCP_ACK_FLAG)
                         ? RTE_TCP_RST_FLAG
                         : RTE_TCP_RST_FLAG | RTE_TCP_ACK_FLAG,
                     NULL, 0);
        }
        goto done;
    }
    if (tcp_header->tcp_flags & RTE_TCP_RST_FLAG) {
        if ((tcp_stream->status == NG_TCP_STATUS_SYN_SENT &&
             (tcp_header->tcp_flags & RTE_TCP_ACK_FLAG) &&
             ack_seq == tcp_stream->send_queued_end_seq) ||
            (tcp_stream->status != NG_TCP_STATUS_SYN_SENT &&
             segment_seq == tcp_stream->recv_next_seq))
            tcp_fail(tcp_stream,
                     tcp_stream->status == NG_TCP_STATUS_SYN_SENT ? ECONNREFUSED : ECONNRESET);
        goto done;
    }
    if (tcp_stream->status == NG_TCP_STATUS_SYN_SENT) {
        if ((tcp_header->tcp_flags & (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG)) ==
                (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG) &&
            ack_seq == tcp_stream->send_queued_end_seq &&
            tcp_stream->send_next_seq == tcp_stream->send_queued_end_seq) {
            tcp_ack(tcp_stream, ack_seq, 0);
            tcp_stream->recv_next_seq = segment_seq + 1;
            tcp_stream->peer_recv_window = advertised_window;
            tcp_stream->last_window_update_seq = segment_seq;
            tcp_stream->last_window_update_ack = ack_seq;
            tcp_mss(tcp_stream, tcp_header, tcp_header_len);
            tcp_stream->status = NG_TCP_STATUS_ESTABLISHED;
            tcp_stream->ack_pending = 1;
            tcp_stream->state_changed_ms = socket_now();
        }
        goto done;
    }
    if (tcp_stream->status == NG_TCP_STATUS_SYN_RCVD) {
        if ((tcp_header->tcp_flags & RTE_TCP_SYN_FLAG) &&
            segment_seq + 1 == tcp_stream->recv_next_seq) {
            if (tcp_stream->send_head)
                tcp_stream->send_head->last_sent_ms = 0;
            goto done;
        }
        if (!(tcp_header->tcp_flags & RTE_TCP_ACK_FLAG) ||
            ack_seq != tcp_stream->send_queued_end_seq ||
            tcp_stream->send_next_seq != tcp_stream->send_queued_end_seq ||
            segment_seq != tcp_stream->recv_next_seq)
            goto done;
        tcp_ack(tcp_stream, ack_seq, 0);
        tcp_stream->status = NG_TCP_STATUS_ESTABLISHED;
    }
    if (tcp_stream->status == NG_TCP_STATUS_CLOSED || tcp_stream->status == NG_TCP_STATUS_LISTEN)
        goto done;
    /* Accept only segments in the receive window. Reordering is recovered by
     * cumulative ACK and sender retransmission; no unbounded out-of-order queue. */
    if (tcp_seq_after(segment_seq, tcp_stream->recv_next_seq) ||
        tcp_seq_before(segment_seq + payload_len + !!(tcp_header->tcp_flags & RTE_TCP_FIN_FLAG),
                       tcp_stream->recv_next_seq)) {
        tcp_stream->ack_pending = 1;
        goto done;
    }
    tcp_stream->last_received_ms = socket_now();
    tcp_stream->keepalive_probes_sent = 0;
    if (tcp_header->tcp_flags & RTE_TCP_ACK_FLAG) {
        if (tcp_seq_after(ack_seq, tcp_stream->send_next_seq)) {
            tcp_stream->ack_pending = 1;
            goto done;
        }
        int update = tcp_seq_after(segment_seq, tcp_stream->last_window_update_seq) ||
                     (segment_seq == tcp_stream->last_window_update_seq &&
                      !tcp_seq_before(ack_seq, tcp_stream->last_window_update_ack));
        int duplicate = !payload_len &&
                        !(tcp_header->tcp_flags & (RTE_TCP_SYN_FLAG | RTE_TCP_FIN_FLAG)) &&
                        advertised_window == tcp_stream->peer_recv_window;
        tcp_ack(tcp_stream, ack_seq, duplicate);
        if (update) {
            tcp_stream->peer_recv_window = advertised_window;
            tcp_stream->last_window_update_seq = segment_seq;
            tcp_stream->last_window_update_ack = ack_seq;
        }
    }
    if (payload_len && !tcp_stream->peer_eof) {
        uint32_t duplicate_prefix_bytes = tcp_stream->recv_next_seq - segment_seq;
        if (duplicate_prefix_bytes < payload_len) {
            uint32_t accepted_bytes = payload_len - duplicate_prefix_bytes;
            if (accepted_bytes > TCP_RX_CAP - tcp_stream->recv_buffer_bytes)
                accepted_bytes = TCP_RX_CAP - tcp_stream->recv_buffer_bytes;
            const unsigned char *src =
                (unsigned char *)tcp_header + tcp_header_len + duplicate_prefix_bytes;
            for (uint32_t i = 0; i < accepted_bytes; ++i)
                tcp_stream->recv_buffer[(tcp_stream->recv_read_offset +
                                         tcp_stream->recv_buffer_bytes + i) %
                                        TCP_RX_CAP] = src[i];
            tcp_stream->recv_buffer_bytes += accepted_bytes;
            tcp_stream->recv_next_seq += accepted_bytes;
        }
        tcp_stream->ack_pending = 1;
    }
    if (tcp_stream->status == NG_TCP_STATUS_TIME_WAIT && (tcp_header->tcp_flags & RTE_TCP_FIN_FLAG))
        tcp_stream->state_changed_ms = socket_now();
    if (!tcp_stream->peer_eof && (tcp_header->tcp_flags & RTE_TCP_FIN_FLAG) &&
        segment_seq + payload_len == tcp_stream->recv_next_seq) {
        ++tcp_stream->recv_next_seq;
        tcp_stream->peer_eof = 1;
        tcp_stream->ack_pending = 1;
        tcp_stream->state_changed_ms = socket_now();
        if (tcp_stream->status == NG_TCP_STATUS_ESTABLISHED)
            tcp_stream->status = NG_TCP_STATUS_CLOSE_WAIT;
        else if (tcp_stream->status == NG_TCP_STATUS_FIN_WAIT_1)
            tcp_stream->status = NG_TCP_STATUS_CLOSING;
        else if (tcp_stream->status == NG_TCP_STATUS_FIN_WAIT_2)
            tcp_stream->status = NG_TCP_STATUS_TIME_WAIT;
    } else if (tcp_header->tcp_flags & (RTE_TCP_FIN_FLAG | RTE_TCP_SYN_FLAG))
        tcp_stream->ack_pending = 1;
done:
    pthread_mutex_unlock(&socket_state_lock);
    rte_pktmbuf_free(packet);
    return 0;
}

static int ng_tcp_out(struct rte_mempool *packet_pool)
{
    pthread_mutex_lock(&socket_state_lock);
    uint64_t now_ms = socket_now();
    struct ng_tcp_stream *tcp_stream = tcp_table.streams;
    while (tcp_stream) {
        struct ng_tcp_stream *next = tcp_stream->next;
        if (tcp_stream->fd < 0 && ((tcp_stream->status == NG_TCP_STATUS_CLOSED) ||
                                   (tcp_stream->status == NG_TCP_STATUS_TIME_WAIT &&
                                    now_ms - tcp_stream->state_changed_ms >= TCP_TIME_WAIT_MS))) {
            tcp_destroy(tcp_stream);
            tcp_stream = next;
            continue;
        }
        if ((tcp_stream->status == NG_TCP_STATUS_FIN_WAIT_2 ||
             tcp_stream->status == NG_TCP_STATUS_CLOSE_WAIT) &&
            tcp_stream->fd < 0 && now_ms - tcp_stream->state_changed_ms > TCP_IDLE_CLOSE_MS)
            tcp_fail(tcp_stream, ETIMEDOUT);
        if (tcp_stream->status != NG_TCP_STATUS_CLOSED &&
            tcp_stream->status != NG_TCP_STATUS_LISTEN) {
            if (tcp_stream->ack_pending)
                tcp_emit(tcp_stream, packet_pool, tcp_stream->send_next_seq, RTE_TCP_ACK_FLAG, NULL,
                         0);
            struct tcp_seg *segment = tcp_stream->send_head;
            if (segment && segment->was_sent &&
                (tcp_stream->peer_recv_window || (segment->flags & RTE_TCP_SYN_FLAG)) &&
                (segment->last_sent_ms == 0 ||
                 now_ms - segment->last_sent_ms >= tcp_stream->congestion.retransmit_timeout_ms)) {
                if (segment->retransmit_count >= TCP_MAX_RETRIES)
                    tcp_fail(tcp_stream, ETIMEDOUT);
                else if (tcp_emit(tcp_stream, packet_pool, segment->start_seq, segment->flags,
                                  segment->data, segment->payload_len) == 0) {
                    if (segment->last_sent_ms)
                        tcp_cc_timeout(&tcp_stream->congestion, tcp_stream->send_next_seq);
                    ++segment->retransmit_count;
                    segment->last_sent_ms = now_ms;
                }
            }
            unsigned segments_left = 32;
            for (segment = tcp_stream->send_head;
                 segment && segments_left && tcp_stream->status != NG_TCP_STATUS_CLOSED;
                 segment = segment->next) {
                if (segment->was_sent)
                    continue;
                uint32_t send_window_bytes =
                    tcp_stream->congestion.congestion_window < tcp_stream->peer_recv_window
                        ? tcp_stream->congestion.congestion_window
                        : tcp_stream->peer_recv_window;
                if (!(segment->flags & RTE_TCP_SYN_FLAG) &&
                    (segment->payload_len || (segment->flags & RTE_TCP_FIN_FLAG))) {
                    uint32_t bytes_in_flight =
                        tcp_stream->send_next_seq - tcp_stream->congestion.send_unacked_seq;
                    if (bytes_in_flight >= send_window_bytes)
                        break;
                    uint32_t available_window_bytes = send_window_bytes - bytes_in_flight;
                    if (segment->payload_len > available_window_bytes) {
                        struct tcp_seg *rest = rte_zmalloc("tcp_split", sizeof(*rest), 0);
                        if (!rest)
                            break;
                        rest->data = rte_malloc("tcp_split_data",
                                                segment->payload_len - available_window_bytes, 0);
                        if (!rest->data) {
                            rte_free(rest);
                            break;
                        }
                        memcpy(rest->data, segment->data + available_window_bytes,
                               segment->payload_len - available_window_bytes);
                        rest->start_seq = segment->start_seq + available_window_bytes;
                        rest->payload_len = segment->payload_len - available_window_bytes;
                        rest->flags = segment->flags;
                        rest->next = segment->next;
                        segment->next = rest;
                        if (tcp_stream->send_tail == segment)
                            tcp_stream->send_tail = rest;
                        segment->payload_len = available_window_bytes;
                    }
                }
                if (tcp_emit(tcp_stream, packet_pool, segment->start_seq, segment->flags,
                             segment->data, segment->payload_len) < 0)
                    break;
                segment->was_sent = 1;
                segment->last_sent_ms = segment->first_sent_ms = now_ms;
                tcp_stream->send_next_seq = tcp_seg_end(segment);
                --segments_left;
            }
            if (tcp_stream->send_head && !tcp_stream->peer_recv_window &&
                now_ms - tcp_stream->last_window_probe_ms >=
                    tcp_stream->congestion.retransmit_timeout_ms) {
                tcp_emit(tcp_stream, packet_pool, tcp_stream->send_next_seq - 1, RTE_TCP_ACK_FLAG,
                         NULL, 0);
                tcp_stream->last_window_probe_ms = now_ms;
            }
            if (tcp_stream->keepalive_enabled && tcp_stream->status == NG_TCP_STATUS_ESTABLISHED &&
                !tcp_stream->send_head &&
                now_ms - tcp_stream->last_received_ms >=
                    (uint64_t)tcp_stream->keepalive_idle_sec * 1000 &&
                now_ms - tcp_stream->last_keepalive_probe_ms >=
                    (uint64_t)tcp_stream->keepalive_interval_sec * 1000) {
                if (tcp_stream->keepalive_probes_sent >= tcp_stream->keepalive_probe_limit)
                    tcp_fail(tcp_stream, ETIMEDOUT);
                else if (tcp_emit(tcp_stream, packet_pool, tcp_stream->send_next_seq - 1,
                                  RTE_TCP_ACK_FLAG, NULL, 0) == 0) {
                    ++tcp_stream->keepalive_probes_sent;
                    tcp_stream->last_keepalive_probe_ms = now_ms;
                }
            }
            if ((tcp_stream->status == NG_TCP_STATUS_SYN_SENT ||
                 tcp_stream->status == NG_TCP_STATUS_SYN_RCVD) &&
                now_ms - tcp_stream->state_changed_ms > TCP_IDLE_CLOSE_MS)
                tcp_fail(tcp_stream, ETIMEDOUT);
        }
        tcp_stream = next;
    }
    pthread_mutex_unlock(&socket_state_lock);
    return 0;
}

static int tcp_socket(int domain, int type, int protocol)
{
    if (stack_fork_child) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (domain != AF_INET || (type & 15) != SOCK_STREAM || (protocol && protocol != IPPROTO_TCP)) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    pthread_mutex_lock(&socket_state_lock);
    int fd = socket_fd_alloc(STACK_SOCKET_TCP);
    if (fd >= 0) {
        struct ng_tcp_stream *tcp_stream = tcp_new(fd);
        if (!tcp_stream) {
            socket_fd_release(fd);
            fd = -1;
        } else {
            tcp_stream->flags = (type & SOCK_NONBLOCK) ? O_NONBLOCK : 0;
            syscall(SYS_fcntl, fd, F_SETFD, (type & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
        }
    }
    pthread_mutex_unlock(&socket_state_lock);
    return fd;
}

static int tcp_ephemeral(struct ng_tcp_stream *tcp_stream)
{
    static uint16_t port = 49151;
    for (unsigned i = 0; i < 16384; ++i) {
        if (++port < 49152)
            port = 49152;
        struct ng_tcp_stream *candidate_stream;
        for (candidate_stream = tcp_table.streams; candidate_stream;
             candidate_stream = candidate_stream->next)
            if (candidate_stream != tcp_stream && candidate_stream->local_port == htons(port))
                break;
        if (!candidate_stream) {
            tcp_stream->local_port = htons(port);
            return 0;
        }
    }
    errno = EADDRINUSE;
    return -1;
}

static int tcp_bind(int fd, const struct sockaddr *socket_addr, socklen_t address_len)
{
    if (!socket_addr || address_len < sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    const struct sockaddr_in *other_socket_addr = (const void *)socket_addr;
    if (other_socket_addr->sin_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (other_socket_addr->sin_addr.s_addr &&
        other_socket_addr->sin_addr.s_addr != g_stack_ipv4_addr) {
        errno = EADDRNOTAVAIL;
        return -1;
    }
    pthread_mutex_lock(&socket_state_lock);
    struct ng_tcp_stream *tcp_stream = tcp_find(fd);
    int result = -1;
    if (!tcp_stream)
        errno = EBADF;
    else if (tcp_stream->local_port || tcp_stream->status != NG_TCP_STATUS_CLOSED)
        errno = EINVAL;
    else {
        struct ng_tcp_stream *candidate_stream;
        for (candidate_stream = tcp_table.streams; candidate_stream;
             candidate_stream = candidate_stream->next)
            /* close() releases the fd before the data plane reaps its CLOSED
             * TCB. That dead record must not reserve the listener port. Keep
             * live bound sockets and closing/TIME_WAIT connections in the
             * normal conflict checks below. */
            if (candidate_stream != tcp_stream &&
                !(candidate_stream->fd < 0 && candidate_stream->status == NG_TCP_STATUS_CLOSED) &&
                other_socket_addr->sin_port &&
                candidate_stream->local_port == other_socket_addr->sin_port &&
                (candidate_stream->status == NG_TCP_STATUS_LISTEN || !tcp_stream->reuse_address ||
                 candidate_stream->status == NG_TCP_STATUS_CLOSED))
                break;
        if (candidate_stream)
            errno = EADDRINUSE;
        else {
            tcp_stream->local_ip = other_socket_addr->sin_addr.s_addr;
            tcp_stream->local_port = other_socket_addr->sin_port;
            result = tcp_stream->local_port ? 0 : tcp_ephemeral(tcp_stream);
        }
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int tcp_listen(int fd, int backlog)
{
    pthread_mutex_lock(&socket_state_lock);
    struct ng_tcp_stream *tcp_stream = tcp_find(fd);
    int result = -1;
    if (!tcp_stream)
        errno = EBADF;
    else if (tcp_stream->status != NG_TCP_STATUS_CLOSED &&
             tcp_stream->status != NG_TCP_STATUS_LISTEN)
        errno = EINVAL;
    else if (tcp_stream->local_port || tcp_ephemeral(tcp_stream) == 0) {
        tcp_stream->status = NG_TCP_STATUS_LISTEN;
        tcp_stream->backlog = backlog < 1 ? 1 : backlog > 128 ? 128 : backlog;
        result = 0;
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int tcp_name(int fd, struct sockaddr *socket_addr, socklen_t *address_len, int peer)
{
    pthread_mutex_lock(&socket_state_lock);
    struct ng_tcp_stream *tcp_stream = tcp_find(fd);
    int result = -1;
    if (!tcp_stream)
        errno = EBADF;
    else if (peer && !tcp_stream->peer_port)
        errno = ENOTCONN;
    else
        result = socket_copy_addr(socket_addr, address_len,
                               peer ? tcp_stream->peer_ip : tcp_stream->local_ip,
                               peer ? tcp_stream->peer_port : tcp_stream->local_port);
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int tcp_accept(int fd, struct sockaddr *socket_addr, socklen_t *address_len)
{
    if (socket_addr && !address_len) {
        errno = EFAULT;
        return -1;
    }
    pthread_mutex_lock(&socket_state_lock);
    int result = -1;
    for (;;) {
        struct ng_tcp_stream *listener = tcp_find(fd), *candidate_stream;
        if (!listener) {
            errno = EBADF;
            break;
        }
        if (listener->status != NG_TCP_STATUS_LISTEN) {
            errno = EINVAL;
            break;
        }
        for (candidate_stream = tcp_table.streams; candidate_stream;
             candidate_stream = candidate_stream->next)
            if (candidate_stream->listener_fd == fd && candidate_stream->fd < 0 &&
                (candidate_stream->status == NG_TCP_STATUS_ESTABLISHED ||
                 candidate_stream->status == NG_TCP_STATUS_CLOSE_WAIT))
                break;
        if (candidate_stream) {
            result = socket_fd_alloc(STACK_SOCKET_TCP);
            if (result < 0)
                break;
            candidate_stream->fd = result;
            candidate_stream->listener_fd = -1;
            syscall(SYS_fcntl, result, F_SETFD, 0);
            if (socket_addr)
                socket_copy_addr(socket_addr, address_len, candidate_stream->peer_ip,
                              candidate_stream->peer_port);
            break;
        }
        if (listener->flags & O_NONBLOCK) {
            errno = EAGAIN;
            break;
        }
        socket_pause();
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int tcp_connect(int fd, const struct sockaddr *socket_addr, socklen_t address_len)
{
    if (!socket_addr || address_len < sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    const struct sockaddr_in *other_socket_addr = (const void *)socket_addr;
    if (other_socket_addr->sin_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    pthread_mutex_lock(&socket_state_lock);
    struct ng_tcp_stream *tcp_stream = tcp_find(fd);
    int result = -1;
    if (!tcp_stream)
        errno = EBADF;
    else if (tcp_stream->status == NG_TCP_STATUS_SYN_SENT)
        errno = EALREADY;
    else if (tcp_stream->status != NG_TCP_STATUS_CLOSED || tcp_stream->peer_port)
        errno = EISCONN;
    else if (tcp_stream->local_port || tcp_ephemeral(tcp_stream) == 0) {
        tcp_stream->local_ip = g_stack_ipv4_addr;
        tcp_stream->peer_ip = other_socket_addr->sin_addr.s_addr;
        tcp_stream->peer_port = other_socket_addr->sin_port;
        tcp_stream->state_changed_ms = socket_now();
        tcp_stream->status = NG_TCP_STATUS_SYN_SENT;
        if (tcp_queue(tcp_stream, NULL, 0, RTE_TCP_SYN_FLAG) < 0)
            tcp_fail(tcp_stream, ENOMEM);
        for (;;) {
            tcp_stream = tcp_find(fd);
            if (!tcp_stream) {
                errno = EBADF;
                break;
            }
            if (tcp_stream->error) {
                errno = tcp_stream->error;
                break;
            }
            if (tcp_stream->status == NG_TCP_STATUS_ESTABLISHED) {
                result = 0;
                break;
            }
            if (tcp_stream->flags & O_NONBLOCK) {
                errno = EINPROGRESS;
                break;
            }
            socket_pause();
        }
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static ssize_t tcp_send(int fd, const void *buffer, size_t buffer_len, int flags)
{
    if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL | MSG_MORE)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (buffer_len && !buffer) {
        errno = EFAULT;
        return -1;
    }
    pthread_mutex_lock(&socket_state_lock);
    ssize_t result = -1;
    for (;;) {
        struct ng_tcp_stream *tcp_stream = tcp_find(fd);
        if (!tcp_stream) {
            errno = EBADF;
            break;
        }
        if (tcp_stream->error) {
            errno = tcp_stream->error;
            break;
        }
        if (tcp_stream->write_shutdown) {
            errno = EPIPE;
            break;
        }
        if (tcp_stream->status != NG_TCP_STATUS_ESTABLISHED &&
            tcp_stream->status != NG_TCP_STATUS_CLOSE_WAIT) {
            errno = ENOTCONN;
            break;
        }
        if (!buffer_len) {
            result = 0;
            break;
        }
        uint32_t room = TCP_TX_CAP - tcp_stream->send_queue_bytes;
        if (room) {
            size_t count = buffer_len < room ? buffer_len : room, queued_bytes = 0;
            while (queued_bytes < count) {
                uint32_t part = count - queued_bytes;
                if (part > tcp_stream->congestion.max_segment_size)
                    part = tcp_stream->congestion.max_segment_size;
                if (tcp_queue(tcp_stream, (const char *)buffer + queued_bytes, part,
                              RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG) < 0)
                    break;
                queued_bytes += part;
            }
            if (queued_bytes)
                result = queued_bytes;
            break;
        }
        if ((tcp_stream->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) {
            errno = EAGAIN;
            break;
        }
        socket_pause();
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static ssize_t tcp_recv(int fd, void *buffer, size_t buffer_len, int flags)
{
    if (flags & ~(MSG_DONTWAIT | MSG_PEEK | MSG_WAITALL)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (buffer_len && !buffer) {
        errno = EFAULT;
        return -1;
    }
    pthread_mutex_lock(&socket_state_lock);
    ssize_t result = -1;
    size_t received_bytes = 0;
    for (;;) {
        struct ng_tcp_stream *tcp_stream = tcp_find(fd);
        if (!tcp_stream) {
            errno = EBADF;
            break;
        }
        if (!buffer_len) {
            result = 0;
            break;
        }
        if (tcp_stream->recv_buffer_bytes) {
            size_t chunk_bytes = buffer_len - received_bytes;
            if (chunk_bytes > tcp_stream->recv_buffer_bytes)
                chunk_bytes = tcp_stream->recv_buffer_bytes;
            for (size_t i = 0; i < chunk_bytes; ++i)
                ((unsigned char *)buffer)[received_bytes + i] =
                    tcp_stream->recv_buffer[(tcp_stream->recv_read_offset + i) % TCP_RX_CAP];
            if (!(flags & MSG_PEEK)) {
                tcp_stream->recv_read_offset =
                    (tcp_stream->recv_read_offset + chunk_bytes) % TCP_RX_CAP;
                tcp_stream->recv_buffer_bytes -= chunk_bytes;
                tcp_stream->ack_pending = 1;
            }
            received_bytes += chunk_bytes;
            result = received_bytes;
            if (!(flags & MSG_WAITALL) || (flags & MSG_PEEK) || received_bytes == buffer_len)
                break;
        }
        if (tcp_stream->error) {
            errno = tcp_stream->error;
            break;
        }
        if (tcp_stream->peer_eof || tcp_stream->read_shutdown) {
            result = received_bytes;
            break;
        }
        if (tcp_stream->status != NG_TCP_STATUS_ESTABLISHED &&
            tcp_stream->status != NG_TCP_STATUS_FIN_WAIT_1 &&
            tcp_stream->status != NG_TCP_STATUS_FIN_WAIT_2) {
            errno = ENOTCONN;
            break;
        }
        if ((tcp_stream->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) {
            errno = EAGAIN;
            break;
        }
        socket_pause();
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int tcp_finish(struct ng_tcp_stream *tcp_stream, int how)
{
    if (how < SHUT_RD || how > SHUT_RDWR) {
        errno = EINVAL;
        return -1;
    }
    if (tcp_stream->status != NG_TCP_STATUS_ESTABLISHED &&
        tcp_stream->status != NG_TCP_STATUS_CLOSE_WAIT) {
        errno = ENOTCONN;
        return -1;
    }
    if (how != SHUT_WR) {
        tcp_stream->read_shutdown = 1;
        tcp_stream->recv_buffer_bytes = 0;
        tcp_stream->ack_pending = 1;
    }
    if (how != SHUT_RD && !tcp_stream->write_shutdown) {
        if (tcp_queue(tcp_stream, NULL, 0, RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG) < 0)
            return -1;
        tcp_stream->write_shutdown = 1;
        tcp_stream->status = tcp_stream->status == NG_TCP_STATUS_CLOSE_WAIT
                                 ? NG_TCP_STATUS_LAST_ACK
                                 : NG_TCP_STATUS_FIN_WAIT_1;
        tcp_stream->state_changed_ms = socket_now();
    }
    return 0;
}

static int tcp_shutdown(int fd, int how)
{
    pthread_mutex_lock(&socket_state_lock);
    struct ng_tcp_stream *tcp_stream = tcp_find(fd);
    int result;
    if (tcp_stream)
        result = tcp_finish(tcp_stream, how);
    else {
        errno = EBADF;
        result = -1;
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int tcp_close(int fd)
{
    pthread_mutex_lock(&socket_state_lock);
    struct ng_tcp_stream *tcp_stream = tcp_find(fd);
    if (!tcp_stream) {
        pthread_mutex_unlock(&socket_state_lock);
        errno = EBADF;
        return -1;
    }
    if (tcp_stream->status == NG_TCP_STATUS_LISTEN) {
        struct ng_tcp_stream *candidate_stream = tcp_table.streams;
        while (candidate_stream) {
            struct ng_tcp_stream *next = candidate_stream->next;
            if (candidate_stream->listener_fd == fd && candidate_stream->fd < 0)
                tcp_destroy(candidate_stream);
            candidate_stream = next;
        }
        tcp_stream->status = NG_TCP_STATUS_CLOSED;
    } else if (tcp_stream->status == NG_TCP_STATUS_ESTABLISHED ||
               tcp_stream->status == NG_TCP_STATUS_CLOSE_WAIT) {
        if (tcp_finish(tcp_stream, SHUT_WR) < 0)
            tcp_fail(tcp_stream, ENOMEM);
    } else if (tcp_stream->status == NG_TCP_STATUS_SYN_SENT ||
               tcp_stream->status == NG_TCP_STATUS_SYN_RCVD)
        tcp_stream->status = NG_TCP_STATUS_CLOSED;
    tcp_stream->fd = -1;
    tcp_stream->listener_fd = -1;
    int result = socket_fd_release(fd);
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int tcp_fcntl(int fd, int cmd, long arg)
{
    pthread_mutex_lock(&socket_state_lock);
    struct ng_tcp_stream *tcp_stream = tcp_find(fd);
    int result = -1;
    if (!tcp_stream)
        errno = EBADF;
    else
        switch (cmd) {
            case F_GETFL:
                result = O_RDWR | tcp_stream->flags;
                break;
            case F_SETFL:
                tcp_stream->flags = arg & O_NONBLOCK;
                result = 0;
                break;
            case F_GETFD:
            case F_SETFD:
                result = syscall(SYS_fcntl, fd, cmd, arg);
                break;
            default:
                errno = EOPNOTSUPP;
                break;
        }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int tcp_ready(int fd)
{
    pthread_mutex_lock(&socket_state_lock);
    struct ng_tcp_stream *tcp_stream = tcp_find(fd);
    int ev = 0;
    if (!tcp_stream)
        ev = EPOLLERR;
    else if (tcp_stream->status == NG_TCP_STATUS_LISTEN) {
        struct ng_tcp_stream *candidate_stream;
        for (candidate_stream = tcp_table.streams; candidate_stream;
             candidate_stream = candidate_stream->next)
            if (candidate_stream->listener_fd == fd && candidate_stream->fd < 0 &&
                (candidate_stream->status == NG_TCP_STATUS_ESTABLISHED ||
                 candidate_stream->status == NG_TCP_STATUS_CLOSE_WAIT)) {
                ev = EPOLLIN;
                break;
            }
    } else {
        if (tcp_stream->recv_buffer_bytes || tcp_stream->peer_eof || tcp_stream->read_shutdown)
            ev |= EPOLLIN;
        if (tcp_stream->peer_eof)
            ev |= EPOLLRDHUP;
        if ((tcp_stream->status == NG_TCP_STATUS_ESTABLISHED ||
             tcp_stream->status == NG_TCP_STATUS_CLOSE_WAIT) &&
            !tcp_stream->write_shutdown && tcp_stream->send_queue_bytes < TCP_TX_CAP)
            ev |= EPOLLOUT;
        if (tcp_stream->error)
            ev |= EPOLLERR | EPOLLIN | EPOLLOUT;
        if (tcp_stream->status == NG_TCP_STATUS_CLOSED && tcp_stream->peer_port)
            ev |= EPOLLHUP;
    }
    pthread_mutex_unlock(&socket_state_lock);
    return ev;
}

static int tcp_ioctl(int fd, unsigned long request, void *arg)
{
    pthread_mutex_lock(&socket_state_lock);
    struct ng_tcp_stream *tcp_stream = tcp_find(fd);
    int result = -1;
    if (!tcp_stream)
        errno = EBADF;
    else if (!arg)
        errno = EFAULT;
    else if (request == FIONBIO) {
        tcp_stream->flags = *(int *)arg ? O_NONBLOCK : 0;
        result = 0;
    } else if (request == FIONREAD) {
        *(int *)arg = tcp_stream->recv_buffer_bytes;
        result = 0;
    } else
        errno = ENOTTY;
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int tcp_setsockopt(int fd, int level, int opt, const void *value, socklen_t size)
{
    if (!value || size < sizeof(int)) {
        errno = EINVAL;
        return -1;
    }
    int option_value;
    memcpy(&option_value, value, sizeof(option_value));
    int result = 0;
    pthread_mutex_lock(&socket_state_lock);
    struct ng_tcp_stream *tcp_stream = tcp_find(fd);
    if (!tcp_stream) {
        errno = EBADF;
        result = -1;
    } else if (level == SOL_SOCKET) {
        switch (opt) {
            case SO_REUSEADDR:
                tcp_stream->reuse_address = !!option_value;
                break;
            case SO_KEEPALIVE:
                tcp_stream->keepalive_enabled = !!option_value;
                tcp_stream->keepalive_probes_sent = 0;
                tcp_stream->last_received_ms = socket_now();
                break;
            case SO_SNDBUF:
            case SO_RCVBUF:
                if (option_value <= 0) {
                    errno = EINVAL;
                    result = -1;
                }
                break;
            default:
                errno = ENOPROTOOPT;
                result = -1;
        }
    } else if (level == IPPROTO_TCP &&
               (opt == TCP_KEEPIDLE || opt == TCP_KEEPINTVL || opt == TCP_KEEPCNT)) {
        if (option_value <= 0 || option_value > 32767) {
            errno = EINVAL;
            result = -1;
        } else if (opt == TCP_KEEPIDLE)
            tcp_stream->keepalive_idle_sec = option_value;
        else if (opt == TCP_KEEPINTVL)
            tcp_stream->keepalive_interval_sec = option_value;
        else
            tcp_stream->keepalive_probe_limit = option_value;
    } else if (level == IPPROTO_TCP && opt == TCP_NODELAY)
        tcp_stream->no_delay = !!option_value;
    else if (level == IPPROTO_TCP && opt == TCP_MAXSEG) {
        if (option_value < 64 || option_value > (int)NG_TCP_MSS || tcp_stream->peer_port) {
            errno = EINVAL;
            result = -1;
        } else
            tcp_stream->congestion.max_segment_size = option_value;
    } else {
        errno = ENOPROTOOPT;
        result = -1;
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int tcp_getsockopt(int fd, int level, int opt, void *value, socklen_t *size)
{
    if (!value || !size) {
        errno = EFAULT;
        return -1;
    }
    pthread_mutex_lock(&socket_state_lock);
    struct ng_tcp_stream *tcp_stream = tcp_find(fd);
    int option_value = 0, result = 0;
    if (!tcp_stream) {
        errno = EBADF;
        result = -1;
    } else if (level == SOL_SOCKET) {
        switch (opt) {
            case SO_ERROR:
                option_value = tcp_stream->error;
                tcp_stream->error = 0;
                break;
            case SO_TYPE:
                option_value = SOCK_STREAM;
                break;
            case SO_ACCEPTCONN:
                option_value = tcp_stream->status == NG_TCP_STATUS_LISTEN;
                break;
            case SO_REUSEADDR:
                option_value = tcp_stream->reuse_address;
                break;
            case SO_KEEPALIVE:
                option_value = tcp_stream->keepalive_enabled;
                break;
            case SO_SNDBUF:
                option_value = TCP_TX_CAP;
                break;
            case SO_RCVBUF:
                option_value = TCP_RX_CAP;
                break;
            default:
                errno = ENOPROTOOPT;
                result = -1;
        }
    } else if (level == IPPROTO_TCP && opt == TCP_NODELAY)
        option_value = tcp_stream->no_delay;
    else if (level == IPPROTO_TCP && opt == TCP_MAXSEG)
        option_value = tcp_stream->congestion.max_segment_size;
    else if (level == IPPROTO_TCP && opt == TCP_KEEPIDLE)
        option_value = tcp_stream->keepalive_idle_sec;
    else if (level == IPPROTO_TCP && opt == TCP_KEEPINTVL)
        option_value = tcp_stream->keepalive_interval_sec;
    else if (level == IPPROTO_TCP && opt == TCP_KEEPCNT)
        option_value = tcp_stream->keepalive_probe_limit;
    else if (level == IPPROTO_TCP && opt == TCP_INFO) {
        struct tcp_info info;
        memset(&info, 0, sizeof(info));
        static const uint8_t states[] = {TCP_CLOSE,      TCP_LISTEN,      TCP_SYN_RECV,
                                         TCP_SYN_SENT,   TCP_ESTABLISHED, TCP_FIN_WAIT1,
                                         TCP_FIN_WAIT2,  TCP_CLOSING,     TCP_TIME_WAIT,
                                         TCP_CLOSE_WAIT, TCP_LAST_ACK};
        info.tcpi_state = states[tcp_stream->status];
        info.tcpi_snd_cwnd =
            tcp_stream->congestion.congestion_window / tcp_stream->congestion.max_segment_size;
        info.tcpi_snd_ssthresh =
            tcp_stream->congestion.slow_start_threshold / tcp_stream->congestion.max_segment_size;
        info.tcpi_snd_mss = tcp_stream->congestion.max_segment_size;
        info.tcpi_rcv_mss = NG_TCP_MSS;
        info.tcpi_rto = tcp_stream->congestion.retransmit_timeout_ms * 1000;
        info.tcpi_rtt = tcp_stream->congestion.smoothed_rtt_ms * 1000;
        info.tcpi_rttvar = tcp_stream->congestion.rtt_variation_ms * 1000;
        info.tcpi_total_retrans = tcp_stream->congestion.retransmit_count;
        info.tcpi_unacked = (tcp_stream->send_next_seq - tcp_stream->congestion.send_unacked_seq +
                             tcp_stream->congestion.max_segment_size - 1) /
                            tcp_stream->congestion.max_segment_size;
        memcpy(value, &info, *size < sizeof(info) ? *size : sizeof(info));
        *size = *size < sizeof(info) ? *size : sizeof(info);
        pthread_mutex_unlock(&socket_state_lock);
        return 0;
    } else {
        errno = ENOPROTOOPT;
        result = -1;
    }
    if (!result) {
        memcpy(value, &option_value, *size < sizeof(option_value) ? *size : sizeof(option_value));
        *size = *size < sizeof(option_value) ? *size : sizeof(option_value);
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

#endif
