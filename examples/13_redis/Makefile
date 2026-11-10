# Linux / DPDK 19.08 shared-library build.
CC ?= cc
APP := dpdknet
SRCS := main.c hook.c epoll_core.c
HDRS := epoll_core.h net.h arp.h nty_tree.h tcp_core.h tcp_cc.h udp_core.h socket_core.h socket_api.h
CFLAGS += -O2 -g -fPIC -D_GNU_SOURCE -pthread -Wall -Wextra
LDLIBS += -pthread -ldl -lm -lrt
# Test and clean targets do not require a DPDK installation.
ifeq ($(strip $(MAKECMDGOALS)),)
NEED_DPDK := yes
else ifneq ($(filter-out clean test,$(MAKECMDGOALS)),)
NEED_DPDK := yes
endif
ifeq ($(NEED_DPDK),yes)
ifeq ($(shell pkg-config --exists libdpdk 2>/dev/null && echo yes),yes)
CFLAGS += $(shell pkg-config --cflags libdpdk) -DALLOW_EXPERIMENTAL_API
DPDK_LIBS := -Wl,--no-as-needed $(shell pkg-config --libs libdpdk) -Wl,--as-needed
else
RTE_SDK ?= $(abspath ../..)
RTE_TARGET ?= x86_64-native-linux-gcc
RTE_INCLUDE := $(RTE_SDK)/$(RTE_TARGET)/include
RTE_LIB := $(RTE_SDK)/$(RTE_TARGET)/lib
ifeq ($(wildcard $(RTE_INCLUDE)/rte_config.h),)
$(error DPDK headers missing: set RTE_SDK and RTE_TARGET, or install libdpdk.pc)
endif
CFLAGS += -march=native -I$(RTE_INCLUDE) -include $(RTE_INCLUDE)/rte_config.h -DALLOW_EXPERIMENTAL_API
DPDK_LIBS := -L$(RTE_LIB) -Wl,--no-as-needed -ldpdk -Wl,--as-needed -lnuma
endif
endif
.PHONY: all clean test
all: build/lib$(APP).so
build/lib$(APP).so: $(SRCS) $(HDRS) Makefile | build
	$(CC) $(CFLAGS) -shared -Wl,-z,defs -Wl,-soname,lib$(APP).so -o $@ $(SRCS) $(LDFLAGS) $(DPDK_LIBS) $(LDLIBS)
build:
	mkdir -p $@
test: | build
	$(CC) -std=c11 -O2 -Wall -Wextra tests/test_tcp_cc.c -o build/test_tcp_cc
	./build/test_tcp_cc
	$(CC) -std=gnu11 -O1 -g -pthread -Wall -Wextra tests/test_tcp_core.c -o build/test_tcp_core
	./build/test_tcp_core
	$(CC) -std=gnu11 -O1 -g -pthread -Wall -Wextra -Wno-unused-function tests/test_udp_core.c -o build/test_udp_core
	./build/test_udp_core
	$(CC) -std=gnu11 -O1 -g -pthread -Wall -Wextra -Wno-unused-function tests/test_socket_api.c -o build/test_socket_api
	./build/test_socket_api
	$(CC) -std=gnu11 -O1 -g -D_GNU_SOURCE -pthread tests/test_epoll_core.c epoll_core.c tests/hook_backend.c -o build/test_epoll_core
	./build/test_epoll_core
	$(CC) -std=gnu11 -O1 -g -D_GNU_SOURCE -pthread -shared -fPIC hook.c epoll_core.c tests/hook_backend.c -ldl -o build/libhook_test.so
	$(CC) -std=gnu11 -O1 -g -D_GNU_SOURCE -pthread tests/test_hook.c -ldl -o build/test_hook
	LD_PRELOAD=$(CURDIR)/build/libhook_test.so ./build/test_hook
clean:
	rm -f build/test_epoll_core build/lib$(APP).so build/test_tcp_cc build/test_tcp_core build/test_hook build/test_udp_core build/test_socket_api build/libhook_test.so
