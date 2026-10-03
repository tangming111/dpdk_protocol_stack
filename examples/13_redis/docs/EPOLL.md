# 用户态 epoll 实现说明

更新日期：2026-10-18。

本版以完善协议栈自身为目标，将应用的 epoll 接入独立的 `epoll_core.c`。普通应用调用 `epoll_create/epoll_create1` 时创建的是本项目的 `eventpoll` 对象；订阅、就绪队列、事件分发和 ONESHOT 均由用户态代码管理。

## 1. 代码在哪里

| 文件 | 责任 |
| --- | --- |
| [epoll_core.c](../epoll_core.c) | 实例表、红黑树订阅、FIFO 就绪队列、主动唤醒、等待与关闭 |
| [epoll_core.h](../epoll_core.h) | `nepoll_*` 接口及锁顺序约定 |
| [hook.c](../hook.c) | 将应用 epoll 调用分发到自有实例；适配 close、fork、dup 等 |
| [main.c](../main.c) | 协议处理循环在 TCP/UDP 处理和定时器执行后调用 `nepoll_notify` |
| [nty_tree.h](../nty_tree.h) | 订阅红黑树使用的原有宏实现 |
| [tests/test_epoll_core.c](../tests/test_epoll_core.c) | 直接测试真实 epoll 核心，可运行内存检查 |
| [tests/test_hook.c](../tests/test_hook.c) | 验证 LD_PRELOAD 分发、混合 fd、ONESHOT、跨线程关闭与公平性 |

`main.c` 中旧的 `eventpoll/epitem/nepoll_*` 实现已移除，避免两套同名事件实现并存。现在 `main.c + hook.c + epoll_core.c` 一起链接成 `build/libdpdknet.so`。

## 2. 调用关系

```text
应用 epoll_create1
  -> hook.c
  -> nepoll_create1
  -> 分配 eventpoll，创建 eventfd 作为句柄和唤醒门铃

应用 epoll_ctl
  -> nepoll_ctl
  -> 在当前实例红黑树中 ADD/MOD/DEL

TCP/UDP 报文处理、发送推进、超时处理
  -> pkt_process 循环末尾 nepoll_notify
  -> nready 检查当前受管 socket
  -> 将就绪订阅加入自己的 FIFO 队列
  -> 写 eventfd，唤醒等待线程

应用 epoll_wait / epoll_pwait
  -> nepoll_wait / nepoll_pwait
  -> 复核当前就绪状态
  -> 从 FIFO 取事件，保留应用的 interest_event.data
  -> LT 事件仍就绪则放回队尾；ONESHOT 分发后停用
  -> 无事件时阻塞在 poll / ppoll
```

创建 epoll 不触发 DPDK 初始化。第一次受管 IPv4 TCP/UDP socket 才初始化 EAL 和数据面线程。epoll 对象有独立的实例表，不混入 TCP/UDP 的 fd 类型表。

## 3. 为什么还有 eventfd 和 poll

`eventfd` 在这里既保留真实 fd 编号，也作为线程间唤醒门铃。它不提供 TCP/UDP 连接和报文收发。创建对象后，调用内核 `SYS_epoll_ctl` 操作该句柄会得到 EINVAL，因为它根本不是内核 epoll 实例；核心测试明确验证了这一点。

`poll` 用于阻塞等待门铃，以及 Redis 内部的管道、AF_UNIX 等原生描述符。这些原生对象的就绪状态仍由内核提供。TCP/UDP 就绪状态来自 `nready` 和协议栈自身的缓冲区、连接状态。

当前用户态 epoll 的正常路径**没有调用内核 epoll_create、epoll_ctl 或 epoll_wait**。内核只提供等待和线程唤醒基础设施。应用的普通文件 I/O、线程、时间和原生管道本来就需要操作系统支持。

有两个保留内核 epoll 的场景：EAL 同线程初始化重入期间，以及 fork 子进程模式。它们创建的实例按原生对象处理；普通应用创建的实例走自己的实现。

## 4. 关键数据结构

### eventpoll

- `fd`：真实 eventfd 句柄。
- `subscriptions`：以被监听 fd 为键的红黑树；注册查询复杂度 O(log N)。
- `ready_head/ready_tail`：FIFO 就绪队列。
- `watch_count`：订阅总数，用于构造原生 poll 快照。
- `waiter_refs/is_closed`：等待线程引用数和关闭状态。
- `next`：全局实例链表。

### epitem

- `fd`：监听的描述符。
- `is_stack_socket`：是否属于用户态 TCP/UDP；注册时确定。
- `interest_event`：完整保存调用者的关注掩码和不透明的 data。
- `is_armed`：ONESHOT 是否仍启用。
- `ready_events`：最近复核得到的就绪掩码。
- `is_queued/ready_next`：防止重复入队，并连接 FIFO。
- `node`：红黑树节点。

就绪队列中只保存订阅节点，不复制节点。DEL 和 close 必须同时摘除树节点和队列节点，之后才能释放内存。

## 5. 各函数如何配合

| 函数 | 作用 |
| --- | --- |
| `find_epoll` | 在锁内按 fd 查实例 |
| `ring_doorbell` | 通过原始 write 系统调用写门铃，避开 hook 重入 |
| `enqueue/dequeue` | 就绪订阅入队、出队；is_queued 保证不重复入队 |
| `remove_item` | 同时删除红黑树节点和队列关联 |
| `refresh_stack` | 遍历启用的受管订阅，用 nready 更新状态并入队 |
| `nepoll_notify` | 协议线程通知入口，更新各实例的受管就绪状态 |
| `nepoll_owned` | hook 判断该 epfd 是否为本项目自己的实例 |
| `nepoll_create/create1` | 验证参数、分配实例和门铃，保持 CLOEXEC 语义 |
| `nepoll_ctl` | 维护订阅，MOD 重启 ONESHOT；变更后唤醒等待者重建快照 |
| `native_snapshot` | 构造门铃和原生 fd 的 poll 数组；不把受管 socket 占位 fd 加入 |
| `native_poll` | 普通等待使用 poll；指定信号掩码时使用 ppoll |
| `wait_events` | 等待核心：复核状态、合并原生事件、分发队列、按截止时间阻塞 |
| `nepoll_wait/pwait` | 普通和带信号掩码的等待入口 |
| `release_epoll` | 释放等待者引用；关闭对象的最后一个等待者负责最终释放 |
| `nepoll_forget` | close 前移除订阅，或关闭整个 epoll；返回是否已处理 epfd 关闭 |
| `nepoll_fork_prepare/listener_fd/child` | 配合 hook 的 atfork 处理锁和子进程归属 |

## 6. 等待、锁与关闭

常用 fd（0～65535）的 epoll 归属通过原子登记表查询，避免原生日志 read/write 在信号处理函数中重入 epoll_lock；更大的 fd 仍使用加锁查询。该优化不代表受管 socket 操作具备完整异步信号安全性。

全局 `epoll_lock` 保护实例、红黑树和就绪队列。检查受管 socket 时，`nready` 会取得 `socket_state_lock`，因此固定顺序为：

```text
epoll_lock -> socket_state_lock
```

协议处理线程调用 `nepoll_notify` 时必须已经释放 `socket_state_lock`。本版调用点位于 `ng_tcp_out`、`ng_udp_out` 之后。fork 也按同样顺序加锁，按逆序解锁。

等待线程在锁内消费旧门铃并构造原生快照，然后释放锁进入 poll。之后到达的通知必须先取得锁再写门铃，因此通知不会落入“检查完状态但还没开始睡眠”的丢失唤醒窗口。

原生 poll 阻塞后返回的快照只用作唤醒线索，分发前会重新查询当前注册项，避免并发 MOD/DEL 后返回旧 data。每次只处理当前一轮队列；LT 事件放到队尾，使 `maxevents=1` 时多个持续就绪的 fd 也能轮流分发。

关闭 epoll 时将对象从实例表移除、清空订阅并唤醒等待者。正在等待的调用返回 EBADF，最后一个引用释放后关闭门铃。fork 子进程清除继承实例的用户态归属，不写父进程共享门铃；继承的真实 fd 仍可关闭。子进程不接管父进程的用户态网络。

hook 禁止对自己的 epoll 句柄 read/write/readv/writev，拒绝 dup 和 fcntl 复制；F_SETFL 始终保留门铃内部的 O_NONBLOCK，避免内部读取意外阻塞。

## 7. 支持范围与下一步

本版支持：多个实例、同一 socket 注册多个实例、TCP/UDP 与管道等原生 fd 混合、LT、ONESHOT 和 MOD 重启、ERR/HUP、超时、EINTR、epoll_pwait 等待阶段的信号掩码、跨线程 ADD/DEL 和关闭、就绪轮转。

明确拒绝：ET、EXCLUSIVE、WAKEUP、自己的 epoll 嵌套、epoll 句柄复制。普通文件和目录注册返回 EPERM。直接绕过 hook 使用 syscall、跨进程共享自己的 epoll、完整内核 open-file-description 别名语义、pthread_cancel 清理未实现。epoll_pwait2 未接入。

这是基础的用户态事件实现，不应把它理解成已经达到内核 epoll 的扩展性：

- 就绪产生端在协议处理循环末尾扫描启用的受管订阅，复杂度 O(N)；尚未在每个 TCB/UDP endpoint 上维护反向订阅列表。
- 原生 fd 兼容层采用 poll 快照，检查和重建也为 O(N)。
- 全局事件锁提供清晰的锁序，仍可能限制多线程扩展性。
- 原有 hook 的独立 poll/select 接口仍保留 1ms 混合扫描；本次替换的是 epoll 路径。

后续可以逐步把报文入队、ACK 释放发送空间、握手完成、FIN、RST 和超时等具体状态变化直接连接到对应订阅，再拆分实例锁。实现 ET 前，必须定义并覆盖每类就绪状态的边沿通知，不能只去掉拒绝标志。

## 8. 验证方式

构建、单元测试、ASan/UBSan 与应用联调命令统一见 [BUILD.md](BUILD.md)。实测结果及日志位置见 [VALIDATION.md](VALIDATION.md)。
