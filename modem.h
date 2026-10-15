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
    MODEM_USSD_NOTIFY = 0,
    MODEM_USSD_REQUEST = 1,
    MODEM_USSD_TERMINATED = 2,
    MODEM_USSD_LOCAL_REPLY = 3,
    MODEM_USSD_NOT_SUPPORTED = 4,
    MODEM_USSD_TIMEOUT = 5,
};

//i don't understand any of these abbreviations, does sipc even provide this info?
//in the comment = what modemmanager would classify this as
enum
{
    MODEM_ACCESS_TECHNOLOGY_GSM = 0, //GSM
    MODEM_ACCESS_TECHNOLOGY_GSM_COMPACT = 1, //GSM compact
    MODEM_ACCESS_TECHNOLOGY_UTRAN = 2, //UMTS
    MODEM_ACCESS_TECHNOLOGY_GSM_EGPRS = 3, //EDGE
    MODEM_ACCESS_TECHNOLOGY_UTRAN_HSDPA = 4, //HSDPA
    MODEM_ACCESS_TECHNOLOGY_UTRAN_HSUPA = 5, //HSUPA
    MODEM_ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA = 6, //HSPA
    MODEM_ACCESS_TECHNOLOGY_EUTRAN = 7, //LTE
    MODEM_ACCESS_TECHNOLOGY_ECGSM = 8, //GSM
    MODEM_ACCESS_TECHNOLOGY_EUTRAN_NB_S1 = 9, //LTE
    MODEM_ACCESS_TECHNOLOGY_EUTRA_5GCN = 10, //LTE
    MODEM_ACCESS_TECHNOLOGY_NR_5GCN = 11, //5GNR
    MODEM_ACCESS_TECHNOLOGY_NR_EPS = 12, //5GNR
    MODEM_ACCESS_TECHNOLOGY_NGRAN = 13, //5GNR & LTE
    MODEM_ACCESS_TECHNOLOGY_EUTRANR = 14, //not checked
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
const char* modem_conf_get_name(int short_name); //retried on next update if returns NULL
const char* modem_conf_get_serial(void); //better known as IMEI; retried on next update if returns NULL
int modem_conf_is_online(void);
int modem_conf_set_online(int is_online);

/* network registration */
int modem_registration_get_mode(void);
int modem_registration_get_operator_code(void);
const char* modem_registration_get_operator_name(int short_form);
int modem_registration_get_status(void); //circuit switched, 3GPP <stat> as in +CREG
int modem_registration_get_ps_status(void); //packet switched (+CGREG, and +CEREG on LTE), same values
int modem_registration_get_rssi(void); //0 for unknown
void modem_registration_get_cell_info(int* act, int* lac, int* cid, int* tac); //-1 for unknown
double modem_registration_get_error_rate(void); //-1 for unknown

/* registration-related callbacks */
void modem_registration_status_update(void);

/* calls */
int modem_call_dial(const char* number);
int modem_call_get_info(const char** number, int* state, int* direction); //bool whether the call exists
int modem_call_answer(void);
int modem_call_hangup(void);
int modem_call_send_dtmf(char which); //ascii code of the digit

/* call-related callbacks */
void modem_call_incoming(void);
void modem_call_remote_hangup(void);

/* sms */
int modem_sms_send_pdu(const char* pdu, size_t pdu_size);
int modem_sms_get_incoming_pdu(int index, const char** pdu, size_t* pdu_size); //bool whether the sms exists

/* sms-related callbacks */
void modem_sms_incoming(int index); //indices must be unique in one session

/* ussd */
int modem_ussd_send(const char* number);
int modem_ussd_reset(void);

/* ussd-related callbacks */
void modem_ussd_reply(int status, const char* message);

/* mobile internet */
int modem_inet_supports_protocol(int protocol);
int modem_inet_set_apn(const char* apn);
int modem_inet_set_username_password(const char* username, const char* password);
const char* modem_inet_get_apn(void);
int modem_inet_set_enabled(int is_enabled);
int modem_inet_get_ipv4_addresses(char ip[4], char dns1[4], char dns2[4]);
int modem_inet_get_ipv6_link_identifier(char linkid[8]); //last 8 bytes of the link-local address
int modem_inet_send_packet(int protocol, const char* pkt, size_t sz);

/* mobile internet callbacks */
void modem_inet_incoming_packet(int protocol, const char* pkt, size_t sz);
void modem_inet_connected(void); //the IP configuration has arrived, see modem_inet_get_ipv4_addresses()
void modem_inet_disconnected(void);

/* utility */
uint16_t ppp_crc(const char* data, size_t size);
void schedule_job(uint64_t after_nsec, int(*callback)(void*), void* opaque);

/* debug */
void modem_debug(int key, int value); //ifdef DEBUG_COMMAND
