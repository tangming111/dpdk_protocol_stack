#include <rte_ether.h>
#include <rte_malloc.h>

#ifndef _NG_ARP_H_
#define _NG_ARP_H_

#define LL_ADD(item, list) do {  \
    item->prev = NULL;           \
    item->next = list;           \
    if(list != NULL) {           \
        list->prev = item;       \
    }                            \
    list = item;                 \
}while(0);


#define LL_REMOVE(item, list) do{                           \
    if(item->prev != NULL) item->prev->next = item->next;   \
    if(item->next != NULL) item->next->prev = item->prev;   \
    if(item == list) list = item->next;                     \
    item->prev = NULL;                                      \
    item->next = NULL;                                      \
}while(0)

struct arp_table_entry {
    uint32_t ip_addr;
    uint8_t mac_addr[RTE_ETHER_ADDR_LEN];

    uint8_t type;//0: dynamic, 1: static

    struct arp_tablke_entry *next;
    struct arp_table_entry *prev;
};

struct arp_table {
    struct arp_table_entry *entries;
    int count;
};

static struct arp_table* arpt = NULL;

static struct arp_table* arp_table_instance() {
    if (arpt == NULL) {
        arpt = (struct arp_table*)rte_malloc("ARP_TABLE", sizeof(struct arp_table), 0);
        if (arpt == NULL) {
            rte_exit(EXIT_FAILURE, "Failed to allocate memory for ARP table\n");
        }
        memset(arpt, 0, sizeof(struct arp_table));
    }
    return arpt;
}


static struct arp_table_entry* ng_get_dst_macaddr(uint32_t ip) {
    struct arp_table* table = arp_table_instance();
    struct arp_table_entry *entry = table->entries;

    while (entry != NULL) {
        if (entry->ip_addr == ip) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}



#endif