/*
Copyright 2022 Sergey Lisov

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#include "modem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int modem_init(void)
{
    printf("> ");
    fflush(stdout);
    return 0;
}

static char* call_number = NULL;
static int call_direction = -1;
static int call_state = -1;
static int is_online = 0;
static int sms_index = -1;
static char* sms_pdu = NULL;
static size_t sms_pdu_len = 0;
static char* apn = NULL;
static char* username = NULL;
static char* password = NULL;

static void set_number(const char* new_number)
{
    if(call_number)
        free(call_number);
    call_number = new_number ? strdup(new_number) : NULL;
}

static void set_pdu(const char* new_pdu, size_t new_len)
{
    if(sms_pdu)
        free(sms_pdu);
    sms_pdu_len = new_len;
    if(new_len)
    {
        sms_pdu = malloc(new_len);
        memcpy(sms_pdu, new_pdu, new_len);
    }
    else
        sms_pdu = NULL;
}

static void set_apn(const char* new_apn)
{
    if(apn)
        free(apn);
    apn = new_apn ? strdup(new_apn) : NULL;
}

static void set_username(const char* new_username)
{
    if(username)
        free(username);
    username = new_username ? strdup(new_username) : NULL;
}

static void set_password(const char* new_password)
{
    if(password)
        free(password);
    password = new_password ? strdup(new_password) : NULL;
}

const char* modem_conf_get_name(int short_name)
{
    return short_name ? "Fake" : "Fake modem (simulator)";
}

const char* modem_conf_get_serial(void)
{
    return "0000FACE";
}

int modem_call_dial(const char* number)
{
    if(!is_online)
    {
        printf("\rCannot dial while offline\n> ");
        fflush(stdout);
        return -1;
    }
    printf("\rDialing %s...\n> ", number);
    fflush(stdout);
    set_number(number);
    call_direction = MODEM_CALL_DIRECTION_OUTGOING;
    call_state = MODEM_CALL_STATE_DIALING;
    return 0;
}

int modem_call_get_info(const char** number, int* state, int* direction)
{
    if(call_direction < 0 || call_state < 0)
        return 0;
    *number = call_number;
    *state = call_state;
    *direction = call_direction;
    return 1;
}

static void do_hangup(void)
{
    set_number(NULL);
    call_direction = -1;
    call_state = -1;
}

int modem_call_hangup(void)
{
    if(call_state < 0)
    {
        printf("\rCannot hang up without a call\n> ");
        fflush(stdout);
        return -1;
    }
    printf("\rHanging up...\n> ");
    fflush(stdout);
    do_hangup();
    return 0;
}

int modem_call_answer(void)
{
    if(call_state != MODEM_CALL_STATE_INCOMING)
    {
        printf("\rCannot answer without an incoming call\n> ");
        fflush(stdout);
        return -1;
    }
    printf("\rAnswering call...\n> ");
    fflush(stdout);
    call_state = MODEM_CALL_STATE_ACTIVE;
    return 0;
}

int modem_poll_update_fds(struct modem_fd_set* a, struct modem_fd_set* b)
{
    modem_poll_fd_set(0, a);
    return 0;
}

int modem_conf_set_online(int online)
{
    if(online)
    {
        printf("\rGoing online...\n> ");
        is_online = 1;
    }
    else
    {
        printf("\rGoing offline...\n> ");
        do_hangup();
        is_online = 0;
    }
    fflush(stdout);
    modem_registration_status_update();
    return 0;
}

int modem_conf_is_online(void)
{
    return is_online;
}

int modem_registration_get_status(void)
{
    return is_online ? MODEM_REGISTRATION_HOME_NETWORK : MODEM_REGISTRATION_NOT_REGISTERED;
}

int modem_registration_get_rssi(void)
{
    return is_online ? -75 : 0;
}

double modem_registration_get_error_rate(void)
{
    return is_online ? 0.001 : -1;
}

int modem_registration_get_mode(void)
{
    return is_online ? 0 : -1;
}

const char* modem_registration_get_operator_name(int is_short)
{
    return is_online ? is_short ? "Fake operator" : "FAKE" : NULL;
}

int modem_registration_get_operator_code(void)
{
    return is_online ? 12345 : -1;
}

int modem_sms_send_pdu(const char* pdu, size_t size)
{
    if(!is_online)
    {
        printf("\rCan not send SMS while offline\n> ");
        fflush(stdout);
        return -1;
    }
    printf("\rSMS being sent:");
    for(size_t i = 0; i < size; i++)
        printf(" %02hhx", pdu[i]);
    printf("\n> ");
    fflush(stdout);
    return 0;
}

int modem_sms_get_incoming_pdu(int index, const char** pdu, size_t* pdu_size)
{
    if(index < 0 || index != sms_index)
        return 0;
    *pdu = sms_pdu;
    *pdu_size = sms_pdu_len;
    return 1;
}

int modem_inet_set_apn(const char* apn)
{
    printf("\rAPN: %s\n> ", apn);
    fflush(stdout);
    set_apn(apn);
    return 0;
}

int modem_inet_set_username_password(const char* username, const char* password)
{
    printf("\rUsername: %s\nPassword: %s\n> ", username, password);
    fflush(stdout);
    set_username(username);
    set_password(password);
    return 0;
}

const char* modem_inet_get_apn()
{
    printf("\rAPN queried: %s\n> ", apn);
    fflush(stdout);
    return apn;
}

int modem_inet_set_enabled(int is_enabled)
{
    if(is_enabled && !is_online)
    {
        printf("\rCannot enable mobile internet while offline\n> ");
        fflush(stdout);
        return -1;
    }
    else if(is_enabled)
        printf("\rEnabling mobile internet...\n> ");
    else
        printf("\rDisabling mobile internet...\n> ");
    fflush(stdout);
    return 0;
}

int modem_inet_get_ipv4_addresses(char buf[4], char buf2[4], char buf3[4])
{
    memset(buf, 10, 4);
    memset(buf2, 11, 4);
    memset(buf3, 12, 4);
    return 0;
}

int modem_inet_send_packet(int protocol, const char* pkt, size_t sz)
{
    char* pkt2 = malloc(sz);
    memcpy(pkt2, pkt, sz);
    if(protocol != MODEM_PPP_PROTOCOL_IPV4)
        return 0;
    for(int i = 0; i < 4; i++)
    {
        char q = pkt2[i+12];
        pkt2[i+12] = pkt2[i+16];
        pkt2[i+16] = q;
    }
    pkt2[20] -= 8;
    pkt2[22] += 8;
    modem_inet_incoming_packet(protocol, pkt2, sz);
    free(pkt2);
    return 0;
}

int modem_poll_poll(const struct modem_fd_set* a, const struct modem_fd_set* b)
{
    static char* line = NULL;
    static size_t linesz = 0;
    if(modem_poll_fd_isset(0, a))
    {
        ssize_t l = getline(&line, &linesz, stdin);
        if(l < 0)
            return 0;
        line[l] = 0;
        while(l && line[l-1] == '\n')
            line[--l] = 0;
        if(!strcmp(line, "help"))
            printf("Commands: help, answer, hangup, incoming, sms\n");
        else if(!strcmp(line, "answer"))
        {
            if(call_state == MODEM_CALL_STATE_DIALING)
                call_state = MODEM_CALL_STATE_ACTIVE;
            else if(call_state == MODEM_CALL_STATE_INCOMING)
                printf("Only the user can answer incoming call\n");
            else
                printf("Cannot answer without a call\n");
        }
        else if(!strcmp(line, "hangup"))
        {
            if(call_state == MODEM_CALL_STATE_ACTIVE)
            {
                do_hangup();
                modem_call_remote_hangup();
            }
            else if(call_state >= 0)
                do_hangup();
            else
                printf("Cannot hang up without a call\n");
        }
        else if(!strncmp(line, "incoming ", 9))
        {
            if(call_state >= 0)
                printf("Cannot get incoming call while talking\n");
            else if(!is_online)
                printf("Cannot get incoming call while offline\n");
            else
            {
                set_number(line+9);
                call_state = MODEM_CALL_STATE_INCOMING;
                call_direction = MODEM_CALL_DIRECTION_INCOMING;
                modem_call_incoming();
            }
        }
        else if(!strcmp(line, "incoming"))
            printf("Usage: incoming <number>\n");
        else if(!strncmp(line, "sms ", 4))
        {
            char* pdu = line + 4;
            size_t l = strlen(pdu);
            if(l % 2 != 0)
                printf("PDU hex length must be even\n");
            else
            {
                l /= 2;
                int valid = 1;
                for(size_t i = 0; i < l && valid; i++)
                {
                    char buf[3] = {pdu[2*i], pdu[2*i+1], 0};
                    char* endp = NULL;
                    int byte = strtol(buf, &endp, 16);
                    if(endp != buf + 2)
                    {
                        valid = 0;
                        printf("PDU contains non-hex characters\n");
                    }
                    pdu[i] = byte;
                }
                if(valid)
                {
                    if(!is_online)
                        printf("Cannot receive SMS while offline\n");
                    else
                    {
                        set_pdu(pdu, l);
                        modem_sms_incoming(++sms_index);
                    }
                }
            }
        }
        else if(!strcmp(line, "sms"))
            printf("Usage: sms <pdu>\n");
        else
            printf("Unknown command: %s\n", line);
        printf("> ");
        fflush(stdout);
    }
    return 0;
}
