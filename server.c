/*
Copyright 2022 Sergey Lisov

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE
#include "modem.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>

struct io_buf
{
    char* data;
    size_t sz;
    size_t cap;
};

enum { BUF_MIN_SIZE = 1024 };

char* in_buf_pop_delim(struct io_buf* b, char delim)
{
    char* pos = memchr(b->data, delim, b->sz);
    if(!pos)
        return NULL;
    *pos = 0;
    char* ans = strdup(b->data);
    b->sz = (b->data+b->sz) - (pos+1);
    memmove(b->data, pos+1, b->sz);
    return ans;
}

char* in_buf_pop_at(struct io_buf* b)
{
    return in_buf_pop_delim(b, '\r');
}

char* in_buf_pop_pdu(struct io_buf* b)
{
    return in_buf_pop_delim(b, '\x1a');
}

int in_buf_populate(struct io_buf* b, int fd)
{
    if(b->sz == b->cap)
    {
        b->cap *= 2;
        if(!b->cap)
            b->cap = BUF_MIN_SIZE;
        b->data = realloc(b->data, b->cap);
    }
    ssize_t q = read(fd, b->data+b->sz, b->cap-b->sz);
    if(q <= 0)
        return -1;
    b->sz += q;
    return 0;
}

void out_buf_write(struct io_buf* b, const char* data, size_t sz)
{
    size_t newlen = b->sz + sz;
    if(newlen > b->cap)
    {
        size_t newcap = b->cap*2;
        if(newcap < newlen)
            newcap = newlen;
        if(newcap < BUF_MIN_SIZE)
            newcap = BUF_MIN_SIZE;
        b->cap = newcap;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data+b->sz, data, sz);
    b->sz = newlen;
}

int out_buf_flush(struct io_buf* b, int fd)
{
    if(!b->sz)
        return 0;
    ssize_t sz = write(fd, b->data, b->sz);
    if(sz <= 0)
        return -1;
    memcpy(b->data, b->data+sz, b->sz-sz);
    b->sz -= sz;
    return 0;
}

#define WRITE_STRING(fd, s) out_buf_write(fd, s, sizeof(s)-1)
#define WRITE_OK(fd) WRITE_STRING(fd, "\r\nOK\r\n")
#define WRITE_OK_STRING(fd, s) WRITE_STRING(fd, s "\r\nOK\r\n")
#define WRITE_ERROR(fd) WRITE_STRING(fd, "\r\nERROR\r\n")

int format_clcc_or_ecav(char** msg, const char* which)
{
    *msg = NULL;
    const char* number;
    int state;
    int direction;
    if(!modem_call_get_info(&number, &state, &direction))
        return 0;
    else
        return asprintf(msg, "\r\n%s: 1,%d,%d,0,0,\"%s\",%d\r\n",
            which,
            direction,
            state,
            number ? number : "",
            (number && number[0] == '+') ? 145 : 129
        ); //TODO: string escape
}

struct conn_status
{
    int operator_format;
    int want_pdu;
    int sms_nr;
};

void poll_at_commands(struct io_buf* in_buf, struct io_buf* out_buf, struct conn_status* st)
{
    char* cmd;
    char* pdu;
    while(
        st->want_pdu
        ? (pdu = in_buf_pop_pdu(in_buf))
        : (cmd = in_buf_pop_at(in_buf))
    )
    {
        if(st->want_pdu)
        {
            cmd = NULL;
            st->want_pdu = 0;
            size_t l = strlen(pdu);
            if(l % 2 == 0)
            {
                size_t l2 = l / 2;
                for(size_t i = 0; i < l2; i++)
                {
                    char buf[3] = {pdu[2*i], pdu[2*i+1], 0};
                    char* ep = NULL;
                    int value = strtol(buf, &ep, 16);
                    if(ep != buf + 2)
                    {
                        l2 = 0;
                        l = 1;
                    }
                    pdu[i] = value;
                }
            }
            if(l % 2 == 0)
            {
                l /= 2;
                l = modem_sms_send_pdu(pdu, l) ? 1 : 0;
            }
            if(l % 2 == 0)
            {
                char* msg = NULL;
                int len = asprintf(&msg, "\r\n+CMGS: %d\r\n\r\nOK\r\n", st->sms_nr++);
                out_buf_write(out_buf, msg, len);
                free(msg);
            }
            else
                WRITE_ERROR(out_buf);
            free(pdu);
        }
        //TODO: document what each command means
        else if(!strcmp(cmd, "AT"))
            WRITE_OK(out_buf);
        else if(!strcmp(cmd, "AT+CGMI"))
            WRITE_OK_STRING(out_buf, "\r\ngeneric\r\n");
        else if(!strcmp(cmd, "ATE0"))
            WRITE_OK(out_buf);
        else if(!strcmp(cmd, "AT+CSCS=\"GSM\""))
            WRITE_OK(out_buf);
        else if(!strcmp(cmd, "AT+CSCS?") || !strcmp(cmd, "AT+CSCS=?"))
            WRITE_OK_STRING(out_buf, "\r\n+CSCS: \"GSM\"\r\n");
        else if(!strcmp(cmd, "AT+CBC"))
            WRITE_OK_STRING(out_buf, "\r\n+CBC: 1,50\r\n");
        else if(!strncmp(cmd, "ATV", 3)) //fake ok
            WRITE_OK(out_buf);
        else if(!strncmp(cmd, "AT&C", 4))
            WRITE_OK(out_buf);
        else if(!strcmp(cmd, "ATI") || !strcmp(cmd, "AT+CGMM") || !strcmp(cmd, "AT+GMM"))
        {
            const char* idstr = modem_conf_get_name(!strcmp(cmd, "ATI"));
            char* buf = NULL;
            int len = asprintf(&buf, "\r\n%s\r\n\r\nOK\r\n", idstr);
            out_buf_write(out_buf, buf, len);
            free(buf);
        }
        else if(!strcmp(cmd, "AT+CGMR") || !strcmp(cmd, "AT+GMR"))
            WRITE_OK_STRING(out_buf, "0.1");
        else if(!strcmp(cmd, "AT+CGSN") || !strcmp(cmd, "AT+GSN"))
        {
            const char* serial = modem_conf_get_serial();
            char* buf = NULL;
            int len = asprintf(&buf, "\r\n%s\r\n\r\nOK\r\n", serial);
            out_buf_write(out_buf, buf, len);
            free(buf);
        }
        else if(!strcmp(cmd, "AT+SIMSTATE?"))
            WRITE_OK_STRING(out_buf, "\r\n+SIMSTATE: 1\r\n"); //TODO: stub
        else if(!strcmp(cmd, "AT+CPIN?"))
            WRITE_OK_STRING(out_buf, "\r\n+CPIN: READY\r\n"); //TODO: stub
        else if(!strcmp(cmd, "AT+CIMI"))
            WRITE_OK_STRING(out_buf, "\r\n246813579\r\n"); //TODO: stub
        else if(!strcmp(cmd, "AT+CREG?") || !strcmp(cmd, "AT+CGREG?"))
        {
            int state = modem_registration_get_status();
            char* buf = NULL;
            int len = asprintf(&buf, "\r\n+CREG: 1,%d\r\n\r\nOK\r\n", state);
            out_buf_write(out_buf, buf, len);
            free(buf);
        }
        else if(!strcmp(cmd, "AT+CSMS?"))
            WRITE_OK_STRING(out_buf, "\r\n+CSMS: 0,1,1,0\r\n");
        else if(!strcmp(cmd, "AT+CSMS=?"))
            WRITE_OK_STRING(out_buf, "\r\n0\r\n");
        else if(!strcmp(cmd, "AT+CSMS=0"))
            WRITE_OK_STRING(out_buf, "\r\n+CSMS: 1,1,0\r\n");
        else if(!strcmp(cmd, "AT+CMGF=?"))
            WRITE_OK_STRING(out_buf, "\r\n+CMGF: (0)\r\n");
        else if(!strcmp(cmd, "AT+CMGF=0"))
            WRITE_OK(out_buf);
        else if(!strcmp(cmd, "AT+CPMS=?"))
            WRITE_OK_STRING(out_buf, "\r\n+CPMS: (\"SM\",\"ME\"),(\"SM\",\"ME\"),(\"SM\",\"ME\")\r\n");
        else if(!strncmp(cmd, "AT+CPMS=", 8))
            WRITE_OK(out_buf);
        else if(!strcmp(cmd, "AT+CNMI=?"))
            WRITE_OK_STRING(out_buf, "\r\n+CNMI: (2),(1),(2),(1),(0)\r\n");
        else if(!strcmp(cmd, "AT+CNMI=2,1,2,1,0"))
            WRITE_OK(out_buf);
        else if(!strcmp(cmd, "AT+CMGL=4")) //TODO: stub
            WRITE_OK(out_buf);
        else if(!strncmp(cmd, "AT+CGSMS=", 9)) //???
            WRITE_OK(out_buf);
        else if(!strncmp(cmd, "AT+CFUN=", 8))
        {
            if(!modem_conf_set_online(atoi(cmd+8)))
                WRITE_OK(out_buf);
            else
                WRITE_ERROR(out_buf);
        }
        else if(!strcmp(cmd, "AT+CFUN?"))
        {
            int q = modem_conf_is_online();
            char* buf = NULL;
            int len = asprintf(&buf, "\r\n+CFUN: %d\r\n\r\nOK\r\n\r\n", q?1:0);
            out_buf_write(out_buf, buf, len);
            free(buf);
        }
        else if(!strcmp(cmd, "AT+CRSM=176,12258,0,0,10")) //TODO: stub
            WRITE_OK_STRING(out_buf, "\r\n+CRSM: 159,10,989422024754212460F6\r\n");
        else if(!strcmp(cmd, "AT+CSQ"))
        {
            int rssi = modem_registration_get_rssi();
            double error_rate = modem_registration_get_error_rate();
            if(rssi == 0)
                rssi = 99;
            else if(rssi <= -113)
                rssi = 0;
            else if(rssi >= -51)
                rssi = 31;
            else
                rssi = (rssi + 113) / 2;
            int ber;
            if(error_rate == -1)
                ber = 99;
            else if(error_rate <= 0.002)
                ber = 0;
            else if(error_rate <= 0.004)
                ber = 1;
            else if(error_rate <= 0.008)
                ber = 2;
            else if(error_rate <= 0.016)
                ber = 3;
            else if(error_rate <= 0.032)
                ber = 4;
            else if(error_rate <= 0.064)
                ber = 5;
            else if(error_rate <= 0.128)
                ber = 6;
            else
                ber = 7;
            char* msg = NULL;
            int len = asprintf(&msg, "\r\n+CSQ: %d,%d\r\n\r\nOK\r\n", rssi, ber);
            out_buf_write(out_buf, msg, len);
            free(msg);
        }
        else if(!strncmp(cmd, "AT+COPS=", 8))
        {
            char* ops = cmd + 8;
            char* colon = strchr(ops, ',');
            *colon = 0;
            int mode = atoi(ops);
            ops = colon + 1;
            colon = strchr(ops, ',');
            if(colon)
                *colon = 0;
            int format = atoi(ops);
            st->operator_format = format;
            if(mode != 3)
                WRITE_ERROR(out_buf);
            else
                WRITE_OK(out_buf);
        }
        else if(!strcmp(cmd, "AT+COPS?"))
        {
            int mode = modem_registration_get_mode();
            int format = st->operator_format;
            char* msg = NULL;
            int len;
            if(format < 2)
            {
                const char* oper = modem_registration_get_operator_name(format);
                if(mode >= 0 && oper)
                    len = asprintf(&msg, "\r\n+COPS: %d,%d,\"%s\"\r\n\r\nOK\r\n\r\n",
                        mode,
                        format,
                        oper
                    ); //TODO: string escape
                else
                    WRITE_ERROR(out_buf);
            }
            else if(format == 2)
            {
                int oper = modem_registration_get_operator_code();
                if(mode >= 0 && oper >= 0)
                    len = asprintf(&msg, "\r\n+COPS: %d,%d,%d\r\n\r\nOK\r\n\r\n",
                        mode,
                        format,
                        oper
                    );
                else
                    WRITE_ERROR(out_buf);
            }
            else
                WRITE_ERROR(out_buf);
            if(msg)
            {
                out_buf_write(out_buf, msg, len);
                free(msg);
            }
        }
        else if(!strncmp(cmd, "ATD", 3))
        {
            char* number = cmd + 3;
            char* sc = strchr(number, ';');
            if(sc)
                *sc = 0;
            if(!modem_call_dial(number))
                WRITE_OK(out_buf);
            else
                WRITE_ERROR(out_buf);
        }
        else if(!strcmp(cmd, "ATA"))
        {
            if(!modem_call_answer())
                WRITE_OK(out_buf);
            else
                WRITE_ERROR(out_buf);
        }
        else if(!strcmp(cmd, "ATH") || !strcmp(cmd, "AT+CLCC"))
        {
            char* msg;
            int len = format_clcc_or_ecav(&msg, "+CLCC");
            out_buf_write(out_buf, msg, len);
            WRITE_OK(out_buf);
            free(msg);
        }
        else if(!strcmp(cmd, "AT+CLCC=?"))
            WRITE_OK(out_buf);
        else if(!strcmp(cmd, "AT+CHUP"))
        {
            if(!modem_call_hangup())
                WRITE_OK(out_buf);
            else
                WRITE_ERROR(out_buf);
        }
        else if(!strncmp(cmd, "AT+CMGS=", 8))
        {
            out_buf_write(out_buf, "\r\n>", 3);
            st->want_pdu = 1;
        }
        else if(!strncmp(cmd, "AT+CMGR=", 8))
        {
            int idx = atoi(cmd+8);
            size_t pdu_size = 0;
            const char* pdu = NULL;
            if(modem_sms_get_incoming_pdu(idx, &pdu, &pdu_size))
            {
                char* buf = NULL;
                int len = asprintf(&buf, "\r\n+CMGR: 0,alpha,%zd\r\n", pdu_size);
                out_buf_write(out_buf, buf, len);
                free(buf);
                for(size_t i = 0; i < pdu_size; i++)
                {
                    char buf[4] = {0};
                    snprintf(buf, 3, "%02hhX", pdu[i]);
                    out_buf_write(out_buf, buf, 2);
                }
                WRITE_OK_STRING(out_buf, "\r\n");
            }
            else
                WRITE_ERROR(out_buf);
        }
        else if(!strncmp(cmd, "ATX", 3))
            WRITE_OK(out_buf);
        else if(!strncmp(cmd, "ATZ", 3))
            WRITE_OK(out_buf);
        else
            WRITE_ERROR(out_buf);
        free(cmd);
    }
}

struct client_conn
{
    struct io_buf in_buf;
    struct io_buf out_buf;
    struct conn_status status;
    int fd;
};

struct modem_fd_set
{
    fd_set set;
    int maxfd;
};

void modem_poll_fd_set(int fd, struct modem_fd_set* set)
{
    FD_SET(fd, &set->set);
    if(fd > set->maxfd)
        set->maxfd = fd;
}

void modem_poll_fd_clr(int fd, struct modem_fd_set* set)
{
    FD_CLR(fd, &set->set);
    while(set->maxfd >= 0 && !FD_ISSET(set->maxfd, &set->set))
        set->maxfd--;
}

int modem_poll_fd_isset(int fd, const struct modem_fd_set* set)
{
    return FD_ISSET(fd, &set->set);
}

struct client_conn* connections[FD_SETSIZE] = {0};
struct modem_fd_set rdsocks;
struct modem_fd_set wrsocks;

static void broadcast_notification(const char* s)
{
    size_t l = strlen(s);
    for(int i = 0; i < FD_SETSIZE; i++)
        if(connections[i])
        {
            out_buf_write(&connections[i]->out_buf, s, l);
            modem_poll_fd_set(i, &wrsocks);
        }
}

void modem_call_incoming(void)
{
    broadcast_notification("\r\n+CRING: VOICE\r\n");
}

void modem_call_remote_hangup(void)
{
    broadcast_notification("\r\nNO CARRIER\r\n");
}

void modem_sms_incoming(int index)
{
    char* msg = NULL;
    int len = asprintf(&msg, "\r\n+CMTI: \"ME\",%d\r\n", index);
    broadcast_notification(msg);
    free(msg);
}

int main(int argc, const char** argv)
{
    const char* pty_path = NULL;
    if(argc)
    {
        argc--;
        argv++;
    }
    if(argc > 1 && !strcmp(*argv, "-f"))
    {
        argc--;
        argv++;
        pty_path = *argv;
        argc--;
        argv++;
    }
    if(argc)
    {
        printf("usage: samsungipcd [-f <pty_path>]\n");
        return 1;
    }
    FD_ZERO(&rdsocks.set);
    rdsocks.maxfd = -1;
    FD_ZERO(&wrsocks.set);
    wrsocks.maxfd = -1;
    if(modem_init())
    {
        fprintf(stderr, "modem init failed\n");
        return 1;
    }
    int sock = -1;
    int pty_fd = -1;
    if(pty_path)
    {
    reopen_pty:
        pty_fd = open(pty_path, O_RDWR | O_NOCTTY);
        if(pty_fd < 0)
        {
            fprintf(stderr, "open(pty_path) failed\n");
            return 1;
        }
        struct client_conn* pty_conn = malloc(sizeof(*pty_conn));
        memset(pty_conn, 0, sizeof(*pty_conn));
        pty_conn->fd = pty_fd;
        connections[pty_fd] = pty_conn;
        modem_poll_fd_set(pty_fd, &rdsocks);
    }
    else
    {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_addr = { .s_addr = inet_addr("0.0.0.0") },
            .sin_port = htons(12345),
        };
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if(bind(sock, (void*)&addr, sizeof(addr)))
        {
            fprintf(stderr, "bind failed\n");
            return 1;
        }
        listen(sock, 1);
        modem_poll_fd_set(sock, &rdsocks);
    }
    for(;;)
    {
        modem_poll_update_fds(&rdsocks, &wrsocks);
        struct modem_fd_set rdreq = rdsocks;
        struct modem_fd_set wrreq = wrsocks;
        int nfds = rdreq.maxfd;
        if(wrsocks.maxfd > nfds)
            nfds = rdreq.maxfd;
        nfds++;
        if(select(nfds, &rdreq.set, &wrreq.set, NULL, NULL) <= 0)
        {
            fprintf(stderr, "select failed\n");
            return 1;
        }
        for(int i = 0; i < nfds; i++)
        {
            if(!modem_poll_fd_isset(i, &rdreq))
                continue;
            if(i == sock)
            {
                int newsock = accept(sock, NULL, NULL);
                if(newsock < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    fprintf(stderr, "accept failed\n");
                    return 1;
                }
                if(newsock >= FD_SETSIZE)
                {
                    fprintf(stderr, "socket fd too big\n");
                    close(newsock);
                    continue;
                }
                fcntl(newsock, F_SETFL, fcntl(newsock, F_GETFL) | O_NONBLOCK);
                modem_poll_fd_set(newsock, &rdsocks);
                struct client_conn* new_conn = malloc(sizeof(*new_conn));
                memset(new_conn, 0, sizeof(new_conn));
                new_conn->fd = newsock;
                connections[newsock] = new_conn;
            }
            else if(connections[i])
            {
                struct client_conn* conn = connections[i];
                if(in_buf_populate(&conn->in_buf, conn->fd) && errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    modem_poll_fd_clr(conn->fd, &rdsocks);
                    modem_poll_fd_clr(conn->fd, &wrsocks);
                    close(conn->fd);
                    connections[conn->fd] = NULL;
                    conn->fd = -1;
                }
                poll_at_commands(&conn->in_buf, &conn->out_buf, &conn->status);
                if(conn->fd < 0)
                {
                    free(conn->in_buf.data);
                    free(conn->out_buf.data);
                    free(conn);
                    if(i == pty_fd)
                        goto reopen_pty;
                }
                else if(conn->out_buf.sz)
                    modem_poll_fd_set(conn->fd, &wrsocks);
                else
                    modem_poll_fd_clr(conn->fd, &wrsocks);
            }
        }
        for(int i = 0; i < nfds; i++)
        {
            if(!modem_poll_fd_isset(i, &wrreq))
                continue;
            if(connections[i])
            {
                struct client_conn* conn = connections[i];
                if(out_buf_flush(&conn->out_buf, conn->fd) && errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    connections[conn->fd] = NULL;
                    modem_poll_fd_clr(conn->fd, &rdsocks);
                    modem_poll_fd_clr(conn->fd, &wrsocks);
                    close(conn->fd);
                    free(conn->in_buf.data);
                    free(conn->out_buf.data);
                    free(conn);
                }
                else if(conn->out_buf.sz)
                    modem_poll_fd_set(conn->fd, &wrsocks);
                else
                    modem_poll_fd_clr(conn->fd, &wrsocks);
            }
        }
        if(modem_poll_poll(&rdreq, &wrreq))
        {
            fprintf(stderr, "error in modem_poll_poll\n");
            return 1;
        }
    }
    return 0;
}
