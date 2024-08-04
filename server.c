/*
Copyright 2022-2024 Sergey Lisov

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
#include <signal.h>

struct io_buf
{
    char* data;
    size_t sz;
    size_t cap;
};

int n_total = 0;
int n_ppp = 0;

enum { BUF_MIN_SIZE = 1024 };

static inline char* in_buf_peek_delim(struct io_buf* b, char delim, size_t* size, int pop)
{
    char* pos = memchr(b->data, delim, b->sz);
    if(!pos)
        return NULL;
    char* ans = NULL;
    if(pop < 2)
    {
        ans = malloc(pos-b->data+1);
        memcpy(ans, b->data, pos-b->data);
        ans[pos-b->data] = 0;
    }
    if(size)
        *size = pos-b->data;
    if(pop)
    {
        b->sz = (b->data+b->sz) - (pos+1);
        memmove(b->data, pos+1, b->sz);
    }
    return ans;
}

static inline char* in_buf_pop_delim(struct io_buf* b, char delim, size_t* size)
{
    return in_buf_peek_delim(b, delim, size, 1);
}

static char* in_buf_peek_at(struct io_buf* b)
{
    return in_buf_peek_delim(b, '\r', NULL, 0);
}

static void in_buf_pop_at(struct io_buf* b)
{
    in_buf_peek_delim(b, '\r', NULL, 2);
}

static char* in_buf_pop_pdu(struct io_buf* b)
{
    return in_buf_pop_delim(b, '\x1a', NULL);
}

static char* in_buf_pop_ppp(struct io_buf* b, size_t* size, int* is_ppp)
{
    if(b->sz >= 2 && b->data[0] == 'A' && b->data[1] == 'T')
    {
        //a valid PPP packet never starts with "AT"
        //if we receive this, the PPP connection must've been abruptly terminated
        *is_ppp = 0;
        n_ppp--;
        return in_buf_peek_at(b);
    }
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

#ifdef API_DEBUG

#define DO_IP_DEBUG
#undef API_DEBUG
#define API_DEBUG(...) fprintf(stderr, __VA_ARGS__)

#else

#define API_DEBUG(...) do {} while(0)

#endif

static int format_clcc_or_ecav(char** msg, const char* which)
{
    *msg = NULL;
    const char* number;
    int state;
    int direction;
    API_DEBUG("modem_call_get_info(...)");
    if(!modem_call_get_info(&number, &state, &direction))
    {
        API_DEBUG(" = 0\n");
        return 0;
    }
    else
    {
        API_DEBUG(" = (\"%s\", %d, %d)\n", number ? number : "(null)", state, direction);
        return asprintf(msg, "\r\n%s: 1,%d,%d,0,0,\"%s\",%d\r\n",
            which,
            direction,
            state,
            number ? number : "",
            (number && number[0] == '+') ? 145 : 129
        ); //TODO: string escape
    }
}

static int format_ccwa(char** msg)
{
    *msg = NULL;
    const char* number;
    int state;
    int direction;
    if(!modem_call_get_info(&number, &state, &direction))
        return 0;
    else
        return asprintf(msg, "\r\n+CCWA: \"%s\",%d,1\r\n",
            number ? number : "",
            (number && number[0] == '+') ? 145 : 129
        ); //TODO: string escape
}

struct conn_status
{
    int operator_format;
    int want_pdu;
    int sms_nr;
    int is_ppp;
    int want_retry;
    int ppp_protocols;
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
#ifdef PPP_DUMP
#warning "PPP dumping enabled. This is a debug feature, do not use in production."
    char* ppp3 = malloc(*p_sz);
    memcpy(ppp3, ppp, *p_sz);
    printf("Escaping PPP:");
    for(size_t i = 0; i < *p_sz; i++)
        printf(" %02hhx", ppp[i]);
    printf("\n");
#endif
    size_t sz = *p_sz;
    size_t sz1 = sz+2;
    for(size_t i = 0; i < sz; i++)
        if(ppp[i] == (char)0x7d || ppp[i] == (char)0x7e || (uint8_t)ppp[i] < 0x20)
            sz1++;
    if(sz1 > *p_cap)
        ppp = realloc(ppp, *p_cap = sz1);
    *p_sz = sz1;
    char* dst = ppp + sz1;
    *--dst = (char)0x7e;
    for(size_t i = sz - 1; i + 1; i--)
        if(ppp[i] == (char)0x7d || ppp[i] == (char)0x7e || (uint8_t)ppp[i] < 0x20)
        {
            *--dst = ppp[i] ^ 0x20;
            *--dst = (char)0x7d;
        }
        else
            *--dst = ppp[i];
    *--dst = (char)0x7e;
#ifdef PPP_DUMP
    sz1 -= 2;
    char* ppp2 = malloc(sz1);
    memcpy(ppp2, ppp+1, sz1);
    int status = unescape_ppp(ppp2, &sz1);
    if(status || sz1 != sz || memcmp(ppp2, ppp3, sz))
    {
        printf("ESCAPE/UNESCAPE BROKEN: %d\n", status);
        printf("Reescape PPP:");
        for(size_t i = 0; i < sz1; i++)
            printf(" %02hhx", ppp2[i]);
        printf("\n");
    }
    free(ppp2);
    free(ppp3);
#endif
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

//internal enum to facilitate storing protocols in a bitmask
//PPP protocol numbers are used for the API instead of these
enum
{
    MODEM_PROTOCOL_IPV4 = 1 << 0,
    MODEM_PROTOCOL_IPV6 = 1 << 1,
};
static const int PPP_PROTOCOLS[2] = {MODEM_PPP_PROTOCOL_IPV4, MODEM_PPP_PROTOCOL_IPV6};

static int handle_ppp(struct io_buf* out_buf, char* ppp, size_t ppp_size, int protocols)
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
    {
        free(ppp);
        return 0;
    }
    if(!ppp_crc_valid(ppp, ppp_size))
    {
        free(ppp);
        return 0;
    }
    uint16_t protocol = (uint16_t)(uint8_t)ppp[2] << 8 | (uint8_t)ppp[3];
    char* pkt = ppp + 4;
    size_t pkt_size = ppp_size - 6;
#ifdef PPP_DUMP
    printf("PPP packet (protocol=0x%x):", protocol);
    for(size_t i = 0; i < pkt_size; i++)
        printf(" %02hhx", pkt[i]);
    printf("\n");
#endif
    if(protocol == 0xc021 /* LCP */)
    {
        if(pkt_size < 4)
        {
            free(ppp);
            return 0;
        }
        if(pkt[0] == 1) //configure-request
        {
            if(pkt[2] == 0 && pkt[3] == 4)
            {
                char buf[32] = {0xff, 0x03, ppp[2], ppp[3], 1, pkt[1], 0x00, 0x08, 0x03, 0x04, 0xc0, 0x23, 0, 0};
                size_t sz = 14;
                size_t cap = 32;
                ppp_crc_fix(buf, sz);
                escape_ppp(buf, &sz, &cap);
                out_buf_write(out_buf, buf, sz);
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
            free(ppp);
            return 1;
        }
        else if(pkt[0] == 6) //terminate-ack
        {
            free(ppp);
            return 1;
        }
    }
    else if(protocol == 0xc023 /* PAP */)
    {
        if(pkt_size < 4 || pkt[0] != (char)1) //authenticate-request
        {
            free(ppp);
            return 0;
        }
        size_t l = (uint16_t)(uint8_t)pkt[2] << 8 | (uint8_t)pkt[3];
        if(l >= pkt_size)
            l = pkt_size;
        if(l < 6)
        {
            free(ppp);
            return 0;
        }
        size_t szuser = (uint8_t)pkt[4];
        size_t pszp = 5 + szuser;
        if(pszp >= l)
        {
            free(ppp);
            return 0;
        }
        size_t szp = pkt[pszp];
        size_t totalsz = pszp + szp + 1;
        if(totalsz > l)
        {
            free(ppp);
            return 0;
        }
        pkt[pszp] = pkt[totalsz] = 0;
        API_DEBUG("modem_inet_set_username_password(\"%s\", \"%s\")", pkt+5, pkt+pszp+1);
        if(!modem_inet_set_username_password(pkt+5, pkt+pszp+1))
        {
            API_DEBUG(" = 0 (OK)\n");
            pkt[0] = 2;
            modem_inet_set_enabled(1);
        }
        else
        {
            API_DEBUG(" = ERROR\n");
            pkt[0] = 3;
        }
        pkt[2] = 0;
        pkt[3] = 5;
        pkt[4] = 0;
        ppp_size = 11;
        ppp_crc_fix(ppp, ppp_size);
        ppp = escape_ppp(ppp, &ppp_size, &ppp_cap);
        out_buf_write(out_buf, ppp, ppp_size);
    }
    else if(protocol == 0x8021 /* IPCP */ && (protocols & MODEM_PROTOCOL_IPV4))
    {
        if(pkt_size < 4)
        {
            free(ppp);
            return 0;
        }
        char local_ip[4], dns1[4], dns2[4];
        API_DEBUG("modem_inet_get_ipv4_addresses(...)");
        if(modem_inet_get_ipv4_addresses(local_ip, dns1, dns2))
        {
            API_DEBUG(" = ERROR\n");
            free(ppp);
            return 0;
        }
        API_DEBUG(" = 0 (OK)\n");
#if defined(PPP_DUMP) || defined(DO_API_DEBUG)
        fprintf(stderr, "IPCP configuration:\n");
        fprintf(stderr, "Local IP: %hhu.%hhu.%hhu.%hhu\n", local_ip[0], local_ip[1], local_ip[2], local_ip[3]);
        fprintf(stderr, "DNS1: %hhu.%hhu.%hhu.%hhu\n", dns1[0], local_ip[1], local_ip[2], local_ip[3]);
        fprintf(stderr, "DNS2: %hhu.%hhu.%hhu.%hhu\n", dns2[0], local_ip[1], local_ip[2], local_ip[3]);
#endif
        if(pkt[0] == 1) //configure-request
        {
            int ip_nak = 0;
            int have_ip = 0;
            int have_rej = 0;
            size_t l = (uint16_t)(uint8_t)pkt[2] << 8 | (uint8_t)pkt[3];
            if(l >= pkt_size)
                l = pkt_size;
            char* q = pkt + 4;
            for(size_t i = 4; i + 1 < l; i += (uint8_t)pkt[i+1])
            {
                if(pkt[i] == 3 && pkt[i+1] == 6 && i + 5 < l && !(have_ip & 1))
                {
                    have_ip |= 1;
                    if(memcmp(pkt+i+2, local_ip, 4))
                    {
                        memcpy(pkt+i+2, local_ip, 4);
                        ip_nak |= (1 << 3);
                    }
                }
                else if(pkt[i] == (char)0x81 && pkt[i+1] == 6 && i + 5 < l && !(have_ip & 2))
                {
                    have_ip |= 2;
                    if(memcmp(pkt+i+2, dns1, 4))
                    {
                        memcpy(pkt+i+2, dns1, 4);
                        ip_nak |= (1 << 9);
                    }
                }
                else if(pkt[i] == (char)0x83 && pkt[i+1] == 6 && i + 5 < l && !(have_ip & 4))
                {
                    have_ip |= 4;
                    if(memcmp(pkt+i+2, dns2, 4))
                    {
                        memcpy(pkt+i+2, dns2, 4);
                        ip_nak |= (1 << 1);
                    }
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
            else
            {
                char* ppp2 = malloc(ppp_cap);
                size_t ppp2_size = ppp_size;
                size_t ppp2_cap = ppp_cap;
                memcpy(ppp2, ppp, ppp2_size);
                char* pkt2 = ppp2 + 4;
                char* q = pkt2 + 4;
                for(size_t i = 4; i + 1 < l; i += (uint8_t)pkt2[i+1])
                    if(pkt2[i] == 3)
                    {
                        size_t sz = (uint8_t)pkt2[i+1];
                        if(sz >= l - i)
                            sz = l - i;
                        memmove(q, pkt2+i, sz);
                        q += sz;
                    }
                pkt2[2] = (q - pkt2) >> 8;
                pkt2[3] = q - pkt2;
                ppp2_size = (q - ppp2) + 2;
                ppp_crc_fix(ppp2, ppp2_size);
                ppp2 = escape_ppp(ppp2, &ppp2_size, &ppp2_cap);
                out_buf_write(out_buf, ppp2, ppp2_size);
                free(ppp2);
                ppp[4] = ip_nak ? 3 : 2; //configure-nak or configure-ack
            }
            ppp_crc_fix(ppp, ppp_size);
            ppp = escape_ppp(ppp, &ppp_size, &ppp_cap);
            out_buf_write(out_buf, ppp, ppp_size);
        }
    }
    else if(protocol == 0x8057 /* IPV6CP */ && (protocols & MODEM_PROTOCOL_IPV6))
    {
        if(pkt_size < 4)
        {
            free(ppp);
            return 0;
        }
        char identifier[8];
        API_DEBUG("modem_inet_get_ipv6_link_identifier(...)");
        if(modem_inet_get_ipv6_link_identifier(identifier))
        {
            API_DEBUG(" = ERROR\n");
            free(ppp);
            return 0;
        }
        API_DEBUG(" = 0 (OK)\n");
#if defined(PPP_DUMP) || defined(DO_API_DEBUG)
        fprintf(stderr, "IPV6CP link identifier: (fe80::)%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx\n", identifier[0], identifier[1], identifier[2], identifier[3], identifier[4], identifier[5], identifier[6], identifier[7]);
#endif
        if(pkt[0] == 1) //configure-request
        {
            int have_rej = 0;
            int nak = 0;
            int have_id = 0;
            size_t l = (uint16_t)(uint8_t)pkt[2] << 8 | (uint8_t)pkt[3];
            if(l >= pkt_size)
                l = pkt_size;
            char* q = pkt + 4;
            for(size_t i = 4; i < pkt_size; i += (uint8_t)pkt[i+1])
            {
                if(pkt[i] == 1 && pkt[i+1] == 10 && i + 10 <= l && !have_id)
                {
                    if(memcmp(pkt+i+2, identifier, 8))
                    {
                        memcpy(pkt+i+2, identifier, 8);
                        nak = 1;
                    }
                    have_id = 1;
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
            else
            {
                char* ppp2 = malloc(ppp_cap);
                size_t ppp2_size = ppp_size;
                size_t ppp2_cap = ppp_cap;
                memcpy(ppp2, ppp, ppp2_size);
                //in our configure-req we pretend to be another node, so we must use a different id
                char* pkt2 = ppp2 + 4;
                for(size_t i = 4; i + 1 < l; i += (uint8_t)pkt[i+1])
                    if(pkt2[i] == 1 && pkt2[i+1] == 10)
                        pkt2[i+9] ^= 1;
                ppp_crc_fix(ppp2, ppp2_size);
                ppp2 = escape_ppp(ppp2, &ppp2_size, &ppp2_cap);
                out_buf_write(out_buf, ppp2, ppp2_size);
                free(ppp2);
                pkt[0] = nak ? 3 : 2; //configure-nak or configure-ack
            }
            ppp_crc_fix(ppp, ppp_size);
            ppp = escape_ppp(ppp, &ppp_size, &ppp_cap);
            out_buf_write(out_buf, ppp, ppp_size);
        }
    }
    else if((protocol == 0x21 /* IPv4 */ && (protocols & MODEM_PROTOCOL_IPV4))
         || (protocol == 0x57 /* IPv6 */ && (protocols & MODEM_PROTOCOL_IPV6)))
    {
        if(pkt_size < 4 || ((uint8_t)pkt[0] & 0xf0) != (protocol == 0x21 ? 0x40 : 0x60))
        {
            free(ppp);
            return 0;
        }
        size_t sz;
        if(protocol == 0x21)
            sz = (uint16_t)(uint8_t)pkt[2] << 8 | (uint8_t)pkt[3];
        else
            sz = (uint32_t)((uint8_t)pkt[4] << 8 | (uint8_t)pkt[5]) + 40;
        if(sz > pkt_size)
            sz = pkt_size;
        API_DEBUG("modem_inet_send_packet(%d, %p, %zd)\n", protocol, pkt, sz);
        modem_inet_send_packet(protocol, pkt, sz);
    }
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

static int format_creg_response(const char* resp_name, char** buf)
{
    API_DEBUG("modem_registration_get_status()");
    int state = modem_registration_get_status();
    API_DEBUG(" = %d\n", state);
    int act, lac, cid, tac;
    API_DEBUG("modem_registration_get_cell_info(");
    modem_registration_get_cell_info(&act, &lac, &cid, &tac);
    API_DEBUG("&%d, &%d, &%d, &%d)\n", act, lac, cid, tac);
    //XXX: for LTE networks we should really return CEREG, not CGREG...
    if(lac < 0 || cid < 0)
        return asprintf(buf, "\r\n+%s: 1,%d\r\n\r\nOK\r\n", resp_name, state);
    else if(act < 0)
        return asprintf(buf, "\r\n+%s: 1,%d,\"%04X\",\"%08X\"\r\n\r\nOK\r\n", resp_name, state, lac, cid);
    else
        return asprintf(buf, "\r\n+%s: 1,%d,\"%04X\",\"%08X\",%d\r\n\r\nOK\r\n", resp_name, state, lac, cid, act);
}

static const char* get_cgdcont_pdp_type(void)
{
    int mask = 0;
    for(size_t i = 0; i < sizeof(PPP_PROTOCOLS) / sizeof(*PPP_PROTOCOLS); i++)
    {
        API_DEBUG("modem_inet_supports_protocol(0x%x)", PPP_PROTOCOLS[i]);
        int status = modem_inet_supports_protocol(PPP_PROTOCOLS[i]);
        API_DEBUG(" = %d\n", status);
        if(status)
            mask |= 1 << i;
    }
    static const char* names[1 << (sizeof(PPP_PROTOCOLS) / sizeof(*PPP_PROTOCOLS))] = {"", "IP", "IPV6", "IPV4V6"};
    return names[mask];
}

static int handle_at_command(struct io_buf* out_buf, struct conn_status* st, char* cmd)
{
    if(!strcmp(cmd, "AT"))
        WRITE_OK(out_buf);
    else if(!strcmp(cmd, "AT+CGMI"))
        WRITE_OK_STRING(out_buf, "\r\ngeneric\r\n");
    else if(!strcmp(cmd, "ATE0"))
        WRITE_OK(out_buf);
    else if(!strcmp(cmd, "AT+CSCS=\"UTF-8\""))
        WRITE_OK(out_buf);
    else if(!strcmp(cmd, "AT+CSCS?") || !strcmp(cmd, "AT+CSCS=?"))
        WRITE_OK_STRING(out_buf, "\r\n+CSCS: \"UTF-8\"\r\n");
    else if(!strncmp(cmd, "ATV", 3)) //fake ok
        WRITE_OK(out_buf);
    else if(!strncmp(cmd, "AT&C", 4))
        WRITE_OK(out_buf);
    else if(!strcmp(cmd, "ATI") || !strcmp(cmd, "AT+CGMM") || !strcmp(cmd, "AT+GMM"))
    {
        API_DEBUG("modem_conf_get_name(%d)", !strcmp(cmd, "ATI"));
        const char* idstr = modem_conf_get_name(!strcmp(cmd, "ATI"));
        API_DEBUG(" = \"%s\"\n", idstr ? idstr : "(null)");
        if(!idstr)
        {
            st->want_retry = 1;
            free(cmd);
            return 0;
        }
        char* buf = NULL;
        int len = asprintf(&buf, "\r\n%s\r\n\r\nOK\r\n", idstr);
        out_buf_write(out_buf, buf, len);
        free(buf);
    }
    else if(!strcmp(cmd, "AT+CGMR") || !strcmp(cmd, "AT+GMR"))
        WRITE_OK_STRING(out_buf, "0.1");
    else if(!strcmp(cmd, "AT+CGSN") || !strcmp(cmd, "AT+GSN"))
    {
        API_DEBUG("modem_conf_get_serial()");
        const char* serial = modem_conf_get_serial();
        API_DEBUG(" = \"%s\"\n", serial ? serial : "(null)");
        if(!serial)
        {
            st->want_retry = 1;
            free(cmd);
            return 0;
        }
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
        *strchr(cmd, '?') = 0;
        char* buf = NULL;
        int len = format_creg_response(cmd+3, &buf);
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
        API_DEBUG("modem_conf_set_online(%d)", atoi(cmd+8));
        if(!modem_conf_set_online(atoi(cmd+8)))
        {
            API_DEBUG(" = 0 (OK)\n");
            WRITE_OK(out_buf);
        }
        else
        {
            API_DEBUG(" = ERROR\n");
            WRITE_ERROR(out_buf);
        }
    }
    else if(!strcmp(cmd, "AT+CFUN?"))
    {
        API_DEBUG("modem_conf_is_online()");
        int q = modem_conf_is_online();
        API_DEBUG(" = %d\n", q);
        char* buf = NULL;
        int len = asprintf(&buf, "\r\n+CFUN: %d\r\n\r\nOK\r\n\r\n", q?1:0);
        out_buf_write(out_buf, buf, len);
        free(buf);
    }
    else if(!strcmp(cmd, "AT+CRSM=176,12258,0,0,10")) //TODO: stub
        WRITE_OK_STRING(out_buf, "\r\n+CRSM: 159,10,989422024754212460F6\r\n");
    else if(!strcmp(cmd, "AT+CSQ"))
    {
        API_DEBUG("modem_registration_get_rssi()");
        int rssi = modem_registration_get_rssi();
        API_DEBUG(" = %d\n", rssi);
        API_DEBUG("modem_registration_get_error_rate()");
        double error_rate = modem_registration_get_error_rate();
        API_DEBUG(" = %lf\n", error_rate);
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
            if(mode == 3)
                st->operator_format = format;
        }
        if(mode != 0 && mode != 3)
            WRITE_ERROR(out_buf);
        else
            WRITE_OK(out_buf);
    }
    else if(!strcmp(cmd, "AT+COPS?"))
    {
        API_DEBUG("modem_registration_get_mode()");
        int mode = modem_registration_get_mode();
        API_DEBUG(" = %d\n", mode);
        int format = st->operator_format;
        char* msg = NULL;
        int len;
        if(format < 2)
        {
            API_DEBUG("modem_registration_get_operator_name(%d)", format);
            const char* oper = modem_registration_get_operator_name(format);
            API_DEBUG(" = \"%s\"\n", oper ? oper : "(null)");
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
            API_DEBUG("modem_registration_get_operator_code()");
            int oper = modem_registration_get_operator_code();
            API_DEBUG(" = %d\n", oper);
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
    else if(!strcmp(cmd, "ATD*99***1#"))
    {
        if(n_ppp < n_total)
        {
            WRITE_OK(out_buf);
            st->is_ppp = 1;
            //if we got here, we clearly weren't in PPP mode before
            n_ppp++;
        }
        else
            //no, we don't actually want to dial this number
            WRITE_ERROR(out_buf);
    }
    else if(!strncmp(cmd, "ATD", 3) && cmd[3])
    {
        char* number = cmd + 3;
        char* sc = strchr(number, ';');
        if(sc)
            *sc = 0;
        API_DEBUG("modem_call_dial(\"%s\")", number);
        if(!modem_call_dial(number))
        {
            API_DEBUG(" = 0 (OK)\n");
            WRITE_OK(out_buf);
        }
        else
        {
            API_DEBUG(" = ERROR\n");
            WRITE_ERROR(out_buf);
        }
    }
    else if(!strcmp(cmd, "ATA"))
    {
        API_DEBUG("modem_call_answer()");
        if(!modem_call_answer())
        {
            API_DEBUG(" = 0 (OK)\n");
            WRITE_OK(out_buf);
        }
        else
        {
            API_DEBUG(" = ERROR\n");
            WRITE_ERROR(out_buf);
        }
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
        API_DEBUG("modem_call_hangup()");
        if(!modem_call_hangup())
        {
            API_DEBUG(" = 0 (OK)\n");
            WRITE_OK(out_buf);
        }
        else
        {
            API_DEBUG(" = ERROR\n");
            WRITE_ERROR(out_buf);
        }
    }
    else if(!strncmp(cmd, "AT+VTS=", 7) && cmd[7])
    {
        char key = cmd[7];
        API_DEBUG("modem_call_send_dtmf('%c')", key);
        if(!modem_call_send_dtmf(key))
        {
            API_DEBUG("= 0 (OK)\n");
            WRITE_OK(out_buf);
        }
        else
        {
            API_DEBUG(" = ERROR\n");
            WRITE_ERROR(out_buf);
        }
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
        API_DEBUG("modem_sms_get_incoming_pdu(%d, ", idx);
        if(modem_sms_get_incoming_pdu(idx, &pdu, &pdu_size))
        {
            API_DEBUG(", ...) = (\"");
#ifdef API_DEBUG
            for(size_t i = 0; i < pdu_size; i++)
                API_DEBUG("\\x%02hhx", pdu[i]);
#endif
            API_DEBUG("\", %zd)\n", pdu_size);
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
    else if(!strcmp(cmd, "AT+CGDCONT=?"))
    {
        char* msg = NULL;
        int len = asprintf(&msg, "\r\n+CGDCONT: (1),\"%s\",,,(0),(0)\r\n", get_cgdcont_pdp_type());
        out_buf_write(out_buf, msg, len);
        free(msg);
        WRITE_OK(out_buf);
    }
    else if(!strncmp(cmd, "AT+CGDCONT=1,\"IP", 16))
    {
        int mask = MODEM_PROTOCOL_IPV4;
        char* apn;
        if(cmd[16] == 'V')
        {
            if(cmd[17] == '6')
            {
                if(!strncmp(cmd+18, "\",\"", 3))
                {
                    mask = MODEM_PROTOCOL_IPV6;
                    apn = cmd+21;
                }
                else
                    mask = -1;
            }
            else if(cmd[17] == '4')
            {
                if(cmd[18] == 'V')
                {
                    if(!strncmp(cmd+19, "6\",\"", 4))
                    {
                        mask |= MODEM_PROTOCOL_IPV6;
                        apn = cmd+23;
                    }
                    else
                        mask = -1;
                }
                else if(!strncmp(cmd+18, "\",\"", 3))
                    apn = cmd+21;
                else
                    mask = -1;
            }
            else
                mask = -1;
        }
        else if(!strncmp(cmd+16, "\",\"", 3))
            apn = cmd+19;
        else
            mask = -1;
        if(mask < 0)
            WRITE_ERROR(out_buf);
        else
        {
            st->ppp_protocols = mask;
            //TODO: string unescape
            char* apn = cmd+19;
            char* quote = strchr(apn, '"');
            if(quote)
                *quote = 0;
            API_DEBUG("modem_inet_set_apn(\"%s\")", apn);
            if(!modem_inet_set_apn(apn))
            {
                API_DEBUG(" = 0 (OK)\n");
                WRITE_OK(out_buf);
            }
            else
            {
                API_DEBUG(" = ERROR\n");
                WRITE_ERROR(out_buf);
            }
        }
    }
    else if(!strcmp(cmd, "AT+CGDCONT?"))
    {
        API_DEBUG("modem_inet_get_apn()");
        const char* apn = modem_inet_get_apn();
        API_DEBUG(" = \"%s\"\n", apn ? apn : "(null)");
        if(!apn)
            WRITE_ERROR(out_buf);
        else
        {
            //TODO: string escape
            char* msg = NULL;
            int len = asprintf(&msg, "\r\n+CGDCONT: 1,\"%s\",\"%s\",\"10.10.10.10\",0,0\r\n\r\nOK\r\n", get_cgdcont_pdp_type(), apn);
            out_buf_write(out_buf, msg, len);
            free(msg);
        }
    }
    else if(!strncmp(cmd, "AT+CGACT=", 9))
    {
        char* q = cmd + 10;
        char* comma = strchr(q, ',');
        *comma = 0;
        int state = atoi(q);
        q = comma + 1;
        int which = atoi(q);
        if(which == 1)
            API_DEBUG("modem_inet_set_enabled(%d)\n", state);
        if(which == 1 && !modem_inet_set_enabled(state))
        {
            API_DEBUG(" = 0 (OK)\n");
            WRITE_OK(out_buf);
        }
        else
        {
            API_DEBUG(" = ERROR\n");
            WRITE_ERROR(out_buf);
        }
    }
    else if(!strcmp(cmd, "AT+CUSD=?"))
        WRITE_OK_STRING(out_buf, "\r\n+CUSD: (1)\r\n");
    else if(!strncmp(cmd, "AT+CUSD=1,\"", 11))
    {
        char* endquote = strchr(cmd+11, '"');
        if(!endquote || (endquote-(cmd+11)) % 2)
            WRITE_ERROR(out_buf);
        else
        {
            size_t number_length = (endquote-(cmd+11)) / 2;
            char* number = malloc(number_length + 1);
            char* p = cmd + 11;
            for(size_t i = 0; i < number_length; i++)
            {
                char buf[3] = {p[0], p[1], 0};
                p += 2;
                char* ep = NULL;
                int value = strtol(buf, &ep, 16);
                if(ep != buf + 2)
                {
                    endquote = NULL;
                    break;
                }
                number[i] = value;
            }
            number[number_length] = 0;
            if(!endquote)
            {
                free(number);
                WRITE_ERROR(out_buf);
            }
            else
            {
                API_DEBUG("modem_ussd_send(\"%s\")", number);
                if(!modem_ussd_send(number))
                {
                    API_DEBUG(" = 0 (OK)\n");
                    WRITE_OK(out_buf);
                }
                else
                {
                    API_DEBUG(" = ERROR\n");
                    WRITE_ERROR(out_buf);
                }
                free(number);
            }
        }
    }
    else if(!strcmp(cmd, "AT+CUSD=2"))
    {
        API_DEBUG("modem_ussd_reset()");
        if(!modem_ussd_reset())
        {
            API_DEBUG(" = 0 (OK)\n");
            WRITE_OK(out_buf);
        }
        else
        {
            API_DEBUG(" = ERROR\n");
            WRITE_ERROR(out_buf);
        }
    }
#ifdef DEBUG_COMMAND
    else if(!strncmp(cmd, "AT+DEBUG=", 9))
    {
        char* cur = cmd+9;
        for(;;)
        {
            char* comma = strchr(cur, ',');
            if(comma)
                *comma = 0;
            char* sc = strchr(cur, ':');
            int v = 0;
            if(sc)
            {
                *sc = 0;
                v = atoi(sc+1);
            }
            int k = atoi(cur);
            modem_debug(k, v);
            if(comma)
                cur = comma + 1;
            else
                break;
        }
        WRITE_OK(out_buf);
    }
#endif
    else
        WRITE_ERROR(out_buf);
    free(cmd);
    return 1;
}

static void poll_at_commands(struct io_buf* in_buf, struct io_buf* out_buf, struct conn_status* st)
{
    st->want_retry = 0;
    size_t ppp_size;
    char* cmd; //also used to store PPP packet
    char* pdu;
    while(
        st->is_ppp
        ? (cmd = in_buf_pop_ppp(in_buf, &ppp_size, &st->is_ppp))
        : (st->want_pdu
        ? (pdu = in_buf_pop_pdu(in_buf))
        : (cmd = in_buf_peek_at(in_buf)))
    )
    {
        if(st->is_ppp)
        {
            if(handle_ppp(out_buf, cmd, ppp_size, st->ppp_protocols))
            {
                st->is_ppp = 0;
                n_ppp--;
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
#ifdef API_DEBUG
                API_DEBUG("modem_sms_send_pdu(\"");
                for(size_t i = 0; i < l; i++)
                    API_DEBUG("\\x%02hhx", pdu[i]);
                API_DEBUG(", %zd)", l);
#endif
                l = modem_sms_send_pdu(pdu, l) ? 1 : 0;
                if(l)
                    API_DEBUG(" = ERROR\n");
                else
                    API_DEBUG(" = 0 (OK)\n");
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
        else if(handle_at_command(out_buf, st, cmd))
            in_buf_pop_at(in_buf);
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
        if(connections[i] && !connections[i]->status.is_ppp)
        {
            out_buf_write(&connections[i]->out_buf, s, l);
            modem_poll_fd_set(i, &wrsocks);
        }
}

void modem_call_incoming(void)
{
    API_DEBUG("callback: modem_call_incoming()\n");
    char* msg;
    int len = format_ccwa(&msg);
    if(msg)
        broadcast_notification(msg);
    broadcast_notification("\r\n+CRING: VOICE\r\n");
    free(msg);
}

void modem_call_remote_hangup(void)
{
    API_DEBUG("callback: modem_call_remote_hangup()\n");
    broadcast_notification("\r\nNO CARRIER\r\n");
}

void modem_sms_incoming(int index)
{
    API_DEBUG("callback: modem_sms_incoming(%d)\n", index);
    char* msg = NULL;
    int len = asprintf(&msg, "\r\n+CMTI: \"ME\",%d\r\n", index);
    broadcast_notification(msg);
    free(msg);
}

void modem_inet_incoming_packet(int protocol, const char* pkt, size_t sz)
{
    API_DEBUG("callback: modem_inet_incoming_packet(%d, %p, %zd)\n", protocol, pkt, sz);
    size_t ppp_sz = sz+6;
    size_t ppp_cap = 2*ppp_sz;
    char* ppp_pkt = malloc(ppp_cap);
    memcpy(ppp_pkt, (const char[4]){0xff, 3, protocol>>8, protocol}, 4);
    memcpy(ppp_pkt+4, pkt, sz);
    ppp_crc_fix(ppp_pkt, ppp_sz);
    ppp_pkt = escape_ppp(ppp_pkt, &ppp_sz, &ppp_cap);
    for(int i = 0; i < FD_SETSIZE; i++)
        if(connections[i] && connections[i]->status.is_ppp)
        {
            out_buf_write(&connections[i]->out_buf, ppp_pkt, ppp_sz);
            modem_poll_fd_set(i, &wrsocks);
        }
    free(ppp_pkt);
}

void modem_inet_disconnected(void)
{
    API_DEBUG("callback: modem_inet_disconnected()\n");
    char ppp[32] = {0xff, 0x03, 0xc0, 0x21, 0x05, 0x00, 0x00, 0x04};
    size_t ppp_size = 10;
    size_t ppp_cap = 32;
    ppp_crc_fix(ppp, ppp_size);
    escape_ppp(ppp, &ppp_size, &ppp_cap);
    for(int i = 0; i < FD_SETSIZE; i++)
        if(connections[i] && connections[i]->status.is_ppp)
        {
            out_buf_write(&connections[i]->out_buf, ppp, ppp_size);
            modem_poll_fd_set(i, &wrsocks);
        }
}

void modem_registration_status_update(void)
{
    char* buf = NULL;
    format_creg_response("CGREG", &buf);
    broadcast_notification(buf);
    free(buf);
}

void modem_ussd_reply(int status, const char* message)
{
    char* buf = NULL;
    if(!message)
        asprintf(&buf, "\r\n+CUSD: %d\r\n", status);
    else
    {
        buf = malloc(28 + 2 * strlen(message));
        sprintf(buf, "\r\n+CUSD: %d,\"", status);
        char* p = buf + strlen(buf);
        while(*message)
        {
            sprintf(p, "%02X", (uint8_t)*message++);
            p += 2;
        }
        memcpy(p, "\",15\r\n", 7);
    }
    broadcast_notification(buf);
    free(buf);
}

static int open_pty(const char* path, struct modem_fd_set* rdsocks)
{
    int fd = open(path, O_RDWR | O_NOCTTY);
    if(fd < 0)
        return -1;
    if(!strncmp(path, "/dev/pty", 8))
    {
        char* path2 = strdup(path);
        path2[5] = 't';
        int fd2 = open(path2, O_RDWR | O_NOCTTY);
        free(path2);
        if(fd2 < 0)
        {
            close(fd);
            return -1;
        }
    }
    struct client_conn* conn = malloc(sizeof(*conn));
    memset(conn, 0, sizeof(*conn));
    conn->fd = fd;
    connections[fd] = conn;
    modem_poll_fd_set(fd, rdsocks);
    n_total++;
    //we start in non-PPP mode, do not increment n_ppp here
    return fd;
}

static void conn_close(struct client_conn* conn)
{
    //we might've died while in PPP mode
    if(conn->fd >= 0)
        close(conn->fd);
    if(conn->status.is_ppp)
        n_ppp--;
    free(conn->in_buf.data);
    free(conn->out_buf.data);
    free(conn);
    n_total--;
}

int main(int argc, const char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    if(argc == 1)
    {
        printf("usage: samsungipcd <gsm_path>...\n");
        return 1;
    }
    FD_ZERO(&rdsocks.set);
    rdsocks.maxfd = -1;
    FD_ZERO(&wrsocks.set);
    wrsocks.maxfd = -1;
    API_DEBUG("modem_init()");
    if(modem_init())
    {
        API_DEBUG(" = ERROR\n");
        fprintf(stderr, "modem init failed\n");
        return 1;
    }
    API_DEBUG(" = 0 (OK)\n");
    for(int i = 1; i < argc; i++)
    {
        int pty_fd = open_pty(argv[i], &rdsocks);
        if(pty_fd < 0)
        {
            fprintf(stderr, "open(\"%s\") failed\n", argv[i]);
            return 1;
        }
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
            if(!connections[i])
                continue;
            struct client_conn* conn = connections[i];
            if(!modem_poll_fd_isset(i, &rdreq))
            {
                if(!conn->status.want_retry)
                    continue;
            }
            else if(in_buf_populate(&conn->in_buf, conn->fd) && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                modem_poll_fd_clr(conn->fd, &rdsocks);
                modem_poll_fd_clr(conn->fd, &wrsocks);
                close(conn->fd);
                connections[conn->fd] = NULL;
                conn->fd = -1;
            }
            poll_at_commands(&conn->in_buf, &conn->out_buf, &conn->status);
            if(conn->fd < 0)
                conn_close(conn);
            else if(conn->out_buf.sz)
                modem_poll_fd_set(conn->fd, &wrsocks);
            else
                modem_poll_fd_clr(conn->fd, &wrsocks);
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
                    conn_close(conn);
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
