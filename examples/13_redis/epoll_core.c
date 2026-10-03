/* Userspace epoll: RB subscription tree + FIFO ready queue. No kernel epoll
 * objects are created here. poll sleeps on native fds and an eventfd doorbell. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "net.h"
#include "epoll_core.h"
#include "nty_tree.h"
#ifndef EPOLLEXCLUSIVE
#define EPOLLEXCLUSIVE (1u << 28)
#endif

struct epitem {
    RB_ENTRY(epitem) node;
    int fd, is_stack_socket, is_armed, is_queued;
    uint32_t ready_events;
    struct epoll_event interest_event;
    struct epitem *ready_next;
};
static int item_compare(struct epitem *left_watch, struct epitem *right_watch)
{
    return (left_watch->fd > right_watch->fd) - (left_watch->fd < right_watch->fd);
}
RB_HEAD(subscription_tree, epitem);
RB_GENERATE_STATIC(subscription_tree, epitem, node, item_compare);
struct eventpoll {
    int fd, is_closed, waiter_refs, watch_count;
    struct subscription_tree subscriptions;
    struct epitem *ready_head, *ready_tail;
    struct eventpoll *next;
};
static pthread_mutex_t epoll_lock = PTHREAD_MUTEX_INITIALIZER;
static struct eventpoll *instances;
/* Native read/write may run inside a signal handler while epoll_lock is held.
 * Common descriptor ownership queries must not reenter that mutex. */
#define EPOLL_FAST_FD_COUNT 65536
static unsigned char epoll_fd_ownership[EPOLL_FAST_FD_COUNT];

static int64_t monotonic_ms(void)
{
    struct timespec timestamp;
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return (int64_t)timestamp.tv_sec * 1000 + timestamp.tv_nsec / 1000000;
}
static struct eventpoll *find_epoll(int fd)
{
    for (struct eventpoll *poll_instance = instances; poll_instance;
         poll_instance = poll_instance->next)
        if (poll_instance->fd == fd && !poll_instance->is_closed)
            return poll_instance;
    return NULL;
}
static void ring_doorbell(struct eventpoll *poll_instance)
{
    uint64_t wakeup_increment = 1;
    int saved_errno = errno;
    (void)syscall(SYS_write, poll_instance->fd, &wakeup_increment, sizeof(wakeup_increment));
    errno = saved_errno;
}
static void enqueue(struct eventpoll *poll_instance, struct epitem *watch, uint32_t ready_events)
{
    watch->ready_events = ready_events;
    if (!ready_events || !watch->is_armed || watch->is_queued)
        return;
    watch->is_queued = 1;
    watch->ready_next = NULL;
    if (poll_instance->ready_tail)
        poll_instance->ready_tail->ready_next = watch;
    else
        poll_instance->ready_head = watch;
    poll_instance->ready_tail = watch;
    ring_doorbell(poll_instance);
}
static struct epitem *dequeue(struct eventpoll *poll_instance)
{
    struct epitem *watch = poll_instance->ready_head;
    poll_instance->ready_head = watch->ready_next;
    if (!poll_instance->ready_head)
        poll_instance->ready_tail = NULL;
    watch->is_queued = 0;
    watch->ready_next = NULL;
    return watch;
}
static void remove_item(struct eventpoll *poll_instance, struct epitem *watch)
{
    struct epitem **queue_link = &poll_instance->ready_head, *previous_watch = NULL;
    while (*queue_link && *queue_link != watch) {
        previous_watch = *queue_link;
        queue_link = &(*queue_link)->ready_next;
    }
    if (*queue_link) {
        *queue_link = watch->ready_next;
        if (poll_instance->ready_tail == watch)
            poll_instance->ready_tail = previous_watch;
    }
    RB_REMOVE(subscription_tree, &poll_instance->subscriptions, watch);
    --poll_instance->watch_count;
    free(watch);
}
static void refresh_stack(struct eventpoll *poll_instance)
{
    struct epitem *watch;
    RB_FOREACH(watch, subscription_tree, &poll_instance->subscriptions)
    if (watch->is_stack_socket && watch->is_armed)
        enqueue(poll_instance, watch,
                nready(watch->fd) & (watch->interest_event.events | EPOLLERR | EPOLLHUP));
}
void nepoll_notify(void)
{
    int saved_errno = errno;
    pthread_mutex_lock(&epoll_lock);
    for (struct eventpoll *poll_instance = instances; poll_instance;
         poll_instance = poll_instance->next)
        refresh_stack(poll_instance);
    pthread_mutex_unlock(&epoll_lock);
    errno = saved_errno;
}
int nepoll_owned(int fd)
{
    if (fd < 0)
        return 0;
    if (fd < EPOLL_FAST_FD_COUNT)
        return __atomic_load_n(&epoll_fd_ownership[fd], __ATOMIC_ACQUIRE);
    pthread_mutex_lock(&epoll_lock);
    int owned = find_epoll(fd) != NULL;
    pthread_mutex_unlock(&epoll_lock);
    return owned;
}
int nepoll_create(int size)
{
    if (size <= 0) {
        errno = EINVAL;
        return -1;
    }
    return nepoll_create1(0);
}
int nepoll_create1(int flags)
{
    if (flags & ~EPOLL_CLOEXEC) {
        errno = EINVAL;
        return -1;
    }
    struct eventpoll *poll_instance = calloc(1, sizeof(*poll_instance));
    if (!poll_instance) {
        errno = ENOMEM;
        return -1;
    }
    poll_instance->fd = syscall(SYS_eventfd2, 0, EFD_NONBLOCK | (flags ? EFD_CLOEXEC : 0));
    if (poll_instance->fd < 0) {
        free(poll_instance);
        return -1;
    }
    RB_INIT(&poll_instance->subscriptions);
    pthread_mutex_lock(&epoll_lock);
    poll_instance->next = instances;
    instances = poll_instance;
    if (poll_instance->fd < EPOLL_FAST_FD_COUNT)
        __atomic_store_n(&epoll_fd_ownership[poll_instance->fd], 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&epoll_lock);
    return poll_instance->fd;
}
int nepoll_ctl(int epoll_fd, int op, int fd, struct epoll_event *event)
{
    int result = -1;
    pthread_mutex_lock(&epoll_lock);
    struct eventpoll *poll_instance = find_epoll(epoll_fd);
    struct epitem lookup_key = {.fd = fd};
    struct epitem *watch =
        poll_instance ? RB_FIND(subscription_tree, &poll_instance->subscriptions, &lookup_key)
                      : NULL;
    if (!poll_instance)
        errno = EBADF;
    else if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD && op != EPOLL_CTL_DEL)
        errno = EINVAL;
    else if (op != EPOLL_CTL_DEL && !event)
        errno = EFAULT;
    else if (op != EPOLL_CTL_DEL && (event->events & (EPOLLET | EPOLLEXCLUSIVE | EPOLLWAKEUP)))
        errno = EOPNOTSUPP;
    else if (syscall(SYS_fcntl, fd, F_GETFD) < 0)
        ;
    else if (fd == epoll_fd || find_epoll(fd))
        errno = EOPNOTSUPP; /* Nested epoll requires cycle detection and propagation. */
    else if (op == EPOLL_CTL_ADD && watch)
        errno = EEXIST;
    else if (op != EPOLL_CTL_ADD && !watch)
        errno = ENOENT;
    else if (op == EPOLL_CTL_DEL) {
        remove_item(poll_instance, watch);
        result = 0;
    } else {
        int stack = netstack_fd_is_managed(fd);
        struct stat file_status;
        if (!stack && (fstat(fd, &file_status) < 0 || S_ISREG(file_status.st_mode) ||
                       S_ISDIR(file_status.st_mode))) {
            errno = EPERM;
            goto done;
        }
        if (!watch) {
            watch = calloc(1, sizeof(*watch));
            if (!watch) {
                errno = ENOMEM;
                goto done;
            }
            watch->fd = fd;
            watch->is_stack_socket = stack;
            RB_INSERT(subscription_tree, &poll_instance->subscriptions, watch);
            ++poll_instance->watch_count;
        }
        watch->interest_event = *event;
        watch->is_armed = 1;
        watch->ready_events = 0;
        refresh_stack(poll_instance);
        result = 0;
    }
    if (result == 0)
        ring_doorbell(poll_instance); /* Also wake sleepers to rebuild native poll snapshots. */
done:
    pthread_mutex_unlock(&epoll_lock);
    return result;
}
static short poll_interest(uint32_t interest_events)
{
    return (interest_events & (EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLRDNORM | EPOLLRDBAND |
                               EPOLLWRNORM | EPOLLWRBAND | EPOLLRDHUP));
}
/* Native snapshot results are used only for sleeping. Recheck current watches
 * under the lock before delivering, so concurrent MOD/DEL never returns stale data. */
static struct pollfd *native_snapshot(struct eventpoll *poll_instance, nfds_t *native_fd_count)
{
    struct pollfd *poll_fds = calloc((size_t)poll_instance->watch_count + 1, sizeof(*poll_fds));
    if (!poll_fds)
        return NULL;
    poll_fds[0] = (struct pollfd){poll_instance->fd, POLLIN, 0};
    *native_fd_count = 1;
    struct epitem *watch;
    RB_FOREACH(watch, subscription_tree, &poll_instance->subscriptions)
    if (!watch->is_stack_socket && watch->is_armed)
        poll_fds[(*native_fd_count)++] =
            (struct pollfd){watch->fd, poll_interest(watch->interest_event.events), 0};
    return poll_fds;
}
static void release_epoll(struct eventpoll *poll_instance)
{
    if (--poll_instance->waiter_refs == 0 && poll_instance->is_closed) {
        syscall(SYS_close, poll_instance->fd);
        free(poll_instance);
    }
}
static int native_poll(struct pollfd *poll_fds, nfds_t native_fd_count, int timeout,
                       const sigset_t *signal_mask)
{
    if (!signal_mask)
        return syscall(SYS_poll, poll_fds, native_fd_count, timeout);
    struct timespec timestamp = {timeout / 1000, (timeout % 1000) * 1000000L};
    return syscall(SYS_ppoll, poll_fds, native_fd_count, timeout < 0 ? NULL : &timestamp,
                   signal_mask, _NSIG / 8);
}
static int wait_events(int epoll_fd, struct epoll_event *events, int maxevents, int timeout,
                       const sigset_t *signal_mask)
{
    if (maxevents <= 0 || maxevents > INT_MAX / (int)sizeof(*events) || !events) {
        errno = !events ? EFAULT : EINVAL;
        return -1;
    }
    int result = -1;
    int64_t deadline_ms = timeout < 0 ? INT64_MAX : monotonic_ms() + timeout;
    pthread_mutex_lock(&epoll_lock);
    struct eventpoll *poll_instance = find_epoll(epoll_fd);
    if (!poll_instance) {
        pthread_mutex_unlock(&epoll_lock);
        errno = EBADF;
        return -1;
    }
    ++poll_instance->waiter_refs;
    for (;;) {
        if (poll_instance->is_closed) {
            errno = EBADF;
            break;
        }
        uint64_t wakeup_count;
        (void)syscall(SYS_read, poll_instance->fd, &wakeup_count, sizeof(wakeup_count));
        refresh_stack(poll_instance);
        nfds_t native_fd_count;
        struct pollfd *poll_fds = native_snapshot(poll_instance, &native_fd_count);
        if (!poll_fds) {
            errno = ENOMEM;
            break;
        }
        if (native_poll(poll_fds, native_fd_count, 0, signal_mask) < 0) {
            free(poll_fds);
            break;
        }
        for (nfds_t i = 1; i < native_fd_count; ++i) {
            struct epitem lookup_key = {.fd = poll_fds[i].fd};
            struct epitem *watch =
                RB_FIND(subscription_tree, &poll_instance->subscriptions, &lookup_key);
            uint32_t ready_events = poll_fds[i].revents & ~POLLNVAL;
            enqueue(poll_instance, watch,
                    ready_events & (watch->interest_event.events | EPOLLERR | EPOLLHUP));
        }
        /* Process only this queue generation; LT items go to the tail. */
        struct epitem *last_ready_watch = poll_instance->ready_tail;
        result = 0;
        while (poll_instance->ready_head && result < maxevents) {
            struct epitem *watch = dequeue(poll_instance);
            uint32_t ready_events = watch->ready_events;
            if (watch->is_stack_socket)
                ready_events =
                    nready(watch->fd) & (watch->interest_event.events | EPOLLERR | EPOLLHUP);
            if (watch->is_armed && ready_events) {
                events[result] = watch->interest_event;
                events[result++].events = ready_events;
                if (watch->interest_event.events & EPOLLONESHOT)
                    watch->is_armed = 0;
                else
                    enqueue(poll_instance, watch, ready_events);
            }
            if (watch == last_ready_watch)
                break;
        }
        if (result || monotonic_ms() >= deadline_ms) {
            free(poll_fds);
            break;
        }
        /* Empty queue: consume our own previous doorbells before unlocking.
         * Any later producer holds this lock and writes a fresh wakeup. */
        (void)syscall(SYS_read, poll_instance->fd, &wakeup_count, sizeof(wakeup_count));
        int64_t remaining_ms = timeout < 0 ? -1 : deadline_ms - monotonic_ms();
        int wait_ms = remaining_ms < 0 ? -1 : remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms;
        if (timeout >= 0 && remaining_ms < 0)
            wait_ms = 0;
        pthread_mutex_unlock(&epoll_lock);
        int poll_result = native_poll(poll_fds, native_fd_count, wait_ms, signal_mask);
        int saved_errno = errno;
        free(poll_fds);
        pthread_mutex_lock(&epoll_lock);
        if (poll_result < 0) {
            errno = saved_errno;
            result = -1;
            break;
        }
        result = -1;
    }
    int saved_errno = errno;
    release_epoll(poll_instance);
    pthread_mutex_unlock(&epoll_lock);
    errno = saved_errno;
    return result;
}
int nepoll_wait(int epoll_fd, struct epoll_event *events, int maxevents, int timeout)
{
    return wait_events(epoll_fd, events, maxevents, timeout, NULL);
}
int nepoll_pwait(int epoll_fd, struct epoll_event *events, int maxevents, int timeout,
                 const sigset_t *signal_mask)
{
    return wait_events(epoll_fd, events, maxevents, timeout, signal_mask);
}
int nepoll_forget(int fd)
{
    pthread_mutex_lock(&epoll_lock);
    int owned = 0;
    struct eventpoll **link = &instances;
    while (*link) {
        struct eventpoll *poll_instance = *link;
        if (poll_instance->fd == fd) {
            owned = 1;
            if (fd < EPOLL_FAST_FD_COUNT)
                __atomic_store_n(&epoll_fd_ownership[fd], 0, __ATOMIC_RELEASE);
            *link = poll_instance->next;
            poll_instance->is_closed = 1;
            while (!RB_EMPTY(&poll_instance->subscriptions))
                remove_item(poll_instance, RB_ROOT(&poll_instance->subscriptions));
            ring_doorbell(poll_instance);
            if (!poll_instance->waiter_refs) {
                syscall(SYS_close, poll_instance->fd);
                free(poll_instance);
            }
            continue;
        }
        struct epitem lookup_key = {.fd = fd};
        struct epitem *watch =
            RB_FIND(subscription_tree, &poll_instance->subscriptions, &lookup_key);
        if (watch) {
            remove_item(poll_instance, watch);
            ring_doorbell(poll_instance);
        }
        link = &poll_instance->next;
    }
    pthread_mutex_unlock(&epoll_lock);
    return owned;
}
void nepoll_fork_prepare(void)
{
    pthread_mutex_lock(&epoll_lock);
}
void nepoll_fork_parent(void)
{
    pthread_mutex_unlock(&epoll_lock);
}
void nepoll_fork_child(void)
{
    /* The child has no protocol workers. Drop ownership without modifying the
     * parent's shared eventfd counters. Its inherited descriptors remain closable. */
    instances = NULL;
    memset(epoll_fd_ownership, 0, sizeof(epoll_fd_ownership));
    pthread_mutex_unlock(&epoll_lock);
}
