/*
Copyright 2022 Sergey Lisov

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE
#include <time.h>
#include <samsung-ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <netpacket/packet.h>
#include <net/if.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "modem.h"

#define INTERFACE_NAME "rmnet0"

#ifndef PREFIX
#define PREFIX "/usr"
#endif

static int voicecall_wrpipe = -1;

static void end_voicecall(void)
{
    if(voicecall_wrpipe >= 0)
    {
        write(voicecall_wrpipe, "\n", 1);
        close(voicecall_wrpipe);
        voicecall_wrpipe = -1;
    }
}

static void start_voicecall(void)
{
    if(voicecall_wrpipe >= 0)
        return;
    int pp[2];
    if(pipe2(pp, O_CLOEXEC))
        return;
    pid_t voicecall_pid = fork();
    if(voicecall_pid == 0)
    {
        voicecall_pid = fork();
        if(!voicecall_pid)
            _exit(0);
        else if(voicecall_pid < 0)
            _exit(127);
        dup2(pp[0], 0);
        execl(PREFIX "/libexec/samsungipcd/voicecall.sh", "voicecall.sh", NULL);
        _exit(127);
    }
    else if(voicecall_pid >= 0)
        voicecall_wrpipe = pp[1];
    else
        close(pp[1]);
    close(pp[0]);
}

struct deque_elem
{
    void* ptr;
    size_t sz;
};

struct deque
{
    struct deque_elem* arr;
    size_t cap;
    size_t start_idx;
    size_t off;
    size_t sz;
};

static size_t deque_push(struct deque* d, void* ptr, size_t sz)
{
    if(d->sz == d->cap)
    {
        size_t cap2 = d->cap * 2;
        if(!cap2)
            cap2 = 16;
        struct deque_elem* new_arr = malloc(cap2 * sizeof(*new_arr));
        for(size_t i = 0; i < d->sz; i++)
            new_arr[i] = d->arr[(i + d->off) % d->sz];
        free(d->arr);
        d->arr = new_arr;
        d->cap = cap2;
        d->off = 0;
    }
    size_t idx = d->sz++;
    d->arr[(d->off+idx)%d->cap] = (struct deque_elem){ptr, sz};
    return d->start_idx+idx;
}

static int deque_get_erase(struct deque* d, size_t idx, void** ans, size_t* sz)
{
    if(idx < d->start_idx || (idx >= d->start_idx + d->sz))
        return 0;
    idx -= d->start_idx;
    size_t inner_idx = (d->off + idx) % d->cap;
    if(!(d->arr[inner_idx].sz + 1))
        return 0;
    *ans = d->arr[inner_idx].ptr;
    *sz = d->arr[inner_idx].sz;
    d->arr[inner_idx].ptr = NULL;
    d->arr[inner_idx].sz = -1;
    if(!idx)
    {
        while(d->sz && !(d->arr[inner_idx].sz + 1))
        {
            inner_idx++;
            if(inner_idx == d->cap)
                inner_idx = 0;
            d->off++;
            d->start_idx++;
            d->sz--;
        }
    }
    return 1;
}

static pthread_mutex_t global_lock;
static int thread_comm[2];

#define LOCKED0(lock) for(int lock_iter = 1; lock_iter ? (lock_iter = 0, pthread_mutex_lock(&lock), 1) : 0; pthread_mutex_unlock(&lock))
#define LOCKED() LOCKED0(global_lock)

static void log_callback(void* data, const char* msg)
{
    puts(msg);
}

static int net_status = IPC_NET_REGISTRATION_STATUS_NONE;
static int plmn_code = -1;
static int rssi = 0;

static int call_state;
static int call_direction;
static char* call_number;

static char* gprs_apn;
static char* gprs_username;
static char* gprs_password;
static char gprs_ip[4];
static char gprs_dns1[4];
static char gprs_dns2[4];
static int need_gprs;
static int have_gprs;
static int rmnet_fd;
static int packet_fd;

static void update_one_owned(char** which, char* new_value)
{
    if(*which)
        free(*which);
    *which = new_value;
}

static void update_one(char** which, const char* new_value)
{
    update_one_owned(which, new_value ? strdup(new_value) : NULL);
}

static struct deque sms_storage;
static pthread_mutex_t sms_storage_lock;

struct thread_req
{
    int type;
    void* ptr;
    size_t size;
};

enum
{
    THREAD_REQ_TYPE_SMS_SEND,
    THREAD_REQ_TYPE_SMS_INCOMING,
    THREAD_REQ_TYPE_SET_GPRS,
    THREAD_REQ_TYPE_DIAL,
    THREAD_REQ_TYPE_SET_ONLINE,
};

static uint8_t seq_cntr = 0;

static int ifdownup(void)
{
    struct ifreq ifr = { .ifr_name = INTERFACE_NAME };
    if(ioctl(rmnet_fd, SIOCGIFFLAGS, &ifr))
        return -1;
    ifr.ifr_flags &= ~(IFF_UP | IFF_RUNNING);
    if(ioctl(rmnet_fd, SIOCSIFFLAGS, &ifr))
        return -1;
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if(ioctl(rmnet_fd, SIOCSIFFLAGS, &ifr))
        return -1;
    return 0;
}

static void* sipc_thread(void* arg)
{
    fcntl(thread_comm[1], F_SETFL, fcntl(thread_comm[1], F_GETFL) | O_NONBLOCK);
    struct ipc_client* fmt = ipc_client_create(IPC_CLIENT_TYPE_FMT);
    struct ipc_client* rfs = ipc_client_create(IPC_CLIENT_TYPE_RFS);
    ipc_client_data_create(fmt);
    ipc_client_data_create(rfs);
    ipc_client_log_callback_register(fmt, log_callback, NULL);
    ipc_client_log_callback_register(rfs, log_callback, NULL);
    int ans = 0;
    if(ipc_client_boot(fmt)
    || ipc_client_open(fmt)
    || ipc_client_open(rfs))
        ans = -1;
    write(thread_comm[1], &ans, sizeof(ans));
    char* smsc_addr = NULL;
    size_t smsc_addr_len = 0;
    for(;;)
    {
        int rdfd = thread_comm[1];
        struct ipc_poll_fds pfds = {
            .fds = &rdfd,
            .count = 1,
        };
        ipc_client_poll(fmt, &pfds, NULL);
        struct thread_req* ptr;
        while(read(thread_comm[1], &ptr, sizeof(ptr)) == sizeof(ptr))
        {
            if(ptr->type == THREAD_REQ_TYPE_SMS_SEND)
            {
                uint8_t* pdu = ptr->ptr;
                size_t sz = ptr->size;
                if(ptr->size && !pdu[0])
                    sz += smsc_addr_len;
                void* blob = malloc(sizeof(struct ipc_sms_send_msg_request_header)+sz);
                struct ipc_sms_send_msg_request_header* hdr = blob;
                hdr->type = IPC_SMS_TYPE_OUTGOING;
                hdr->msg_type = IPC_SMS_MSG_TYPE_SINGLE;
                hdr->unknown = 0;
                hdr->length = sz;
                uint8_t* payload = (void*)(hdr+1);
                if(sz != ptr->size)
                {
                    memcpy(payload + (sz - ptr->size), ptr->ptr, ptr->size);
                    payload[0] = smsc_addr_len;
                    memcpy(payload+1, smsc_addr, smsc_addr_len);
                }
                else
                    memcpy(payload, ptr->ptr, ptr->size);
                printf("PDU before submit:");
                for(size_t i = 0; i < sz; i++)
                    printf(" %02hhx", payload[i]);
                printf("\n");
                ipc_client_send(fmt, seq_cntr++, IPC_SMS_SEND_MSG, IPC_TYPE_EXEC, blob, sizeof(struct ipc_sms_send_msg_request_header)+sz);
                free(blob);
                free(ptr->ptr);
            }
            else if(ptr->type == THREAD_REQ_TYPE_SET_GPRS)
            {
                int enabled = ptr->size;
                if(enabled && !need_gprs)
                {
                    need_gprs = 1;
                    struct ipc_gprs_define_pdp_context_data data;
                    char apn[sizeof(data.apn)] = {0};
                    LOCKED()
                        strncpy(apn, gprs_apn, sizeof(apn));
                    ipc_gprs_define_pdp_context_setup(&data, 1, 1, apn);
                    ipc_client_send(fmt, seq_cntr++, IPC_GPRS_DEFINE_PDP_CONTEXT, IPC_TYPE_SET, &data, sizeof(data));
                }
                if(!enabled && need_gprs)
                {
                    need_gprs = 0;
                    struct ipc_gprs_pdp_context_request_set_data data;
                    char username[sizeof(data.username)] = {0};
                    char password[sizeof(data.password)] = {0};
                    ipc_gprs_pdp_context_request_set_setup(&data, 0, 1, username, password);
                    ipc_client_send(fmt, seq_cntr++, IPC_GPRS_PDP_CONTEXT, IPC_TYPE_SET, &data, sizeof(data));
                    int old_have;
                    LOCKED()
                    {
                        old_have = have_gprs;
                        have_gprs = 0;
                    }
                    if(old_have)
                    {
                        struct thread_req* req = malloc(sizeof(*req));
                        req->type = THREAD_REQ_TYPE_SET_GPRS;
                        req->ptr = NULL;
                        req->size = 0;
                        write(thread_comm[1], &req, sizeof(req));
                    }
                }
            }
            else if(ptr->type == THREAD_REQ_TYPE_DIAL)
            {
                char* number = ptr->ptr;
                if(number)
                {
                    struct ipc_call_outgoing_data data = {
                        .type = IPC_CALL_TYPE_VOICE,
                        .identity = IPC_CALL_IDENTITY_DEFAULT,
                        .number_length = strlen(number),
                        .prefix = IPC_CALL_PREFIX_NONE,
                    };
                    strncpy(data.number, number, sizeof(data.number));
                    ipc_client_send(fmt, seq_cntr++, IPC_CALL_OUTGOING, IPC_TYPE_EXEC, &data, sizeof(data));
                    free(number);
                }
                else if(ptr->size)
                {
                    ipc_client_send(fmt, seq_cntr++, IPC_CALL_ANSWER, IPC_TYPE_EXEC, NULL, 0);
                    start_voicecall();
                }
                else
                {
                    ipc_client_send(fmt, seq_cntr++, IPC_CALL_RELEASE, IPC_TYPE_EXEC, NULL, 0);
                    end_voicecall();
                    call_state = -1;
                    call_direction = -1;
                }
            }
            else if(ptr->type == THREAD_REQ_TYPE_SET_ONLINE)
            {
                if(ptr->size == 1)
                {
                    ipc_client_send(fmt, seq_cntr++, IPC_PWR_PHONE_PWR_UP, IPC_TYPE_EXEC, NULL, 0);
                    struct ipc_pwr_phone_state_request_data data = {
                        .state = IPC_PWR_PHONE_STATE_REQUEST_NORMAL,
                    };
                    ipc_client_send(fmt, seq_cntr++, IPC_PWR_PHONE_STATE, IPC_TYPE_EXEC, &data, sizeof(data));
                }
                else
                    ipc_client_send(fmt, seq_cntr++, IPC_PWR_PHONE_PWR_OFF, IPC_TYPE_EXEC, NULL, 0);
            }
            free(ptr);
        }
        struct ipc_message msg;
        memset(&msg, 0, sizeof(msg));
        if(ipc_client_recv(fmt, &msg))
            continue;
        if(msg.command == IPC_NET_REGIST)
        {
            LOCKED()
            {
                net_status = ((struct ipc_net_regist_response_data*)msg.data)->status;
                if(net_status == IPC_NET_REGISTRATION_STATUS_NONE
                || net_status == IPC_NET_REGISTRATION_STATUS_SEARCHING)
                    rssi = 0;
                printf("net_status = %d\n", net_status);
            }
        }
        else if(msg.command == IPC_NET_SERVING_NETWORK)
        {
            char plmn[6] = {0};
            memcpy(plmn, ((struct ipc_net_serving_network_data*)msg.data)->plmn, 5);
            LOCKED()
            {
                plmn_code = atoi(plmn);
                printf("PLMN = %d\n", plmn_code);
            }
        }
        else if(msg.command == IPC_DISP_RSSI_INFO)
        {
            LOCKED()
            {
                rssi = -((struct ipc_disp_rssi_info_data*)msg.data)->rssi;
                printf("rssi = %d\n", rssi);
            }
        }
        else if(msg.command == IPC_GEN_PHONE_RES)
        {
            struct ipc_gen_phone_res_data* pld = msg.data;
            if(ipc_gen_phone_res_check(msg.data))
                printf("warning: received IPC_GEN_PHONE_RES with failure status\n");
            else if(IPC_COMMAND(pld->group, pld->index) == IPC_GPRS_DEFINE_PDP_CONTEXT && need_gprs)
            {
                struct ipc_gprs_pdp_context_request_set_data data;
                char username[sizeof(data.username)];
                char password[sizeof(data.password)];
                LOCKED()
                {
                    strncpy(username, gprs_username, sizeof(username));
                    strncpy(password, gprs_password, sizeof(password));
                }
                ipc_gprs_pdp_context_request_set_setup(&data, 1, 1, username, password);
                ipc_client_send(fmt, seq_cntr++, IPC_GPRS_PDP_CONTEXT, IPC_TYPE_SET, &data, sizeof(data));
            }
        }
        else if(msg.command == IPC_SMS_DEVICE_READY)
            ipc_client_send(fmt, seq_cntr++, IPC_SMS_SVC_CENTER_ADDR, IPC_TYPE_GET, NULL, 0);
        else if(msg.command == IPC_SMS_SVC_CENTER_ADDR)
        {
            uint8_t* smsc = (uint8_t*)msg.data;
            smsc_addr_len = smsc[0];
            smsc_addr = realloc(smsc_addr, smsc_addr_len);
            memcpy(smsc_addr, smsc+1, smsc_addr_len);
        }
        else if(msg.command == IPC_SMS_SEND_MSG) //ack
        {
            struct ipc_sms_send_msg_response_data* resp = msg.data;
            printf("IPC_SMS_SEND_MSG response, id=%hhx, ack=%hx\n", resp->id, resp->ack);
        }
        else if(msg.command == IPC_SMS_INCOMING_MSG)
        {
            struct ipc_sms_deliver_report_request_data delr;
            struct ipc_sms_incoming_msg_header* hdr = msg.data;
            delr.type = IPC_SMS_TYPE_STATUS_REPORT;
            delr.ack = IPC_SMS_ACK_NO_ERROR;
            delr.id = hdr->id;
            ipc_client_send(fmt, seq_cntr++, IPC_SMS_DELIVER_REPORT, IPC_TYPE_EXEC, &delr, sizeof(delr));
            size_t length = hdr->length;
            char* buf = malloc(length);
            memcpy(buf, hdr+1, length);
            size_t idx;
            LOCKED0(sms_storage_lock)
                idx = deque_push(&sms_storage, buf, length);
            struct thread_req* req = malloc(sizeof(*req));
            req->type = THREAD_REQ_TYPE_SMS_INCOMING;
            req->ptr = NULL;
            req->size = idx;
            write(thread_comm[1], &req, sizeof(req));
        }
        else if(msg.command == IPC_GPRS_IP_CONFIGURATION)
        {
            struct ipc_gprs_ip_configuration_data* data = msg.data;
            LOCKED()
            {
                memcpy(gprs_ip, data->ip, 4);
                memcpy(gprs_dns1, data->dns1, 4);
                memcpy(gprs_dns2, data->dns2, 4);
                have_gprs = 1;
            }
            ifdownup();
        }
        else if(msg.command == IPC_CALL_STATUS)
        {
            struct ipc_call_status_data* st = msg.data;
            printf("%d\n", st->status);
            LOCKED()
            {
                switch(st->status)
                {
                case IPC_CALL_STATUS_DIALING:
                    call_state = MODEM_CALL_STATE_DIALING;
                    start_voicecall();
                    break;
                case IPC_CALL_STATUS_CONNECTED:
                    call_state = MODEM_CALL_STATE_ACTIVE;
                    start_voicecall();
                    break;
                case IPC_CALL_STATUS_RELEASED:
                {
                    call_state = -1;
                    call_direction = -1;
                    end_voicecall();
                    struct thread_req* req = malloc(sizeof(*req));
                    req->type = THREAD_REQ_TYPE_DIAL;
                    req->ptr = NULL;
                    req->size = 0;
                    write(thread_comm[1], &req, sizeof(req));
                    break;
                }
                }
            }
        }
        else if(msg.command == IPC_CALL_INCOMING)
        {
            struct ipc_call_incoming_data* data = msg.data;
            if(data->type == IPC_CALL_TYPE_VOICE)
            {
                struct thread_req* req = malloc(sizeof(*req));
                req->type = THREAD_REQ_TYPE_DIAL;
                req->ptr = NULL;
                req->size = 1;
                write(thread_comm[1], &req, sizeof(req));
            }
        }
        free(msg.data);
    }
    return 0;
}

static int open_rmnet(void)
{
    struct ifreq ifr = {
        .ifr_name = INTERFACE_NAME,
    };
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if(sock < 0)
        return -1;
    if(setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, INTERFACE_NAME, sizeof(INTERFACE_NAME)-1))
    {
        close(sock);
        return -1;
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
    return sock;
}

static int open_packet(void)
{
    struct ifreq ifr = {
        .ifr_name = INTERFACE_NAME,
    };
    struct sockaddr_ll addr = {
        .sll_family = AF_PACKET,
        .sll_protocol = 0,
    };
    int sock = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
    if(sock < 0)
        return -1;
    if(ioctl(sock, SIOCGIFINDEX, &ifr)
    || ((addr.sll_ifindex = ifr.ifr_ifindex), bind(sock, (void*)&addr, sizeof(addr))))
    {
        close(sock);
        return -1;
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
    return sock;
}

int modem_init(void)
{
    pid_t p = fork();
    if(!p)
    {
        execl(PREFIX "/libexec/samsungipcd/init.sh", "init.sh", NULL);
        if(errno == ENOENT)
            _exit(0); //OK if the script is missing
        _exit(127);
    }
    if(p < 0)
        return -1;
    int status;
    if(waitpid(p, &status, 0) != p || status)
        return -1;
    rmnet_fd = open_rmnet();
    if(rmnet_fd < 0)
        return -1;
#if 0
    packet_fd = open_packet();
    if(packet_fd < 0)
        return -1;
#endif
    pthread_mutex_init(&global_lock, NULL);
    pthread_mutex_init(&sms_storage_lock, NULL);
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, thread_comm))
        return -1;
    pthread_t st;
    pthread_create(&st, NULL, sipc_thread, NULL);
    pthread_detach(st);
    int ans;
    if(read(thread_comm[0], &ans, sizeof(ans)) != sizeof(ans))
        ans = -1;
    fcntl(thread_comm[0], F_SETFL, fcntl(thread_comm[0], F_GETFL) | O_NONBLOCK);
    return ans;
}

int modem_poll_update_fds(struct modem_fd_set* rs, struct modem_fd_set* ws)
{
    modem_poll_fd_set(thread_comm[0], rs);
    //modem_poll_fd_set(packet_fd, rs);
    return 0;
}

static int do_poll(const struct modem_fd_set* rs, const struct modem_fd_set* ws, int target, struct thread_req** req)
{
#if 0
    if(rs && modem_poll_fd_isset(packet_fd, rs))
    {
        char buf[1500];
        ssize_t sz;
        while((sz = read(packet_fd, buf, sizeof(buf))) >= 0)
            modem_inet_incoming_packet(MODEM_PPP_PROTOCOL_IPV4, buf, sz);
    }
#endif
    if(rs && !modem_poll_fd_isset(thread_comm[0], rs))
        return 0;
    struct thread_req* cmd;
    while(read(thread_comm[0], &cmd, sizeof(cmd)) == sizeof(cmd))
    {
        if(req && cmd->type == target)
        {
            *req = cmd;
            return 1;
        }
        else if(cmd->type == THREAD_REQ_TYPE_SMS_INCOMING)
        {
            size_t idx = cmd->size;
            modem_sms_incoming(idx);
        }
        else if(cmd->type == THREAD_REQ_TYPE_DIAL)
        {
            if(cmd->size)
            {
                LOCKED()
                {
                    call_state = MODEM_CALL_STATE_INCOMING;
                    call_direction = MODEM_CALL_DIRECTION_INCOMING;
                }
                modem_call_incoming();
            }
            else
            {
                LOCKED()
                {
                    call_state = -1;
                    call_direction = -1;
                    update_one(&call_number, NULL);
                }
                modem_call_remote_hangup();
            }
        }
        else if(cmd->type == THREAD_REQ_TYPE_SET_GPRS && !cmd->size)
            modem_inet_disconnected();
        free(cmd);
    }
    return 0;
}

int modem_poll_poll(const struct modem_fd_set* rs, const struct modem_fd_set* ws)
{
    return do_poll(rs, ws, 0, NULL);
}

static struct thread_req* wait_event(int type)
{
    fcntl(thread_comm[0], F_SETFL, fcntl(thread_comm[0], F_GETFL) & ~O_NONBLOCK);
    struct thread_req* req;
    if(!do_poll(NULL, NULL, type, &req))
        req = NULL;
    fcntl(thread_comm[0], F_SETFL, fcntl(thread_comm[0], F_GETFL) | O_NONBLOCK);
    return req;
}

const char* modem_conf_get_name(int short_name) //XXX
{
    return short_name ? "libsamsung-ipc" : "samsung";
}

const char* modem_conf_get_serial(void) //XXX
{
    return "dummy";
}

static int online_status = 0;

int modem_conf_is_online(void)
{
    return online_status;
}

int modem_conf_set_online(int is_online)
{
    if(online_status && !is_online)
    {
        int old_have;
        LOCKED()
        {
            old_have = have_gprs;
            have_gprs = 0;
        }
        if(old_have)
            modem_inet_disconnected();
    }
    struct thread_req* req = malloc(sizeof(*req));
    req->type = THREAD_REQ_TYPE_SET_ONLINE;
    req->ptr = NULL;
    req->size = is_online;
    if(write(thread_comm[0], &req, sizeof(req)) != sizeof(req))
        return -1;
    online_status = is_online;
    return 0;
}

int modem_registration_get_mode(void) //XXX
{
    return MODEM_REGISTRATION_MODE_AUTOMATIC;
}

int modem_registration_get_operator_code(void)
{
    int ans;
    LOCKED() ans = plmn_code;
    return ans;
}

const char* modem_registration_get_operator_name(int short_form) //FIXME
{
    return NULL;
}

int modem_registration_get_status(void)
{
    return net_status - 1;
}

int modem_registration_get_rssi(void)
{
    int ans;
    LOCKED() ans = rssi;
    return ans;
}

double modem_registration_get_error_rate(void) //FIXME
{
    return -1;
}

//NYI

int modem_call_dial(const char* number)
{
    LOCKED()
    {
        call_state = MODEM_CALL_STATE_DIALING;
        call_direction = MODEM_CALL_DIRECTION_OUTGOING;
        update_one(&call_number, number);
    }
    struct thread_req* q = malloc(sizeof(*q));
    q->type = THREAD_REQ_TYPE_DIAL;
    q->ptr = strdup(number);
    q->size = 0;
    write(thread_comm[0], &q, sizeof(q));
    return 0;
}

int modem_call_get_info(const char** number, int* state, int* direction)
{
    int ans;
    LOCKED()
    {
        if(call_direction >= 0 && call_state >= 0)
        {
            *direction = call_direction;
            *state = call_state;
            *number = call_number;
            ans = 1;
        }
        else
        {
            call_direction = -1;
            call_state = -1;
            update_one(&call_number, NULL);
            ans = 0;
        }
    }
    return ans;
}

static void do_send_answer(int q)
{
    struct thread_req* req = malloc(sizeof(*req));
    req->type = THREAD_REQ_TYPE_DIAL;
    req->ptr = NULL;
    req->size = q;
    write(thread_comm[0], &req, sizeof(req));
}

int modem_call_answer(void)
{
    do_send_answer(1);
    return 0;
}

int modem_call_hangup(void)
{   
    do_send_answer(0);
    return 0;
}

int modem_sms_send_pdu(const char* pdu, size_t pdu_size)
{
    struct thread_req* req = malloc(sizeof(*req));
    req->type = THREAD_REQ_TYPE_SMS_SEND;
    req->ptr = malloc(pdu_size);
    memcpy(req->ptr, pdu, pdu_size);
    req->size = pdu_size;
    if(write(thread_comm[0], &req, sizeof(req)) != sizeof(req))
        return -1;
    return 0;
}

int modem_sms_get_incoming_pdu(int index, const char** pdu, size_t* pdu_size)
{
    void* pdu0 = NULL;
    int ans;
    LOCKED0(sms_storage_lock)
        ans = deque_get_erase(&sms_storage, index, &pdu0, pdu_size);
    *pdu = pdu0;
    return ans;
}

int modem_inet_set_apn(const char* apn)
{
    LOCKED() update_one(&gprs_apn, apn);
    return 0;
}

int modem_inet_set_username_password(const char* username, const char* password)
{
    LOCKED()
    {
        update_one(&gprs_username, username);
        update_one(&gprs_password, password);
    }
    return 0;
}

const char* modem_inet_get_apn(void)
{
    return gprs_apn;
}

int modem_inet_set_enabled(int is_enabled)
{
    struct thread_req* req = malloc(sizeof(*req));
    req->type = THREAD_REQ_TYPE_SET_GPRS;
    req->ptr = NULL;
    req->size = is_enabled;
    if(write(thread_comm[0], &req, sizeof(req)) != sizeof(req))
        return -1;
    return 0;
}

int modem_inet_get_ipv4_addresses(char ip[4], char dns1[4], char dns2[4])
{
    int hg;
    LOCKED()
    {
        hg = have_gprs;
        memcpy(ip, gprs_ip, 4);
        memcpy(dns1, gprs_dns1, 4);
        memcpy(dns2, gprs_dns2, 4);
    }
    if(!hg)
        return -1;
    return 0;
}

int modem_inet_send_packet(int protocol, const char* pkt, size_t sz)
{
    if(protocol == MODEM_PPP_PROTOCOL_IPV4)
    {
        if(sz < 20)
            return -1;
        struct sockaddr_in dst = { .sin_family = AF_INET };
        memcpy(&dst.sin_addr.s_addr, pkt+16, 4);
        return sendto(rmnet_fd, pkt, sz, 0, (void*)&dst, sizeof(dst)) == sz;
    }
    else
        return -1;
}
