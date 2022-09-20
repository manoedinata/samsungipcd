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

static char* in_buf_pop_delim(struct io_buf* b, char delim, size_t* size)
{
    char* pos = memchr(b->data, delim, b->sz);
    if(!pos)
        return NULL;
    char* ans = malloc(pos-b->data+1);
    memcpy(ans, b->data, pos-b->data);
    ans[pos-b->data] = 0;
    if(size)
        *size = pos-b->data;
    b->sz = (b->data+b->sz) - (pos+1);
    memmove(b->data, pos+1, b->sz);
    return ans;
}

static char* in_buf_pop_at(struct io_buf* b)
{
    return in_buf_pop_delim(b, '\r', NULL);
}

static char* in_buf_pop_pdu(struct io_buf* b)
{
    return in_buf_pop_delim(b, '\x1a', NULL);
}

static char* in_buf_pop_ppp(struct io_buf* b, size_t* size)
{
    return in_buf_pop_delim(b, '\x7e', size);
}

static int in_buf_populate(struct io_buf* b, int fd)
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

static void out_buf_write(struct io_buf* b, const char* data, size_t sz)
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

static int out_buf_flush(struct io_buf* b, int fd)
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

static int format_clcc_or_ecav(char** msg, const char* which)
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
    int is_gprs;
    int operator_format;
    int want_pdu;
    int sms_nr;
    int is_ppp;
};

static int unescape_ppp(char* ppp, size_t* p_sz)
{
    size_t sz = *p_sz;
    char* out = ppp;
    for(size_t i = 0; i < sz; i++)
    {
        if(ppp[i] == (char)0x7d)
        {
            if(i == sz - 1)
                return -1;
            i++;
            *out++ = ppp[i] ^ 0x20;
        }
        else
            *out++ = ppp[i];
    }
    *p_sz = out - ppp;
    return 0;
}

static char* escape_ppp(char* ppp, size_t* p_sz, size_t* p_cap)
{
    size_t sz = *p_sz;
    size_t sz1 = sz+2;
    for(size_t i = 0; i < sz; i++)
        if(ppp[i] == (char)0x7d || ppp[i] == (char)0x7e || (uint8_t)ppp[i] <= 0x20)
            sz1++;
    if(sz1 > *p_cap)
        ppp = realloc(ppp, *p_cap = sz1);
    *p_sz = sz1;
    char* dst = ppp + sz1;
    *--dst = (char)0x7e;
    for(size_t i = sz - 1; i + 1; i--)
        if(ppp[i] == (char)0x7d || ppp[i] == (char)0x7e || (uint8_t)ppp[i] <= 0x20)
        {
            *--dst = ppp[i] ^ 0x20;
            *--dst = (char)0x7d;
        }
        else
            *--dst = ppp[i];
    *--dst = (char)0x7e;
    return ppp;
}

static int ppp_crc_valid(const char* ppp, size_t ppp_size)
{
    return ppp_crc(ppp, ppp_size) == 0xf0b8;
}

static void ppp_crc_fix(char* ppp, size_t ppp_size)
{
    uint16_t crc = ~ppp_crc(ppp, ppp_size-2);
    ppp[ppp_size-2] = (char)crc;
    ppp[ppp_size-1] = (char)(crc >> 8);
}

static int handle_ppp(struct io_buf* out_buf, char* ppp, size_t ppp_size)
{
    if(!ppp)
        return 0;
    size_t ppp_cap = ppp_size;
    if(!ppp_size || unescape_ppp(ppp, &ppp_size) || ppp_size < 6)
    {
        free(ppp);
        return 0;
    }
    if(ppp[0] != (char)0xff || ppp[1] != (char)3)
        return 0;
    if(!ppp_crc_valid(ppp, ppp_size))
        return 0;
    uint16_t protocol = (uint16_t)(uint8_t)ppp[2] << 8 | (uint8_t)ppp[3];
    char* pkt = ppp + 4;
    size_t pkt_size = ppp_size - 6;
#if 0
    printf("PPP packet (protocol=0x%hx):", protocol);
    for(size_t i = 0; i < pkt_size + 2; i++)
        printf(" %02hhx", pkt[i]);
    printf("\n");
#endif
    if(protocol == 0xc021 /* LCP */)
    {
        if(pkt_size < 4)
            return 0;
        if(pkt[0] == 1) //configure-request
        {
            if(pkt[2] == 0 && pkt[3] == 4)
            {
                char* ppp2 = malloc(ppp_cap);
                memcpy(ppp2, ppp, ppp_size);
                size_t ppp2_size = ppp_size;
                size_t ppp2_cap = ppp_cap;
                ppp_crc_fix(ppp, ppp_size);
                ppp2 = escape_ppp(ppp2, &ppp2_size, &ppp2_cap);
                out_buf_write(out_buf, ppp2, ppp2_size);
                free(ppp2);
                pkt[0] = 2; //configure-ack
                ppp_crc_fix(ppp, ppp_size);
                ppp = escape_ppp(ppp, &ppp_size, &ppp_cap);
                out_buf_write(out_buf, ppp, ppp_size);
            }
            else
            {
                pkt[0] = 4; //configure-reject
                ppp_crc_fix(ppp, ppp_size);
                ppp = escape_ppp(ppp, &ppp_size, &ppp_cap);
                out_buf_write(out_buf, ppp, ppp_size);
            }
        }
        else if(pkt[0] == 5) //terminate-request
        {
            for(int i = 5; i <= 6; i++)
            {
                char buf[32] = {0xff, 0x03, ppp[2], ppp[3], i, pkt[1], 0x00, 0x04, 0, 0};
                size_t sz = 10;
                size_t cap = 32;
                ppp_crc_fix(buf, sz);
                escape_ppp(buf, &sz, &cap);
                out_buf_write(out_buf, buf, sz);
            }
            if(protocol == 0xc021)
            {
                free(ppp);
                return 1;
            }
        }
    }
    else if(protocol == 0x8021 /* IPCP */)
    {
        if(pkt_size < 4)
            return 0;
        static char local_ip[4];
        if(modem_inet_get_ipv4_address(local_ip))
            memset(local_ip, 0, 4);
        if(pkt[0] == 1) //configure-request
        {
            int ip_ok = 1;
            int have_ip = 0;
            int have_rej = 0;
            size_t l = (uint16_t)(uint8_t)pkt[2] << 8 | (uint8_t)pkt[3];
            if(l >= pkt_size)
                l = pkt_size;
            char* q = pkt + 4;
            for(size_t i = 4; i + 1 < l; i += (uint8_t)pkt[i+1])
            {
                if(pkt[i] == 3 && pkt[i+1] == 6 && i + 5 < l && !have_ip)
                {
                    have_ip = 1;
                    if(memcmp(pkt+i+2, local_ip, 4))
                        ip_ok = 0;
                }
                else
                {
                    have_rej = 1;
                    size_t sz = (uint8_t)pkt[i+1];
                    if(sz >= l - i)
                        sz = l - i;
                    memmove(q, pkt+i, sz);
                    q += sz;
                }
            }
            if(have_rej)
            {
                pkt[0] = 4; //configure-reject
                pkt[2] = (q - pkt) >> 8;
                pkt[3] = q - pkt;
                ppp_size = (q - ppp) + 2;
            }
            else if(!ip_ok)
            {
                pkt[0] = 3; //configure-nak
                pkt[2] = 0;
                pkt[3] = 10;
                pkt[4] = 3;
                pkt[5] = 6;
                memcpy(pkt+6, local_ip, 4);
                ppp_size = 16;
            }
            else
            {
                char* ppp2 = malloc(ppp_cap);
                size_t ppp2_size = ppp_size;
                size_t ppp2_cap = ppp_cap;
                memcpy(ppp2, ppp, ppp2_size);
                ppp_crc_fix(ppp2, ppp2_size);
                ppp2 = escape_ppp(ppp2, &ppp2_size, &ppp2_cap);
                out_buf_write(out_buf, ppp2, ppp2_size);
                free(ppp2);
                pkt[0] = 2; //configure-ack
            }
            ppp_crc_fix(ppp, ppp_size);
            ppp = escape_ppp(ppp, &ppp_size, &ppp_cap);
            out_buf_write(out_buf, ppp, ppp_size);
        }
    }
    else if(protocol == 0x21 /* IPv4 */ || protocol == 0x57 /* IPv6 */)
        modem_inet_send_packet(protocol, pkt, pkt_size);
    else
    {
        char buf[32] = {0xff, 0x03, 0xc0, 0x21, 0x08, 0x00, 0x00, 0x06, ppp[2], ppp[3], 0, 0};
        size_t sz = 12;
        size_t cap = 32;
        ppp_crc_fix(buf, sz);
        escape_ppp(buf, &sz, &cap);
        out_buf_write(out_buf, buf, sz);
    }
    free(ppp);
    return 0;
}

static void poll_at_commands(struct io_buf* in_buf, struct io_buf* out_buf, struct conn_status* st)
{
    char* ppp;
    size_t ppp_size;
    char* cmd;
    char* pdu;
    while(
        st->is_ppp
        ? (ppp = in_buf_pop_ppp(in_buf, &ppp_size))
        : (st->want_pdu
        ? (pdu = in_buf_pop_pdu(in_buf))
        : (cmd = in_buf_pop_at(in_buf)))
    )
    {
        if(st->is_ppp)
        {
            if(handle_ppp(out_buf, ppp, ppp_size))
            {
                printf("Disabling PPP...\n");
                st->is_ppp = 0;
            }
            continue;
        }
        else if(st->want_pdu)
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
        else if(!st->is_gprs && !strcmp(cmd, "AT+CSMS?"))
            WRITE_OK_STRING(out_buf, "\r\n+CSMS: 0,1,1,0\r\n");
        else if(!st->is_gprs && !strcmp(cmd, "AT+CSMS=?"))
            WRITE_OK_STRING(out_buf, "\r\n0\r\n");
        else if(!st->is_gprs && !strcmp(cmd, "AT+CSMS=0"))
            WRITE_OK_STRING(out_buf, "\r\n+CSMS: 1,1,0\r\n");
        else if(!st->is_gprs && !strcmp(cmd, "AT+CMGF=?"))
            WRITE_OK_STRING(out_buf, "\r\n+CMGF: (0)\r\n");
        else if(!st->is_gprs && !strcmp(cmd, "AT+CMGF=0"))
            WRITE_OK(out_buf);
        else if(!st->is_gprs && !strcmp(cmd, "AT+CPMS=?"))
            WRITE_OK_STRING(out_buf, "\r\n+CPMS: (\"SM\",\"ME\"),(\"SM\",\"ME\"),(\"SM\",\"ME\")\r\n");
        else if(!st->is_gprs && !strncmp(cmd, "AT+CPMS=", 8))
            WRITE_OK(out_buf);
        else if(!st->is_gprs && !strcmp(cmd, "AT+CNMI=?"))
            WRITE_OK_STRING(out_buf, "\r\n+CNMI: (2),(1),(2),(1),(0)\r\n");
        else if(!st->is_gprs && !strcmp(cmd, "AT+CNMI=2,1,2,1,0"))
            WRITE_OK(out_buf);
        else if(!st->is_gprs && !strcmp(cmd, "AT+CMGL=4")) //TODO: stub
            WRITE_OK(out_buf);
        else if(!st->is_gprs && !strncmp(cmd, "AT+CGSMS=", 9)) //???
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
        else if(!st->is_gprs && !strcmp(cmd, "AT+CSQ"))
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
        else if(!st->is_gprs && !strncmp(cmd, "AT+COPS=", 8))
        {
            char* ops = cmd + 8;
            char* colon = strchr(ops, ',');
            if(colon)
                *colon = 0;
            int mode = atoi(ops);
            if(colon)
            {
                ops = colon + 1;
                colon = strchr(ops, ',');
                if(colon)
                    *colon = 0;
                int format = atoi(ops);
                st->operator_format = format;
            }
            if(mode != 3)
                WRITE_ERROR(out_buf);
            else
                WRITE_OK(out_buf);
        }
        else if(!st->is_gprs && !strcmp(cmd, "AT+COPS?"))
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
        else if(st->is_gprs && !strcmp(cmd, "ATD*99***1#"))
        {
            WRITE_OK(out_buf);
            st->is_ppp = 1;
            modem_inet_set_enabled(1);
        }
        else if(!st->is_gprs && !strncmp(cmd, "ATD", 3))
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
        else if(!st->is_gprs && !strcmp(cmd, "ATA"))
        {
            if(!modem_call_answer())
                WRITE_OK(out_buf);
            else
                WRITE_ERROR(out_buf);
        }
        else if(!st->is_gprs && (!strcmp(cmd, "ATH") || !strcmp(cmd, "AT+CLCC")))
        {
            char* msg;
            int len = format_clcc_or_ecav(&msg, "+CLCC");
            out_buf_write(out_buf, msg, len);
            WRITE_OK(out_buf);
            free(msg);
        }
        else if(!st->is_gprs && !strcmp(cmd, "AT+CLCC=?"))
            WRITE_OK(out_buf);
        else if(!st->is_gprs && !strcmp(cmd, "AT+CHUP"))
        {
            if(!modem_call_hangup())
                WRITE_OK(out_buf);
            else
                WRITE_ERROR(out_buf);
        }
        else if(!st->is_gprs && !strncmp(cmd, "AT+CMGS=", 8))
        {
            out_buf_write(out_buf, "\r\n>", 3);
            st->want_pdu = 1;
        }
        else if(!st->is_gprs && !strncmp(cmd, "AT+CMGR=", 8))
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
        else if(st->is_gprs && !strncmp(cmd, "ATX", 3))
            WRITE_OK(out_buf);
        else if(!strncmp(cmd, "ATZ", 3))
            WRITE_OK(out_buf);
        else if(st->is_gprs && !strcmp(cmd, "AT+CGDCONT=?"))
            WRITE_OK_STRING(out_buf, "\r\n+CGDCONT: (1),\"IP\",,,(0),(0)\r\n");
        else if(st->is_gprs && !strncmp(cmd, "AT+CGDCONT=1,\"IP\",\"", 19))
        {
            //TODO: string unescape
            char* apn = cmd+19;
            char* quote = strchr(apn, '"');
            if(quote)
                *quote = 0;
            if(!modem_inet_set_apn(apn))
                WRITE_OK(out_buf);
            else
                WRITE_ERROR(out_buf);
        }
        else if(st->is_gprs && !strcmp(cmd, "AT+CGDCONT?"))
        {
            const char* apn = modem_inet_get_apn();
            if(!apn)
                WRITE_ERROR(out_buf);
            else
            {
                //TODO: string escape
                char* msg = NULL;
                int len = asprintf(&msg, "\r\n+CGDCONT: 1,\"IP\",\"%s\",\"10.10.10.10\",0,0\r\n\r\nOK\r\n", apn);
                out_buf_write(out_buf, msg, len);
                free(msg);
            }
        }
        else if(st->is_gprs && !strncmp(cmd, "AT+CGACT=", 9))
        {
            char* q = cmd + 10;
            char* comma = strchr(q, ',');
            *comma = 0;
            int state = atoi(q);
            q = comma + 1;
            int which = atoi(q);
            if(which == 1 && !modem_inet_set_enabled(state))
                WRITE_OK(out_buf);
            else
                WRITE_ERROR(out_buf);
        }
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

void modem_inet_incoming_packet(int protocol, const char* pkt, size_t sz)
{
    size_t ppp_sz = sz+6;
    size_t ppp_cap = 2*ppp_sz;
    char* ppp_pkt = malloc(ppp_cap);
    memcpy(ppp_pkt, (const char[4]){0xff, 3, protocol>>8, protocol}, 4);
    memcpy(ppp_pkt+4, pkt, sz);
    ppp_crc_fix(ppp_pkt, ppp_sz);
    ppp_pkt = escape_ppp(ppp_pkt, &ppp_sz, &ppp_cap);
    for(int i = 0; i < FD_SETSIZE; i++)
        if(connections[i] && connections[i]->status.is_ppp)
            out_buf_write(&connections[i]->out_buf, ppp_pkt, ppp_sz);
    free(ppp_pkt);
}

static int open_pty(const char* path, struct modem_fd_set* rdsocks, int is_gprs)
{
    int fd = open(path, O_RDWR | O_NOCTTY);
    if(fd < 0)
        return -1;
    struct client_conn* conn = malloc(sizeof(*conn));
    memset(conn, 0, sizeof(*conn));
    conn->fd = fd;
    conn->status.is_gprs = is_gprs;
    connections[fd] = conn;
    modem_poll_fd_set(fd, rdsocks);
    return fd;
}

int main(int argc, const char** argv)
{
    if(argc != 3)
    {
        printf("usage: samsungipcd <gsm_path> <gprs_path>\n");
        return 1;
    }
    const char* pty_path = argv[1];
    const char* pty2_path = argv[2];
    FD_ZERO(&rdsocks.set);
    rdsocks.maxfd = -1;
    FD_ZERO(&wrsocks.set);
    wrsocks.maxfd = -1;
    if(modem_init())
    {
        fprintf(stderr, "modem init failed\n");
        return 1;
    }
    int pty_fd = open_pty(pty_path, &rdsocks, 0);
    if(pty_fd < 0)
    {
        fprintf(stderr, "open(pty_path) failed\n");
        return 1;
    }
    int pty2_fd = open_pty(pty2_path, &rdsocks, 1);
    if(pty2_fd < 0)
    {
        fprintf(stderr, "open(pty2_path) failed\n");
        return 1;
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
            if(connections[i])
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
                    {
                        pty_fd = open_pty(pty_path, &rdsocks, 0);
                        if(pty_fd < 0)
                        {
                            fprintf(stderr, "open(pty_path) failed\n");
                            return 1;
                        }
                    }
                    else if(i == pty2_fd)
                    {
                        pty2_fd = open_pty(pty2_path, &rdsocks, 1);
                        if(pty2_fd < 0)
                        {
                            fprintf(stderr, "open(pty2_path) failed\n");
                            return 1;
                        }
                    }
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
