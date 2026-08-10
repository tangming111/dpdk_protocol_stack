# DPDK 网络协议栈架构文档 (07_netarch)

## 项目概述

`07_netarch` 是一个基于 **DPDK 19.08.2** 实现的用户态轻量级网络协议栈。它在 DPDK 的轮询模式驱动（PMD）之上，直接从网卡收发以太网帧，在用户态实现了 **ARP**、**ICMP** 和 **UDP** 协议的处理，并向上层提供了类 BSD Socket 的编程接口（`nsocket` / `nbind` / `nsendto` / `nrecvfrom` / `nclose`）。

编译产物为 `dpdk_netarch`，支持 shared/static 两种链接方式。

---

## 编译开关一览

代码通过宏开关控制功能模块，全部定义在 [main.c](main.c#L14-L22)：

| 宏 | 含义 |
|---|---|
| `ENABLE_SEND` | 启用发送功能（TX queue 配置） |
| `ENABLE_ARP` | 启用 ARP 包处理 |
| `ENABLE_ICMP` | 启用 ICMP Echo Reply |
| `ENABLE_ARP_REPLY` | 启用默认 ARP 广播 MAC 常量 |
| `ENABLE_DPDK_DUG` | 启用 ARP 表调试打印 |
| `ENABLE_TIMER` | 启用定时器（周期性 ARP 扫描） |
| `ENABLE_RINGBUFFER` | 启用 in/out 环形缓冲区 |
| `ENABLE_MULTHREAD` | 启用多线程（独立包处理线程） |
| `ENABLE_UDP_APP` | 启用 UDP 应用层（socket API + echo server） |

---

## 4+1 架构视图

### 1. 逻辑视图 (Logical View)

描述系统的主要功能模块及其分层依赖关系。

```mermaid
graph TB
    subgraph APP_LAYER["应用层"]
        UES["UDP Echo Server<br/>udp_server_entry()"]
        CUA["自定义 UDP 应用"]
    end

    subgraph SOCKET_LAYER["Socket API 层<br/>(类 POSIX 接口)"]
        NSOCK["nsocket()"]
        NBIND["nbind()"]
        NSEND["nsendto()"]
        NRECV["nrecvfrom()"]
        NCLOSE["nclose()"]
    end

    subgraph PROTO_LAYER["协议处理层"]
        UDP_PROC["UDP 处理<br/>udp_process / udp_out"]
        ICMP_PROC["ICMP 处理<br/>Echo Reply"]
        ARP_PROC["ARP 处理<br/>请求/应答"]
        ARP_TABLE[("ARP 表<br/>IP→MAC 映射")]
    end

    subgraph IO_LAYER["包 I/O 抽象层"]
        IORING["inout_ring<br/>in_ring: RX→处理<br/>out_ring: 处理→TX"]
    end

    subgraph DPDK_LAYER["DPDK 驱动层"]
        RX_BURST["rte_eth_rx_burst()"]
        TX_BURST["rte_eth_tx_burst()"]
        TIMER["rte_timer"]
    end

    subgraph DATA_LAYER["数据面抽象"]
        MBUF_POOL["rte_mempool<br/>(mbuf pool)"]
        MBUF["rte_mbuf"]
    end

    %% 应用层 → Socket API
    UES --> NSOCK
    UES --> NBIND
    UES --> NSEND
    UES --> NRECV
    CUA --> NSOCK
    CUA --> NSEND
    CUA --> NRECV

    %% Socket API → 协议处理
    NSEND --> UDP_PROC
    NRECV --> UDP_PROC
    UDP_PROC --> ARP_TABLE

    %% 协议处理 → I/O 抽象
    ARP_PROC --> IORING
    ICMP_PROC --> IORING
    UDP_PROC --> IORING

    %% I/O 抽象 → DPDK 驱动
    IORING --> TX_BURST
    RX_BURST --> IORING

    %% 驱动 → 数据面
    RX_BURST --> MBUF_POOL
    TX_BURST --> MBUF_POOL
    MBUF --> MBUF_POOL

    %% 定时器
    TIMER --> ARP_PROC

    style APP_LAYER fill:#90EE90,stroke:#333
    style SOCKET_LAYER fill:#87CEEB,stroke:#333
    style PROTO_LAYER fill:#FFD700,stroke:#333
    style IO_LAYER fill:#FFA500,stroke:#333
    style DPDK_LAYER fill:#D3D3D3,stroke:#333
    style DATA_LAYER fill:#DDA0DD,stroke:#333
```

**分层依赖规则**：上层可依赖下层，通过 `rte_ring` 和 `offload` 结构体解耦；跨层通信全部走共享内存（无锁队列）。

---

### 2. 进程视图 (Process View)

描述系统的并发模型：**主线程 + 包处理线程 + UDP 服务线程**，通过 `rte_ring` 和 `pthread` 条件变量通信。

```mermaid
graph LR
    subgraph MAIN_THREAD["主线程 lcore 0"]
        RX["RX 轮询<br/>rte_eth_rx_burst"]
        TX["TX 轮询<br/>rte_eth_tx_burst"]
        TMGMT["定时器管理<br/>rte_timer_manage"]
    end

    subgraph WORKER_THREAD["包处理线程 lcore 1"]
        PARSE["协议解析<br/>Eth→ARP/IP→UDP/ICMP"]
        ARP_REPLY["ARP 应答 / 表更新"]
        ICMP_REPLY["ICMP 回声应答"]
        UDP_IN["UDP 数据入队<br/>→ host->rcv_ring"]
        UDP_OUT["UDP 数据出队<br/>host->snd_ring →"]
    end

    subgraph APP_THREAD["UDP 服务线程 lcore 2"]
        RECV["nrecvfrom<br/>阻塞等待"]
        BIZ["业务处理<br/>回显 / 自定义"]
        SEND["nsendto<br/>发送响应"]
    end

    IN_RING["🔄 in_ring<br/>(rte_ring)"]:::queue
    OUT_RING["🔄 out_ring<br/>(rte_ring)"]:::queue
    RCV_RING["🔄 host->rcv_ring<br/>(per-socket)"]:::queue
    SND_RING["🔄 host->snd_ring<br/>(per-socket)"]:::queue

    RX -->|"sp_enqueue_burst"| IN_RING
    IN_RING -->|"mc_dequeue_burst"| PARSE
    ARP_REPLY --> OUT_RING
    ICMP_REPLY --> OUT_RING
    UDP_OUT --> OUT_RING
    OUT_RING -->|"sc_dequeue_burst"| TX

    PARSE --> UDP_IN
    UDP_IN -->|"sp_enqueue"| RCV_RING
    RCV_RING -->|"sc_dequeue<br/>+pthread_cond_wait"| RECV

    SEND -->|"sp_enqueue"| SND_RING
    SND_RING -->|"sc_dequeue"| UDP_OUT

    UDP_IN -.->|"pthread_cond_signal"| RECV

    classDef queue fill:#FFB6C1,stroke:#333,stroke-width:2px
    classDef thread fill:#E6E6FA,stroke:#333,stroke-width:2px
```

**关键同步机制：**

| 机制 | 用途 | 实现 |
|---|---|---|
| `rte_ring` (lock-free) | 线程间数据传递 | DPDK 无锁环形队列，SP/SC/MP/MC 模式 |
| `pthread_mutex_t` | 保护 `nrecvfrom` 的等待条件 | 每个 `localhost` 独立 |
| `pthread_cond_t` | `nrecvfrom` 阻塞等待数据到达 | `pthread_cond_wait` / `pthread_cond_signal` |
| `rte_eal_remote_launch` | 在指定 lcore 上启动线程 | DPDK 多核框架，绑定 CPU 亲和性 |

**数据流总结**:

```
网卡 ──RX──> in_ring ──> Worker(协议处理) ──> out_ring ──TX──> 网卡
                              │
                              ├──> host->rcv_ring ──> App(nrecvfrom)
                              │
                    App(nsendto) ──> host->snd_ring ──┘
```

---

### 3. 开发视图 (Development View)

描述源代码的模块划分、文件组织与编译关系。

#### 3.1 文件结构 & 编译依赖

```mermaid
graph TB
    subgraph PROJECT["07_netarch/"]
        MAIN_C["main.c<br/>(792 行)"]:::src
        ARP_H["arp.h<br/>(67 行)"]:::hdr
        MK["Makefile"]:::build
    end

    subgraph BUILD["build/"]
        BIN["dpdk_netarch"]
        BIN_S["dpdk_netarch-shared"]
        BIN_A["dpdk_netarch-static"]
    end

    subgraph DPDK_LIBS["DPDK 库 (pkg-config: libdpdk)"]
        EAL["librte_eal"]
        ETHDEV["librte_ethdev"]
        MBUF_LIB["librte_mbuf"]
        MEMPOOL["librte_mempool"]
        RING_LIB["librte_ring"]
        TIMER_LIB["librte_timer"]
        NET_LIB["librte_net"]
    end

    MAIN_C -->|"#include"| ARP_H
    MAIN_C --> EAL
    MAIN_C --> ETHDEV
    MAIN_C --> MBUF_LIB
    MAIN_C --> MEMPOOL
    MAIN_C --> RING_LIB
    MAIN_C --> TIMER_LIB
    MAIN_C --> NET_LIB

    MK -->|"$(CC) 编译链接"| BIN
    MK -->|"$(CC) 编译链接"| BIN_S
    MK -->|"$(CC) 编译链接"| BIN_A

    classDef src fill:#FFEEDD,stroke:#333
    classDef hdr fill:#DDFFDD,stroke:#333
    classDef build fill:#EEEEFF,stroke:#333
```

#### 3.2 main.c 内部模块划分

```mermaid
graph TB
    subgraph MAIN_MOD["main.c 函数模块"]
        subgraph INIT["端口初始化"]
            NG_INIT["ng_init_port()"]
            PORT_CFG["port_conf_default"]
        end
        subgraph ARP_MOD["ARP 模块"]
            ENC_ARP["ng_encode_arp_pkt()"]
            SEND_ARP["ng_send_arp()"]
            ARP_TIMER["arp_request_timer_cb()"]
        end
        subgraph ICMP_MOD["ICMP 模块"]
            ENC_ICMP["ng_encode_icmp_pkt()"]
            SEND_ICMP["ng_send_icmp()"]
            CKSUM["ng_rcmp_checksum()"]
        end
        subgraph UDP_MOD["UDP 模块"]
            ENC_UDP["ng_encode_udp_apppkt()"]
            UDP_PKT["ng_udp_pkt()"]
            UDP_PROC["udp_process()"]
            UDP_OUT["udp_out()"]
        end
        subgraph PKT["包处理"]
            PKT_PROC["pkt_process()"]
        end
        subgraph SOCKET_API["Socket API"]
            NSOCK["nsocket()"]
            NBIND["nbind()"]
            NSEND["nsendto()"]
            NRECV["nrecvfrom()"]
            NCLOSE["nclose()"]
        end
        subgraph APP_MOD["UDP Server"]
            UDP_SRV["udp_server_entry()"]
        end
        subgraph RING_MOD["Ring 管理"]
            RING_INST["ring_instance()"]
        end
        MAIN_FN["main()"]
    end

    MAIN_FN --> NG_INIT
    MAIN_FN --> RING_INST
    MAIN_FN --> ARP_TIMER
    MAIN_FN --> UDP_SRV
    MAIN_FN --> PKT_PROC
    PKT_PROC --> ENC_ARP
    PKT_PROC --> ENC_ICMP
    PKT_PROC --> UDP_PROC
    PKT_PROC --> UDP_OUT
    UDP_PROC --> NSEND
    NRECV --> UDP_PROC

    style INIT fill:#FFE4B5,stroke:#333
    style ARP_MOD fill:#FFD700,stroke:#333
    style ICMP_MOD fill:#87CEEB,stroke:#333
    style UDP_MOD fill:#90EE90,stroke:#333
    style PKT fill:#DDA0DD,stroke:#333
    style SOCKET_API fill:#FFB6C1,stroke:#333
    style APP_MOD fill:#98FB98,stroke:#333
    style RING_MOD fill:#F0E68C,stroke:#333
```

#### 3.3 分层架构依赖

```mermaid
graph TD
    subgraph L1["应用层"]
        APP1["udp_server_entry"]
        APP2["自定义 UDP 应用"]
    end

    subgraph L2["Socket API 适配层"]
        S1["nsocket / nbind / nsendto / nrecvfrom / nclose"]
        S2["localhost 链表管理"]
        S3["offload 结构体封装"]
    end

    subgraph L3["协议处理层"]
        P1["UDP 包构造/解析"]
        P2["ICMP Echo Reply"]
        P3["ARP 请求/应答 + ARP 表"]
    end

    subgraph L4["数据面抽象层"]
        D1["rte_ring (in/out/snd/rcv)"]
        D2["rte_mbuf + rte_mempool"]
    end

    subgraph L5["DPDK 驱动层"]
        DRV1["rte_eth_rx_burst / rte_eth_tx_burst"]
        DRV2["rte_timer"]
    end

    subgraph L6["硬件"]
        NIC["NIC 网卡"]
    end

    APP1 --> S1
    APP2 --> S1
    S1 --> S2
    S1 --> S3
    S2 --> P1
    P1 --> P3
    P2 --> D1
    P3 --> D1
    P1 --> D1
    D1 --> D2
    D2 --> DRV1
    DRV1 --> NIC

    style L1 fill:#90EE90,stroke:#333
    style L2 fill:#87CEEB,stroke:#333
    style L3 fill:#FFD700,stroke:#333
    style L4 fill:#FFA500,stroke:#333
    style L5 fill:#D3D3D3,stroke:#333
    style L6 fill:#C0C0C0,stroke:#333
```

---

### 4. 物理视图 (Physical View)

描述系统在物理节点和 CPU 核心上的部署拓扑。

```mermaid
graph TB
    subgraph HOST["物理主机 (192.168.196.132)"]
        subgraph NUMA["NUMA Node 0"]
            subgraph LCORE0["lcore 0 (Master)"]
                RX0["RX 轮询"]
                TX0["TX 轮询"]
                TM0["Timer 管理"]
            end
            subgraph LCORE1["lcore 1 (Worker)"]
                PIN1["从 in_ring 取包"]
                POUT1["向 out_ring 发包"]
            end
            subgraph LCORE2["lcore 2 (App)"]
                RIN2["nrecvfrom 阻塞"]
                SOUT2["nsendto 发送"]
            end

            SHMEM["📦 DPDK 共享内存<br/>────────────────<br/>mbuf_pool (4095)<br/>in_ring / out_ring (1024)<br/>host->snd_ring / rcv_ring<br/>arp_table 链表<br/>localhost_list 链表"]:::shared

            NIC["🖧 物理网卡 NIC<br/>DPDK Port 0"]:::nic
        end
    end

    subgraph LAN["外部网络 LAN 192.168.196.0/24"]
        OTHER["其他主机 .1 ~ .254"]
        GW["网关 / 路由器"]
    end

    LCORE0 -->|"RX→in_ring<br/>out_ring→TX"| SHMEM
    LCORE1 -->|"in_ring→处理→out_ring<br/>UDP→rcv_ring<br/>snd_ring→UDP"| SHMEM
    LCORE2 -->|"rcv_ring→nrecvfrom<br/>nsendto→snd_ring"| SHMEM

    NIC <-->|"rte_eth_rx_burst<br/>rte_eth_tx_burst"| LCORE0
    NIC <-->|"以太网帧"| OTHER
    NIC <-->|"以太网帧"| GW

    classDef shared fill:#FFDDDD,stroke:#333,stroke-width:2px
    classDef nic fill:#DDDDDD,stroke:#333,stroke-width:2px
    classDef lcore fill:#E6E6FA,stroke:#333
```

**资源分配表：**

| 资源 | 配置 | 说明 |
|---|---|---|
| 网卡端口 | `gdpdkportid = 0` | 使用第一个可用 DPDK 端口 |
| RX / TX 队列数 | 各 1 个 | 单队列模型 |
| 队列描述符深度 | 1024 | 每队列 |
| mbuf 池大小 | 4095 | `NUM_MBUFS = 4096 - 1` |
| in/out ring 大小 | 1024 槽位 | `RING_SIZE = 1024` |
| snd/rcv ring 大小 | 1024 槽位/每 socket | 同上 |
| 定时器周期 | ~10ms (@2GHz) | `TIMER_RESOLUTION_CYCLES = 2×10⁹` |
| Worker lcore | 动态分配 (lcore 1) | `rte_get_next_lcore()` |
| App lcore | 动态分配 (lcore 2) | `rte_get_next_lcore()` |

---

### +1. 场景视图 (Scenarios)

通过 4 个关键用例串联上述四个视图。

---

#### 场景 1：ARP 请求 → 应答 → 表学习

```mermaid
sequenceDiagram
    participant REMOTE as 外部主机<br/>192.168.196.100
    participant NIC as NIC
    participant MAIN as 主线程<br/>lcore 0
    participant IN as in_ring
    participant WORKER as 包处理线程<br/>lcore 1
    participant OUT as out_ring
    participant ARPT as ARP 表<br/>arp_table

    Note over REMOTE,ARPT: === ARP Request：查询 192.168.196.132 的 MAC ===

    REMOTE->>NIC: ARP Request (广播)<br/>Who has 192.168.196.132?
    NIC->>MAIN: 以太网帧到达
    MAIN->>IN: rte_ring_sp_enqueue_burst()
    IN->>WORKER: rte_ring_mc_dequeue_burst()

    WORKER->>WORKER: 解析 EtherType == ARP
    WORKER->>WORKER: arp_opcode == REQUEST ?<br/>arp_tip == 192.168.196.132 ?

    alt arp_tip == 本机 IP
        WORKER->>WORKER: ng_send_arp(OP_REPLY)
        WORKER->>OUT: rte_ring_mp_enqueue_burst()
        OUT->>MAIN: rte_ring_sc_dequeue_burst()
        MAIN->>NIC: rte_eth_tx_burst()
        NIC->>REMOTE: ARP Reply<br/>192.168.196.132 is at xx:xx:xx:xx:xx:xx
    else arp_tip != 本机 IP
        WORKER->>WORKER: 丢弃
    end

    Note over REMOTE,ARPT: === ARP Reply：学习远端 MAC 地址 ===

    REMOTE->>NIC: ARP Reply<br/>192.168.196.100 is at aa:bb:cc:dd:ee:ff
    NIC->>MAIN: 以太网帧到达
    MAIN->>IN->>WORKER: (同上路径)

    WORKER->>WORKER: arp_opcode == REPLY
    WORKER->>ARPT: ng_get_dst_macaddr(arp_sip)

    alt MAC 不在表中
        WORKER->>ARPT: LL_ADD 创建新条目<br/>IP=192.168.196.100<br/>MAC=aa:bb:cc:dd:ee:ff<br/>type=DYNAMIC, count++
    else MAC 已存在
        WORKER->>WORKER: 跳过
    end
```

---

#### 场景 2：UDP Echo Server 收发包全流程

```mermaid
sequenceDiagram
    participant CLIENT as 远程客户端<br/>192.168.196.100:12345
    participant NIC as NIC
    participant MAIN as 主线程 lcore 0
    participant IN as in_ring
    participant OUT as out_ring
    participant WORKER as 包处理线程 lcore 1
    participant RCV as host->rcv_ring
    participant SND as host->snd_ring
    participant APP as UDP 服务线程 lcore 2

    Note over APP: === 1. 启动：创建 socket + bind + 阻塞等待 ===

    APP->>APP: nsocket(AF_INET, SOCK_DGRAM, 0)<br/>→ 创建 localhost (fd=3)
    APP->>APP: nbind(3, 192.168.196.132:8889)
    APP->>APP: nrecvfrom(3, buf, ...)<br/>→ pthread_cond_wait 阻塞

    Note over CLIENT,APP: === 2. 接收路径 ===

    CLIENT->>NIC: UDP: 192.168.196.100:12345<br/>→ 192.168.196.132:8889<br/>Payload: "Hello"
    NIC->>MAIN: rte_eth_rx_burst()
    MAIN->>IN: enqueue
    IN->>WORKER: dequeue

    WORKER->>WORKER: Eth → IP → UDP 逐层解析
    WORKER->>WORKER: get_hostinfo_fromip_port()<br/>查找 localhost
    WORKER->>WORKER: 构造 offload 结构体<br/>{src_ip, src_port, dst_ip, dst_port, data}

    WORKER->>RCV: rte_ring_sp_enqueue(offload)
    WORKER-->>APP: pthread_cond_signal() 唤醒

    Note over CLIENT,APP: === 3. 应用处理 ===

    APP->>RCV: rte_ring_sc_dequeue(&offload)
    APP->>APP: 打印 + 准备回显
    APP->>APP: nsendto(3, "Hello", 5, clientaddr)

    Note over CLIENT,APP: === 4. 发送路径 ===

    APP->>APP: 构造 offload → host->snd_ring
    APP->>SND: rte_ring_sp_enqueue(offload)

    WORKER->>SND: udp_out() → sc_dequeue(&offload)
    WORKER->>WORKER: ng_get_dst_macaddr(dst_ip)

    alt MAC 已知
        WORKER->>WORKER: ng_udp_pkt() 构造 UDP 包
        WORKER->>OUT: rte_ring_mp_enqueue_burst(udp_mbuf)
    else MAC 未知
        WORKER->>WORKER: ng_send_arp(REQUEST) → out_ring
        WORKER->>SND: offload 重新入队 (等 ARP 回复后重试)
    end

    OUT->>MAIN: sc_dequeue_burst()
    MAIN->>NIC: rte_eth_tx_burst()
    NIC->>CLIENT: UDP Echo Reply<br/>Payload: "Hello"
```

---

#### 场景 3：ICMP Ping 响应

```mermaid
sequenceDiagram
    participant PINGER as Ping 发起方<br/>192.168.196.50
    participant NIC as NIC
    participant MAIN as 主线程 lcore 0
    participant IN as in_ring
    participant WORKER as 包处理线程 lcore 1
    participant OUT as out_ring

    PINGER->>NIC: ICMP Echo Request<br/>Type=8, ID=0x1234, Seq=1<br/>Src=192.168.196.50 Dst=192.168.196.132

    NIC->>MAIN: rte_eth_rx_burst()
    MAIN->>IN: enqueue
    IN->>WORKER: dequeue

    WORKER->>WORKER: Eth → IP → ICMP 解析
    WORKER->>WORKER: next_proto_id == IPPROTO_ICMP ?
    WORKER->>WORKER: icmp_type == ECHO_REQUEST ?<br/>dst_ip == 192.168.196.132 ?

    alt 条件满足
        Note over WORKER: ng_send_icmp() 构造流程：<br/>1. Ethernet 头 (MAC 互换)<br/>2. IP 头 (src↔dst 互换, checksum)<br/>3. ICMP 头: type=0(Echo Reply)<br/>   ident/seq 原样, checksum

        WORKER->>OUT: rte_ring_mp_enqueue_burst(icmp_mbuf)
        OUT->>MAIN: dequeue
        MAIN->>NIC: rte_eth_tx_burst()
        NIC->>PINGER: ICMP Echo Reply<br/>Type=0, ID=0x1234, Seq=1
    else 不满足
        WORKER->>WORKER: 丢弃
    end
```

---

#### 场景 4：定时器驱动的 ARP 网段扫描

```mermaid
sequenceDiagram
    participant TIMER as rte_timer
    participant MAIN as 主线程 lcore 0
    participant CB as arp_request_timer_cb
    participant ARPT as ARP 表
    participant OUT as out_ring

    loop 每 ~10ms
        MAIN->>MAIN: cur_tsc - prev_tsc<br/>> TIMER_RESOLUTION_CYCLES ?
        MAIN->>MAIN: rte_timer_manage()

        alt 定时器到期
            MAIN->>CB: arp_request_timer_cb(tim, mbuf_pool)

            loop i = 1 to 254
                CB->>CB: target_ip = 192.168.196.i
                CB->>ARPT: ng_get_dst_macaddr(target_ip)

                alt MAC 未知
                    CB->>CB: ng_send_arp(REQUEST,<br/>dst_mac=FF:FF:FF:FF:FF:FF)
                else MAC 已知
                    CB->>CB: ng_send_arp(REQUEST,<br/>dst_mac=已知MAC)
                end

                CB->>OUT: rte_ring_mp_enqueue_burst(arp_mbuf)
            end

            MAIN->>OUT: dequeue → rte_eth_tx_burst()
        end
    end

    Note over CB,ARPT: ARP 扫描策略：<br/>• 每次触发扫描整个 C 类网段 (.1~.254)<br/>• 已知 MAC → 单播 ARP<br/>• 未知 MAC → 广播 ARP<br/>• ~10ms 周期确保 ARP 表持续刷新
```

---

## 包处理完整流程

```mermaid
flowchart TD
    A(["以太网帧到达网卡"]) --> B["主线程 rte_eth_rx_burst() 收包"]
    B --> C["入队 → ring->in<br/>(rte_ring_sp_enqueue_burst)"]
    C --> D["Worker 线程 pkt_process()<br/>从 ring->in 出队<br/>(rte_ring_mc_dequeue_burst)"]
    D --> E{"解析 Ethernet 头<br/>ether_type = ?"}

    E -->|"ARP (0x0806)"| F["解析 ARP 操作码"]
    F --> G{"arp_tip == 本机 IP ?"}
    G -->|"是"| H{"arp_opcode = ?"}
    H -->|"REQUEST"| I["ng_send_arp(OP_REPLY)"]
    I --> J["入队 → ring->out"]
    H -->|"REPLY"| K{"MAC 在表中 ?"}
    K -->|"否"| L["LL_ADD 创建<br/>arp_table_entry"]
    K -->|"是"| M["跳过"]
    G -->|"否"| M

    E -->|"IPv4 (0x0800)"| N["解析 IP 头<br/>next_proto_id = ?"]
    N -->|"UDP (17)"| O["udp_process()"]
    O --> P{"查找 localhost<br/>dst_ip + dst_port ?"}
    P -->|"找到"| Q["创建 offload"]
    Q --> R["入队 → host->rcv_ring"]
    R --> S["pthread_cond_signal()<br/>唤醒 nrecvfrom"]
    P -->|"未找到"| M

    N -->|"ICMP (1)"| T{"icmp_type == ECHO_REQUEST<br/>且 dst_ip == 本机 ?"}
    T -->|"是"| U["ng_send_icmp()<br/>构造 Echo Reply"]
    U --> J
    T -->|"否"| M

    E -->|"其他"| M

    J --> V["主线程从 ring->out 出队<br/>(rte_ring_sc_dequeue_burst)"]
    V --> W["rte_eth_tx_burst() 发送"]
    W --> X["rte_pktmbuf_free() 释放 mbuf"]
    X --> Z(["结束"])

    M --> Z

    style A fill:#90EE90,stroke:#333
    style Z fill:#FFB6C1,stroke:#333
    style I fill:#FFD700,stroke:#333
    style L fill:#87CEEB,stroke:#333
    style Q fill:#DDA0DD,stroke:#333
    style U fill:#98FB98,stroke:#333
```

---

## 核心数据结构

### localhost（Socket 端点）

```
┌──────────────────────────────────────────────────┐
│                  localhost                        │
├──────────────────────────────────────────────────┤
│  fd           │ socket 文件描述符 (从 3 开始)      │
│  status       │ 阻塞/非阻塞标志                    │
│  local_ip     │ 绑定的本机 IP                      │
│  local_port   │ 绑定的本机端口 (网络字节序)         │
│  local_mac    │ 绑定的本机 MAC                     │
│  protocol     │ IPPROTO_UDP 或 IPPROTO_TCP         │
│  snd_ring     │ → 发送环形队列 (rte_ring*)         │
│  rcv_ring     │ → 接收环形队列 (rte_ring*)         │
│  next / prev  │ 双向链表指针                       │
│  cond         │ 条件变量 (nrecvfrom 阻塞等待)       │
│  mutex        │ 互斥锁 (保护 cond)                 │
└──────────────────────────────────────────────────┘
```

**生命周期**：`nsocket()` 创建 → `nbind()` 绑定 → `nsendto()/nrecvfrom()` 使用 → `nclose()` 销毁

### offload（包负载描述符）

```
┌──────────────────────────────────────┐
│              offload                  │
├──────────────────────────────────────┤
│  src_ip    │ 源 IP 地址               │
│  dst_ip    │ 目的 IP 地址             │
│  src_port  │ 源端口 (网络字节序)       │
│  dst_port  │ 目的端口 (网络字节序)     │
│  protocol  │ 协议类型                  │
│  data      │ → 负载数据 (动态分配)     │
│  data_len  │ 负载数据长度              │
└──────────────────────────────────────┘
```

**作用**：在线程间传递应用层数据，避免直接传递 `rte_mbuf`（解耦协议栈与 Socket API 层）。

### inout_ring（全局 I/O 环形缓冲区）

```
┌─────────────────────────────┐
│         inout_ring          │
├─────────────────────────────┤
│  in  → rte_ring* │ 入方向   │
│  out → rte_ring* │ 出方向   │
└─────────────────────────────┘
   单例模式 (ring_instance)
```

**队列使用模式**：

| 操作 | 模式 | 含义 |
|---|---|---|
| `rte_ring_sp_enqueue_burst` | SP (Single Producer) | 主线程独占写入 in_ring |
| `rte_ring_mc_dequeue_burst` | MC (Multi Consumer) | Worker 线程消费 |
| `rte_ring_mp_enqueue_burst` | MP (Multi Producer) | Worker/Timer 写入 out_ring |
| `rte_ring_sc_dequeue_burst` | SC (Single Consumer) | 主线程独占消费 out_ring |

### arp_table_entry（ARP 表条目）

```
┌──────────────────────────────────────┐
│          arp_table_entry              │
├──────────────────────────────────────┤
│  ip_addr  │ IPv4 地址                 │
│  mac_addr │ MAC 地址 (6 字节)          │
│  type     │ 0: 动态   1: 静态          │
│  next     │ → 链表下一节点             │
│  prev     │ → 链表上一节点             │
└──────────────────────────────────────┘
```

双向链表，由 `arp_table->entries` 指向表头，`LL_ADD` / `LL_REMOVE` 宏操作。

---

## 设计要点总结

| 维度 | 设计决策 | 优势 | 局限 |
|---|---|---|---|
| **数据面** | 全部基于 DPDK rte_mbuf + rte_mempool | 零拷贝，Hugepage 内存 | 强依赖 DPDK 环境 |
| **线程模型** | 主线程(IO) + Worker(协议) + App(业务) 三级流水线 | 收发与处理解耦 | 单 Worker，无负载均衡 |
| **IPC 通信** | rte_ring (无锁队列) + pthread 条件变量 | 低延迟，无需内核参与 | 仅限同一进程内线程 |
| **ARP 解析** | 定时器全量扫描 C 类网段 (1~254) | 简单可靠 | O(254) 每次触发，浪费带宽 |
| **Socket API** | 仿 POSIX 接口，内部映射到 localhost + rte_ring | 降低应用移植成本 | 仅支持 UDP (TCP 未实现) |
| **包构造** | 手动逐层填充协议头 + 计算校验和 | 完全可控，无额外依赖 | 代码量大，容易出错 |
| **内存管理** | rte_malloc 大页内存 + 手动 free | 适合 DPDK 场景 | 需关注内存泄漏 |

---

## 编译与运行

```bash
# 编译（需要在 DPDK 环境中）
cd examples/07_netarch
make

# 运行
sudo ./build/dpdk_netarch -l 0,1,2 -n 4 -- -p 0x1
#   -l 0,1,2 : 使用 lcore 0,1,2
#   -n 4     : 4 个内存通道
#   -- -p 0x1: 使用端口 0
```

---

> 文档生成日期: 2026-08-03
