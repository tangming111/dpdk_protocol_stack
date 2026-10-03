#ifndef _DPDKNET_NET_H_
#define _DPDKNET_NET_H_
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#ifdef __cplusplus
extern "C" {
#endif
/* IPv4 TCP/UDP APIs used by the preload adapter. */
int nisdgram(int fd);
ssize_t ndgram_sendmsg(int fd, const struct msghdr *message, int flags);
ssize_t ndgram_recvmsg(int fd, struct msghdr *message, int flags);

int nsocket(int domain, int type, int protocol);
int nbind(int fd, const struct sockaddr *socket_addr, socklen_t len);
int nlisten(int fd, int backlog);
int naccept(int fd, struct sockaddr *socket_addr, socklen_t *len);
int nconnect(int fd, const struct sockaddr *socket_addr, socklen_t len);
ssize_t nrecv(int fd, void *buffer, size_t len, int flags);
ssize_t nsend(int fd, const void *buffer, size_t len, int flags);
int nshutdown(int fd, int how);
int nclose(int fd);
int ngetsockname(int fd, struct sockaddr *socket_addr, socklen_t *len);
int ngetpeername(int fd, struct sockaddr *socket_addr, socklen_t *len);

int nfcntl(int fd, int command, long argument);
int nioctl(int fd, unsigned long request, void *argument);

int nready(int fd);

int nsetsockopt(int fd, int level, int option, const void *value, socklen_t len);
int ngetsockopt(int fd, int level, int option, void *value, socklen_t *len);

/* Initialize once; fini drains output for at most two seconds before joining
 * the data-plane threads. Reinitialization after fini is not supported. */
int netstack_init(void);
void netstack_fini(void);
int netstack_fd_is_managed(int fd);
void nfork_prepare(void);
void nfork_parent(void);
void nfork_child(void);

/* Datagram APIs preserve message boundaries and source/destination address. */
ssize_t nrecvfrom(int fd, void *buffer, size_t len, int flags, struct sockaddr *socket_addr,
                  socklen_t *address_len);
ssize_t nsendto(int fd, const void *buffer, size_t len, int flags,
                const struct sockaddr *socket_addr, socklen_t address_len);

/* Userspace epoll implementation is declared in epoll_core.h. */
int netstack_is_init_thread(void);

#ifdef __cplusplus
}

#endif
#endif
