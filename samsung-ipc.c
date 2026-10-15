/*
Copyright 2022-2025 Sonya Sireneva

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

static char* imei;

static int net_status = IPC_NET_REGISTRATION_STATUS_NONE;
static int net_tech = IPC_NET_ACCESS_TECHNOLOGY_UNKNOWN;
static int net_lac = -1;
static int net_cid = -1;
static int net_tac = -1;
static int plmn_code = -1;
static int rssi = 0;

static int call_state;
static int call_id;
static int call_direction;
static char* call_number;

static char* gprs_apn;
static char* gprs_username;
static char* gprs_password;
static char gprs_ip[4];
static char gprs_dns1[4];
static char gprs_dns2[4];
static char ipv6_link_id[8];
static int need_gprs;
static int device_ready; //set once the modem reported SMS_DEVICE_READY and got its service domain
static int gprs_enable_pending; //data was requested before that
static int have_gprs;
static int have_ipv6_link;
static int rmnet_fd;
static int packet_fd;
static int rmnet_fd_v6;
static int packet_fd_v6;
static int ussd_active;

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
    THREAD_REQ_TYPE_REGIST,
    THREAD_REQ_TYPE_DTMF_TONE,
    THREAD_REQ_TYPE_IMEI,
    THREAD_REQ_TYPE_USSD,
#ifdef DEBUG_COMMAND
    THREAD_REQ_TYPE_DEBUG,
#endif
};

static uint8_t seq_cntr = 0;

static int ifdownup(void)
{
    int fd = open("/proc/sys/net/ipv6/conf/" INTERFACE_NAME "/autoconf", O_WRONLY);
    if(fd < 0)
        return -1;
    if(write(fd, "0\n", 2) != 2)
    {
        close(fd);
        return -1;
    }
    close(fd);
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

static const int channel_volumes[4][2] = {
    {IPC_SND_VOLUME_TYPE_VOICE, 0},
    {IPC_SND_VOLUME_TYPE_SPEAKER, 127},
    {IPC_SND_VOLUME_TYPE_HEADSET, 0},
    {IPC_SND_VOLUME_TYPE_BTVOICE, 0},
};

//XXX: length is 8-bit in many requests. we should probably check for overflow when creating them
static void* sipc_thread(void* arg)
{
    fcntl(thread_comm[1], F_SETFL, fcntl(thread_comm[1], F_GETFL) | O_NONBLOCK);
    struct ipc_client* fmt = ipc_client_create(IPC_CLIENT_TYPE_FMT);
    struct ipc_client* rfs = ipc_client_create(IPC_CLIENT_TYPE_RFS);
    ipc_client_data_create(fmt);
    ipc_client_data_create(rfs);
#ifndef API_DEBUG //avoid polluting the output
    ipc_client_log_callback_register(fmt, log_callback, NULL);
    ipc_client_log_callback_register(rfs, log_callback, NULL);
#endif
    int ans = 0;
    if(ipc_client_boot(fmt)
    || ipc_client_open(fmt)
    || ipc_client_open(rfs))
        ans = -1;
    write(thread_comm[1], &ans, sizeof(ans));
    char* smsc_addr = NULL;
    size_t smsc_addr_len = 0;
    int mode_probe_bit = 0;
    int mode_current = 0;
    int modem_supports_ipv6 = 0;
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
                if(enabled && !device_ready)
                {
                    //too early: the service domain has not been set up yet, redo this once the modem is ready
                    gprs_enable_pending = 1;
                    enabled = 0;
                }
                else if(!enabled)
                    gprs_enable_pending = 0;
                if(enabled)
                {
                    //also when already enabled: a redial after a failed activation (e.g. the modem was still
                    //re-registering after the service domain change) has to restart the whole sequence
                    need_gprs = 1;
                    struct ipc_gprs_define_pdp_context_data data;
                    char apn[sizeof(data.apn)] = {0};
                    LOCKED()
                    {
                        strncpy(apn, gprs_apn, sizeof(apn));
                        //assume the best once again, in case the previous probe request failed due to some other error
                    }
                    modem_supports_ipv6 = 1;
                    //Shannon (LTE) modems only attach to the packet domain with an LTE attach profile
                    //in place, and without it they stay in "not ready" for the PDP activation below
                    struct ipc_gprs_lte_attach_apn_info_data attach;
                    if(!ipc_gprs_lte_attach_apn_info_setup(&attach, 1, apn, NULL, NULL))
                        ipc_client_send(fmt, seq_cntr++, IPC_GPRS_LTE_ATTACH_APN_INFO, IPC_TYPE_SET, &attach, sizeof(attach));
                    ipc_gprs_define_pdp_context_setup(&data, 1, 1, apn);
                    //if the modem doesn't support ipv6, it will probably respond with an error
                    //we'll then retry the request without this assignment to get ipv4 connectivity
                    data.magic = 6;
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
                        .prefix = (*number == '+') ? IPC_CALL_PREFIX_INTL : IPC_CALL_PREFIX_NONE,
                    };
                    strncpy(data.number, number, sizeof(data.number));
                    ipc_client_send(fmt, seq_cntr++, IPC_CALL_OUTGOING, IPC_TYPE_EXEC, &data, sizeof(data));
                    free(number);
                    for(int i = 0; i < 4; i++)
                    {
                        struct ipc_snd_spkr_volume_ctrl_data ctrl = {
                            .type = channel_volumes[i][0],
                            .volume= channel_volumes[i][1],
                        };
                        ipc_client_send(fmt, seq_cntr++, IPC_SND_SPKR_VOLUME_CTRL, IPC_TYPE_SET, &ctrl, sizeof(ctrl));
                    }
                }
                else if(ptr->size)
                {
                    ipc_client_send(fmt, seq_cntr++, IPC_CALL_ANSWER, IPC_TYPE_EXEC, NULL, 0);
                    for(int i = 0; i < 4; i++)
                    {
                        struct ipc_snd_spkr_volume_ctrl_data ctrl = {
                            .type = channel_volumes[i][0],
                            .volume= channel_volumes[i][1],
                        };
                        ipc_client_send(fmt, seq_cntr++, IPC_SND_SPKR_VOLUME_CTRL, IPC_TYPE_SET, &ctrl, sizeof(ctrl));
                    }
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
            else if(ptr->type == THREAD_REQ_TYPE_DTMF_TONE)
                ipc_client_send(fmt, seq_cntr++, IPC_CALL_CONT_DTMF, IPC_TYPE_SET, ptr->ptr, ptr->size);
            else if(ptr->type == THREAD_REQ_TYPE_USSD)
            {
                struct ipc_ss_ussd_header* data = malloc(sizeof(*data) + ptr->size - 1);
                data->state = ((char*)ptr->ptr)[0];
                data->dcs = ((char*)ptr->ptr)[1];
                data->length = ptr->size - 2;
                memcpy(data + 1, (char*)ptr->ptr + 2, ptr->size - 2);
                ipc_client_send(fmt, seq_cntr++, IPC_SS_USSD, IPC_TYPE_EXEC, data, sizeof(*data) + data->length);
                free(data);
            }
#ifdef DEBUG_COMMAND
            else if(ptr->type == THREAD_REQ_TYPE_DEBUG)
            {
                int k = (intptr_t)ptr->ptr;
                int v = ptr->size;
                switch(k)
                {
                    //...
                case 0:
                {
                    struct ipc_net_mode_sel_data data = {
                        .mode_sel = v,
                    };
                    ipc_client_send(fmt, seq_cntr++, IPC_NET_MODE_SEL, IPC_TYPE_SET, &data, sizeof(data));
                }
                }
            }
#endif
            free(ptr);
        }
        struct ipc_message msg;
        memset(&msg, 0, sizeof(msg));
        if(ipc_client_recv(fmt, &msg))
            continue;
#define GET(x) ((char*)(1+&(x)) <= (char*)msg.data + msg.size ? (x) : 0)
#define PTRCHECK(x, sz, fallback) ((char*)(x) + (size_t)(sz) <= (char*)msg.data + msg.size ? (x) : (void*)(fallback))
        if(msg.command == IPC_NET_REGIST)
        {
            LOCKED()
            {
                struct ipc_net_regist_response_data* data = msg.data;
                net_status = GET(data->status);
                net_tech = GET(data->act);
                net_lac = GET(data->lac);
                net_cid = GET(data->cid);
                if(msg.size >= 13)
                    net_tac = *(uint16_t*)(msg.data+11);
                if(net_status == IPC_NET_REGISTRATION_STATUS_NONE
                || net_status == IPC_NET_REGISTRATION_STATUS_SEARCHING)
                {
                    int old_have = have_gprs;
                    have_gprs = 0;
                    if(old_have)
                    {
                        struct thread_req* req = malloc(sizeof(*req));
                        req->type = THREAD_REQ_TYPE_SET_GPRS;
                        req->ptr = NULL;
                        req->size = 0;
                        write(thread_comm[1], &req, sizeof(req));
                    }
                    rssi = 0;
                    net_lac = -1;
                    net_cid = -1;
                    net_tac = -1;
                }
                printf("net_status = %d, act = %d, lac = %d, cid = %d, tac = %d\n", net_status, net_tech, net_lac, net_cid, net_tac);
            }
            struct thread_req* req = malloc(sizeof(*req));
            req->type = THREAD_REQ_TYPE_REGIST;
            req->ptr = NULL;
            req->size = 0;
            write(thread_comm[1], &req, sizeof(req));
        }
        else if(msg.command == IPC_NET_SERVING_NETWORK)
        {
            char plmn[6] = {0};
            char dummy_plmn[5] = {0};
            memcpy(plmn, PTRCHECK(((struct ipc_net_serving_network_data*)msg.data)->plmn, 5, dummy_plmn), 5);
            LOCKED()
            {
                plmn_code = atoi(plmn);
                net_lac = GET(((struct ipc_net_serving_network_data*)msg.data)->lac);
                if(msg.size >= 18)
                {
                    //undocumented in libsamsung-ipc
                    net_cid = *(uint32_t*)(msg.data+12);
                    net_tac = *(uint16_t*)(msg.data+16);
                }
                printf("PLMN = %d, lac = %d, cid = %d, tac = %d\n", plmn_code, net_lac, net_cid, net_tac);
            }
            struct thread_req* req = malloc(sizeof(*req));
            req->type = THREAD_REQ_TYPE_REGIST;
            req->ptr = NULL;
            req->size = 0;
            write(thread_comm[1], &req, sizeof(req));
        }
        else if(msg.command == IPC_DISP_RSSI_INFO)
        {
            LOCKED()
            {
                rssi = -GET(((struct ipc_disp_rssi_info_data*)msg.data)->rssi);
                printf("rssi = %d\n", rssi);
            }
        }
        else if(msg.command == IPC_GEN_PHONE_RES)
        {
            struct ipc_gen_phone_res_data* pld = msg.data;
            if(msg.size >= sizeof(*pld)) //is packet valid?
            {
                int cmd = IPC_COMMAND(pld->group, pld->index);
                if(cmd == IPC_NET_MODE_SEL)
                {
                    int failed = (pld->code & 1); //different from normal success check
                    if(!failed)
                        mode_current |= 1 << mode_probe_bit;
                    if(++mode_probe_bit < 8)
                    {
                        struct ipc_net_mode_sel_data data = {
                            .mode_sel = mode_current | (1 << mode_probe_bit),
                        };
                        ipc_client_send(fmt, seq_cntr++, IPC_NET_MODE_SEL, IPC_TYPE_SET, &data, sizeof(data));
                    }
                    else
                        printf("mode probe: supported modes = %d\n", mode_current);
                }
                else if(cmd == IPC_GPRS_DEFINE_PDP_CONTEXT && need_gprs)
                {
                    int failed = (pld->code & 1); //same custom check as above
                    if(failed && modem_supports_ipv6)
                    {
                        struct ipc_gprs_define_pdp_context_data data;
                        char apn[sizeof(data.apn)] = {0};
                        LOCKED()
                        {
                            strncpy(apn, gprs_apn, sizeof(apn));
                            modem_supports_ipv6 = 0;
                        }
                        ipc_gprs_define_pdp_context_setup(&data, 1, 1, apn);
                        ipc_client_send(fmt, seq_cntr++, IPC_GPRS_DEFINE_PDP_CONTEXT, IPC_TYPE_SET, &data, sizeof(data));
                        modem_supports_ipv6 = 0;
                    }
                    else
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
                else if(ipc_gen_phone_res_check(msg.data))
                {
                    if(cmd == IPC_SS_USSD)
                    {
                        struct thread_req* req = malloc(sizeof(*req));
                        req->type = THREAD_REQ_TYPE_USSD;
                        req->ptr = NULL;
                        req->size = 0;
                        write(thread_comm[1], &req, sizeof(req));
                    }
                    else
                        printf("warning: received IPC_GEN_PHONE_RES with failure status\n");
                }
            }
        }
        else if(msg.command == IPC_SMS_DEVICE_READY)
        {
            //the modem boots with circuit switched service only; packet data needs CS+PS
            struct ipc_net_service_domain_config_data domain;
            ipc_net_service_domain_config_setup(&domain, IPC_NET_SERVICE_DOMAIN_CONFIG_CS_PS);
            ipc_client_send(fmt, seq_cntr++, IPC_NET_SERVICE_DOMAIN_CONFIG, IPC_TYPE_SET, &domain, sizeof(domain));
            device_ready = 1;
            if(gprs_enable_pending)
            {
                gprs_enable_pending = 0;
                struct thread_req* req = malloc(sizeof(*req));
                req->type = THREAD_REQ_TYPE_SET_GPRS;
                req->ptr = NULL;
                req->size = 1;
                write(thread_comm[1], &req, sizeof(req));
            }
            ipc_client_send(fmt, seq_cntr++, IPC_SMS_SVC_CENTER_ADDR, IPC_TYPE_GET, NULL, 0);
        }
        else if(msg.command == IPC_SMS_SVC_CENTER_ADDR)
        {
            uint8_t* smsc = (uint8_t*)msg.data;
            smsc_addr_len = GET(smsc[0]);
            if(smsc_addr_len < msg.size)
            {
                smsc_addr = realloc(smsc_addr, smsc_addr_len);
                memcpy(smsc_addr, smsc+1, smsc_addr_len);
            }
        }
        else if(msg.command == IPC_SMS_SEND_MSG) //ack
        {
            struct ipc_sms_send_msg_response_data* resp = msg.data;
            printf("IPC_SMS_SEND_MSG response, id=%hhx, ack=%hx\n", GET(resp->id), GET(resp->ack));
        }
        else if(msg.command == IPC_SMS_INCOMING_MSG)
        {
            struct ipc_sms_deliver_report_request_data delr;
            struct ipc_sms_incoming_msg_header* hdr = msg.data;
            delr.type = IPC_SMS_TYPE_STATUS_REPORT;
            delr.ack = IPC_SMS_ACK_NO_ERROR;
            delr.id = GET(hdr->id);
            size_t length = GET(hdr->length);
            if(length < msg.size)
            {
                ipc_client_send(fmt, seq_cntr++, IPC_SMS_DELIVER_REPORT, IPC_TYPE_EXEC, &delr, sizeof(delr));
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
        }
        else if(msg.command == IPC_GPRS_IP_CONFIGURATION)
        {
            struct ipc_gprs_ip_configuration_data* data = msg.data;
            uint8_t* ptr = (void*)data;
            printf("packet:");
            for(int i = 0; i < msg.size; i++)
                printf(" %02hhx", ptr[i]);
            printf("\n");
            if(!(GET(data->field_flag) & 1)) //IP address is all we really need, DNS may be missing
            {
                printf("gprs_ip_configuration with incomplete data, no idea what to do\n");
                //disconnect the modem
                struct ipc_gprs_pdp_context_request_set_data data;
                char username[sizeof(data.username)] = {0};
                char password[sizeof(data.password)] = {0};
                ipc_gprs_pdp_context_request_set_setup(&data, 0, 1, username, password);
                ipc_client_send(fmt, seq_cntr++, IPC_GPRS_PDP_CONTEXT, IPC_TYPE_SET, &data, sizeof(data));
                need_gprs = 0;
                LOCKED()
                    have_gprs = 0;
                //signal ppp disconnection
                struct thread_req* req = malloc(sizeof(*req));
                req->type = THREAD_REQ_TYPE_SET_GPRS;
                req->ptr = NULL;
                req->size = 0;
                write(thread_comm[1], &req, sizeof(req));
            }
            else
            {
                LOCKED()
                {
                    char stub_ip[8] = {0};
                    memcpy(gprs_ip, PTRCHECK(data->ip, 4, stub_ip), 4);
                    memset(gprs_dns1, 0, 4);
                    memset(gprs_dns2, 0, 4);
                    if(GET(data->field_flag) & 2)
                        memcpy(gprs_dns1, PTRCHECK(data->dns1, 4, stub_ip), 4);
                    if(GET(data->field_flag) & 4)
                        memcpy(gprs_dns2, PTRCHECK(data->dns2, 4, stub_ip), 4);
                    char* ipv6_link_local = (char*)data + 35;
                    if(!memcmp(PTRCHECK(ipv6_link_local, 8, stub_ip), "\xfe\x80\x00\x00\x00\x00\x00", 8))
                    {
                        have_ipv6_link = 1;
                        memcpy(ipv6_link_id, PTRCHECK(ipv6_link_local+8, 8, stub_ip), 8);
                    }
                    else
                        have_ipv6_link = 0;
                    /*printf("ip = 0x%x\n", *(int*)data->ip);
                    printf("dns1 = 0x%x\n", *(int*)data->dns1);
                    printf("dns2 = 0x%x\n", *(int*)data->dns2);*/
                    have_gprs = 1;
                }
                ifdownup();
            }
        }
        else if(msg.command == IPC_GPRS_CALL_STATUS)
        {
            struct ipc_gprs_call_status_data* st = msg.data;
            printf("%d\n", GET(st->status));
            if(st->status != IPC_GPRS_STATUS_ENABLED)
            {
                struct thread_req* req = malloc(sizeof(*req));
                req->type = THREAD_REQ_TYPE_SET_GPRS;
                req->ptr = NULL;
                req->size = 0;
                write(thread_comm[1], &req, sizeof(req));
            }
        }
        else if(msg.command == IPC_CALL_STATUS)
        {
            struct ipc_call_status_data* st = msg.data;
            printf("%d\n", GET(st->status));
            LOCKED()
            {
                switch(GET(st->status))
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
            if(GET(data->type) == IPC_CALL_TYPE_VOICE)
            {
                call_id = GET(data->id);
                //ask modem for call list
                ipc_client_send(fmt, seq_cntr++, IPC_CALL_LIST, IPC_TYPE_GET, NULL, 0);
            }
        }
        else if(msg.command == IPC_CALL_LIST)
        {
#if 0
            {
                uint8_t* data = msg.data;
                for(int i = 0; i < msg.size; i++)
                    printf("%02hhx ", data[i]);
                printf("\n");
            }
#endif
            struct ipc_call_list_header* h = msg.data;
            struct ipc_call_list_entry* end = (void*)((char*)msg.data + msg.size);
            uint8_t count = GET(h->count);
            struct ipc_call_list_entry* cur = (void*)(h+1);
            int found = 0;
            for(size_t i = 0; i < count && !found; i++)
            {
                char* number = (char*)(cur + 1);
                size_t len = GET(cur->number_length);
                if(GET(cur->type) == IPC_CALL_TYPE_VOICE
                && GET(cur->id) == call_id)
                {
                    found = 1;
                    char* num_alloc = malloc(len+1);
                    for(size_t i = 0; i < len; i++)
                        num_alloc[i] = GET(number[i]);
                    num_alloc[GET(cur->number_length)] = 0;
                    struct thread_req* req = malloc(sizeof(*req));
                    req->type = THREAD_REQ_TYPE_DIAL;
                    req->ptr = num_alloc;
                    req->size = len;
                    write(thread_comm[1], &req, sizeof(req));
                }
                cur = (void*)(number + len);
            }
            if(!found)
            {
                struct thread_req* req = malloc(sizeof(*req));
                req->type = THREAD_REQ_TYPE_DIAL;
                req->ptr = 0;
                req->size = 1;
                write(thread_comm[1], &req, sizeof(req));
            }
        }
        else if(msg.command == IPC_PWR_PHONE_PWR_UP || msg.command == IPC_PWR_PHONE_PWR_OFF)
        {
            //modem is ready, request the IMEI now
            struct ipc_misc_me_sn_request_data data = {
                .type = IPC_MISC_ME_SN_SERIAL_NUM,
            };
            ipc_client_send(fmt, seq_cntr++, IPC_MISC_ME_SN, IPC_TYPE_GET, &data, sizeof(data));
            struct ipc_net_mode_sel_data data1 = {
                //libsamsung-ipc constants are misleading. actually this seems to be a bitfield
                //1 = GSM
                //2 = 3G
                //4 = unused
                //8 = 4G
                //32 = unused
                //16, 64, 128 = unsupported, error if attempted to set
                //future modems may support more modes, so we probe the modem and enable all supported modes
                .mode_sel = 1 << mode_probe_bit,
            };
            ipc_client_send(fmt, seq_cntr++, IPC_NET_MODE_SEL, IPC_TYPE_SET, &data1, sizeof(data1));
        }
        else if(msg.command == IPC_MISC_ME_SN)
        {
            struct ipc_misc_me_sn_response_data* data = msg.data;
            if(msg.size >= sizeof(*data) && data->length <= 32)
            {
                char* imei = malloc(data->length + 1);
                memcpy(imei, data->data, data->length);
                imei[data->length] = 0;
                struct thread_req* req = malloc(sizeof(*req));
                req->type = THREAD_REQ_TYPE_IMEI;
                req->ptr = imei;
                req->size = data->length + 1;
                write(thread_comm[1], &req, sizeof(req));
            }
        }
        else if(msg.command == IPC_SS_USSD)
        {
            struct ipc_ss_ussd_header* data = msg.data;
            if(msg.size >= sizeof(*data) && msg.size >= sizeof(*data) + data->length)
            {
                uint8_t* src = (void*)(data + 1);
                uint8_t* ptr = NULL;
                size_t size = 0;
                if(data->dcs == 15) //7-bit encoding
                {
                    size = data->length * 8 / 7;
                    ptr = malloc(size + 2);
                    uint8_t* p = ptr;
                    *p++ = data->state;
                    size_t i;
                    for(i = 0; i + 6 < data->length; i += 7)
                    {
                        *p++ = src[i] & 127;
                        for(size_t j = 0; j < 6; j++)
                            *p++ = ((src[i+j] >> (7 - j)) | (src[i+j+1] << (j + 1))) & 127;
                        *p++ = src[i+6] >> 1;
                    }
                    if(i < data->length)
                    {
                        *p++ = src[i] & 127;
                        for(size_t j = 0; j < data->length - i; j++)
                            *p++ = ((src[i+j] >> (7 - j)) | (src[i+j+1] << (j + 1))) & 127;
                    }
                    *p++ = 0;
                }
                else if(data->dcs == 72 && data->length % 2 == 0) //utf-16
                {
                    size = 0;
                    for(size_t i = 0; i < data->length; i += 2)
                        if(!src[i] && src[i+1] < 128)
                            size++;
                        else if(src[i] < 8)
                            size += 2;
                        else if((src[i] & 0xfc) == 0xd8)
                        {
                            size += 4;
                            i += 2;
                            if(i >= data->length || (src[i] & 0xfc) != 0xdc)
                                goto bad_encoding;
                        }
                        else if((src[i] & 0xfc) == 0xdc)
                            goto bad_encoding;
                        else
                            size += 3;
                    ptr = malloc(size + 2);
                    uint8_t* p = ptr;
                    *p++ = data->state;
                    for(size_t i = 0; i < data->length; i += 2)
                    {
                        uint32_t codepoint = ((uint16_t)src[i] << 8) | src[i+1];
                        if(codepoint >= 0xd800 && codepoint < 0xdc00)
                        {
                            codepoint &= 1023;
                            codepoint <<= 10;
                            i += 2;
                            codepoint |= (src[2] & 3) << 8;
                            codepoint |= src[i+1];
                            codepoint += 65536;
                        }
                        if(codepoint < 128)
                            *p++ = codepoint;
                        else if(codepoint < 2048)
                        {
                            *p++ = 0xc0 | (codepoint >> 6);
                            *p++ = 0x80 | (codepoint & 63);
                        }
                        else if(codepoint < 65536)
                        {
                            *p++ = 0xe0 | (codepoint >> 12);
                            *p++ = 0x80 | ((codepoint >> 6) & 63);
                            *p++ = 0x80 | (codepoint & 63);
                        }
                        else
                        {
                            *p++ = 0xf0 | (codepoint >> 18);
                            *p++ = 0x80 | ((codepoint >> 12) & 63);
                            *p++ = 0x80 | ((codepoint >> 6) & 63);
                            *p++ = 0x80 | (codepoint & 63);
                        }
                    }
                    *p++ = 0;
                }
                else
                    goto bad_encoding;
                struct thread_req* req = malloc(sizeof(*req));
                req->type = THREAD_REQ_TYPE_USSD;
                req->ptr = ptr;
                req->size = size;
                write(thread_comm[1], &req, sizeof(req));
            bad_encoding:;
            }
        }
        // msg.command = 0x0b0d -> returns some JSON
#undef PTRCHECK
#undef GET
        free(msg.data);
    }
    return 0;
}

static int open_rmnet(int family)
{
    struct ifreq ifr = {
        .ifr_name = INTERFACE_NAME,
    };
    int sock = socket(family, SOCK_RAW, IPPROTO_RAW);
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

static int open_packet(int eth_proto)
{
    struct ifreq ifr = {
        .ifr_name = INTERFACE_NAME,
    };
    struct sockaddr_ll addr = {
        .sll_family = AF_PACKET,
        .sll_protocol = 0,
    };
    int sock = socket(AF_PACKET, SOCK_DGRAM, htons(eth_proto));
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
    rmnet_fd = open_rmnet(AF_INET);
    if(rmnet_fd < 0)
        return -1;
    rmnet_fd_v6 = open_rmnet(AF_INET6);
    if(rmnet_fd_v6 < 0)
        return -1;
#ifdef REVERSE_PACKET_FORWARDING
    packet_fd = open_packet(ETH_P_IP);
    if(packet_fd < 0)
        return -1;
#endif
    packet_fd_v6 = open_packet(ETH_P_IPV6);
    if(packet_fd_v6 < 0)
        return -1;
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
#ifdef REVERSE_PACKET_FORWARDING
    modem_poll_fd_set(packet_fd, rs);
#endif
    modem_poll_fd_set(packet_fd_v6, rs);
    return 0;
}

static int do_poll(const struct modem_fd_set* rs, const struct modem_fd_set* ws, int target, struct thread_req** req)
{
#ifdef REVERSE_PACKET_FORWARDING
    if(rs && modem_poll_fd_isset(packet_fd, rs))
    {
        char buf[1500];
        ssize_t sz;
        while((sz = read(packet_fd, buf, sizeof(buf))) >= 0)
            modem_inet_incoming_packet(MODEM_PPP_PROTOCOL_IPV4, buf, sz);
    }
#endif
    if(rs && modem_poll_fd_isset(packet_fd_v6, rs))
    {
        char buf[1500];
        ssize_t sz;
        while((sz = read(packet_fd_v6, buf, sizeof(buf))) >= 0)
        {
#ifndef REVERSE_PACKET_FORWARDING
            if(sz <= 6)
                continue;
            size_t pos = 40;
            uint8_t kind = buf[6];
            //handle extended headers
            while(pos + 1 < sz && (kind == 0 || kind == 43 || kind == 60))
            {
                kind = buf[pos];
                pos += 2 + (uint8_t)buf[pos+1];
            }
            if(kind != 58 /* ICMPv6 */)
                continue;
#endif
            modem_inet_incoming_packet(MODEM_PPP_PROTOCOL_IPV6, buf, sz);
        }
    }
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
                    if(cmd->ptr)
                        update_one_owned(&call_number, cmd->ptr);
                }
                modem_call_incoming();
            }
            else
            {
                modem_call_remote_hangup();
                LOCKED()
                {
                    call_state = -1;
                    call_direction = -1;
                    update_one(&call_number, NULL);
                }
            }
        }
        else if(cmd->type == THREAD_REQ_TYPE_SET_GPRS && !cmd->size)
            modem_inet_disconnected();
        else if(cmd->type == THREAD_REQ_TYPE_REGIST)
            modem_registration_status_update();
        else if(cmd->type == THREAD_REQ_TYPE_IMEI)
            update_one_owned(&imei, cmd->ptr);
        else if(cmd->type == THREAD_REQ_TYPE_USSD && !cmd->ptr)
            modem_ussd_reply(MODEM_USSD_TIMEOUT, NULL);
        else if(cmd->type == THREAD_REQ_TYPE_USSD)
        {
            int state = ((uint8_t*)cmd->ptr)[0] - 1;
            char* contents = (char*)cmd->ptr + 1;
            ussd_active = (state == MODEM_USSD_REQUEST);
            modem_ussd_reply(state, contents);
            free(cmd->ptr);
        }
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

const char* modem_conf_get_serial(void)
{
    if(!imei)
        do_poll(NULL, NULL, 0, NULL);
    return imei;
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
    int ans;
    LOCKED() ans = net_status - 1;
    return ans;
}

void modem_registration_get_cell_info(int* act, int* lac, int* cid, int* tac)
{
    *lac = net_lac;
    *cid = net_cid;
    *tac = net_tac;
    switch(net_tech) //wild guesses, i'm not sure i understand what's what
    {
    case IPC_NET_ACCESS_TECHNOLOGY_GSM:
    case IPC_NET_ACCESS_TECHNOLOGY_GSM2:
    case IPC_NET_ACCESS_TECHNOLOGY_GPRS:
        *act = MODEM_ACCESS_TECHNOLOGY_GSM;
        break;
    case IPC_NET_ACCESS_TECHNOLOGY_EDGE:
        *act = MODEM_ACCESS_TECHNOLOGY_GSM_EGPRS;
        break;
    case IPC_NET_ACCESS_TECHNOLOGY_UMTS:
        *act = MODEM_ACCESS_TECHNOLOGY_UTRAN;
        break;
    case 33: //not present in libsamsung-ipc headers, probably means LTE
        *act = MODEM_ACCESS_TECHNOLOGY_EUTRAN;
        break;
    default:
        *act = -1;
        break;
    }
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

static void dtmf_key(char key, int status)
{
    struct thread_req* req = malloc(sizeof(*req));
    struct ipc_call_cont_dtmf_data* p = malloc(sizeof(*p));
    p->status = status;
    p->tone = key;
    req->type = THREAD_REQ_TYPE_DTMF_TONE;
    req->ptr = p;
    req->size = sizeof(*p);
    write(thread_comm[0], &req, sizeof(req));
}

static int dtmf_job(void* opaque)
{
    dtmf_key((char)(uintptr_t)opaque, IPC_CALL_DTMF_STATUS_STOP);
    return 0; //do not reschedule
}

int modem_call_send_dtmf(char key)
{
    dtmf_key(key, IPC_CALL_DTMF_STATUS_START);
    schedule_job(200000000, dtmf_job, (void*)(uintptr_t)key);
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

int modem_inet_supports_protocol(int proto)
{
    return proto == MODEM_PPP_PROTOCOL_IPV4 || proto == MODEM_PPP_PROTOCOL_IPV6;
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

int modem_inet_get_ipv6_link_identifier(char link_id[8])
{
    int hg;
    LOCKED()
    {
        hg = have_gprs && have_ipv6_link;
        memcpy(link_id, ipv6_link_id, 8);
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
    else if(protocol == MODEM_PPP_PROTOCOL_IPV6)
    {
        if(sz < 40)
            return -1;
        struct sockaddr_in6 dst = { .sin6_family = AF_INET6 };
        memcpy(&dst.sin6_addr, "\xff\x02\0\0\0\0\0\0\0\0\0\0\0\0\x00\x02", 16);
        return sendto(rmnet_fd_v6, pkt, sz, 0, (void*)&dst, sizeof(dst)) == sz;
    }
    else
        return -1;
}

int modem_ussd_send(const char* name)
{
    size_t utf16_size = 0;
    for(size_t i = 0; name[i]; i++)
        if((uint8_t)name[i] >= 128)
            utf16_size++;
        else
            utf16_size += 2;
    uint8_t* ptr;
    size_t size;
    if(!name[utf16_size / 2]) //request is fully ascii, can encode as 7-bit
    {
        size_t l = utf16_size / 2;
        size = (7 * l + 7) / 8;
        ptr = malloc(size + 2);
        uint8_t* p = ptr;
        *p++ = ussd_active ? 2 : 1;
        *p++ = 15;
        size_t i;
        for(i = 0; i + 7 < l; i += 8)
            for(size_t j = 0; j < 7; j++)
                *p++ = ((uint8_t)name[i+j] >> j) | ((uint8_t)name[i+j+1] << (7 - j));
        if(i < l)
            for(size_t j = 0; j < l - i; j++)
                *p++ = ((uint8_t)name[i+j] >> j) | ((uint8_t)name[i+j+1] << (7 - j));
        size += 2;
    }
    else //encode as utf-16
    {
        ptr = malloc(utf16_size + 2);
        uint8_t* p = ptr;
        *p++ = ussd_active ? 2 : 1;
        *p++ = 72;
        for(size_t i = 0; name[i]; i++)
        {
            uint8_t q = name[i];
            if(q < 128)
            {
                *p++ = q;
                continue;
            }
            else if(q < 192 || q >= 254)
            {
                free(ptr);
                return -1;
            }
            int n_bytes;
            if(q >= 252)
                n_bytes = 6;
            else if(q >= 248)
                n_bytes = 5;
            else if(q >= 240)
                n_bytes = 4;
            else if(q >= 216)
                n_bytes = 3;
            else
                n_bytes = 2;
            uint32_t value = q & ((1 << (7 - n_bytes)) - 1);
            for(int j = 1; j < n_bytes; j++)
            {
                q = name[++i];
                if(q < 128 || q >= 192)
                {
                    free(ptr);
                    return -1;
                }
                value = value << 6 | (q & 63);
            }
            if(value >= 0x110000 || (value & 0xfff800) == 0xd800)
            {
                free(ptr);
                return -1;
            }
            else if(value >= 0x10000)
            {
                value -= 0x10000;
                *p++ = 0xd8 | (value >> 18);
                *p++ = value >> 10;
                *p++ = 0xdc | ((value >> 8) & 3);
                *p++ = value;
            }
            else
            {
                *p++ = value >> 8;
                *p++ = value;
            }
        }
        size = p - ptr;
    }
    struct thread_req* req = malloc(sizeof(*req));
    req->type = THREAD_REQ_TYPE_USSD;
    req->ptr = ptr;
    req->size = size;
    if(write(thread_comm[0], &req, sizeof(req)) != sizeof(req))
        return -1;
    //we do not check for errors here. if ussd fails, we report it as termination from the modem
    return 0;
}

int modem_ussd_reset(void)
{
    ussd_active = 0;
    return 0;
}

#ifdef DEBUG_COMMAND
void modem_debug(int k, int v)
{
    struct thread_req* req = malloc(sizeof(*req));
    req->type = THREAD_REQ_TYPE_DEBUG;
    req->ptr = (void*)(intptr_t)k;
    req->size = v;
    write(thread_comm[0], &req, sizeof(req));
}
#endif
