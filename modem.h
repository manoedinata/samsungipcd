#pragma once
#include <stddef.h>
#include <stdint.h>

/*
This API currently assumes that the modem is a global singleton.

Lifetime of passed pointers is until the end of the current call.
Lifetime of returned pointers is until the next API call.
*/

/* constants */
enum
{
    MODEM_CALL_STATE_ACTIVE = 0,
    MODEM_CALL_STATE_HANGUP = 1,
    MODEM_CALL_STATE_DIALING = 2,
    MODEM_CALL_STATE_INCOMING = 4,
};

enum
{
    MODEM_CALL_DIRECTION_OUTGOING = 0,
    MODEM_CALL_DIRECTION_INCOMING = 1,
};

enum
{
    MODEM_REGISTRATION_NOT_REGISTERED = 0,
    MODEM_REGISTRATION_HOME_NETWORK = 1,
    MODEM_REGISTRATION_SEARCHING = 2,
    MODEM_REGISTRATION_DENIED = 3,
    MODEM_REGISTRATION_UNKNOWN = 4,
    MODEM_REGISTRATION_ROAMING = 5,
};

enum
{
    MODEM_REGISTRATION_MODE_AUTOMATIC = 0,
    MODEM_REGISTRATION_MODE_MANUAL = 1,
    MODEM_REGISTRATION_MODE_DEREGISTER = 2,
    MODEM_REGISTRATION_MODE_MANUAL_AUTOMATIC = 4,
};

enum
{
    MODEM_PPP_PROTOCOL_IPV4 = 0x21,
    MODEM_PPP_PROTOCOL_IPV6 = 0x57,
};

/* init */
int modem_init(void);

/* polling logic for select */
struct modem_fd_set;
int modem_poll_update_fds(struct modem_fd_set* rdset, struct modem_fd_set* wrset);
int modem_poll_poll(const struct modem_fd_set* rdset, const struct modem_fd_set* wrset);

/* polling callbacks */

void modem_poll_fd_set(int fd, struct modem_fd_set* set);
void modem_poll_fd_clr(int fd, struct modem_fd_set* set);
int modem_poll_fd_isset(int fd, const struct modem_fd_set* set);

/* generic configuration */
const char* modem_conf_get_name(int short_name);
const char* modem_conf_get_serial(void);
int modem_conf_is_online(void);
int modem_conf_set_online(int is_online);

/* network registration */
int modem_registration_get_mode(void);
int modem_registration_get_operator_code(void);
const char* modem_registration_get_operator_name(int short_form);
int modem_registration_get_status(void);
int modem_registration_get_rssi(void); //0 for unknown
double modem_registration_get_error_rate(void); //-1 for unknown

/* calls */
int modem_call_dial(const char* number);
int modem_call_get_info(const char** number, int* state, int* direction); //bool whether the call exists
int modem_call_answer(void);
int modem_call_hangup(void);

/* call-related callbacks */
void modem_call_incoming(void);
void modem_call_remote_hangup(void);

/* sms */
int modem_sms_send_pdu(const char* pdu, size_t pdu_size);
int modem_sms_get_incoming_pdu(int index, const char** pdu, size_t* pdu_size); //bool whether the sms exists

/* sms-related callbacks */
void modem_sms_incoming(int index); //indices must be unique in one session

/* mobile internet */
int modem_inet_set_apn(const char* apn);
const char* modem_inet_get_apn(void);
int modem_inet_set_enabled(int is_enabled);
int modem_inet_get_ipv4_address(char ip[4]);
int modem_inet_send_packet(int protocol, const char* pkt, size_t sz);

/* mobile internet callbacks */
void modem_inet_incoming_packet(int protocol, const char* pkt, size_t sz);

/* utility */
uint16_t ppp_crc(const char* data, size_t size);
