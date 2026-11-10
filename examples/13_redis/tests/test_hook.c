#include <assert.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/un.h>
static void *wait_added(void *arg)
{
    struct epoll_event event;
    int rc = epoll_wait(*(int *)arg, &event, 1, 500);
    assert(rc == 1 && event.data.u64 == 42);
    return NULL;
}

static int wake_fd;
static void *make_ready(void *arg)
{
    (void)arg;
    usleep(20000);
    void (*setready)(int, int) = dlsym(RTLD_DEFAULT, "hook_test_ready");
    setready(wake_fd, EPOLLIN);
    return NULL;
}
static void *wait_closed(void *arg)
{
    struct epoll_event e;
    assert(epoll_wait(*(int *)arg, &e, 1, -1) == -1 && errno == EBADF);
    return NULL;
}

static int signal_output_fd;
static volatile sig_atomic_t signal_write_result;
static void write_from_signal(int signal_number)
{
    (void)signal_number;
    signal_write_result = write(signal_output_fd, "s", 1);
}

int main(void)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    assert(fcntl(fd, F_GETFL) & O_NONBLOCK);
    assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    int a = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    assert(a >= 0 && (fcntl(a, F_GETFL) & O_NONBLOCK));
    close(a);
    char x[3] = {0}, y[5] = {0};
    struct iovec iovecs[2] = {{x, 2}, {y, 4}};
    assert(readv(fd, iovecs, 2) == 6 && !memcmp(x, "ab", 2) && !memcmp(y, "cdef", 4));
    assert(writev(fd, iovecs, 2) == 6);
    struct msghdr message = {0};
    message.msg_iov = iovecs;
    message.msg_iovlen = 2;
    assert(sendmsg(fd, &message, 0) == 6);
    memset(x, 0, sizeof(x));
    memset(y, 0, sizeof(y));
    assert(recvmsg(fd, &message, MSG_PEEK) == 6);
    assert(!memcmp(x, "ab", 2) && !memcmp(y, "cdef", 4) && message.msg_flags == 0);
    message.msg_iovlen = IOV_MAX + 1;
    assert(sendmsg(fd, &message, 0) == -1 && errno == EMSGSIZE);
    message.msg_iovlen = 2;
    message.msg_name = x;
    assert(sendmsg(fd, &message, 0) == -1 && errno == EOPNOTSUPP);
    int available = 0;
    assert(ioctl(fd, FIONREAD, &available) == 0 && available == 6);
    int p[2];
    assert(pipe(p) == 0);
    /* A signal interrupting the epoll lock must still write native log fds. */
    void (*lock_events)(void) = dlsym(RTLD_DEFAULT, "nepoll_fork_prepare");
    void (*unlock_events)(void) = dlsym(RTLD_DEFAULT, "nepoll_fork_parent");
    assert(lock_events && unlock_events);
    struct sigaction signal_action = {.sa_handler = write_from_signal};
    sigemptyset(&signal_action.sa_mask);
    assert(sigaction(SIGUSR1, &signal_action, NULL) == 0);
    signal_output_fd = p[1];
    lock_events();
    raise(SIGUSR1);
    unlock_events();
    assert(signal_write_result == 1 && read(p[0], x, 1) == 1 && x[0] == 's');

    int ep = epoll_create1(EPOLL_CLOEXEC);
    assert(ep >= 0);
    struct epoll_event e = {.events = EPOLLIN, .data.u64 = 0x1122334455667788ULL}, out[4];
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e) == 0);
    e.data.u64 = 7;
    assert(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &e) == 0);
    assert(write(p[1], "z", 1) == 1);
    int count = epoll_wait(ep, out, 4, 10);
    assert(count == 2);
    int seen = 0;
    for (int i = 0; i < count; ++i) {
        if (out[i].data.u64 == 7)
            seen |= 1;
        if (out[i].data.u64 == 0x1122334455667788ULL)
            seen |= 2;
    }
    assert(seen == 3);
    e.events = EPOLLIN | EPOLLONESHOT;
    e.data.u64 = 9;
    assert(epoll_ctl(ep, EPOLL_CTL_MOD, fd, &e) == 0);
    assert(read(p[0], x, 1) == 1);
    assert(epoll_wait(ep, out, 4, 0) == 1 && out[0].data.u64 == 9);
    assert(epoll_wait(ep, out, 4, 0) == 0);
    assert(epoll_ctl(ep, EPOLL_CTL_MOD, fd, &e) == 0 && epoll_wait(ep, out, 4, 0) == 1);
    e.events = EPOLLIN | EPOLLET;
    assert(epoll_ctl(ep, EPOLL_CTL_MOD, fd, &e) == -1 && errno == EOPNOTSUPP);
    int ep2 = epoll_create1(0);
    pthread_t waiter;
    assert(pthread_create(&waiter, NULL, wait_added, &ep2) == 0);
    usleep(10000);
    e.events = EPOLLIN;
    e.data.u64 = 42;
    assert(epoll_ctl(ep2, EPOLL_CTL_ADD, fd, &e) == 0);
    assert(pthread_join(waiter, NULL) == 0);
    close(ep2);
    void (*setready)(int, int) = dlsym(RTLD_DEFAULT, "hook_test_ready");
    assert(setready);
    setready(fd, 0);
    /* This is our eventfd-backed object, not a kernel epoll instance. */
    int (*owned)(int) = dlsym(RTLD_DEFAULT, "nepoll_owned");
    assert(owned && owned(ep));
    assert(fcntl(ep, F_GETFD) & FD_CLOEXEC);
    assert(dup(ep) == -1 && errno == EOPNOTSUPP);
    assert(fcntl(ep, F_DUPFD, 0) == -1 && errno == EOPNOTSUPP);
    assert(epoll_create(0) == -1 && errno == EINVAL);
    assert(epoll_create1(123) == -1 && errno == EINVAL);
    int ep3 = epoll_create(1);
    e.events = EPOLLIN;
    e.data.u64 = 88;
    assert(epoll_ctl(ep3, EPOLL_CTL_ADD, fd, &e) == 0);
    assert(epoll_ctl(ep3, EPOLL_CTL_ADD, fd, &e) == -1 && errno == EEXIST);
    wake_fd = fd;
    assert(pthread_create(&waiter, NULL, make_ready, NULL) == 0);
    assert(epoll_wait(ep3, out, 1, -1) == 1 && out[0].data.u64 == 88);
    assert(pthread_join(waiter, NULL) == 0);
    /* LT requeues, then removal and descriptor close remove queued items. */
    sigset_t mask;
    sigemptyset(&mask);
    assert(epoll_pwait(ep3, out, 1, 0, &mask) == 1);
    assert(fcntl(ep3, F_SETFL, 0) == 0);
    assert(read(ep3, out, sizeof(out)) == -1 && errno == EINVAL);
    assert(epoll_wait(ep3, out, 1, 0) == 1);
    assert(epoll_ctl(ep3, EPOLL_CTL_DEL, fd, NULL) == 0);
    assert(epoll_wait(ep3, out, 1, 10) == 0);
    assert(pthread_create(&waiter, NULL, wait_closed, &ep3) == 0);
    usleep(10000);
    assert(close(ep3) == 0);
    assert(pthread_join(waiter, NULL) == 0);
    /* maxevents=1 must rotate ready descriptors across calls. */
    int fair = epoll_create1(0), other = socket(AF_INET, SOCK_STREAM, 0);
    e.data.u64 = 101;
    assert(epoll_ctl(fair, EPOLL_CTL_ADD, fd, &e) == 0);
    e.data.u64 = 102;
    assert(epoll_ctl(fair, EPOLL_CTL_ADD, other, &e) == 0);
    assert(epoll_wait(fair, out, 1, 0) == 1);
    uint64_t first = out[0].data.u64;
    assert(epoll_wait(fair, out, 1, 0) == 1 && out[0].data.u64 != first);
    assert(close(other) == 0);
    close(fair);
    setready(fd, 0);
    struct pollfd poll_fds[2] = {{fd, POLLIN, 0}, {p[0], POLLIN, 0}};
    assert(poll(poll_fds, 2, 2) == 0);
    assert(write(p[1], "q", 1) == 1);
    assert(poll(poll_fds, 2, 10) == 1 && poll_fds[1].revents & POLLIN);
    assert(read(p[0], x, 1) == 1);
    setready(fd, EPOLLIN | EPOLLOUT);
    fd_set r, w;
    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_SET(fd, &r);
    FD_SET(fd, &w);
    struct timeval tv = {0, 10000};
    assert(select(fd + 1, &r, &w, NULL, &tv) == 2);
    /* Native AF_UNIX connect and accept4 must reach libc with their flags. */
    char path[100];
    snprintf(path, sizeof(path), "/tmp/dpdknet-hook-%ld.sock", (long)getpid());
    int server = socket(AF_UNIX, SOCK_STREAM, 0), client = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(server >= 0 && client >= 0);
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    strcpy(un.sun_path, path);
    assert(bind(server, (void *)&un, sizeof(un)) == 0 && listen(server, 1) == 0 &&
           connect(client, (void *)&un, sizeof(un)) == 0);
    int conn = accept4(server, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    assert(conn >= 0 && (fcntl(conn, F_GETFD) & FD_CLOEXEC) && (fcntl(conn, F_GETFL) & O_NONBLOCK));
    close(conn);
    close(client);
    close(server);
    unlink(path);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(fd);
        int c = socket(AF_INET, SOCK_STREAM, 0);
        _exit(c < 0 ? 1 : 0);
    }
    int child_status;
    assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) &&
           WEXITSTATUS(child_status) == 0);
    assert(write(fd, "x", 1) == 1);
    assert(epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL) == 0);
    close(fd);
    close(ep);
    close(p[0]);
    close(p[1]);
    puts("hook: fd flags, vector I/O, native connect/accept4, mixed epoll/poll/select, oneshot, "
         "deadlines passed");
    return 0;
}
