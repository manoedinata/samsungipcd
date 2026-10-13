/*
Copyright 2022-2024 Sonya Sireneva

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <pthread.h>
#include <sys/socket.h>
#include <poll.h>
#include <pppd/pppd.h>

#define PPP_HOOK
#include "buffers.h"

static int dev_ppp_fd = -1;
static int tty_fd = -1;
static struct termios original;
static int tcsetattr_done = 0;
static struct io_buf out_buf;
static pthread_mutex_t out_buf_mtx = PTHREAD_MUTEX_INITIALIZER;

static void* ppp_thread(void* o)
{
    int ppp_master = (intptr_t)o;
    fcntl(ppp_master, F_SETFL, fcntl(ppp_master, F_GETFL) | O_NONBLOCK);
    char* ppp_packet = 0;
    size_t sz = 0;
    struct io_buf in_buf = {};
    pthread_mutex_lock(&out_buf_mtx);
    for(;;)
    {
        struct pollfd polls[2] = {
            {.fd = ppp_master, .events = POLLIN},
            {.fd = tty_fd, .events = POLLIN},
        };
        if(ppp_packet)
            polls[0].events |= POLLOUT;
        if(out_buf.sz)
            polls[1].events |= POLLOUT;
        pthread_mutex_unlock(&out_buf_mtx);
        poll(polls, 2, -1);
        pthread_mutex_lock(&out_buf_mtx);
        //discard outgoing zeroes
        char c;
        read(ppp_master, &c, 1);
        //do io on the tty
        out_buf_flush(&out_buf, tty_fd);
        in_buf_populate(&in_buf, tty_fd);
        if(ppp_packet)
        {
            //if we already have an incoming ppp packet, send it. strip ppp header and crc checksum
            if(write(ppp_master, ppp_packet+2, sz-4) > 0)
            {
                free(ppp_packet);
                ppp_packet = 0;
                sz = 0;
            }
        }
        else
        {
            //otherwise try to parse one from in_buf
            while(!ppp_packet)
            {
                ppp_packet = in_buf_pop_ppp(&in_buf, &sz, 0);
                if(!ppp_packet)
                    break;
                if(unescape_ppp(ppp_packet, &sz) || sz <= 4 || !ppp_crc_valid(ppp_packet, sz) || (uint8_t)ppp_packet[0] != 0xff || (uint8_t)ppp_packet[1] != 3)
                {
                    free(ppp_packet);
                    ppp_packet = 0;
                    sz = 0;
                }
            }
#ifdef PPP_DUMP
            if(ppp_packet)
            {
                printf("PPP packet:");
                for(size_t i = 0; i < sz; i++)
                    printf(" %02hhx", ppp_packet[i]);
                printf("\n");
            }
#endif
        }
    }
    return 0;
}

static int tty_connect(void)
{
    if(dev_ppp_fd >= 0)
    {
        unlink("/dev/ppp");
        close(dev_ppp_fd);
    }
    //Not ppp_devnam: that's only ever written by tty.c's own
    //connect_tty(), which this replaces - it stays empty here. devnam
    //(pppd.h) is the one setdevname() actually populates while
    //parsing the device name option, before this channel is even
    //selected.
    int fd = open(devnam, O_RDWR);
    if(fd < 0)
    {
        error("Failed to open %s: %s", devnam, strerror(errno));
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

//Not named tty_disestablish_ppp: pppd's own sys-linux.c already
//exports a *real*, different function with that exact name (used by
//its built-in tty.c channel). dlopen(RTLD_GLOBAL) lets the main
//executable's own symbol shadow a same-named one in a later-loaded
//shared object, so `.disestablish_ppp = tty_disestablish_ppp` below
//was silently binding to pppd's own serial-line implementation
//instead of this one - confirmed via `nm -D /usr/sbin/pppd` showing
//it as a real global, and by the resulting "Couldn't set tty to PPP
//discipline: Invalid argument" (pppd's own tty_establish_ppp trying a
//TIOCSETD ioctl on what's actually a plain pty, which can't take the
//PPP line discipline the way a real serial port can).
static void samsungipcd_disestablish_ppp(int fd)
{
    if(!tcsetattr_done)
        return;
    if(tcsetattr(fd, TCSAFLUSH, &original))
        warn("Failed to restore tty original attributes: %s", strerror(errno));
    tcsetattr_done = 0;
}

static void tty_send(unsigned char* p, int len)
{
    static char* tmp_buf = 0;
    static size_t tmp_cap = 0;
    pthread_mutex_lock(&out_buf_mtx);
    size_t sz = len + 2;
    if(tmp_cap < sz)
        tmp_buf = realloc(tmp_buf, tmp_cap = sz);
    memcpy(tmp_buf, p, len);
    ppp_crc_fix(tmp_buf, sz);
    tmp_buf = escape_ppp(tmp_buf, &sz, &tmp_cap);
    out_buf_write(&out_buf, tmp_buf, sz);
    pthread_mutex_unlock(&out_buf_mtx);
    for(int i = 0; i < len; i++)
        p[i] = 0;
}

extern int ppp_dev_fd; //exported by accident??

//Same reasoning as samsungipcd_disestablish_ppp above: pppd's own
//sys-linux.c already exports a real, different "tty_establish_ppp".
static int samsungipcd_establish_ppp(int fd)
{
    struct termios tios;
    if(tcgetattr(fd, &tios))
    {
        error("tcgetattr failed: %s", strerror(errno));
        return -1;
    }
    original = tios;
    cfmakeraw(&tios);
    if(tcsetattr(fd, TCSAFLUSH, &tios))
    {
        error("tcsetattr failed: %s", strerror(errno));
        return -1;
    }
    int pipe[2];
    if(socketpair(AF_UNIX, SOCK_DGRAM, 0, pipe))
    {
        error("socketpair failed: %s", strerror(errno));
        samsungipcd_disestablish_ppp(fd);
        return -1;
    }
    pthread_t pth;
    if(pthread_create(&pth, 0, ppp_thread, (void*)(intptr_t)pipe[1]))
    {
        error("Failed to start PPP thread: %s", strerror(errno));
        close(pipe[0]);
        close(pipe[1]);
        samsungipcd_disestablish_ppp(fd);
        return -1;
    }
    ppp_dev_fd = pipe[0];
    fcntl(ppp_dev_fd, F_SETFL, fcntl(ppp_dev_fd, F_GETFL) | O_NONBLOCK);
    snoop_send_hook = tty_send;
    tty_fd = fd;
    return pipe[0];
}

//pppd's find_option() (options.c) falls through to `the_channel->options`
//for any option name it doesn't recognize in general_options/auth_options,
//and dereferences it unconditionally (`opt->name` with no NULL check) -
//leaving this NULL segfaults pppd the moment it parses *any* option that
//isn't in those two tables (which is most of them: all of ipcp.c/lcp.c's
//options route through this same fallthrough before ever reaching the
//per-protocol tables). An empty, merely-terminated table is enough since
//this plugin doesn't need any options of its own.
static option_t no_channel_options[] = {
    { NULL }
};

static struct channel tty_channel = {
    .options = no_channel_options,
    .connect = tty_connect,
    .establish_ppp = samsungipcd_establish_ppp,
    .disestablish_ppp = samsungipcd_disestablish_ppp,
};

void plugin_init(void)
{
    dev_ppp_fd = open("/dev/ppp", O_WRONLY | O_CREAT | O_EXCL, 0000);
    the_channel = &tty_channel;
}

const char pppd_version[] = PPPD_VERSION;
