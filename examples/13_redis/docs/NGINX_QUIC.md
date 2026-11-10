# Nginx 加载 libdpdknet.so：TCP 与 QUIC/HTTP/3 实测

本次使用现有 `build/libdpdknet.so`，没有修改协议栈实现或重新编译替换该 so。已通过普通 HTTP/TCP，以及 QUIC/HTTP/3 握手、内容校验和并发流测试。Nginx 的 SIGTERM 退出检查未通过，详见下文。

会话日期为2026-11-05；Linux测试主机日志时钟为2026-11-04，因此原始产物目录使用后者的时间。主机为 `192.168.196.129`，工程根目录为 `/home/king/share/dpdk/dpdk-stable-19.08.2/examples/13_redis`。

## 1. 环境与数据路径

| 项目 | 本次配置 |
| --- | --- |
| 操作系统 | Ubuntu16.04，Linux4.4.240，GCC5.4 |
| Nginx | 1.30.6，HTTP SSL、HTTP v3、debug及poll模块 |
| TLS | 局部编译OpenSSL3.5.1；测试自签名证书；TLS1.3 |
| HTTP/3客户端 | 局部Python3.11.13 + aioquic1.2.0，ALPN仅配置h3，无HTTP/1或HTTP/2回退 |
| 客户端关键依赖 | cryptography42.0.8、pyOpenSSL24.1.0、service-identity24.1.0；完整清单见build/quic-lab/logs/client-requirements.txt |
| 网络 | 临时veth + network namespace + DPDK AF_PACKET，关闭临时接口校验和及分段卸载 |
| 服务端地址 | 用户态192.0.2.2，TCP18080 / UDP18443；不将此IP分配给内核接口 |
| 客户端地址 | 临时命名空间内核网络192.0.2.1 |
| 进程 | daemon off，master_process off，单进程；events使用poll；quic_gso off，无reuseport |
| 原so SHA256 | e808931e9c9fe6bb5dce88710dbb08a36a610a4a2fc0f0adb11fe70593489167 |

QUIC使用UDP，连接ID、TLS握手、QUIC可靠传输和HTTP/3流由Nginx处理；底层IPv4/UDP及socket事件由本so提供。它不会使用 `ng_tcp_stream` 的TCP状态机。普通HTTP对照测试才会走 `ng_tcp_process/ng_tcp_out`。

```mermaid
flowchart LR
    C[aioquic客户端：仅h3] --> K[命名空间内核UDP]
    K --> V[临时veth]
    V --> D[DPDK AF_PACKET]
    D --> U[libdpdknet.so IPv4/UDP与poll]
    U --> N[Nginx QUIC + TLS1.3 + HTTP/3]
```

官方构建说明：[Nginx QUIC/HTTP3](https://nginx.org/en/docs/quic.html)。本次在工程 `build/quic-lab` 中局部构建依赖，没有安装到系统Nginx/OpenSSL/Python目录。

## 2. 兼容性设置

- **poll后端**：当前自有epoll对EPOLLET返回EOPNOTSUPP；Nginx的epoll后端需要边沿触发，因此明确编入 `--with-poll_module`，配置 `events { use poll; }`。poll仍经hook访问用户态socket，不是内核UDP回退。
- **单进程**：现有库的fork子进程不支持重新创建受管理socket，因此设置 `daemon off; master_process off;`；master/worker模式已补测失败，见第6节；reload未验证。
- **指定IPv4监听地址**：使用192.0.2.2，避免通配监听依赖IP_PKTINFO辅助数据；未验证多地址/IPv6。
- **GSO与reuseport**：未开启；没有验证内核UDP_SEGMENT、多worker共享监听或QUIC迁移。
- **客户端证书校验**：测试客户端关闭校验以使用临时自签名证书，仅用于本隔离实验。

## 3. 实测结果与证据

| 检查 | 结果 |
| --- | --- |
| 现有so加载到Nginx进程 | 通过，保存/proc/PID/maps快照 |
| UDP18443由本so管理 | 通过，bind审计调用nisdgram，输出managed_udp=1；否则探针直接失败退出 |
| 同一Nginx进程普通HTTP/TCP | 通过，GET /hello.txt返回200及17字节正确正文 |
| QUIC握手 | 通过，客户端明确输出ALPN=h3 |
| HTTP/3小文件 | 通过，GET /hello.txt返回200，正文及SHA256一致 |
| HTTP/3大文件 | 通过，262144字节逐字节校验 |
| 同一QUIC连接四并发流 | 通过，四个262144字节响应全部校验一致 |
| Nginx服务端日志交叉验证 | 六条200 HTTP/3.0、http3=h3；另有一条普通HTTP/1.1 |
| 退出 | 未通过SIGTERM退出检查；预检长时间未退出，最终测试清理等待3秒后强制终止自己的PID |
| 临时资源清理 | 通过，最终测试Nginx进程、veth及命名空间均已删除；当前没有保留运行中的测试服务 |

客户端完成的HTTP/3 stream ID为0、4、8、12、16、20。小文件17字节；大文件由bytes(range(256))重复1024次生成，SHA256为：

```text
2312394bd99545d9de131c24efb781e765ac1aec243f2ed9347597a793a415e9
```

这些结果验证了本次配置的数据路径，不能解释为默认epoll/master-worker模式或生产环境完整兼容；SIGTERM退出问题仍待定位，没有在本次任务中修改库来规避或宣称已修复。

## 4. 文件与复跑

新增的可保留测试文件：

- `tests/smoke_nginx_quic.sh`：创建隔离网络、证书及配置，启动加载so的Nginx，先测TCP再测HTTP/3，检查服务端访问日志，最后清理自身资源。
- `tests/nginx_quic_client.py`：仅HTTP/3客户端，断言h3、200状态、正文及并发流。
- `tests/nginx_quic_audit.c`：辅助LD_PRELOAD探针，检查18443的成功bind确实属于受管理UDP；自动编译到build目录。

当前主机已保留全部局部工具，可直接复跑：

```bash
cd /home/king/share/dpdk/dpdk-stable-19.08.2/examples/13_redis
sudo bash tests/smoke_nginx_quic.sh
```

脚本自动设置DPDK动态库路径、DPDKNET_IP、EAL参数和唯一file-prefix；仅对Nginx子进程设置LD_PRELOAD，客户端保持内核网络。已有工具位置：

```text
build/quic-lab/nginx/sbin/nginx
build/quic-lab/openssl/bin/openssl
build/quic-lab/python/bin/python3.11
```

局部工具的构建命令保存在 `build/quic-lab/build-tools.sh`；依赖源码位于同目录src，下载地址为nginx.org、openssl.org和python.org，SHA256记录在 `logs/source-sha256.log`。脚本依赖这些已解包的源码，不是通用依赖安装器。Python最初因静态OpenSSL链接顺序缺少_ssl，实际运行环境单独编译补齐了该扩展；构建脚本已补充 `LDFLAGS=-Wl,--no-as-needed`，但修改后的全量依赖构建未重新执行。已实际运行的客户端环境通过后续完整请求验证。

最终Nginx配置核心如下，脚本会用每次独立目录替换证书、根目录和日志路径：

```nginx
daemon off;
master_process off;
worker_processes 1;
events { worker_connections 128; use poll; }
http {
    sendfile off;
    server {
        listen 192.0.2.2:18443 quic;
        listen 192.0.2.2:18080;
        server_name quic-lab;
        ssl_certificate /path/to/run/cert.pem;
        ssl_certificate_key /path/to/run/key.pem;
        ssl_protocols TLSv1.3;
        quic_gso off;
        root /path/to/run/html;
    }
}
```

## 5. 本次原始产物

最终成功运行目录：

```text
build/quic-lab/run-20261104-082701-92368/
```

| 文件 | 证据 |
| --- | --- |
| client.log | h3握手与六个请求的校验结果 |
| tcp-client.log | 普通HTTP/TCP验证 |
| server.log | DPDK初始化与managed_udp=1 |
| preload-maps.log | so实际载入进程的映射记录 |
| logs/access.log | HTTP版本、h3标志、状态码与响应字节数 |
| logs/error.log | Nginx debug日志，含QUIC处理细节 |
| versions.log | Nginx/OpenSSL版本、configure参数及原so哈希 |
| wire.pcap / tcpdump.log | ARP/UDP18443抓包，130包，抓包统计无内核丢包；未捕获普通HTTP/TCP |
| nginx.conf | 本次生成的完整配置 |

汇总日志为 `build/quic-lab/logs/smoke-nginx-quic.log`。较早的 `server-preflight.log` 只代表服务端预检，当时aioquic尚未安装，不能当成完整QUIC通过记录。测试代码还执行了bash语法检查、Python3.11编译检查。

## 6. 默认epoll与master/worker补充实测

本轮继续使用同一个so，SHA256仍为 `e808931e9c9fe6bb5dce88710dbb08a36a610a4a2fc0f0adb11fe70593489167`，没有修改协议栈或重新构建替换它。扩展测试脚本后，每种模式都实际尝试普通HTTP/TCP和QUIC/HTTP3，即使前一项失败也继续运行QUIC客户端。

| 测试模式 | 事件后端 | HTTP/TCP | QUIC/HTTP3 | 结论 |
| --- | --- | --- | --- | --- |
| single-poll，单进程 | 明确use poll | 通过 | 通过：h3握手及6请求含四并发流 | 基准通过 |
| single-default，单进程 | 不写use，日志确认自动选epoll | 失败：连接被关闭、未收到HTTP响应 | 通过：h3握手及6请求含四并发流 | 默认epoll部分路径可工作，但整体不兼容 |
| master-poll，master+2worker | 明确use poll | 超时失败 | 握手失败，ConnectionError，0条HTTP/3访问日志 | 多进程模式失败 |
| master-default，master+2worker | 不写use，日志确认自动选epoll | 超时失败 | 握手失败，ConnectionError，0条HTTP/3访问日志 | 默认多进程模式失败 |
| master-default-reuseport | 默认epoll配置，QUIC监听增加reuseport | 服务启动失败后请求不可达 | 服务启动失败后握手失败 | SO_REUSEPORT不支持，尚未到事件循环/worker服务阶段 |

前三种master/worker描述中的进程数来自配置；在master-poll和master-default中，进程快照及Nginx日志确实记录了master和两个worker。reuseport那一项则在创建监听时已失败，没有成功启动worker。`result.txt`中的startup_rc只检查启动两秒后主PID是否存在，不能单独证明Nginx全部初始化成功或请求可用。

### 6.1 默认epoll的错误与QUIC结果必须分别看

single-default日志确认使用epoll，包含以下错误：

```text
[emerg] epoll_ctl(EPOLL_CTL_ADD, eventfd) failed (95: Operation not supported)
[alert] epoll_ctl() failed (95: Operation not supported)
[alert] epoll_ctl(1, 18) failed (95: Operation not supported) while waiting for request
```

普通HTTP客户端报 `RemoteDisconnected`。但随后同一个Nginx进程仍完成ALPN=h3握手和六个HTTP/3请求，服务端有六条200、HTTP/3.0、http3=h3的访问记录，256KiB数据与四并发流均校验成功。

源码依据：`epoll_core.c:187`明确拒绝EPOLLET/EPOLLEXCLUSIVE/EPOLLWAKEUP；Nginx读写事件处理使用NGX_CLEAR_EVENT的路径会请求边沿触发。因此这些错误不能忽略；同时也不能根据错误推断全部UDP/QUIC路径必然不可用。本次实测结论是“QUIC请求通过，TCP失败且存在事件注册错误”，不是“默认epoll模式完整通过”。

### 6.2 多进程失败与fork处理的关系

在master-poll和master-default中：

- master成功绑定受管理UDP，审计输出managed_udp=1；两个worker实际启动。
- TCP客户端等不到HTTP响应；QUIC客户端发出Initial尝试后握手失败，无h3协商完成记录，无HTTP/3访问记录。
- 临时接口抓包均捕获7个客户端发往18443的UDP包，证明QUIC确实尝试发包。这些AF_PACKET抓包不能作为双向流量完整性的证明。

源码显示 `hook_fork_child → nfork_child` 会执行各协议子进程重置；`tcp_fork_child_reset` 清空TCP表，`udp_fork_child_reset` 清空UDP表，`socket_fork_child_reset` 清空fd协议归属表并设置stack_fork_child；hook还设置fork_child_mode，使新socket/epoll创建回退到内核路径。父进程中的用户态协议状态和处理线程没有按Nginx worker模型共享或重建。

据此分析，多进程模式缺乏继承并继续使用父进程用户态监听的支持，与实测失败一致。此处是结合源码作出的机制分析，不是已完成的多进程修复。

### 6.3 reuseport额外验证

QUIC监听配置改为：

```nginx
listen 192.0.2.2:18443 quic reuseport;
```

实测Nginx直接启动失败：

```text
[emerg] setsockopt(SO_REUSEPORT) 192.0.2.2:18443 failed (92: Protocol not available)
```

随后TCP请求不可达、QUIC握手失败。由于服务没有成功启动，这一项不能解释为“worker成功服务后QUIC出错”。

### 6.4 复跑与日志

```bash
cd /home/king/share/dpdk/dpdk-stable-19.08.2/examples/13_redis
sudo env NGINX_TEST_MODE=single-default bash tests/smoke_nginx_quic.sh
sudo env NGINX_TEST_MODE=master-poll bash tests/smoke_nginx_quic.sh
sudo env NGINX_TEST_MODE=master-default bash tests/smoke_nginx_quic.sh
sudo env NGINX_TEST_MODE=master-default-reuseport bash tests/smoke_nginx_quic.sh
# 通过的对照模式：
sudo env NGINX_TEST_MODE=single-poll bash tests/smoke_nginx_quic.sh
```

每条命令独立运行；失败模式返回1，不能用set -e将它们串起来后误以为所有模式都执行过。默认不设置NGINX_TEST_MODE时，脚本仍运行single-poll。

本轮汇总目录：`build/quic-lab/modes-20261104-084421-92664/`。其中五个模式各有一个同名log，summary.txt记录脚本退出码。每个log末尾的Artifacts指向独立运行目录，其中result.txt分别记录startup/audit/TCP/QUIC状态，processes.log记录进程树，nginx.conf记录完整配置，client.log和logs/access.log记录真实QUIC结果，logs/error.log记录Nginx错误。

本轮使用独立进程组管理每个Nginx实例，清理时仅向本次创建的进程组发送信号；结束后核验无残留Nginx master/worker、临时命名空间或接口。没有修改so来修复EPOLLET、fork或SO_REUSEPORT，也没有把资源清理方式的变化认定为已修复先前SIGTERM异常。
