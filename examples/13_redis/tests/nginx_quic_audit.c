#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    int (*next_bind)(int,const struct sockaddr*,socklen_t)=dlsym(RTLD_NEXT,"bind");
    int result=next_bind(fd,addr,len);
    if(result==0 && addr && addr->sa_family==AF_INET && ntohs(((const struct sockaddr_in*)addr)->sin_port)==18443) {
        int (*is_udp)(int)=dlsym(RTLD_DEFAULT,"nisdgram");
        int managed=is_udp ? is_udp(fd) : 0;
        char msg[100]; int n=snprintf(msg,sizeof(msg),"DPDK_QUIC_MANAGED_BIND fd=%d managed_udp=%d\n",fd,managed);
        syscall(SYS_write,2,msg,n);
        if(!managed) _exit(91);
    }
    return result;
}
