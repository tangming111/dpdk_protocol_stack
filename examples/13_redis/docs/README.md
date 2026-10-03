# 13_redis 文档索引

本目录是可通过 LD_PRELOAD 加载的 IPv4 TCP/UDP 用户态协议栈，产物为 build/libdpdknet.so，支持已验收场景中的 Redis 和 iperf3。应用 epoll 使用自己的 epoll_core.c。

## 按问题查文档

| 想了解什么 | 文档 |
| --- | --- |
| Linux ip 命令入门、单机两个端点分配 IP、ping 和 TCP/UDP 双向打流 | [LINUX_IP_LAB.md](LINUX_IP_LAB.md) |
| 目录结构、运行线程、fd 分流、hook 各个函数、KNI 与示例代码 | [CODE_GUIDE.md](CODE_GUIDE.md) |
| 如何编译、加载到应用、执行测试、排查环境问题 | [BUILD.md](BUILD.md) |
| TCP 窗口、Reno、定时处理、UDP 数据报语义与功能边界 | [PROTOCOLS.md](PROTOCOLS.md) |
| 自己的 epoll 如何注册、排队、唤醒、等待和关闭 | [EPOLL.md](EPOLL.md) |
| 实际测试过的版本、场景、结果及日志 | [VALIDATION.md](VALIDATION.md) |

不熟悉 Linux 网络命令时先读 LINUX_IP_LAB。首次阅读协议栈代码建议：CODE_GUIDE → BUILD → PROTOCOLS → EPOLL；验收结果单独查询 VALIDATION。

## 文件夹结构

```text
13_redis/
├── main.c                    初始化、n* 接口、协议分发和 DPDK 收发
├── hook.c                    LD_PRELOAD 拦截与接口适配
├── epoll_core.c / .h          用户态事件实现
├── net.h                     socket 接口声明
├── tcp_core.h / tcp_cc.h      TCP 与拥塞控制实现
├── udp_core.h                UDP 实现
├── arp.h / nty_tree.h         ARP 结构与树宏
├── Makefile / .clang-format  构建与格式配置
├── docs/                     全部项目 Markdown 文档（本目录）
├── tests/                    单元、接口与应用联调测试
└── build/                    共享库、日志、工具和历史源码备份
```

源码和 Makefile 仍在 13_redis 根目录，测试仍在 tests/。文档中的 shell 命令及未加链接的源码、日志路径默认相对于 13_redis；源码超链接使用 ../ 跳回项目目录。build/format-before 中的文件是历史备份，不参与当前构建。

## 文档维护约定

架构与函数说明只维护在 CODE_GUIDE；协议语义与限制在 PROTOCOLS；事件内部机制在 EPOLL；执行命令在 BUILD；测试事实在 VALIDATION；通用 Linux 网络命令与双端点练习在 LINUX_IP_LAB。其它文档通过链接引用，避免复制整段内容导致版本不一致。
