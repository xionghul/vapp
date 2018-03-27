/*
 * client.c
 *
 * Copyright (c) 2014 Virtual Open Systems Sarl.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <linux/if_ether.h>
#include <linux/if_tun.h>
#include <net/if.h> 
#include <linux/if.h>

#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/unistd.h>

#include "client.h"
#include "shm.h"
#include "vhost_user.h"
#include "vhost_uapi.h"

extern int app_running;
int tap_fd = -1;

Client* new_client(const char* path)
{
    Client* client = (Client*) calloc(1, sizeof(Client));

    //TODO: handle errors here

    strncpy(client->sock_path, path ? path : VHOST_SOCK_NAME, PATH_MAX);
    client->status = INSTANCE_CREATED;

    return client;
}

int init_client(Client* client)
{
    struct sockaddr_un un;
    size_t len;

    if (client->status != INSTANCE_CREATED)
        return 0;

    // Create the socket
    if ((client->sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        return -1;
    }

    un.sun_family = AF_UNIX;
    strcpy(un.sun_path, client->sock_path);

    len = sizeof(un.sun_family) + strlen(client->sock_path);

    // Connect
    if (connect(client->sock, (struct sockaddr *) &un, len) == -1) {
        perror("connect");
        return -1;
    }

    client->status = INSTANCE_INITIALIZED;

    init_fd_list(&client->fd_list, FD_LIST_SELECT_POLL);

    return 0;
}

int end_client(Client* client)
{
    if (client->status != INSTANCE_INITIALIZED)
        return 0;

    // Close the socket
    close(client->sock);

    client->status = INSTANCE_END;

    return 0;
}

int vhost_ioctl(Client* client, VhostUserRequest request, ...)
{
    void *arg;
    va_list ap;

    VhostUserMsg msg;
    struct vhost_vring_file *file = 0;
    int need_reply = 0;
    int fds[VHOST_MEMORY_MAX_NREGIONS];
    size_t fd_num = 0;

    va_start(ap, request);
    arg = va_arg(ap, void *);

    memset(&msg,0,sizeof(VhostUserMsg));
    msg.request = request;
    msg.flags &= ~VHOST_USER_VERSION_MASK;
    msg.flags |= VHOST_USER_VERSION;
    msg.size = 0;

    switch (request) {
    case VHOST_USER_GET_FEATURES:
    case VHOST_USER_GET_VRING_BASE:
        need_reply = 1;
        break;

    case VHOST_USER_SET_FEATURES:
    case VHOST_USER_SET_LOG_BASE:
        msg.u64 = *((uint64_t*) arg);
        msg.size = MEMB_SIZE(VhostUserMsg,u64);
        break;

    case VHOST_USER_SET_OWNER:
    case VHOST_USER_RESET_OWNER:
        break;

    case VHOST_USER_SET_MEM_TABLE:
        memcpy(&msg.memory, arg, sizeof(VhostUserMemory));
        msg.size = MEMB_SIZE(VhostUserMemory,nregions);
        msg.size += MEMB_SIZE(VhostUserMemory,padding);
        for(;fd_num < msg.memory.nregions;fd_num++) {
            fds[fd_num] = shm_fds[fd_num];
            msg.size += sizeof(VhostUserMemoryRegion);
        }
        break;

    case VHOST_USER_SET_LOG_FD:
        fds[fd_num++] = *((int*) arg);
        break;

    case VHOST_USER_SET_VRING_NUM:
    case VHOST_USER_SET_VRING_BASE:
        memcpy(&msg.state, arg, MEMB_SIZE(VhostUserMsg,state));
        msg.size = MEMB_SIZE(VhostUserMsg,state);
        break;

    case VHOST_USER_SET_VRING_ADDR:
        memcpy(&msg.addr, arg, MEMB_SIZE(VhostUserMsg,addr));
        msg.size = MEMB_SIZE(VhostUserMsg,addr);
        break;

    case VHOST_USER_SET_VRING_KICK:
    case VHOST_USER_SET_VRING_CALL:
    case VHOST_USER_SET_VRING_ERR:
        file = arg;
        msg.u64 = file->index;
        msg.size = MEMB_SIZE(VhostUserMsg,u64);
        if (file->fd > 0) {
            fds[fd_num++] = file->fd;
        }
        break;

    case VHOST_USER_NONE:
        break;
    default:
        return -1;
    }


    if (vhost_user_send_fds(client->sock, &msg, fds, fd_num) < 0) {
        fprintf(stderr, "ioctl send\n");
        return -1;
    }

    if (need_reply) {

        msg.request = VHOST_USER_NONE;
        msg.flags = 0;

        if (vhost_user_recv_fds(client->sock, &msg, fds, &fd_num) < 0) {
            fprintf(stderr, "ioctl rcv failed\n");
            return -1;
        }

        assert((msg.request == request));
        assert(((msg.flags & VHOST_USER_VERSION_MASK) == VHOST_USER_VERSION));

        switch (request) {
        case VHOST_USER_GET_FEATURES:
            *((uint64_t*) arg) = msg.u64;
            break;
        case VHOST_USER_GET_VRING_BASE:
            memcpy(arg, &msg.state, sizeof(struct vhost_vring_state));
            break;
        default:
            return -1;
        }

    }

    va_end(ap);

    return 0;
}

int init_kernel_client(Client* client)
{
    if (client->status != INSTANCE_CREATED)
        return 0;

    // open /dev/vhost-net.
	client->sock = open("/dev/vhost-net", O_RDWR);

	if (client->sock < 0) {
		printf("couldn't open vhostfd!\n");
		return -1;
	}

    client->status = INSTANCE_INITIALIZED;

    init_fd_list(&client->fd_list, FD_LIST_SELECT_POLL);
    return 0;
}

#define PATH_NET_TUN "/dev/net/tun"
int init_tap()
{
    int fd;
    struct ifreq ifr;
    unsigned int features;
    fd = open(PATH_NET_TUN, O_RDWR);
    if (fd < 0) {
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (ioctl(fd, TUNGETFEATURES, &features) == -1) {
        features = 0;
    }
	strcpy(ifr.ifr_name, "tap0");

    if(ioctl(fd, TUNSETIFF, (void *) &ifr) == -1)
		return -1;

	fcntl(fd, F_SETFL, O_NONBLOCK);

	tap_fd = fd;

	return fd;
}


int vhost_kernel_call(int fd, unsigned long int request,
                             void *arg)
{
    return ioctl(fd, request, arg);
}

int vhost_kernel_ioctl(Client* client, unsigned long int request, ...)
{
    void *arg;
    va_list ap;

#if 0
    struct vhost_vring_file *file = 0;
    int need_reply = 0;
    int fds[VHOST_MEMORY_MAX_NREGIONS];
    size_t fd_num = 0;
#endif

    va_start(ap, request);
    arg = va_arg(ap, void *);

    switch (request) {
    case VHOST_SET_OWNER:
    case VHOST_RESET_OWNER:
		vhost_kernel_call(client->sock, request, NULL);
        break;
    case VHOST_GET_FEATURES:
		vhost_kernel_call(client->sock, request, arg);
        break;

    case VHOST_SET_MEM_TABLE:
		vhost_kernel_call(client->sock, request, arg);
        break;

    case VHOST_SET_VRING_NUM:
    case VHOST_SET_VRING_BASE:
    case VHOST_SET_VRING_KICK:
    case VHOST_SET_VRING_CALL:
    case VHOST_SET_VRING_ADDR:
    case VHOST_NET_SET_BACKEND:
		vhost_kernel_call(client->sock, request, arg);
        break;

#if 0
    case VHOST_GET_VRING_BASE:
        need_reply = 1;
        break;

    case VHOST_SET_FEATURES:
    case VHOST__SET_LOG_BASE:
        msg.u64 = *((uint64_t*) arg);
        msg.size = MEMB_SIZE(VhostKernelMsg,u64);
        break;

    case VHOST_SET_LOG_FD:
        fds[fd_num++] = *((int*) arg);
        break;

    case VHOST_SET_VRING_ERR:
        file = arg;
        msg.u64 = file->index;
        msg.size = MEMB_SIZE(VhostKernelMsg,u64);
        if (file->fd > 0) {
            fds[fd_num++] = file->fd;
        }
        break;
#endif
    default:
		printf("request %lx not handled!\n", request);
        return -1;
    }


#if 0
    if (vhost_user_send_fds(client->sock, &msg, fds, fd_num) < 0) {
        fprintf(stderr, "ioctl send\n");
        return -1;
    }

    if (need_reply) {

        msg.request = VHOST_USER_NONE;
        msg.flags = 0;

        if (vhost_user_recv_fds(client->sock, &msg, fds, &fd_num) < 0) {
            fprintf(stderr, "ioctl rcv failed\n");
            return -1;
        }

        assert((msg.request == request));
        assert(((msg.flags & VHOST_USER_VERSION_MASK) == VHOST_USER_VERSION));

        switch (request) {
        case VHOST_USER_GET_FEATURES:
            *((uint64_t*) arg) = msg.u64;
            break;
        case VHOST_USER_GET_VRING_BASE:
            memcpy(arg, &msg.state, sizeof(struct vhost_vring_state));
            break;
        default:
            return -1;
        }

    }
#endif

    va_end(ap);

    return 0;
}

int loop_client(Client* client)
{
    // externally modified
    app_running = 1;

    while (app_running) {
        traverse_fd_list(&client->fd_list);
        if (client->handlers.poll_handler) {
            client->handlers.poll_handler(client->handlers.context);
        }
#ifdef DUMP_PACKETS
        sleep(1);
#endif
    }

    return 0;
}

int set_handler_client(Client* client, AppHandlers* handlers)
{
    memcpy(&client->handlers,handlers, sizeof(AppHandlers));

    return 0;
}
