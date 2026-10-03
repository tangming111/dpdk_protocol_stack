# TCP/UDP 协议实现与支持边界

更新日期：2026-10-18。本文集中说明协议行为、资源限制及进程生命周期；代码结构见 [CODE_GUIDE.md](CODE_GUIDE.md)，运行命令见 [BUILD.md](BUILD.md)，测试结果见 [VALIDATION.md](VALIDATION.md)。

IPv4 TCP/UDP 的报文构造、解析、连接状态和收发队列由本库维护，经 DPDK 收发。AF_UNIX、IPv6 和普通文件通过 libc 处理；具体分流规则见代码导读。

## 1. LD_PRELOAD 接口与事件等待

接管动态链接程序通过 libc 调用的 IPv4 TCP 和 UDP socket。支持的主要接口：

- `socket/bind/listen/accept/accept4/connect/shutdown/close`。
- `read/write/recv/send/recvfrom/sendto/readv/writev/recvmsg/sendmsg` 的 TCP 字节流和 UDP 数据报用途。
- `fcntl/fcntl64` 的 GETFL/SETFL/GETFD/SETFD；`ioctl` 的 FIONBIO/FIONREAD。
- `getsockname/getpeername/setsockopt/getsockopt`。
- `epoll_create/epoll_create1/epoll_ctl/epoll_wait/epoll_pwait`、`poll/select`。

每个协议栈 socket 都占用一个真实、空的内核 eventfd，作为描述符编号的保留位。这样不会再把 1024 以上的普通文件误当成协议栈 fd，也能让 iperf3 使用 fd_set。eventfd 本身不传输 TCP 数据。所有 TCB 与发送/接收队列修改由协议栈互斥锁保护。

普通应用的 `epoll` 使用自己的 eventpoll 对象、红黑树订阅和 FIFO 就绪队列。协议线程通过 nready 产生受管事件并写 eventfd 唤醒；管道、AF_UNIX 等原生 fd 由 poll 提供就绪状态，统一交给自己的队列分发。无事件时按调用方超时阻塞，不再由 epoll_wait 每 1ms 扫描。支持 LT、ONESHOT、轮转、epoll_pwait 等待掩码；拒绝 ET、EXCLUSIVE、WAKEUP、自己的 epoll 嵌套和句柄复制。原生 poll/select 的混合适配仍使用 1ms 扫描。

非阻塞 accept/recv/send 在没有资源或数据时返回 EAGAIN。非阻塞 connect 返回 EINPROGRESS，可通过写就绪与 SO_ERROR 判断结果。read 的部分返回值与剩余数据顺序已修正；EOF 在缓冲数据读取完毕后持续返回 0。阻塞等待释放协议栈锁，再重新查找 fd。

已实现 SO_REUSEADDR、SO_KEEPALIVE、TCP_NODELAY、TCP_MAXSEG、TCP_KEEPIDLE/KEEPINTVL/KEEPCNT，以及 SO_ERROR、SO_TYPE、SO_ACCEPTCONN、TCP_INFO 等查询。收发缓冲采用固定上限，SO_SNDBUF/SO_RCVBUF 正数请求接受后按实现上限使用，getsockopt 返回实际大小。其它未实现选项返回 ENOPROTOOPT，不再一律伪造成功。

## 2. TCP 发送窗口与可靠传输

TCB 分开维护三个发送位置：

- `send_queued_end_seq`：应用已排入发送队列数据的末尾。
- `send_next_seq`：已经实际发出的序号末尾。
- `congestion.send_unacked_seq`：尚未累计确认的第一个序号，即 SND.UNA。

在途数据为 `send_next_seq - congestion.send_unacked_seq`。允许发送的窗口为 `min(congestion.congestion_window, peer_recv_window)`；只有还有可用空间才继续发送。应用数据按协商 MSS 分段；窗口不足一个 MSS 时进一步拆分未发送段。SYN/FIN 也占用序号，FIN 受发送窗口约束。

发送段保留在有界队列中，收到合法累计 ACK 后才释放；部分 ACK 只裁剪已确认前缀。发送队列上限为每连接 1MiB，空间不足时返回短写或 EAGAIN。分配失败、输出 ring 满和 mbuf 失败不会让已入队数据被静默遗忘；后续扫描继续尝试。

接收缓冲为每连接 65535 字节的循环字节缓冲。通告窗口反映真实剩余空间，应用读取后触发窗口更新。处理所有带负载的 TCP 段，不再依赖 PSH；重复前缀不会再次交付。当前对未来序号的乱序包发送累计 ACK，丢弃该乱序负载，由发送方恢复，尚未实现乱序重组队列。

所有序号判断采用 32 位有符号差值，覆盖序号回绕。握手验证 ACK；检查 IPv4/TCP 长度、校验和和分片标志。未知连接收到非 RST 报文时回复 RST。接收 RST 时检查连接阶段与序号，避免无条件终止连接。

## 3. Reno 与定时器

- 初始拥塞窗口为 10 MSS；初始 slow_start_threshold 为 65535 字节。
- 慢启动：新的累计 ACK 推进时增长 congestion_window，每个 ACK 的增长不超过一个 MSS。
- 拥塞避免：累计 ACK 字节作为 avoidance_acked_bytes，约每确认一个 congestion_window 增加一个 MSS。
- 三次重复 ACK：按在途数据减半计算 slow_start_threshold，至少两个 MSS，进入快速重传/快速恢复。
- 退出快速恢复：采用 Reno，在首个推进的 ACK 后退出；不是 NewReno，也没有 SACK scoreboard。
- RTO：初值 1 秒；采用平滑 RTT 与 RTT 方差更新，范围 1～60 秒。重传段不采 RTT（Karn）；超时后 congestion_window 降至一个 MSS，RTO 指数退避。
- 最老未确认段负责重传计时；ACK 进展重启该计时，RTT 采样仍保留原始发送时刻。
- 重传次数上限 8；握手长期无进展、FIN_WAIT_2 等阶段有超时处理，错误以 socket error 与就绪事件向应用报告。
- 接收方零窗口时暂停正常数据重传计数，以周期性旧序号 ACK 探测窗口，窗口重新开放后继续发送。
- keepalive_enabled 按已设置的 idle/interval/count 发探测并处理失败；默认关闭。
- TIME_WAIT 保留 60 秒；关闭后的 TCB 在数据面扫描中回收，避免 ACK 到来前释放仍被使用的对象。

这些定时器使用 CLOCK_MONOTONIC，在后台 TCP 数据面循环中检查，不依赖旧的 ENABLE_TIMER 或 ARP 的 rte_timer 开关。

<a id="udp"></a>

绑定冲突检查会忽略已经关闭（fd < 0 且状态为 CLOSED）、等待后台回收的旧控制块，因此关闭监听后可以立即重新绑定同一端口，无需等待数据面扫描。仍然有效的已绑定 socket、监听 socket 和关闭过程中的连接按现有复用规则检查；不会因为 fd < 0 就跳过 SYN_RCVD 或 TIME_WAIT。回归测试同时验证默认配置与 SO_REUSEADDR 下的重新监听。

## 4. UDP

### 4.1 调用与报文路径

```text
socket(AF_INET, SOCK_DGRAM, 0/IPPROTO_UDP)
    -> hook.c
    -> nsocket -> udp_socket_create
    -> eventfd 编号 + UDP endpoint + fd 类型登记

sendto / send / write / sendmsg / writev
    -> UDP 发送队列（每条记录是一个完整数据报）
    -> ng_udp_out -> ARP 查询 -> 构造 Ethernet + IPv4 + UDP
    -> DPDK out ring -> rte_eth_tx_burst

rte_eth_rx_burst -> DPDK in ring
    -> IPv4/UDP 分发 -> ng_udp_process
    -> 长度/校验和检查 -> 按地址和端口匹配 endpoint
    -> UDP 接收队列
    -> recvfrom / recv / read / recvmsg / readv
```

`socket_fd_types[fd]` 现在是类型登记：0 表示不受管，1 表示 TCP，2 表示 UDP。`netstack_fd_is_managed()` 判断是否受管，`nisdgram()` 判断是否为 UDP。eventfd 仅保留编号，不承载 UDP 数据。通用 n* 接口按类型分发到 TCP 或 UDP，避免把两种控制块强转后读取错误字段。

`udp_core.h` 是新的 UDP 实现，由 main.c 包含。旧教学 UDP 函数仍在 main.c 中，但不再作为运行路径；新收包入口为 `ng_udp_process()`。

### 4.2 数据报语义

- 每次 sendto/send/write/sendmsg/writev 对应一个数据报，向量 I/O 先聚合，再作为一个包发送。
- recvfrom/recv/recvmsg/readv 一次最多消费一个数据报，不合并多个包。
- 接收缓冲区太小时，只复制能容纳的前缀，消费整个包，丢弃该包余下字节。不会像旧实现一样把剩余字节重新放回队列。
- MSG_PEEK 不消费数据报；MSG_TRUNC 作为输入标志时返回原包长度；recvmsg 的 msg_flags 在截断时包含 MSG_TRUNC。
- 保留并返回真实来源 IPv4 地址、端口和 sin_family；正确处理调用方提供的小 sockaddr 缓冲区。
- 支持零长度数据报。其接收返回值为 0，但不代表连接 EOF；后续包仍可继续接收。
- UDP connect 不执行握手，只设置默认对端并过滤后续新到达的数据报；connect(AF_UNSPEC) 解除默认对端。
- sendto 可显式指定目标地址；未 connect 且不提供目标时返回 EDESTADDRREQ。
- 未绑定发送时从 49152～65535 分配 UDP 临时端口。TCP 和 UDP 使用独立的端口空间，可以绑定同一数值端口。
- 支持 SO_REUSEADDR 的同端口匹配；已 connect 的完整对端匹配优先于通配监听 socket，满足 iperf3 多 UDP 流的分发需求。

### 4.3 事件、选项与资源

- 支持阻塞及 O_NONBLOCK/MSG_DONTWAIT；无数据时返回 EAGAIN。
- poll/select/epoll 使用 UDP 接收队列和发送队列容量计算就绪事件；零长度数据报也能触发读就绪。
- FIONREAD 返回首个数据报长度，不是全部队列字节数。
- 支持 SO_TYPE、SO_ERROR、SO_REUSEADDR、SO_SNDBUF/SO_RCVBUF，以及 IPv4 TOS、TTL、MTU_DISCOVER 选项；TCP 专有选项不会误用于 UDP。
- 默认每方向最多 1MiB、1024 个数据报；SO_SNDBUF/SO_RCVBUF 调整字节上限，范围 4096～4MiB，包数上限仍为 1024。接收队列满时丢包，发送队列满时非阻塞发送返回 EAGAIN。
- 输出 ring 满或 mbuf 暂时不足时保留数据报，后续循环重试；不会误报为 ARP 超时。
- 未解析到对端 MAC 时发送 ARP 请求，3 秒仍无结果则丢弃该待发数据报，并设置 EHOSTUNREACH。
- close 释放 fd 和接收队列；已排入发送队列的数据报保留到输出完成或 ARP 超时。关闭后端口立即可重新绑定。
- 进程正常退出时的有界排空包含 UDP 发送队列。fork 子进程继续沿用原有 libc 回退策略，不接管父进程的数据面。

UDP 输出会生成非零 UDP 校验和；IPv4 接收允许 UDP checksum 为 0（未启用校验），非零时必须校验通过。畸形长度、错误校验和和 IPv4 分片不交付应用。

### 4.4 限制

当前按 MTU 1500、IPv4 头 20 字节、UDP 头 8 字节实现，单包 UDP 负载最大 **1472 字节**。超出直接返回 EMSGSIZE，不做短写，不自动拆成多个 UDP 数据报，也没有悄悄回退内核。

没有 IP 分片/重组、路由/网关、广播、多播、ICMP 错误回报或 IP_RECVERR 扩展错误队列。对端需要在可直接 ARP 到达的二层网络。不存在 TCP 式重传、拥塞窗口或可靠交付保证，这是 UDP 与 TCP 的语义区别。

某些 iperf3 版本默认 UDP 长度大于 1472，因此请显式指定 `-l 1200`。不能将默认包过大产生的 EMSGSIZE 当成已经回退到了内核。

## 5. Redis 的 fork 与退出问题

注册 pthread_atfork，在 fork 前固定协议栈及事件表的一致状态。父进程继续使用原连接；子进程没有继承可运行的数据面线程，因此清除自己的受管 fd 标记，关闭继承描述符时直接走内核。子进程新建 socket 走 libc，不尝试运行父进程的 DPDK 实例。这样可用于 Redis BGSAVE/AOF 类后台文件工作，但不提供 fork 后继续使用同一用户态网络连接的功能。

正常进程退出时，so 析构函数会排入 FIN，最多等待 2 秒，尽力把待发数据、控制消息和 ACK 排空，再结束两个后台线程。该步骤解决了 iperf3 主进程输出 Done 后，最后一个控制字节仍停留在用户态队列、导致服务端一直等待的问题。SIGKILL、_exit、崩溃不执行正常析构，不能保证排空。

## 6. 当前支持边界

这是已完成上述场景验证的基础用户态 TCP/UDP 实现，还不是完整 Linux socket/TCP 替代品：

1. 当前接管 IPv4 TCP/UDP。iperf3 使用 `-4`；UDP 使用 `-u -l 1200`。UDP 负载超过 1472 字节返回 EMSGSIZE，尚无 IP 分片/重组，见[UDP 限制](#udp)。
2. 当前直接 ARP 查询对端，适用于同一二层网络；没有路由表、网关下一跳选择、PMTU、IPv4 分片重组或 VLAN 处理。
3. 没有 TCP 窗口扩大、SACK、时间戳协商、乱序缓存、NewReno/CUBIC。接收窗口和 congestion_window 都限制在 65535 字节，长 RTT/大带宽链路受限。
4. 未实现 sendfile/splice、pselect/ppoll、epoll_pwait2、异步取消与信号语义的完整 POSIX 行为。socket 的 dup/dup2/dup3/F_DUPFD 显式拒绝；不要使用 iperf3 `-Z`。
5. 未实现的 socket 选项会报错；SO_RCVTIMEO/SO_SNDTIMEO 等选项不受支持。TCP_INFO 提供已实现字段，其余字段为 0。
6. 在同一进程里，首次 IPv4 TCP/UDP socket 会初始化 DPDK；该进程的所有 IPv4 TCP/UDP socket 都由本栈接管。Redis 应显式绑定 DPDKNET_IP，不能假定 loopback TCP 自动绕回内核。
7. Redis 以 `--daemonize no` 启动。已运行的数据面不能在 daemonize 的 fork 子进程中继续使用；后台文件保存与服务进程本身 daemonize 是两回事。
8. 只保证本文记录的版本与测试场景；更新版本 Redis/iperf3、TLS、复制、集群和长时间压力仍需单独验收。没有声称已完成生产级兼容性验证。

构建、运行及重复测试命令见 [BUILD.md](BUILD.md)。
