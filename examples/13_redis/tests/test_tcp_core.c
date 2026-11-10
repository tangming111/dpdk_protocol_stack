#include "packet_test_support.h"
#include "../tcp_core.h"
static struct rte_mbuf *pop(void)
{
    assert(output.count);
    struct rte_mbuf *packet = output.items[0];
    memmove(output.items, output.items + 1, --output.count * sizeof(void *));
    return packet;
}

static struct rte_tcp_hdr *wire_tcp(struct rte_mbuf *packet)
{
    return (void *)(packet->data + 14 + 20);
}

static void clear_output(void)
{
    while (output.count)
        rte_pktmbuf_free(pop());
}

static void input(struct ng_tcp_stream *tcp_stream, uint32_t seq, uint32_t ack, unsigned flags,
                  unsigned wnd, const void *data, unsigned n)
{
    struct rte_mbuf *packet = calloc(1, sizeof(*packet));
    packet->size = 54 + n;
    struct rte_ipv4_hdr *ipv4_header = (void *)(packet->data + 14);
    ipv4_header->version_ihl = 0x45;
    ipv4_header->total_length = htons(40 + n);
    ipv4_header->next_proto_id = IPPROTO_TCP;
    ipv4_header->src_addr = tcp_stream->peer_ip;
    ipv4_header->dst_addr = tcp_stream->local_ip;
    ipv4_header->hdr_checksum = rte_ipv4_cksum(ipv4_header);
    struct rte_tcp_hdr *tcp_header = wire_tcp(packet);
    tcp_header->src_port = tcp_stream->peer_port;
    tcp_header->dst_port = tcp_stream->local_port;
    tcp_header->sent_seq = htonl(seq);
    tcp_header->recv_ack = htonl(ack);
    tcp_header->data_off = 0x50;
    tcp_header->tcp_flags = flags;
    tcp_header->rx_win = htons(wnd);
    if (n)
        memcpy(tcp_header + 1, data, n);
    tcp_header->cksum = rte_ipv4_udptcp_cksum(ipv4_header, tcp_header);
    ng_tcp_process(packet);
}

/* Reproduce iperf3 rebuilding its listener before ng_tcp_out reaps the old
 * listener. No data-plane tick is allowed between close and the new bind. */
static void test_listener_rebind(void)
{
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = g_stack_ipv4_addr;
    address.sin_port = htons(15201);
    int enabled = 1;
    for (int reuse = 0; reuse <= 1; ++reuse) {
        int listener_fd = tcp_socket(AF_INET, SOCK_STREAM, 0);
        int replacement_fd = tcp_socket(AF_INET, SOCK_STREAM, 0);
        assert(listener_fd >= 0 && replacement_fd >= 0);
        if (reuse)
            assert(tcp_setsockopt(replacement_fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                                  sizeof(enabled)) == 0);
        assert(tcp_bind(listener_fd, (void *)&address, sizeof(address)) == 0);
        /* A live bound socket is not the same as a closed, unreaped record. */
        assert(tcp_bind(replacement_fd, (void *)&address, sizeof(address)) == -1 &&
               errno == EADDRINUSE);
        assert(tcp_listen(listener_fd, 4) == 0);
        assert(tcp_bind(replacement_fd, (void *)&address, sizeof(address)) == -1 &&
               errno == EADDRINUSE);
        struct ng_tcp_stream *old_listener = tcp_find(listener_fd);
        assert(tcp_close(listener_fd) == 0);
        assert(old_listener->fd < 0 && old_listener->status == NG_TCP_STATUS_CLOSED);
        assert(tcp_bind(replacement_fd, (void *)&address, sizeof(address)) == 0);
        assert(tcp_listen(replacement_fd, 4) == 0);
        int other_fd = tcp_socket(AF_INET, SOCK_STREAM, 0);
        address.sin_port = htons(15202);
        assert(tcp_bind(other_fd, (void *)&address, sizeof(address)) == 0);
        assert(tcp_close(other_fd) == 0);
        address.sin_port = htons(15201);
        assert(tcp_close(replacement_fd) == 0);
        ng_tcp_out(NULL);
        assert(!tcp_table.streams);
    }
    /* The fix must not indiscriminately ignore every fd-less TCB. */
    struct ng_tcp_stream *time_wait = tcp_new(-1);
    assert(time_wait);
    time_wait->local_ip = g_stack_ipv4_addr;
    time_wait->local_port = address.sin_port;
    time_wait->status = NG_TCP_STATUS_TIME_WAIT;
    int replacement_fd = tcp_socket(AF_INET, SOCK_STREAM, 0);
    assert(tcp_bind(replacement_fd, (void *)&address, sizeof(address)) == -1 &&
           errno == EADDRINUSE);
    assert(tcp_setsockopt(replacement_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) ==
           0);
    assert(tcp_bind(replacement_fd, (void *)&address, sizeof(address)) == 0);
    assert(tcp_close(replacement_fd) == 0);
    fake_ms += TCP_TIME_WAIT_MS + 1;
    ng_tcp_out(NULL);
    assert(!tcp_table.streams);
    puts("tcp_core: immediate listener rebind, live-port conflicts, alternate port and TIME_WAIT "
         "passed");
}

int main(void)
{
    assert(tcpInstance() == &tcp_table);
    g_stack_ipv4_addr = htonl(0xc0a80102);
    int fd = tcp_socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    assert(fd >= 0);
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = htonl(0xc0a80103);
    peer.sin_port = htons(5201);
    assert(tcp_connect(fd, (void *)&peer, sizeof(peer)) == -1 && errno == EINPROGRESS);
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    assert(tcp_name(fd, (void *)&local, &local_len, 0) == 0);
    struct ng_tcp_stream *tcp_stream = tcp_find(fd);
    ng_tcp_out(NULL);
    struct rte_mbuf *packet = pop();
    assert(wire_tcp(packet)->tcp_flags == RTE_TCP_SYN_FLAG);
    free(packet);
    uint32_t isn = tcp_stream->send_next_seq;
    input(tcp_stream, 100, isn, RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    assert(tcp_stream->status == NG_TCP_STATUS_ESTABLISHED &&
           tcp_stream->congestion.send_unacked_seq == isn);
    ng_tcp_out(NULL);
    clear_output();
    /* Binary data, partial reads and MSG_PEEK preserve order. */
    const char payload[] = {0, 1, 2, 3, 4, 5};
    input(tcp_stream, 101, isn, RTE_TCP_ACK_FLAG, 65535, payload, 6);
    unsigned char buf[4096];
    assert(tcp_recv(fd, buf, 2, MSG_PEEK) == 2 && tcp_stream->recv_buffer_bytes == 6);
    assert(tcp_recv(fd, buf, 2, 0) == 2 && buf[1] == 1);
    assert(tcp_recv(fd, buf, 8, 0) == 4 && buf[0] == 2 && buf[3] == 5);
    assert(tcp_recv(fd, buf, 1, 0) == -1 && errno == EAGAIN);
    /* Retransmitted payload must never be delivered twice; out-of-order data is not ACKed as
     * received. */
    input(tcp_stream, 101, isn, RTE_TCP_ACK_FLAG, 65535, payload, 6);
    assert(!tcp_stream->recv_buffer_bytes);
    input(tcp_stream, 109, isn, RTE_TCP_ACK_FLAG, 65535, payload, 6);
    assert(tcp_stream->recv_next_seq == 107 && !tcp_stream->recv_buffer_bytes);
    ng_tcp_out(NULL);
    clear_output();
    /* Advertised window smaller than MSS splits an unsent segment. */
    input(tcp_stream, 107, isn, RTE_TCP_ACK_FLAG, 100, NULL, 0);
    memset(buf, 0x5a, sizeof(buf));
    assert(tcp_send(fd, buf, 2000, 0) == 2000);
    ng_tcp_out(NULL);
    packet = pop();
    assert(packet->size == 154);
    uint32_t first = tcp_stream->congestion.send_unacked_seq;
    free(packet);
    assert(tcp_stream->send_next_seq - first == 100 && tcp_stream->send_queue_bytes == 2000);
    input(tcp_stream, 107, first + 100, RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    ng_tcp_out(NULL);
    clear_output();
    assert(tcp_stream->send_next_seq == first + 2000);
    /* A timeout retransmits owned data and reduces cwnd; advancing ACK frees it. */
    uint32_t oldrto = tcp_stream->congestion.retransmit_timeout_ms;
    fake_ms += oldrto + 1;
    ng_tcp_out(NULL);
    assert(output.count &&
           tcp_stream->congestion.congestion_window == tcp_stream->congestion.max_segment_size &&
           tcp_stream->congestion.retransmit_count);
    clear_output();
    input(tcp_stream, 107, tcp_stream->send_next_seq, RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    assert(!tcp_stream->send_head && !tcp_stream->send_queue_bytes);
    /* Zero-window persist, then reopening without consuming application data. */
    input(tcp_stream, 107, tcp_stream->send_next_seq, RTE_TCP_ACK_FLAG, 0, NULL, 0);
    assert(tcp_send(fd, buf, 10, 0) == 10);
    fake_ms += tcp_stream->congestion.retransmit_timeout_ms + 1;
    ng_tcp_out(NULL);
    packet = pop();
    assert(packet->size == 54 &&
           ntohl(wire_tcp(packet)->sent_seq) == tcp_stream->send_next_seq - 1);
    free(packet);
    input(tcp_stream, 107, tcp_stream->send_next_seq, RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    ng_tcp_out(NULL);
    clear_output();
    input(tcp_stream, 107, tcp_stream->send_next_seq, RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    /* Three duplicate ACKs trigger fast retransmit without changing SND.NXT. */
    uint32_t before = tcp_stream->send_next_seq;
    assert(tcp_send(fd, buf, 1000, 0) == 1000);
    ng_tcp_out(NULL);
    clear_output();
    input(tcp_stream, 107, before, RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    input(tcp_stream, 107, before, RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    input(tcp_stream, 107, before, RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    assert(tcp_stream->congestion.in_fast_recovery && tcp_stream->send_head->last_sent_ms == 0);
    ng_tcp_out(NULL);
    assert(output.count && tcp_stream->send_next_seq == before + 1000);
    clear_output();
    input(tcp_stream, 107, tcp_stream->send_next_seq, RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    assert(!tcp_stream->congestion.in_fast_recovery && !tcp_stream->send_head);
    int one = 1;
    assert(tcp_setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) == 0);
    assert(tcp_setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &one, sizeof(one)) == 0);
    assert(tcp_setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &one, sizeof(one)) == 0);
    fake_ms += 1001;
    ng_tcp_out(NULL);
    assert(output.count && tcp_stream->keepalive_probes_sent == 1);
    clear_output();
    input(tcp_stream, 107, tcp_stream->send_next_seq, RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    assert(tcp_stream->keepalive_probes_sent == 0);
    /* FIN carrying bytes delivers data before persistent EOF. */
    input(tcp_stream, 107, tcp_stream->send_next_seq, RTE_TCP_ACK_FLAG | RTE_TCP_FIN_FLAG, 65535,
          "xy", 2);
    assert(tcp_recv(fd, buf, 4, 0) == 2 && buf[0] == 'x');
    assert(tcp_recv(fd, buf, 4, 0) == 0);
    assert(tcp_recv(fd, buf, 4, 0) == 0);
    assert(tcp_close(fd) == 0);
    ng_tcp_out(NULL);
    clear_output();
    input(tcp_stream, 110, tcp_stream->send_next_seq, RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    ng_tcp_out(NULL);
    assert(!netstack_fd_is_managed(fd) && tcp_table.streams == NULL);
    /* Passive handshake validates ACK and accept(NULL,NULL) works. */
    int l = tcp_socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    peer.sin_addr.s_addr = g_stack_ipv4_addr;
    peer.sin_port = htons(6379);
    assert(tcp_bind(l, (void *)&peer, sizeof(peer)) == 0 && tcp_listen(l, 4) == 0);
    assert(tcp_accept(l, NULL, NULL) == -1 && errno == EAGAIN);
    struct ng_tcp_stream template;
    memset(&template, 0, sizeof(template));
    template.local_ip = g_stack_ipv4_addr;
    template.peer_ip = htonl(0xc0a80103);
    template.local_port = htons(6379);
    template.peer_port = htons(1234);
    input(&template, 700, 0, RTE_TCP_SYN_FLAG, 65535, NULL, 0);
    tcp_stream = tcp_table.streams;
    assert(tcp_stream->fd == -1 && tcp_stream->listener_fd == l);
    ng_tcp_out(NULL);
    clear_output();
    input(tcp_stream, 701, tcp_stream->send_next_seq + 1, RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    assert(tcp_stream->status == NG_TCP_STATUS_SYN_RCVD);
    input(tcp_stream, 701, tcp_stream->send_next_seq, RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    int a = tcp_accept(l, NULL, NULL);
    assert(a >= 0 && a != l);
    assert(tcp_close(l) == 0);
    tcp_stream = tcp_find(a);
    assert(tcp_stream);
    tcp_close(a);
    ng_tcp_out(NULL);
    clear_output();
    input(tcp_stream, 701, tcp_stream->send_next_seq, RTE_TCP_ACK_FLAG | RTE_TCP_FIN_FLAG, 65535,
          NULL, 0);
    assert(tcp_stream->status == NG_TCP_STATUS_TIME_WAIT);
    ng_tcp_out(NULL);
    clear_output();
    fake_ms += TCP_TIME_WAIT_MS + 1;
    ng_tcp_out(NULL);
    clear_output();
    assert(!tcp_table.streams);
    test_listener_rebind();
    puts("tcp_core: active/passive handshake, binary stream, partial read, duplicate/order, window "
         "split, retransmit, persist, FIN/EOF passed");
    return 0;
}
