# 构建与运行 libdpdknet.so

文档索引见 [README.md](README.md)。源码结构见 [CODE_GUIDE.md](CODE_GUIDE.md)，协议语义与限制见 [PROTOCOLS.md](PROTOCOLS.md)，测试结果见 [VALIDATION.md](VALIDATION.md)。

如果不熟悉 ip/netns/veth，先看 [Linux ip 命令与双端点打流教程](LINUX_IP_LAB.md)。它演示两端都用内核的基础网络实验，与本文加载 so 的实验分开。

本文所有命令都在 Linux 执行；除显式 cd 外，工作目录是 examples/13_redis，不能在 docs/ 中直接执行 make。

## 1. 构建 DPDK 共享库

已有可用的 DPDK 动态库时不必重编。旧版 make 构建需要：

```bash
cd /path/to/dpdk-stable-19.08.2
export RTE_SDK=$PWD
export RTE_TARGET=x86_64-native-linux-gcc
# 确认目标构建配置为共享库；若修改此项，需要重新构建 DPDK。
grep CONFIG_RTE_BUILD_SHARED_LIB "$RTE_TARGET/.config"
# 期望 CONFIG_RTE_BUILD_SHARED_LIB=y
ls "$RTE_TARGET/include/rte_config.h" "$RTE_TARGET/lib/libdpdk.so"
```

若现有目标是静态构建，先将目标 `.config` 的 CONFIG_RTE_BUILD_SHARED_LIB 改为 y，再在该目标目录执行 clean 和 make。不要把未用 PIC 编译的静态 DPDK 直接链接到本 so。

## 2. 编译协议栈

```bash
cd "$RTE_SDK/examples/13_redis"
make RTE_TARGET=x86_64-native-linux-gcc -j2
export LD_LIBRARY_PATH="$RTE_SDK/$RTE_TARGET/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ldd build/libdpdknet.so
```

产物是 `examples/13_redis/build/libdpdknet.so`。Makefile 优先使用 libdpdk.pc，否则使用 RTE_SDK/RTE_TARGET。显式设置 RTE_TARGET，避免旧 shell 遗留的 x86_64-native-linuxapp-gcc 指向不存在的目录。

默认不启用 KNI，不需要 /dev/kni 或内核 KNI 模块。原 KNI 教学代码仍保留；有明确需求时可通过编译宏启用，但本次应用测试没有依赖 KNI。

## 3. 无需 DPDK 网卡的单元与 hook 测试

```bash
make test
```

执行六组测试：拥塞控制；真实 TCP 核心的模拟报文；真实 UDP 核心的数据报测试；真实统一 socket API（注册表、操作表、消息、fd 复用、accept 和 fork）；自己的 epoll 核心；真实 Linux fd + 测试后端的 LD_PRELOAD 接口。最后一组使用 `build/libhook_test.so`，它不是应用运行时的协议栈库。

### 3.1 epoll 核心内存检查

```bash
gcc -std=gnu11 -O1 -g -D_GNU_SOURCE -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/test_epoll_core.c epoll_core.c tests/hook_backend.c \
  -o build/test_epoll_sanitize
ASAN_OPTIONS=detect_leaks=1 ./build/test_epoll_sanitize
```

## 4. 隔离式真实 DPDK 应用联调

需要 root、iproute2、ethtool、Python 3、Redis、iperf3，以及 DPDK AF_PACKET PMD。脚本建立临时 veth 和 network namespace，使用 192.0.2.1/192.0.2.2；结束后删除自己创建的网络设备和命名空间，不绑定物理网卡。

```bash
sudo env RTE_SDK="$RTE_SDK" RTE_TARGET="$RTE_TARGET" \
    PATH="$PATH" LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
    bash tests/smoke_linux.sh
```

脚本验证 Redis PING/SET/GET、256KiB 二进制数据、流水线、4 个并发客户端、BGSAVE，以及真实用户态 UDP API、iperf3 TCP/UDP 服务端/客户端/反向/4 连接并行传输，并验证同一服务端进程连续运行这些模式后重新回到 TCP 正向。未安装 iperf3 时会明确输出 SKIP；此时不能把整套 iperf3 验收记为通过。

本次主机临时解包的 iperf3 可以这样复测：

```bash
sudo env RTE_SDK="$RTE_SDK" RTE_TARGET=x86_64-native-linux-gcc \
    PATH="$PWD/build/iperf-tools/root/usr/bin:$PATH" \
    LD_LIBRARY_PATH="$PWD/build/iperf-tools/root/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH" \
    bash tests/smoke_linux.sh
```

AF_PACKET 测试必须关闭临时 veth 的分段/校验和卸载；脚本已经处理。不要把这个测试中的软件 PMD 吞吐当作物理网卡性能。

### 4.1 在当前 Linux 主机上分步骤手动测试

下面使用两个 SSH 终端登录同一台 Linux。终端 A 运行加载 so 的 Redis/iperf3；终端 B 运行网络命名空间中的普通客户端。使用已验证的 AF_PACKET PMD，不需要绑定物理网卡或启用 KNI。所有命令在 Linux 的 bash 执行，不是在 Windows PowerShell 中执行。

#### 第一步：两个终端都准备环境

两个终端分别执行；路径按当前共享目录所在 Linux 主机填写：

```bash
sudo -s
cd /home/king/share/dpdk/dpdk-stable-19.08.2/examples/13_redis
export STACK_ROOT=$PWD
export RTE_SDK=$(cd ../.. && pwd)
export RTE_TARGET=x86_64-native-linux-gcc
export STACK_SO="$STACK_ROOT/build/libdpdknet.so"
export PATH="$STACK_ROOT/build/iperf-tools/root/usr/bin:$PATH"
export LD_LIBRARY_PATH="$RTE_SDK/$RTE_TARGET/lib:$STACK_ROOT/build/iperf-tools/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
command -v redis-server redis-cli iperf3 ip ethtool python3
```

build/iperf-tools 是之前测试时解包的 iperf3；如果它不存在，可以使用已安装到系统中的 iperf3。应先确认上面的 command -v 能找到全部工具。

只在终端 A 编译：

```bash
make RTE_TARGET="$RTE_TARGET" -j2
ldd build/libdpdknet.so
```

预期没有 not found。不要全局 export LD_PRELOAD；仅在运行用户态应用的单条命令前设置，终端 B 的对端保持内核网络。

#### 第二步：终端 A 创建测试网络

下面的 dnmanual、dnmanual0、dnmanual1 必须是空闲名称；如果提示已经存在，先确认其归属，不能继续沿用不明对象。

```bash
ip netns add dnmanual
ip link add dnmanual0 type veth peer name dnmanual1
ip link set dnmanual1 netns dnmanual
ip link set dnmanual0 up
ip netns exec dnmanual ip link set lo up
ip netns exec dnmanual ip addr add 192.0.2.1/24 dev dnmanual1
ip netns exec dnmanual ip link set dnmanual1 up
ethtool -K dnmanual0 tx off rx off tso off gso off gro off
ip netns exec dnmanual ethtool -K dnmanual1 tx off rx off tso off gso off gro off

export DPDKNET_IP=192.0.2.2
CPU=$(awk '/Cpus_allowed_list/{split($2,a,/[-,]/);print a[1]}' /proc/self/status)
export DPDKNET_EAL_ARGS="-l $CPU -n 1 --no-huge --no-pci --file-prefix=dnmanual --vdev=net_af_packet0,iface=dnmanual0"
```

不要用 ip addr 把 192.0.2.2 配置给内核接口，它由用户态协议栈使用。网络结构是：内核对端 192.0.2.1 → veth → DPDK AF_PACKET → 用户态协议栈 192.0.2.2。关闭卸载后不应有相关失败项；如果 ethtool 报错，应先排查。

#### 第三步：Redis

终端 A 启动用户态 Redis，保持前台运行：

```bash
LD_PRELOAD="$STACK_SO" redis-server \
  --bind "$DPDKNET_IP" --port 16379 --daemonize no \
  --save '' --appendonly no \
  --dir "$STACK_ROOT/build" --dbfilename manual-test.rdb
```

终端 B 逐条执行：

```bash
ip netns exec dnmanual redis-cli -h 192.0.2.2 -p 16379 PING
ip netns exec dnmanual redis-cli -h 192.0.2.2 -p 16379 SET demo hello
ip netns exec dnmanual redis-cli -h 192.0.2.2 -p 16379 GET demo
```

预期依次为 PONG、OK、hello。还可以执行已有扩展测试，覆盖 256KiB 二进制数据、100 条流水线、4 个并发客户端和 BGSAVE：

```bash
ip netns exec dnmanual python3 tests/redis_stream.py 192.0.2.2 16379
```

测试完成后，在终端 A 按 Ctrl+C，等待 Redis 退出，再开始 iperf3。测试端口为 16379，不使用原有的 127.0.0.1:6379 服务。

#### 第四步：iperf3 TCP 与 UDP

终端 A 启动用户态服务端，同一个服务端可以依次接受下面所有测试。2026-10-20 已修复关闭后重绑定时的端口误占用，并增加同一服务端进程连续测试。更新共享库后，必须停止旧 iperf3 进程并重新启动，运行中的进程不会自动切换到新库：

```bash
LD_PRELOAD="$STACK_SO" iperf3 -4 -s -B "$DPDKNET_IP" -p 15201
```

终端 B 依次执行 TCP 正向、反向和 4 连接并行：

```bash
ip netns exec dnmanual iperf3 -4 -c 192.0.2.2 -p 15201 -t 10
ip netns exec dnmanual iperf3 -4 -c 192.0.2.2 -p 15201 -t 10 -R
ip netns exec dnmanual iperf3 -4 -c 192.0.2.2 -p 15201 -t 10 -P 4
```

然后执行 UDP 正向、反向和 4 流并行：

```bash
ip netns exec dnmanual iperf3 -4 -c 192.0.2.2 -p 15201 -u -l 1200 -b 1M -t 10
ip netns exec dnmanual iperf3 -4 -c 192.0.2.2 -p 15201 -u -l 1200 -b 1M -t 10 -R
ip netns exec dnmanual iperf3 -4 -c 192.0.2.2 -p 15201 -u -l 1200 -b 1M -t 10 -P 4
```

TCP 应成功完成连接并报告传输字节和速率；UDP 应成功完成并报告速率、抖动和丢包统计。UDP 不保证零丢包。这里主要验证功能，不能把 veth/AF_PACKET 的速率视为物理网卡性能。UDP 必须显式指定不超过 1472 的负载，建议使用 -l 1200；不要使用 -Z。

#### 第五步：可选，测试用户态主动连接

先在终端 A 按 Ctrl+C 停止用户态 iperf3 服务端。然后在终端 B 启动普通内核服务端：

```bash
ip netns exec dnmanual iperf3 -4 -s -B 192.0.2.1 -p 15201
```

终端 A 分别运行用户态 TCP、UDP 客户端：

```bash
LD_PRELOAD="$STACK_SO" iperf3 -4 -c 192.0.2.1 -p 15201 -t 10
LD_PRELOAD="$STACK_SO" iperf3 -4 -c 192.0.2.1 -p 15201 -u -l 1200 -b 1M -t 10
```

这一步验证主动握手和发送路径，而不仅是被动监听。

#### 第六步：观察与清理

可在另一个 root 终端观察测试报文：

```bash
tcpdump -ni dnmanual0 'arp or tcp port 16379 or port 15201'
```

通过 ps 找到本次 Redis/iperf3 的 PID，再执行 `grep libdpdknet /proc/PID/maps` 可确认已加载共享库。单纯出现在 maps 只证明已加载；结合数据面抓包以及内核 ss 看不到对应 TCP 监听端口，才能进一步核对流量路径。内核对端命名空间中正常会有内核 socket。

先在对应终端 Ctrl+C 停止本次测试服务和 tcpdump，等待退出，再在终端 A 清理自己创建的网络与测试文件：

```bash
ip link del dnmanual0
ip netns del dnmanual
rm -f "$STACK_ROOT/build/manual-test.rdb"
```

不需要停止已有 Redis 服务，也不要删除其它命名空间。遇到连接失败，先检查终端 A 的 EAL 输出、网卡名、绑定 IP，以及 ldd 是否存在缺失库；不要用 ping 作为本栈可用性的判断，因为当前路径没有提供 ICMP echo 服务。

## 5. 使用实际 DPDK 数据网卡

准备专用数据网卡、匹配的 vfio/uio 驱动、hugepages 和对应权限。SSH 管理网卡继续使用内核驱动。确认 DPDK 能识别目标网卡，再设置：

```bash
export LD_LIBRARY_PATH="$RTE_SDK/$RTE_TARGET/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export DPDKNET_IP=192.168.196.132
export DPDKNET_EAL_ARGS="-l 0-2 -n 4 --file-prefix=redis_stack -w 0000:03:00.0"
export STACK_SO="$PWD/build/libdpdknet.so"
```

IP、lcore 和 PCI BDF 按自己的环境修改。当前对端需要处于可直接 ARP 到达的二层网络；DPDKNET_IP 不应同时配置到其它内核接口。EAL 参数用空格分隔，不解析 shell 引号。默认 IP 是 192.168.196.132。

运行 Redis：

```bash
LD_PRELOAD="$STACK_SO" redis-server \
    --bind "$DPDKNET_IP" --port 6379 --daemonize no \
    --save '' --appendonly no
```

在对端机器执行：

```bash
redis-cli -h 192.168.196.132 -p 6379 PING
redis-cli -h 192.168.196.132 -p 6379 SET demo hello
redis-cli -h 192.168.196.132 -p 6379 GET demo
```

运行 iperf3 用户态 TCP 服务端（先停止使用同一网卡的 Redis 进程）：

```bash
export DPDKNET_EAL_ARGS="-l 0-2 -n 4 --file-prefix=iperf_stack -w 0000:03:00.0"
LD_PRELOAD="$STACK_SO" iperf3 -4 -s -B "$DPDKNET_IP"
```

对端分别运行：

```bash
iperf3 -4 -c 192.168.196.132 -t 10
iperf3 -4 -c 192.168.196.132 -t 10 -R
iperf3 -4 -c 192.168.196.132 -t 10 -P 4
```

用户态客户端：对端先运行普通 `iperf3 -4 -s`，本机执行：

```bash
LD_PRELOAD="$STACK_SO" iperf3 -4 -c 192.168.196.133 -t 10
```

一个物理网卡不能由多个独立 EAL 主进程同时独占。以上 Redis 与 iperf3 命令分开执行。UDP 测试使用 `-u -l 1200 -b 1M`，可加 `-R` 或 `-P 4`；超出 1472 字节负载会返回 EMSGSIZE。不要使用 `-Z`；边界和未实现功能在 PROTOCOLS.md 中逐项列明。

### 5.1 iperf3 UDP 运行

沿用本节前面设置的 LD_LIBRARY_PATH、DPDKNET_IP、DPDKNET_EAL_ARGS 和 STACK_SO；以下示例地址按实际环境替换。

用户态服务端：

```bash
LD_PRELOAD="$STACK_SO" iperf3 -4 -s -B "$DPDKNET_IP"
```

普通对端分别执行：

```bash
iperf3 -4 -c 192.168.196.132 -u -l 1200 -b 1M -t 10
iperf3 -4 -c 192.168.196.132 -u -l 1200 -b 1M -t 10 -R
iperf3 -4 -c 192.168.196.132 -u -l 1200 -b 1M -t 10 -P 4
```

用户态客户端：对端先运行普通 iperf3 服务端，本机运行：

```bash
LD_PRELOAD="$STACK_SO" iperf3 -4 -c 192.168.196.133 -u -l 1200 -b 1M -t 10
```

iperf3 的控制连接依旧使用 TCP；加载 so 的一侧，其 TCP 控制连接和 UDP 数据报现在都经过本协议栈。

## 6. 常见问题

| 现象 | 检查 |
| --- | --- |
| DPDK headers missing | RTE_SDK/RTE_TARGET 是否指向真实构建目录 |
| DPDK so not found | LD_LIBRARY_PATH 与 ldd 输出 |
| undefined relocation / non-PIC | DPDK 是否启用了共享库与 PIC |
| no supported Ethernet device | 网卡驱动、EAL PCI 参数或 AF_PACKET PMD 是否可用 |
| error setting mempool handler | DPDK mempool/PMD 库是否被 --as-needed 丢弃；Makefile 已为 DPDK 库使用 --no-as-needed |
| Redis bind 失败 | bind 的地址必须是 DPDKNET_IP 或 0.0.0.0，检查 IPv4 设置 |
| AF_PACKET 下 SYN 到达但无响应 | 使用脚本关闭 veth offload；检查真实报文校验和 |
| 不支持的 socket/epoll 选项报错 | 查阅支持边界，不能依赖以前“忽略后返回成功”的行为 |

构建日志中原教学函数的 unused-function 警告不影响链接；新模块的测试无需这些遗留函数。
