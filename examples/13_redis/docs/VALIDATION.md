# 验证环境与测试记录

日期：2026-10-16（会话日期）。所有应用结果在提供的 Linux 主机上实际执行。

## 验证环境

- Ubuntu 16.04，Linux 4.4.240，DPDK 19.08.2，GCC。
- Redis 3.0.6，iperf3 3.0.11。
- DPDK AF_PACKET PMD + 临时 veth + network namespace。
- 关闭测试 veth 的 checksum/TSO/GSO/GRO 卸载，使 AF_PACKET 抓到的报文有完整校验和。
- iperf3 及依赖通过 apt 下载并解包到 `build/iperf-tools`，没有安装到系统目录。

## 2026-10-16 TCP 与应用验证

| 检查 | 结果 |
| --- | --- |
| DPDK 19.08.2 共享库编译与严格未定义符号链接检查 | 通过 |
| make test：序号回绕、慢启动、拥塞避免、Reno 快速恢复、RTO 退避 | 通过 |
| make test：主被动握手、字节流、部分读取、重复包、乱序包、窗口拆分、超时重传、persist、keepalive_enabled、FIN/EOF、TIME_WAIT | 通过 |
| make test：libc 回退、AF_UNIX connect/accept4、混合 epoll/poll/select、ONESHOT、并发新增 watch、fork | 通过 |
| TCP 核心 AddressSanitizer + UndefinedBehaviorSanitizer，含泄漏检测 | 通过 |
| Redis 3.0.6：PING、SET、GET | 通过 |
| Redis：256KiB 二进制 SET/GET、100 条流水线 PING | 通过 |
| Redis：4 个并发客户端、BGSAVE 后继续服务 | 通过 |
| iperf3 3.0.11：用户态服务端普通 TCP | 通过 |
| iperf3：用户态服务端反向 TCP（-R） | 通过 |
| iperf3：用户态服务端 4 连接并行 TCP（-P 4） | 通过 |
| iperf3：用户态客户端连接内核服务端 | 通过 |

测试使用 DPDK AF_PACKET PMD、临时 veth 和 network namespace，没有绑定或更改物理网卡。iperf3 的软件包只下载、解压到 build/iperf-tools，没有全局安装。测试结束会停止自己启动的应用并删除临时网络对象和测试 RDB。

原始日志保存在 build/build-linux.log、test-linux.log、sanitize-linux.log、smoke-linux.log 及各应用 *-smoke.log。build 是忽略目录，重新检出时按 BUILD.md 执行可重新生成这些记录。

这份记录证明所列版本和场景的功能通过，不代表物理网卡吞吐、长期压力、所有 Redis/iperf3 版本或完整 POSIX/TCP 兼容性。支持范围已扩展为 IPv4 TCP/UDP；其余边界见 [PROTOCOLS.md](PROTOCOLS.md)。

## 2026-10-17 UDP 更新

新增用户态 UDP 数据面、socket/connect/sendto/recvfrom、消息 I/O 与就绪通知。

| 检查 | 结果 |
| --- | --- |
| UDP 核心：数据报边界、peek/trunc、零长度、校验和、peer 过滤、队列上限、ring 背压、ARP 超时 | 通过 |
| UDP 核心 ASan/UBSan，含泄漏检测 | 通过 |
| 真实 LD_PRELOAD UDP 客户端/服务端，断言 nisdgram(fd) | 通过 |
| UDP 1472 字节、二进制和零长度数据报 | 通过 |
| iperf3 UDP 用户态服务端普通、反向、4 连接并行 | 通过 |
| iperf3 UDP 用户态客户端 | 通过 |
| Redis、TCP 核心、hook 和 iperf3 TCP 回归 | 通过 |

日志见 build/*-udp-linux.log、udp-*-smoke.log、iperf3-udp*-smoke.log。测试为 MTU 1500、同二层网络，iperf3 使用 -l 1200 -b 1M；没有进行分片、广播、多播或长期压力验收。细节见 [UDP 协议说明](PROTOCOLS.md#udp)。

## 2026-10-18 自有 epoll 版本复测

| 验证 | 结果 |
| --- | --- |
| DPDK 19.08 共享库构建 | 通过 |
| make test：TCP CC、TCP core、UDP core、自己的 epoll core、preload hook | 全部通过 |
| 原始 SYS_epoll_ctl 操作自有句柄返回 EINVAL，证明句柄不是内核 epoll | 通过 |
| TCP/UDP 与管道混合、LT、ONESHOT/MOD、主动唤醒、跨线程关闭、公平性 | 通过 |
| EINTR、epoll_pwait 等待信号掩码、超时、删除已入队节点 | 通过 |
| epoll 核心 ASan + UBSan，启用泄漏检查 | 通过，无报告 |
| Redis PING/SET/GET、256KiB 二进制、100 条流水线、4 客户端、BGSAVE | 通过 |
| 真实 preload UDP API 与内核对端交换数据报 | 通过 |
| iperf3 TCP/UDP 服务端、客户端、反向、并行 4 连接 | 全部通过 |

最新测试日志：`build/build-epoll-linux.log`、`build/test-epoll-linux.log`、`build/sanitize-epoll-linux.log`、`build/smoke-epoll-linux.log`。本次应用联调使用临时 veth + network namespace 和 DPDK AF_PACKET PMD，不代表物理 PMD 压测结果。自有事件核心的性能和支持边界见 [EPOLL.md](EPOLL.md)。

## 测试方法与日志定位

UDP API 联调通过 dlsym 获取 nisdgram，并断言 fd 属于用户态 UDP，防止内核路径误通过；udp_peer.py 提供独立内核对端。实际测试入口见 [代码导读](CODE_GUIDE.md)，复测命令见 [BUILD.md](BUILD.md)。

关键结果与原始记录：

- `build/build-linux.log`：共享库编译、链接结果。仍有原教学代码中的 unused-function 警告。
- `build/test-linux.log`：拥塞控制、报文状态机、LD_PRELOAD/混合事件等待测试。
- `build/smoke-linux.log`：应用联调总结果。
- `build/redis-server-smoke.log`：Redis 启动、后台保存、关闭日志。
- `build/iperf3-normal-smoke.log`、`iperf3-reverse-smoke.log`、`iperf3-parallel-smoke.log`、`iperf3-client-smoke.log`：实际 TCP 传输结果。
- `build/wire-smoke.log`：临时 veth 抓包文字记录。

## 2026-10-18 变量命名整理后的回归

TCP/UDP 地址方向、队列、拥塞控制、时间单位及主要局部变量已采用语义化名称，源码备份和初始映射保存在 build/naming-before/。标准导出函数名和报文格式保持不变。

- 共享库构建和 ldd -r 符号检查通过，五组 make test 通过。
- TCP、UDP、epoll 核心的 ASan/UBSan 与泄漏检查通过。
- 最终完整 smoke_linux.sh 通过：Redis 大数据、流水线、并发、BGSAVE、正常退出，以及 UDP API、iperf3 TCP/UDP 各模式。
- 联调发现并修复旧有的信号重入问题：Redis 主线程持有 epoll_lock 时，退出信号中的日志 write 通过 nepoll_owned 重复加锁。常用 fd 归属改为原子表查询，并增加持锁期间由 SIGUSR1 触发原生管道 write 的回归测试。完整异步信号安全边界见 EPOLL.md。

日志：build/build-naming-linux.log、build/test-naming-linux.log、build/sanitize-naming-linux.log、build/smoke-naming-linux.log、build/link-naming-linux.log。最终联调没有手动终止测试应用；最初定位死锁的中断运行不作为最终通过记录。

## 2026-10-20 iperf3 连续测试的监听重绑定修复

问题：关闭监听后，旧 TCB 已变为 CLOSED 且 fd=-1，但在后台回收前仍被 tcp_bind 判为端口占用。修复仅排除此类已关闭记录，保留活跃绑定、监听与 TIME_WAIT 的冲突/复用检查；没有固定监听端口。

| 检查 | 结果 |
| --- | --- |
| 隔离的修复前 tcp_core.h 配合新增回归测试 | 在关闭后立即重新绑定的成功断言处失败，复现缺陷 |
| 修复版：关闭后无后台回收，立即 bind/listen；默认与 SO_REUSEADDR 两种配置 | 通过 |
| 活跃绑定、活跃监听仍返回 EADDRINUSE；15202 与 15201 可分别绑定 | 通过 |
| TIME_WAIT 在未开启复用时仍阻止绑定，开启后依现有规则允许 | 通过 |
| 共享库构建、五组 make test、TCP 核心 ASan/UBSan（含泄漏检查） | 通过 |
| 同一个 iperf3 服务端 PID 连续完成 TCP 正向、反向、并行4流、UDP 正向、反向、并行4流，再回到 TCP 正向 | 七轮均通过，每轮2秒；服务端没有重新启动，无监听/地址占用错误 |
| Redis、UDP API、原有 iperf3 单轮服务端及 TCP/UDP 主动客户端回归 | 全部通过 |

完整 smoke_linux.sh 返回 0，共19条 PASS。本次使用独立的临时 veth/network namespace 和 AF_PACKET PMD；未修改手动测试的 dnmanual 网络或停止其它已有应用。

日志：build/rebind-before-linux.log（预期失败）、build/build-rebind-linux.log、build/test-rebind-linux.log、build/sanitize-rebind-linux.log、build/smoke-rebind-linux.log。持续服务端日志为 build/iperf3-persistent-server.log，各客户端结果为 build/iperf3-persistent-*.log。

## 2026-10-21 Linux ip 双端点教程验证

对应 [LINUX_IP_LAB.md](LINUX_IP_LAB.md)。同一 Linux 上创建两个独立网络命名空间，用一对 veth 连接，分别配置192.0.2.10/24和192.0.2.20/24；两端均使用内核网络，不设置 LD_PRELOAD，不启动 DPDK。

| 验证 | 结果 |
| --- | --- |
| 两端接口/IP、直连路由选择、双向 ping | 通过 |
| 邻居表和接口收发计数查询 | 通过 |
| TCP 正向、反向、并行4连接 | 通过 |
| UDP 正向、反向，1200字节负载，目标10Mbit/s | 通过，正常输出接收、抖动及丢包统计；不要求零丢包 |
| 交换服务端/客户端后执行 TCP、UDP | 通过 |
| 接口 down/up，A 地址由.10改为.11后由B ping，再恢复.10 | 通过 |
| 26个文档 bash 命令块的 bash -n 语法检查 | 通过 |

验证脚本 build/validate_ip_lab.sh 返回0，共9条 PASS，原始日志为 build/ip-lab-validation.log 和 build/ip-lab-*.log。验证时每轮打流2秒；文档手动示例为10秒。命名空间和接口名称使用PID后缀避免冲突，测试对象在退出时清理；原有dnmanual保持未动。

## 2026-11-05 Nginx QUIC/HTTP3 联调

使用现有build/libdpdknet.so，Nginx1.30.6 + OpenSSL3.5.1，poll事件后端、单进程、指定IPv4监听、关闭QUIC GSO；独立AF_PACKET/veth/netns。实际通过普通HTTP/TCP、QUIC ALPN=h3握手、HTTP/3小文件、256KiB文件及同连接四并发流。审计nisdgram确认UDP socket归本so管理，Nginx访问日志包含六条200 HTTP/3.0 http3=h3。

SIGTERM退出未通过；最终脚本等待3秒后强制终止自己创建的Nginx PID并清理临时网络。没有修改协议栈实现，不将该限制计为修复。完整配置、版本、证据和复跑方法见 [NGINX_QUIC.md](NGINX_QUIC.md)。成功日志为build/quic-lab/logs/smoke-nginx-quic.log，原始目录为build/quic-lab/run-20261104-082701-92368（Linux日志时钟与会话日期不同）。

### 2026-11-05 默认epoll、多worker与QUIC补测

使用原so、Nginx1.30.6，五种模式均实际运行TCP和QUIC客户端。single-poll全部通过；single-default自动选择epoll，QUIC的h3握手、六请求和四并发流通过，但TCP失败，出现epoll_ctl/EOPNOTSUPP(95)。master-poll和master-default均确认两个worker启动，TCP超时、QUIC握手失败。master-default-reuseport在启动时因SO_REUSEPORT/ENOPROTOOPT(92)失败。

汇总与日志：build/quic-lab/modes-20261104-084421-92664。详见[NGINX_QUIC.md第6节](NGINX_QUIC.md#6-默认epoll与masterworker补充实测)。本轮只扩展测试与文档，未修改或替换协议栈so；临时进程组及网络已清理。
