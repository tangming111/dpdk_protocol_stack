#ifndef DPDKNET_EPOLL_CORE_H
#define DPDKNET_EPOLL_CORE_H
#include <sys/epoll.h>
#include <signal.h>
int nepoll_create(int size);
int nepoll_create1(int flags);
int nepoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int nepoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
int nepoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout,
                 const sigset_t *mask);
int nepoll_owned(int fd);
/* Called only outside socket_state_lock: lock order is epoll_lock -> socket_state_lock. */
void nepoll_notify(void);
int nepoll_forget(int fd);
void nepoll_fork_prepare(void);
void nepoll_fork_parent(void);
void nepoll_fork_child(void);
#endif
