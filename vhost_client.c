/*
 * vhost_client.c
 *
 * Copyright (c) 2014 Virtual Open Systems Sarl.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "packet.h"
#include "shm.h"
#include "stat.h"
#include "vhost_client.h"
#include "vhost_uapi.h"

#define VHOST_CLIENT_TEST_MESSAGE        (arp_request)
#define VHOST_CLIENT_TEST_MESSAGE_LEN    (sizeof(arp_request))
#define VHOST_CLIENT_PAGE_SIZE \
            ALIGN(sizeof(struct vhost_vring)+BUFFER_SIZE*VHOST_VRING_SIZE, ONEMEG)

static int _call_client(FdNode* node);
static int avail_handler_client(void* context, void* buf, size_t size);
extern int tap_fd;
unsigned char rx_buffer[1024] = "0";

VhostClient* new_vhost_client(const char* path)
{
    VhostClient* vhost_client = (VhostClient*) calloc(1, sizeof(VhostClient));
    int idx = 0;

    //TODO: handle errors here

    vhost_client->client = new_client(path);

    vhost_client->page_size = VHOST_CLIENT_PAGE_SIZE;

    // Create and attach shm to memory regions
    vhost_client->memory.nregions = VHOST_CLIENT_VRING_NUM;
    for (idx = 0; idx < vhost_client->memory.nregions; idx++) {
        void* shm = init_shm(VHOST_SOCK_NAME, vhost_client->page_size, idx);
        if (!shm) {
            fprintf(stderr, "Creatig shm %d failed\n", idx);
            free(vhost_client->client);
            free(vhost_client);
            return 0;
        }
        vhost_client->memory.regions[idx].guest_phys_addr = (uintptr_t) shm;
        vhost_client->memory.regions[idx].memory_size = vhost_client->page_size;
        vhost_client->memory.regions[idx].userspace_addr = (uintptr_t) shm;
        vhost_client->memory.regions[idx].mmap_offset = 0;
    }

    // init vrings on the shm (through memory regions)
    if (vring_table_from_memory_region(vhost_client->vring_table_shm, VHOST_CLIENT_VRING_NUM,
            &vhost_client->memory) != 0) {
        // TODO: handle error here
    }

    return vhost_client;
}

int init_vhost_client(VhostClient* vhost_client)
{
    int idx;
	int fd;

    if (!vhost_client->client)
        return -1;

#ifndef VHOST_KERNEL
    if (init_client(vhost_client->client) != 0)
        return -1;

    vhost_ioctl(vhost_client->client, VHOST_USER_SET_OWNER, 0);
    vhost_ioctl(vhost_client->client, VHOST_USER_GET_FEATURES, &vhost_client->features);
    vhost_ioctl(vhost_client->client, VHOST_USER_SET_MEM_TABLE, &vhost_client->memory);
#else
	if((fd = init_tap()) == -1) {
		printf("open tap failed!\n");
		return -1;
	}
	vhost_client->client->sock = fd;
	
	if (init_kernel_client(vhost_client->client) != 0) {
		printf("open tap failed!\n");
		return -1;
	}

    vhost_kernel_ioctl(vhost_client->client, VHOST_SET_OWNER, 0);
	vhost_kernel_ioctl(vhost_client->client, VHOST_GET_FEATURES, &vhost_client->features);
	vhost_kernel_ioctl(vhost_client->client, VHOST_SET_MEM_TABLE, &vhost_client->vhost_memory);
#endif

    // push the vring table info to the vhost kernel.
    if (set_host_vring_table(vhost_client->vring_table_shm, VHOST_CLIENT_VRING_NUM,
            vhost_client->client) != 0) {
        // TODO: handle error here
    }

    // VringTable initalization
    vhost_client->vring_table.handler.context = (void*) vhost_client;
    vhost_client->vring_table.handler.avail_handler = avail_handler_client;
    vhost_client->vring_table.handler.map_handler = 0;

    for (idx = 0; idx < VHOST_CLIENT_VRING_NUM; idx++) {
        vhost_client->vring_table.vring[idx].kickfd = vhost_client->vring_table_shm[idx]->kickfd;
        vhost_client->vring_table.vring[idx].callfd = vhost_client->vring_table_shm[idx]->callfd;
        vhost_client->vring_table.vring[idx].desc = vhost_client->vring_table_shm[idx]->desc;
        vhost_client->vring_table.vring[idx].avail = &vhost_client->vring_table_shm[idx]->avail;
        vhost_client->vring_table.vring[idx].used = &vhost_client->vring_table_shm[idx]->used;
        vhost_client->vring_table.vring[idx].num = VHOST_VRING_SIZE;
        vhost_client->vring_table.vring[idx].last_avail_idx = 0;
        vhost_client->vring_table.vring[idx].last_used_idx = 0;
    }

	struct vhost_vring_file file;
	file.index = VHOST_CLIENT_VRING_IDX_RX;
	file.fd = fd;
	vhost_kernel_ioctl(vhost_client->client, VHOST_NET_SET_BACKEND, &file);
	file.index = VHOST_CLIENT_VRING_IDX_TX;
	file.fd = fd;
	vhost_kernel_ioctl(vhost_client->client, VHOST_NET_SET_BACKEND, &file);

    // Add handler for RX kickfd
    add_fd_list(&vhost_client->client->fd_list, FD_READ,
            vhost_client->vring_table.vring[VHOST_CLIENT_VRING_IDX_RX].callfd,
            (void*) vhost_client, _call_client);

    return 0;
}

int end_vhost_client(VhostClient* vhost_client)
{
    int i = 0;

    vhost_ioctl(vhost_client->client, VHOST_USER_RESET_OWNER, 0);

    // free all shared memory mappings
    for (i = 0; i<vhost_client->memory.nregions; i++)
    {
        end_shm(vhost_client->client->sock_path,
                (void*) (uintptr_t) vhost_client->memory.regions[i].guest_phys_addr,
                vhost_client->memory.regions[i].memory_size, i);
    }

    end_client(vhost_client->client);

    //TODO: should this be here?
    free(vhost_client->client);
    vhost_client->client = 0;

    return 0;
}

static int send_packet(VhostClient* vhost_client, void* p, size_t size)
{
    int r = 0;
    uint32_t tx_idx = VHOST_CLIENT_VRING_IDX_TX;

	printf("TX:addr:%p,len:%d\n", p, size);
    dump_buffer(p, size);
    r = put_vring(&vhost_client->vring_table, tx_idx, p, size);

    if (r != 0)
        return -1;

    return kick(&vhost_client->vring_table, tx_idx);
}

static int avail_handler_client(void* context, void* buf, size_t size)
{
    // consume the packet
#if 1
    dump_buffer(buf, size);
#endif

    return 0;
}

//change to call_client.
static int _call_client(FdNode* node)
{
    VhostClient* vhost_client = (VhostClient*) node->context;
    int callfd = node->fd;
    ssize_t r;
    uint64_t call_it = 0;

    r = read(callfd, &call_it, sizeof(call_it));

    if (r < 0) {
        perror("recv call");
    } else if (r == 0) {
        fprintf(stdout, "call fd closed\n");
        del_fd_list(&vhost_client->client->fd_list, FD_READ, callfd);
    } else {
        int idx = VHOST_CLIENT_VRING_IDX_RX;
#if 1
        fprintf(stdout, "Got called %ld\n", call_it);
#endif

        process_avail_vring(&vhost_client->vring_table, idx);
    }

    return 0;
}

static int poll_client(void* context)
{
    VhostClient* vhost_client = (VhostClient*) context;
    uint32_t tx_idx = VHOST_CLIENT_VRING_IDX_TX;

    if (process_used_vring(&vhost_client->vring_table, tx_idx) != 0) {
        fprintf(stderr, "handle_used_vring failed.\n");
        return -1;
    }

    if (send_packet(vhost_client, (void*) VHOST_CLIENT_TEST_MESSAGE,
            VHOST_CLIENT_TEST_MESSAGE_LEN) != 0) {
        fprintf(stdout, "Send packet failed.\n");
        return -1;
    }
	arp_request[10] += 1;

	usleep(900000);
    uint32_t rx_idx = VHOST_CLIENT_VRING_IDX_RX;
    if (process_used_vring(&vhost_client->vring_table, rx_idx) != 0) {
        fprintf(stderr, "handle_used_vring failed.\n");
        return -1;
    }
#if 1
    int r = put_vring(&vhost_client->vring_table, rx_idx, rx_buffer, 1024);
    if (r != 0) {
        return -1;
	}

	//printf("RX:%d \n", r);
    //dump_buffer(rx_buffer, r);
#endif

    update_stat(&vhost_client->stat,1);
    print_stat(&vhost_client->stat);

    return 0;
}

static AppHandlers vhost_client_handlers =
{
        .context = 0,
        .in_handler = 0,
        .poll_handler = poll_client
};

int run_vhost_client(VhostClient* vhost_client)
{
    if (init_vhost_client(vhost_client) != 0) {
        return -1;
	}
#if 1
    int r = put_vring(&vhost_client->vring_table, VHOST_CLIENT_VRING_IDX_RX, rx_buffer, 1024);
    if (r != 0)
        return -1;
#endif
#if 1
    vhost_client_handlers.context = vhost_client;
    set_handler_client(vhost_client->client, &vhost_client_handlers);

    start_stat(&vhost_client->stat);
    loop_client(vhost_client->client);
    stop_stat(&vhost_client->stat);

    end_vhost_client(vhost_client);
#endif

    return 0;
}
