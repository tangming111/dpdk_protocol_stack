# Linux ip 命令与单机双端点打流实验

更新日期：2026-10-21。目标是在同一台 Linux 上建立两个独立网络端点，分别配置 IP，进行 ping、TCP 和 UDP 双向测试。本文使用内核网络，不依赖 DPDK、KNI 或物理网卡；用户态协议栈测试另见 [BUILD.md](BUILD.md)。

## 1. 先理解实验结构

```text
一台 Linux，共用同一个内核

默认网络命名空间：运行 SSH 和操作命令，不给实验配置 IP

  网络命名空间 iplab_a                    网络命名空间 iplab_b
  ┌────────────────────────┐            ┌────────────────────────┐
  │ 进程：ping / iperf3     │            │ 进程：ping / iperf3     │
  │ 内核 TCP / UDP / ICMP   │            │ 内核 TCP / UDP / ICMP   │
  │ 接口：ipa0              │◀── veth ──▶│ 接口：ipb0              │
  │ IP：192.0.2.10/24       │            │ IP：192.0.2.20/24       │
  └────────────────────────┘            └────────────────────────┘
```

| 名称 | 含义 |
| --- | --- |
| network namespace / netns | 网络命名空间，隔离接口、地址、路由和 socket 端口等网络状态 |
| veth pair | 一对虚拟以太网接口，一端发出的帧到达另一端，像一根虚拟网线 |
| interface / dev / link | 网络接口，既可以是物理网卡，也可以是虚拟接口 |
| lo / loopback | 回环接口，每个命名空间都有自己的回环网络 |
| 192.0.2.10/24 | IPv4 地址和前缀长度；/24 对应掩码 255.255.255.0 |
| 5201 | iperf3 本实验使用的端口，是进程的服务入口，不是网卡编号 |

两个端点在同一个 192.0.2.0/24 子网内，直接通过 ARP 找到对方，无需网关、NAT、网桥或开启 IP 转发。192.0.2.0/24 是文档示例地址段，本实验只在隔离的虚拟网络中使用。

命名空间不是一台新虚拟机，也没有独立内核。两个终端本身不会隔离网络；进程是否进入指定命名空间才决定它使用哪套网络状态。

## 2. Shell 和权限基础

所有命令在 Linux bash 中执行，不是在 Windows PowerShell 中执行。打开两个 SSH 终端，分别称为“终端 A”和“终端 B”。

两个终端都先执行：

```bash
sudo -s
id -u
```

id -u 应输出 0。sudo -s 启动 root Shell；输入 exit 可退出。创建命名空间和 veth、修改接口配置需要管理员权限。

| 写法 | 解释 |
| --- | --- |
| pwd、cd、ls | 查看当前目录、切换目录、查看文件 |
| NAME=value | 设置当前 Shell 的变量，等号两侧不能有空格 |
| export NAME=value | 让之后启动的子进程也继承该变量 |
| $NAME、${NAME} | 取变量值；未设置时通常展开为空 |
| $(命令) | 执行命令，取得其标准输出 |
| 行末的反斜杠 | 一条命令分多行输入；反斜杠后不要再加空格 |
| > file、2>&1 | 标准输出写文件、将标准错误合并到标准输出 |
| 命令末尾的 & | 后台运行；$! 可取得刚启动的后台进程 PID |
| Ctrl+C | 通常向前台程序发送 SIGINT，要求它停止 |

环境变量只存在于对应 Shell 及其子进程中，新终端需要重新设置。命名空间和接口则会持续存在，关闭终端不会自动删除它们。

本实验不要加载用户态协议栈。在两个终端执行：

```bash
unset LD_PRELOAD
command -v ip ping iperf3
ip -V
iperf3 --version
```

这里假定这些工具已安装。若 command -v 找不到，Ubuntu 可按需安装 iproute2、iputils-ping、iperf3；安装会修改系统软件包，请在自己的测试环境中决定是否执行。

本项目先前解包的 iperf3 也可直接使用。在项目 examples/13_redis 目录、两个终端分别设置：

```bash
export PATH="$PWD/build/iperf-tools/root/usr/bin:$PATH"
export LD_LIBRARY_PATH="$PWD/build/iperf-tools/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
command -v iperf3
```

## 3. ip 命令的通用结构

```text
ip [选项] 操作对象 动作 参数
```

| 对象 | 管理内容 | 查询例子 |
| --- | --- | --- |
| link | 接口创建、删除、启用、停用、MTU 等 | ip link show |
| addr | 接口 IP 地址 | ip -4 addr show |
| route | 路由表及路由选择 | ip -4 route show |
| neigh | 邻居表，IPv4 中主要是 ARP 记录 | ip neigh show |
| netns | 网络命名空间 | ip netns list |

常见选项：-4 只看 IPv4；-6 只看 IPv6；-o 尽量每条记录显示一行；-s 查看统计；-d 查看细节。本文使用完整对象名称，便于理解。

常见动作：show/list 是查看，add 是增加，set 是修改，del 是删除。up/down 表示接口的管理状态。dev 表示后面指定接口名称。

## 4. 创建两个端点并分别配置 IP

这一节只在终端 A 执行一次。先检查已有对象：

```bash
ip netns list
ip link show
```

本实验会创建 iplab_a、iplab_b、ipa0、ipb0。如果这些名字已经存在，先确认是否是自己先前建立的实验对象；不要覆盖或删除来源不明的对象。下面各条应成功后再执行下一条。

### 4.1 创建两个网络命名空间

```bash
ip netns add iplab_a
ip netns add iplab_b
ip netns list
```

此时创建了两套隔离的网络环境，各自通常只有尚未启用的 lo，没有连接两端的实验网卡。

### 4.2 创建一对 veth

```bash
ip link add ipa0 type veth peer name ipb0
ip link show ipa0
ip link show ipb0
```

逐项解释：link add 是创建接口；ipa0 是第一端名字；type veth 指定接口类型；peer name ipb0 指定另一端名字。刚创建时两端都在默认命名空间中。

### 4.3 分别把两个接口移入各自的命名空间

```bash
ip link set ipa0 netns iplab_a
ip link set ipb0 netns iplab_b
```

这移动的是网络接口，不是移动当前终端。移动后，默认环境中查询 ip link show ipa0 会找不到它，这是正常现象。要在接口所属环境中查看：

```bash
ip netns exec iplab_a ip link show ipa0
ip netns exec iplab_b ip link show ipb0
```

### 4.4 启用回环接口并给两端分配地址

```bash
ip netns exec iplab_a ip link set lo up
ip netns exec iplab_b ip link set lo up

ip netns exec iplab_a ip addr add 192.0.2.10/24 dev ipa0
ip netns exec iplab_b ip addr add 192.0.2.20/24 dev ipb0

ip netns exec iplab_a ip link set ipa0 up
ip netns exec iplab_b ip link set ipb0 up
```

以其中一条为例：

```text
ip netns exec iplab_a | ip addr add 192.0.2.10/24 dev ipa0
在 iplab_a 内运行      | 将地址添加到 ipa0 接口
```

设置 up 不会自动分配 IP；分配 IP 也不等于启用接口。这里分别执行两种操作。

### 4.5 检查配置

```bash
ip netns exec iplab_a ip -4 addr show dev ipa0
ip netns exec iplab_b ip -4 addr show dev ipb0
ip netns exec iplab_a ip -4 route show
ip netns exec iplab_b ip -4 route show
ip netns exec iplab_a ip route get 192.0.2.20
```

应分别看到 192.0.2.10/24 和 192.0.2.20/24。添加地址且接口启用后，通常会自动生成类似下面的直连路由：

```text
192.0.2.0/24 dev ipa0 ... src 192.0.2.10
```

这表示：目标属于 192.0.2.0/24 时，从 ipa0 直接发送，并选择本端地址作为源地址。ip route get 是查询实际路由选择，不发送 ping 数据包。

## 5. ip netns exec 到底做了什么

```bash
ip netns exec iplab_a ping -c 3 192.0.2.20
```

前半段选择网络环境，后半段是要运行的普通命令。命令结束后，你的当前 Shell 仍在原来的网络命名空间。

若希望一个 Shell 内的所有命令都使用端点 A 的网络，可以执行：

```bash
ip netns exec iplab_a bash
ip addr show
exit
```

exit 返回外层 Shell。提示符不一定显示命名空间，不能只看 root@ubuntu 判断网络归属。可以在这样的子 Shell 中执行 ip netns identify，查询对应的命名空间名字。

## 6. 先测试两端互相 ping

终端 A 或 B 均可执行：

```bash
ip netns exec iplab_a ping -c 3 192.0.2.20
ip netns exec iplab_b ping -c 3 192.0.2.10
```

-c 3 表示发送三次后结束。预期有回复，摘要显示三包发送、三包接收。首次通信可能先进行 ARP 查询，因此第一包耗时可能与后续不同。

查看邻居表和接口计数：

```bash
ip netns exec iplab_a ip neigh show dev ipa0
ip netns exec iplab_b ip neigh show dev ipb0
ip netns exec iplab_a ip -s link show dev ipa0
ip netns exec iplab_b ip -s link show dev ipb0
```

邻居表把对端 IP 映射到 MAC 地址。REACHABLE 表示邻居可达；STALE 表示缓存需要在后续通信中重新确认，不等于已经断网；FAILED 通常表示邻居解析失败。统计中的 RX/TX 分别是接收/发送。

## 7. TCP 双向打流

保持一个终端运行服务端，另一个终端运行客户端。“打流”就是持续发送测试数据，测量接收量、速率等。

### 7.1 终端 B：在端点 B 启动服务端

```bash
ip netns exec iplab_b iperf3 -4 -s -B 192.0.2.20 -p 5201
```

该命令保持前台运行，不要看到没有 Shell 提示符就以为卡住。它正在等待客户端。

### 7.2 终端 A：从 A 向 B 发送 TCP 数据

```bash
ip netns exec iplab_a iperf3 -4 -c 192.0.2.20 -B 192.0.2.10 -p 5201 -t 10
```

TCP 是默认模式。数据方向为 A → B，TCP ACK 等控制报文也会反向传输。

### 7.3 终端 A：让 B 向 A 发送 TCP 数据

```bash
ip netns exec iplab_a iperf3 -4 -c 192.0.2.20 -B 192.0.2.10 -p 5201 -t 10 -R
```

-R 改变数据发送方向，数据为 B → A；A 依然是发起连接的客户端，B 依然是监听的服务端。它是反向测试，不是两个方向同时发送。

### 7.4 终端 A：四条并行 TCP 流

```bash
ip netns exec iplab_a iperf3 -4 -c 192.0.2.20 -B 192.0.2.10 -p 5201 -t 10 -P 4
```

-P 4 表示四条并行数据连接，不是创建四个命名空间。为了兼容本项目的旧 iperf3，本文不依赖新版本才支持的双向选项。

## 8. UDP 双向打流

继续使用终端 B 中同一个 iperf3 服务端，不需要换成另一个 UDP 专用服务程序。在终端 A 分别执行：

```bash
# A -> B，目标发送速率 10 Mbit/s
ip netns exec iplab_a iperf3 -4 -c 192.0.2.20 -B 192.0.2.10 -p 5201 -u -l 1200 -b 10M -t 10

# B -> A
ip netns exec iplab_a iperf3 -4 -c 192.0.2.20 -B 192.0.2.10 -p 5201 -u -l 1200 -b 10M -t 10 -R
```

-u 使用 UDP 数据传输，-l 1200 设置 UDP 负载长度，-b 10M 设置目标发送速率。这里的 M 表示速率数量级，单位为 bit/s，不是每秒 10 MB。目标发送速率与实测接收速率可能不同。

iperf3 的协调控制连接仍使用 TCP，即使带了 -u。UDP 结果会报告抖动、丢包数和丢包比例；UDP 本身不提供可靠交付和重传。1200 字节是本实验为方便对照用户态协议栈选用的值，不表示 Linux 内核 UDP 的负载上限是 1472。

## 9. 交换客户端、服务端角色

前面的 -R 已验证两个数据方向；这一节进一步让端点 B 主动连接端点 A。

先在终端 B 按 Ctrl+C，停止原来端点 B 的 iperf3 服务端。

终端 A 启动端点 A 服务端：

```bash
ip netns exec iplab_a iperf3 -4 -s -B 192.0.2.10 -p 5201
```

终端 B 执行：

```bash
ip netns exec iplab_b iperf3 -4 -c 192.0.2.10 -B 192.0.2.20 -p 5201 -t 10
ip netns exec iplab_b iperf3 -4 -c 192.0.2.10 -B 192.0.2.20 -p 5201 -u -l 1200 -b 10M -t 10
```

这次 B 是连接发起方，A 是监听方，普通模式下数据方向为 B → A。

## 10. iperf3 输出和参数速查

| 参数/输出 | 解释 |
| --- | --- |
| -s、-c IP | 服务端、客户端 |
| -4 | 使用 IPv4 |
| -B IP | 绑定本进程的本地地址，大小写与 -b 不同 |
| -p 5201 | 服务端端口 |
| -t 10 | 测试时长 10 秒 |
| -R | 服务端向客户端发送测试数据 |
| -P 4 | 四条并行流 |
| -u、-l 1200、-b 10M | UDP、负载长度、目标速率 |
| -1 | 服务端完成一轮测试后退出；本文连续测试不使用 |
| Transfer | 测试传输的数据量 |
| Bandwidth / Bitrate | 传输速率，不同版本标题可能不同 |
| sender / receiver | 发送方统计和接收方统计 |
| Retr / Cwnd | TCP 重传统计和拥塞窗口，是否显示取决于模式和版本 |
| Jitter / Lost | UDP 抖动和丢包统计 |

一次功能测试应正常结束，并看到双方传输统计。两端共享 CPU 和内存，虚拟网络也受调度、队列和卸载影响；这些结果不能当作物理网卡的吞吐能力。

## 11. 常见网络命令练习

下面针对本实验对象操作，不要对 SSH 管理网卡照抄修改命令。

| 目的 | 命令例子 |
| --- | --- |
| 查看 A 的 IPv4 地址 | ip netns exec iplab_a ip -4 addr show |
| 查看 A 的接口细节 | ip netns exec iplab_a ip -d link show dev ipa0 |
| 查看 A 的收发统计 | ip netns exec iplab_a ip -s link show dev ipa0 |
| 查看 A 到 B 的选路 | ip netns exec iplab_a ip route get 192.0.2.20 |
| 查看 A 的 ARP 邻居 | ip netns exec iplab_a ip neigh show |
| 关闭 A 接口 | ip netns exec iplab_a ip link set ipa0 down |
| 重新启用 A 接口 | ip netns exec iplab_a ip link set ipa0 up |
| 查看 B 的 TCP 监听 | ip netns exec iplab_b ss -lnt |
| 查看命名空间内进程 | ip netns pids iplab_b |

例如要体验接口 down/up，可在没有运行打流时执行：

```bash
ip netns exec iplab_a ip link set ipa0 down
ip netns exec iplab_a ip link set ipa0 up
```

如果要更换 A 的地址，先停止相关客户端/服务端，再删除旧地址、添加新地址：

```bash
ip netns exec iplab_a ip addr del 192.0.2.10/24 dev ipa0
ip netns exec iplab_a ip addr add 192.0.2.11/24 dev ipa0
```

之后使用 -B 时也要改成 192.0.2.11。练习结束若要恢复，则删除 .11 并重新添加 .10；不要重复添加已经存在的相同地址。

## 12. 抓包观察 ARP、ICMP、TCP 和 UDP

有 tcpdump 时，可在额外的 root 终端执行：

```bash
ip netns exec iplab_a tcpdump -ni ipa0 'arp or icmp or tcp port 5201 or udp port 5201'
```

-n 不把地址/端口解析成名称；-i 指定接口。抓包保持前台运行，Ctrl+C 停止。

如果 ARP 缓存已经建立，再次 ping 不一定出现 ARP 请求。正常的 TCP 测试可以观察到握手、数据、ACK 和关闭；UDP 测试则能看到 TCP 控制连接与 UDP 数据包。

## 13. 排错顺序

| 现象 | 含义或检查方向 |
| --- | --- |
| Operation not permitted | 检查 id -u 是否为 0；受限容器即使 root 也可能缺少网络管理权限 |
| File exists | 对象或地址已存在，先查状态，不要反复执行 add |
| 默认环境看不到 ipa0/ipb0 | 接口已移入命名空间，应在对应 netns 中查看 |
| Cannot open network namespace | 名字拼错、未创建，或创建步骤失败 |
| Network is unreachable | 查看源端接口是否 UP、IP/前缀是否正确、路由是否存在 |
| 邻居状态 FAILED | 检查 veth 两端是否连接、是否启用、对端 IP 是否配置正确 |
| Connection refused | 目标可达但没有对应监听，或存在明确拒绝规则；检查服务端输出及 ss |
| iperf3 连接超时 | 检查接口、路由、ARP、目标地址和命名空间内的防火墙规则 |
| Address already in use | 同一命名空间中端口已有进程绑定；先查询，不要盲目启动多个服务端 |
| Cannot assign requested address | -B 指定的地址不属于该进程所在网络环境 |
| p: command not found | 把 ip 少输入了字母 i |

按接口 → 地址 → 路由 → 邻居 → 服务监听 → 应用参数的顺序检查。不要为这个直连实验清空整机路由或防火墙，也不需要修改默认网关。

## 14. 清理实验

先在运行服务端和 tcpdump 的终端按 Ctrl+C，等待退出。可以检查是否还有属于本实验的进程：

```bash
ip netns pids iplab_a
ip netns pids iplab_b
```

确认只针对本实验对象后，删除 veth 的一端：

```bash
ip netns exec iplab_a ip link del ipa0
ip netns del iplab_a
ip netns del iplab_b
ip netns list
```

删除 veth 一端会同时删除另一端。不要只删除命名空间名字而让里面的服务继续运行；有进程引用时，网络命名空间可能仍然存活，但名字已经不可用于后续 exec。

重复实验时从创建步骤开始。如果只退出终端，没有清理对象，再次 add 会报 File exists。本教程不使用也不删除项目先前的 dnmanual 命名空间。

## 15. 与项目 DPDK 测试的区别

| 项目 | 本文 Linux 网络实验 | BUILD.md 中用户态协议栈实验 |
| --- | --- | --- |
| 两端位置 | 两个独立 netns | 默认环境中的 DPDK 进程与一个内核 netns 对端 |
| 两端 IP 如何设置 | 两端都用 ip addr add | 内核对端用 ip addr；用户态端由 DPDKNET_IP 设置 |
| TCP/UDP 由谁处理 | 两端都是 Linux 内核 | 一端内核，另一端由 libdpdknet.so 处理 |
| 是否设置 LD_PRELOAD | 不设置 | 用户态应用加载 libdpdknet.so |
| 是否配置 DPDK EAL | 不需要 | 需要 |
| ICMP ping | 本实验可正常测试 | 当前用户态协议栈没有提供 ICMP echo 服务 |
| 是否关闭卸载 | 本实验不要求 | AF_PACKET 接入自有协议栈时按 BUILD.md 关闭 |

先把本文两端都用内核的实验理解并跑通，再阅读 [BUILD.md](BUILD.md) 的 DPDK 实验，就能明确区分 Linux 网络配置和用户态协议处理各自负责的部分。

## 16. 本教程验证情况

2026-10-21 已在项目 Linux 主机上验证地址配置、直连路由、双向 ping、TCP 正向/反向/并行、UDP 正反向、交换客户端与服务端，以及接口 down/up 和更换地址。验证时每轮打流为2秒，文档中的手动命令为10秒；为避免与已有对象冲突，验证采用带 PID 的唯一命名空间和接口名，IP 与拓扑保持一致。

两端均未加载 libdpdknet.so。验证辅助脚本返回0，临时网络已清理，没有改动已有 dnmanual。详细记录见 [VALIDATION.md](VALIDATION.md)，原始日志为项目 build/ip-lab-validation.log 及 build/ip-lab-*.log。这是内核虚拟网络功能验证，不是用户态协议栈或物理网卡性能验收。
