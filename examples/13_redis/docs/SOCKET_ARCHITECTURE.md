# Socket 协议分派架构

更新日期：2026-10-29。

## 模块与调用路径

```text
libc socket/I/O → hook.c → net.h 声明的 n* 接口
                              ↓
                         socket_api.h
                   静态协议表 + socket_ops
                      ↙               ↘
                 tcp_core.h         udp_core.h
                      ↘               ↙
                         socket_core.h
                  公共 fd、锁和共享辅助函数
```

为限制本次改动，沿用项目的内部实现头文件组织：协议核心及 socket_api.h 随 main.c 编译，未增加动态加载、运行时注册/卸载或独立协议编译单元。

## 两种分派

创建阶段尚无 fd，socket_protocol_find 按 domain、去除标志后的 socket 类型和 protocol 匹配静态 socket_protocols；protocol=0 选择该类型默认协议。hook 的接管判断和 nsocket 使用同一个注册表。原生接口回退、初始化重入和 fork 子进程回退仍由 hook 管理。

操作阶段按 socket_fd_kind(fd) 直接索引注册项的 socket_ops，然后调用 bind/connect/send/recv/sendmsg/recvmsg/close/ready 等类型安全的函数指针。未登记或越界 fd 返回 EBADF；nready 保持事件掩码约定，返回 EPOLLERR。UDP 缺少 listen/accept 钩子，统一入口返回 EOPNOTSUPP。

nisdgram、ndgram_sendmsg、ndgram_recvmsg 保留为兼容接口。hook 的消息接口已经不依赖这些 UDP 专用入口。

## 协议差异与公共设施

TCP/UDP 分别保留端口冲突规则、connect、状态机、缓冲和关闭实现。UDP 多个 iovec 合成一个数据报；TCP 消息适配器保留原有单次 gather/scatter 和 1MiB 上限。无有效负载地址的 iovec 在拷贝前返回错误。

socket_core.h 提供公共锁、fd 分配/释放/类型查询、时钟、阻塞等待时的解锁重查、IPv4 校验和检查及地址拷贝。UDP 不再调用 TCP 的辅助函数；统一 API 测试逆序包含两种核心来验证这点。

本次不改变共享锁策略，也不加入公共 socket 对象或控制块指针缓存。操作表查询为 O(1)，协议内部 tcp_find/udp_find 仍使用现有链表。函数指针分派本身不代表吞吐提升。

## 生命周期

- 分配占位 eventfd 时直接登记最终协议类型，失败时回收登记和 fd。accept 使用同一公共分配路径登记 TCP。
- close 立即解除 fd 登记；TCP FIN/TIME_WAIT、UDP 待发数据所需控制块仍由协议数据面回收，不能在分派层直接释放。
- fd 可以被后续另一种协议复用，旧的无 fd 控制块不能覆盖新的类型登记。
- fork prepare 保持原有锁顺序。child 遍历注册表调用协议 reset 钩子，再清空 fd 登记并释放公共锁；父进程连接不受子进程回退影响。
- 现有并发 fd 复用边界及不支持受管 dup 的限制没有通过本次函数表重构扩展。

## 扩展与验证

增加协议时需增加类型编号、协议实现/操作表和静态注册项，并明确不支持的操作及 fork 清理。没有运行时可变注册状态。

make test 新增 tests/test_socket_api.c，直接编译生产注册表、统一接口和两个真实协议核心，只模拟 DPDK 报文设施及系统句柄。覆盖协议匹配、非法 fd、UDP 消息边界/地址/截断、TCP 握手/peek/关闭、跨协议 fd 复用、accept 登记和 fork 清理。原有协议、epoll 与 LD_PRELOAD hook 测试继续运行；真实应用联调使用 tests/smoke_linux.sh。
