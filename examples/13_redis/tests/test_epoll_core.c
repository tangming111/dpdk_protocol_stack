/* Direct core tests also run under ASan/UBSan without libc interposition. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "../net.h"
#include "../epoll_core.h"
void hook_test_ready(int fd, int events);
static pthread_t waiting_thread;
static int stack_fd;
static void handle_signal(int sig)
{
    (void)sig;
}
static void *produce(void *arg)
{
    usleep(20000);
    if (arg)
        pthread_kill(waiting_thread, SIGUSR1);
    else
        hook_test_ready(stack_fd, EPOLLIN);
    return NULL;
}
int main(void)
{
    int ep = nepoll_create1(EPOLL_CLOEXEC), pipefd[2];
    stack_fd = nsocket(AF_INET, SOCK_STREAM, 0);
    assert(ep >= 0 && stack_fd >= 0 && pipe(pipefd) == 0);
    struct epoll_event e = {.events = EPOLLIN, .data.u64 = 1}, out[4];
    assert(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, pipefd[0], &e) == -1 && errno == EINVAL);
    hook_test_ready(stack_fd, 0);
    assert(nepoll_ctl(ep, EPOLL_CTL_ADD, stack_fd, &e) == 0);
    e.data.u64 = 2;
    assert(nepoll_ctl(ep, EPOLL_CTL_ADD, pipefd[0], &e) == 0);
    pthread_t producer;
    assert(pthread_create(&producer, NULL, produce, NULL) == 0);
    assert(nepoll_wait(ep, out, 4, -1) == 1 && out[0].data.u64 == 1);
    pthread_join(producer, NULL);
    assert(write(pipefd[1], "x", 1) == 1);
    assert(nepoll_wait(ep, out, 4, 0) == 2);
    char ch;
    assert(read(pipefd[0], &ch, 1) == 1);
    hook_test_ready(stack_fd, 0);
    assert(nepoll_wait(ep, out, 4, 10) == 0);
    struct sigaction action = {.sa_handler = handle_signal};
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    waiting_thread = pthread_self();
    assert(pthread_create(&producer, NULL, produce, (void *)1) == 0);
    assert(nepoll_wait(ep, out, 4, -1) == -1 && errno == EINTR);
    pthread_join(producer, NULL);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    assert(pthread_create(&producer, NULL, produce, (void *)1) == 0);
    assert(nepoll_pwait(ep, out, 4, 60, &mask) == 0);
    pthread_join(producer, NULL);
    /* Closing a socket while it is queued must unlink it before freeing it. */
    hook_test_ready(stack_fd, EPOLLIN);
    assert(nepoll_forget(stack_fd) == 0);
    nclose(stack_fd);
    assert(nepoll_wait(ep, out, 4, 0) == 0);
    assert(nepoll_forget(ep) == 1);
    close(pipefd[0]);
    close(pipefd[1]);
    puts("epoll_core: own handle, mixed queue, wakeup, LT, timeout, EINTR, signal mask, queued "
         "close passed");
    return 0;
}
