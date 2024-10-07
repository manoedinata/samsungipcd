#pragma once
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "modem.h"

struct io_buf
{
    char* data;
    size_t sz;
    size_t cap;
};

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
#ifndef PPP_HOOK
    if(b->sz >= 2 && b->data[0] == 'A' && b->data[1] == 'T')
    {
        //a valid PPP packet never starts with "AT"
        //if we receive this, the PPP connection must've been abruptly terminated
        *is_ppp = 0;
        n_ppp--;
        return in_buf_peek_at(b);
    }
#endif
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
