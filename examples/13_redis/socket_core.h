/* Internal implementation header, included by the protocol engines in one TU.
 * The including TU supplies the Linux/DPDK types (or packet-test substitutes).
 * Protocol mutations retain the existing shared socket_state_lock. */
#ifndef DPDKNET_SOCKET_CORE_H
#define DPDKNET_SOCKET_CORE_H
#define STACK_MAX_FDS 65536

enum stack_socket_kind {
    STACK_SOCKET_NONE,
    STACK_SOCKET_TCP,
    STACK_SOCKET_UDP,
    STACK_SOCKET_KIND_COUNT
};

static pthread_mutex_t socket_state_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char socket_fd_types[STACK_MAX_FDS];
static int stack_fork_child;

static enum stack_socket_kind socket_fd_kind(int fd)
{
    if (fd < 0 || fd >= STACK_MAX_FDS)
        return STACK_SOCKET_NONE;
    return __atomic_load_n(&socket_fd_types[fd], __ATOMIC_ACQUIRE);
}

int netstack_fd_is_managed(int fd)
{
    enum stack_socket_kind kind = socket_fd_kind(fd);
    return kind > STACK_SOCKET_NONE && kind < STACK_SOCKET_KIND_COUNT;
}

int nisdgram(int fd)
{
    return socket_fd_kind(fd) == STACK_SOCKET_UDP;
}

/* Called under socket_state_lock; use the final protocol from allocation on. */
static int socket_fd_alloc(enum stack_socket_kind kind)
{
    int fd = (int)syscall(SYS_eventfd2, 0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0)
        return -1;
    if (fd >= STACK_MAX_FDS) {
        syscall(SYS_close, fd);
        errno = EMFILE;
        return -1;
    }
    __atomic_store_n(&socket_fd_types[fd], kind, __ATOMIC_RELEASE);
    return fd;
}

static int socket_fd_release(int fd)
{
    if (!netstack_fd_is_managed(fd)) {
        errno = EBADF;
        return -1;
    }
    __atomic_store_n(&socket_fd_types[fd], STACK_SOCKET_NONE, __ATOMIC_RELEASE);
    return (int)syscall(SYS_close, fd);
}

void nfork_prepare(void)
{
    pthread_mutex_lock(&socket_state_lock);
}

void nfork_parent(void)
{
    pthread_mutex_unlock(&socket_state_lock);
}

/* The API layer invokes each protocol's child reset before dropping this lock. */
static void socket_fork_child_reset(void)
{
    memset(socket_fd_types, 0, sizeof(socket_fd_types));
    stack_fork_child = 1;
    pthread_mutex_unlock(&socket_state_lock);
}

static uint64_t socket_now(void)
{
    struct timespec timestamp;
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return (uint64_t)timestamp.tv_sec * 1000 + timestamp.tv_nsec / 1000000;
}

static int socket_ipv4_valid(struct rte_ipv4_hdr *ipv4_header)
{
    uint16_t saved = ipv4_header->hdr_checksum;
    ipv4_header->hdr_checksum = 0;
    uint16_t expected = rte_ipv4_cksum(ipv4_header);
    ipv4_header->hdr_checksum = saved;
    return saved == expected;
}

static void socket_pause(void)
{
    struct timespec timestamp = {0, 1000000};
    pthread_mutex_unlock(&socket_state_lock);
    nanosleep(&timestamp, NULL);
    pthread_mutex_lock(&socket_state_lock);
}

static int socket_copy_addr(struct sockaddr *socket_addr, socklen_t *address_len, uint32_t ipv4_addr,
                         uint16_t port_number)
{
    if (!address_len || !socket_addr) {
        errno = EFAULT;
        return -1;
    }
    struct sockaddr_in other_socket_addr;
    memset(&other_socket_addr, 0, sizeof(other_socket_addr));
    other_socket_addr.sin_family = AF_INET;
    other_socket_addr.sin_addr.s_addr = ipv4_addr;
    other_socket_addr.sin_port = port_number;
    memcpy(socket_addr, &other_socket_addr,
           *address_len < sizeof(other_socket_addr) ? *address_len : sizeof(other_socket_addr));
    *address_len = sizeof(other_socket_addr);
    return 0;
}

#endif
