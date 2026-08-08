/**
 * (C) 2007-22 - ntop.org and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not see see <http://www.gnu.org/licenses/>
 *
 */


#include <errno.h>           // for errno
#include <stdarg.h>          // for va_end, va_list, va_start
#include <stdbool.h>
#include <stdlib.h>          // for free, atoi, calloc, strtol
#include <string.h>          // for memcmp, memcpy, memset, strlen, strerror
#include <sys/time.h>        // for gettimeofday, timeval
#include <time.h>            // for time, localtime, strftime
#include "config.h"          // for PACKAGE_BUILDDATE, PACKA...
#include "n2n.h"
#include "n2n_wire.h"
#include "random_numbers.h"  // for n2n_rand
#include "sn_selection.h"    // for sn_selection_criterion_default
#include "uthash.h"          // for UT_hash_handle, HASH_DEL, HASH_ITER, HAS...

#ifdef HAVE_LIBPTHREAD
#include <pthread.h>
#endif

#ifdef _WIN32
#include "win32/defs.h"
#include <ws2def.h>
#else
#include <arpa/inet.h>       // for inet_ntop
#include <netdb.h>           // for addrinfo, freeaddrinfo, gai_strerror
#include <sys/socket.h>      // for AF_INET, PF_INET, bind, setsockopt, shut...
#include <syslog.h>          // for closelog, openlog, syslog, LOG_DAEMON
#endif


/* ************************************** */

SOCKET open_socket_bind (const n2n_sock_t *local_address,
                         int type /* 0 = UDP, TCP otherwise */) {

    SOCKET sock_fd;
    struct sockaddr_storage local_sockaddr;
    socklen_t local_sockaddr_len;
    int sockopt;

    if(!local_address
       || ((local_address->family != AF_INET) && (local_address->family != AF_INET6))) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    local_sockaddr_len = fill_sockaddr_for_family((struct sockaddr *)&local_sockaddr,
                                                   sizeof(local_sockaddr),
                                                   local_address,
                                                   local_address->family);
    if(!local_sockaddr_len) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    if((int)(sock_fd = socket(local_address->family,
                              ((type == 0) ? SOCK_DGRAM : SOCK_STREAM), 0)) < 0) {
        traceEvent(TRACE_ERROR, "Unable to create socket for address family %u [%s][%d]\n",
                   local_address->family, strerror(errno), sock_fd);
        return(-1);
    }

#ifndef _WIN32
    /* fcntl(sock_fd, F_SETFL, O_NONBLOCK); */
#endif

    sockopt = 1;
    setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&sockopt, sizeof(sockopt));

#ifdef IPV6_V6ONLY
    if(local_address->family == AF_INET6) {
        /* One IPv6 socket can also carry IPv4 peers as mapped addresses. */
        sockopt = 0;
        if(setsockopt(sock_fd, IPPROTO_IPV6, IPV6_V6ONLY,
                      (char *)&sockopt, sizeof(sockopt)) != 0) {
            traceEvent(TRACE_WARNING, "Unable to enable IPv4 traffic on IPv6 socket [%s]\n",
                       strerror(errno));
        }
    }
#endif

    if(bind(sock_fd, (struct sockaddr *)&local_sockaddr, local_sockaddr_len) == -1) {
        traceEvent(TRACE_ERROR, "Bind error on local port %u [%s]\n",
                   local_address->port, strerror(errno));
        closesocket(sock_fd);
        return(-1);
    }

    return(sock_fd);
}


SOCKET open_socket (int local_port, in_addr_t address,
                    int type /* 0 = UDP, TCP otherwise */) {

    n2n_sock_t local_address;
    uint32_t address_network_order;

    memset(&local_address, 0, sizeof(local_address));
    local_address.family = AF_INET;
    local_address.port = local_port;
    address_network_order = htonl(address);
    memcpy(local_address.addr.v4, &address_network_order, IPV4_SIZE);

    return open_socket_bind(&local_address, type);
}


static int traceLevel = 2 /* NORMAL */;
static int useSyslog = 0;
#ifndef _WIN32
static int syslog_opened = 0;
#endif
static FILE *traceFile = NULL;

int getTraceLevel () {

    return(traceLevel);
}

void setTraceLevel (int level) {

    traceLevel = level;
}

void setUseSyslog (int use_syslog) {

    useSyslog = use_syslog;
}

void setTraceFile (FILE *f) {

    traceFile = f;
}

void closeTraceFile () {

    if((traceFile != NULL) && (traceFile != stdout)) {
        fclose(traceFile);
    }
#ifndef _WIN32
    if(useSyslog && syslog_opened) {
        closelog();
        syslog_opened = 0;
    }
#endif
}

#define N2N_TRACE_DATESIZE 32
void _traceEvent (int eventTraceLevel, char* file, int line, char * format, ...) {

    va_list va_ap;

    if(traceFile == NULL) {
        traceFile = stdout;
    }

    if(eventTraceLevel <= traceLevel) {
        char buf[1024];
        char out_buf[1280];
        char theDate[N2N_TRACE_DATESIZE];
        char *extra_msg = "";
        time_t theTime = time(NULL);
        int i;

        /* We have two paths - one if we're logging, one if we aren't
         * Note that the no-log case is those systems which don't support it(WIN32),
         * those without the headers !defined(USE_SYSLOG)
         * those where it's parametrically off...
         */

        memset(buf, 0, sizeof(buf));
        strftime(theDate, N2N_TRACE_DATESIZE, "%d/%b/%Y %H:%M:%S", localtime(&theTime));

        va_start(va_ap, format);
        vsnprintf(buf, sizeof(buf) - 1, format, va_ap);
        va_end(va_ap);

        if(eventTraceLevel == 0 /* TRACE_ERROR */) {
            extra_msg = "ERROR: ";
        } else if(eventTraceLevel == 1 /* TRACE_WARNING */) {
            extra_msg = "WARNING: ";
        }

        while(buf[strlen(buf) - 1] == '\n') {
            buf[strlen(buf) - 1] = '\0';
        }

#ifndef _WIN32
        if(useSyslog) {
            if(!syslog_opened) {
                openlog("n2n", LOG_PID, LOG_DAEMON);
                syslog_opened = 1;
            }

            snprintf(out_buf, sizeof(out_buf), "%s%s", extra_msg, buf);
            syslog(LOG_INFO, "%s", out_buf);
        } else {
#endif
            for(i = strlen(file) - 1; i > 0; i--) {
                if((file[i] == '/') || (file[i] == '\\')) {
                    i++;
                    break;
                }
            }
            snprintf(out_buf, sizeof(out_buf), "%s [%s:%d] %s%s", theDate, &file[i], line, extra_msg, buf);
            fprintf(traceFile, "%s\n", out_buf);
            fflush(traceFile);
#ifndef _WIN32
        }
#endif
    }

}


/* *********************************************** */


/* stringify in_addr type to ipstr_t */
char* inaddrtoa (ipstr_t out, struct in_addr addr) {

    if(!inet_ntop(AF_INET, &addr, out, sizeof(ipstr_t)))
        out[0] = '\0';

    return out;
}


/* addr should be in network order. Things are so much simpler that way. */
char* intoa (uint32_t /* host order */ addr, char* buf, uint16_t buf_len) {

    char *cp, *retStr;
    uint8_t byteval;
    int n;

    cp = &buf[buf_len];
    *--cp = '\0';

    n = 4;
    do {
        byteval = addr & 0xff;
        *--cp = byteval % 10 + '0';
        byteval /= 10;
        if(byteval > 0) {
            *--cp = byteval % 10 + '0';
            byteval /= 10;
            if(byteval > 0) {
                *--cp = byteval + '0';
            }
        }
        *--cp = '.';
        addr >>= 8;
    } while(--n > 0);

    /* Convert the string to lowercase */
    retStr = (char*)(cp + 1);

    return(retStr);
}


/** Convert subnet prefix bit length to host order subnet mask. */
uint32_t bitlen2mask (uint8_t bitlen) {

    uint8_t i;
    uint32_t mask = 0;

    for (i = 1; i <= bitlen; ++i) {
        mask |= 1 << (32 - i);
    }

    return mask;
}


/** Convert host order subnet mask to subnet prefix bit length. */
uint8_t mask2bitlen (uint32_t mask) {

    uint8_t i, bitlen = 0;

    for (i = 0; i < 32; ++i) {
        if((mask << i) & 0x80000000) {
            ++bitlen;
        } else {
            break;
        }
    }

    return bitlen;
}


/* *********************************************** */

char * macaddr_str (macstr_t buf,
                    const n2n_mac_t mac) {

    snprintf(buf, N2N_MACSTR_SIZE, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0] & 0xFF, mac[1] & 0xFF, mac[2] & 0xFF,
             mac[3] & 0xFF, mac[4] & 0xFF, mac[5] & 0xFF);

    return(buf);
}

/* *********************************************** */

static int parse_port_number (const char *text, uint16_t *port) {

    char *end = NULL;
    long value;

    if(!text || !text[0])
        return -1;

    errno = 0;
    value = strtol(text, &end, 10);
    if(errno || !end || end[0] || (value < 1) || (value > 65535))
        return -1;

    *port = (uint16_t)value;
    return 0;
}


static int split_supernode_address (const char *spec,
                                    char *host, size_t host_size,
                                    char *port, size_t port_size) {

    const char *host_start = spec;
    const char *host_end;
    const char *port_start;
    const char *first_colon;
    const char *last_colon;
    size_t host_len;

    if(!spec || !spec[0])
        return -1;

    if(spec[0] == '[') {
        host_start = spec + 1;
        host_end = strchr(host_start, ']');
        if(!host_end || (host_end == host_start) || (host_end[1] != ':'))
            return -1;
        port_start = host_end + 2;
    } else {
        first_colon = strchr(spec, ':');
        last_colon = strrchr(spec, ':');
        /* IPv6 literals with a port must use [address]:port. */
        if(!last_colon || (first_colon != last_colon) || (last_colon == spec))
            return -1;
        host_end = last_colon;
        port_start = last_colon + 1;
    }

    host_len = (size_t)(host_end - host_start);
    if((host_len >= host_size) || (strlen(port_start) >= port_size))
        return -1;

    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    strcpy(port, port_start);
    return 0;
}


int parse_bind_address (n2n_sock_t *sock, const char *spec,
                        uint16_t default_port, uint8_t default_family) {

    char work[N2N_EDGE_SN_HOST_SIZE];
    char *host = work;
    char *port_text = NULL;
    char *closing_bracket;
    char *first_colon;
    char *last_colon;
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *entry;
    uint16_t port = default_port;
    size_t len;

    if(!sock || !spec)
        return -1;

    len = strlen(spec);
    if(!len || (len >= sizeof(work)))
        return -1;

    memset(sock, 0, sizeof(*sock));
    memcpy(work, spec, len + 1);

    if(work[0] == '[') {
        host = work + 1;
        closing_bracket = strchr(host, ']');
        if(!closing_bracket)
            return -1;
        if(closing_bracket[1]) {
            if(closing_bracket[1] != ':')
                return -1;
            port_text = closing_bracket + 2;
        }
        *closing_bracket = '\0';
    } else {
        first_colon = strchr(work, ':');
        last_colon = strrchr(work, ':');
        if(first_colon && (first_colon == last_colon)) {
            *last_colon = '\0';
            port_text = last_colon + 1;
        } else if(strspn(work, "0123456789") == strlen(work)) {
            port_text = work;
            host = work + strlen(work);
        }
    }

    if(port_text && (parse_port_number(port_text, &port) != 0))
        return -1;

    sock->port = port;
    if(!host[0]) {
        sock->family = default_family;
        return ((default_family == AF_UNSPEC) || (default_family == AF_INET)
                || (default_family == AF_INET6)) ? 0 : -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST;
    if(getaddrinfo(host, NULL, &hints, &addresses) != 0)
        return -1;

    for(entry = addresses; entry; entry = entry->ai_next) {
        if(((entry->ai_family == AF_INET) || (entry->ai_family == AF_INET6))
           && (fill_n2nsock(sock, entry->ai_addr) == 0)) {
            sock->port = port;
            freeaddrinfo(addresses);
            return 0;
        }
    }

    freeaddrinfo(addresses);
    return -1;
}


/** Resolve an IPv4 or IPv6 supernode address. IPv6 literals use [addr]:port. */
int supernode2sock (n2n_sock_t *sn, const char *addrIn) {

    char supernode_host[N2N_EDGE_SN_HOST_SIZE];
    char supernode_port[6];
    uint16_t port;
    int nameerr;
    struct addrinfo aihints;
    struct addrinfo *ainfo = NULL;
    struct addrinfo *entry;
    static const int preferred_families[] = {AF_INET, AF_INET6};
    size_t family_index;

    if(!sn)
        return -4;

    memset(sn, 0, sizeof(*sn));
    sn->family = AF_INVALID;

    if(!addrIn || (strlen(addrIn) >= N2N_EDGE_SN_HOST_SIZE)) {
        traceEvent(TRACE_WARNING, "supernode address is empty or too long");
        return -3;
    }

    if(split_supernode_address(addrIn, supernode_host, sizeof(supernode_host),
                               supernode_port, sizeof(supernode_port)) != 0
       || (parse_port_number(supernode_port, &port) != 0)) {
        traceEvent(TRACE_WARNING,
                   "supernode2sock sees malformed supernode parameter (-l <host:port>) %s",
                   addrIn ? addrIn : "(null)");
        return -3;
    }

    memset(&aihints, 0, sizeof(aihints));
    aihints.ai_family = AF_UNSPEC;
    aihints.ai_socktype = SOCK_DGRAM;
    nameerr = getaddrinfo(supernode_host, supernode_port, &aihints, &ainfo);
    if(nameerr != 0) {
        traceEvent(TRACE_WARNING, "supernode2sock fails to resolve supernode host %s, %d: %s",
                   supernode_host, nameerr, gai_strerror(nameerr));
        return -2;
    }

    /* Keep IPv4 preference for dual-address DNS names, then fall back to IPv6. */
    for(family_index = 0;
        family_index < sizeof(preferred_families) / sizeof(preferred_families[0]);
        ++family_index) {
        for(entry = ainfo; entry; entry = entry->ai_next) {
            if((entry->ai_family == preferred_families[family_index])
               && (fill_n2nsock(sn, entry->ai_addr) == 0)) {
                traceEvent(TRACE_INFO, "supernode2sock resolved IPv%s address for %s",
                           (sn->family == AF_INET6) ? "6" : "4", supernode_host);
                freeaddrinfo(ainfo);
                return 0;
            }
        }
    }

    freeaddrinfo(ainfo);
    sn->family = AF_INVALID;
    traceEvent(TRACE_WARNING, "supernode2sock found no usable address for %s", supernode_host);
    return -1;
}


#ifdef HAVE_LIBPTHREAD
N2N_THREAD_RETURN_DATATYPE resolve_thread(N2N_THREAD_PARAMETER_DATATYPE p) {

    n2n_resolve_parameter_t *param = (n2n_resolve_parameter_t*)p;
    n2n_resolve_ip_sock_t   *entry, *tmp_entry;
    time_t                  rep_time = N2N_RESOLVE_INTERVAL / 10;
    time_t                  now;

    while(1) {
        sleep(N2N_RESOLVE_INTERVAL / 60); /* wake up in-between to check for signaled requests */

        // what's the time?
        now = time(NULL);

        // lock access
        pthread_mutex_lock(&param->access);

        // is it time to resolve yet?
        if(((param->request)) || ((now - param->last_resolved) > rep_time)) {
            HASH_ITER(hh, param->list, entry, tmp_entry) {
                // resolve
                entry->error_code = supernode2sock(&entry->sock, entry->org_ip);
                // if socket changed and no error
                if(!sock_equal(&entry->sock, entry->org_sock)
                  && (!entry->error_code)) {
                    // flag the change
                    param->changed = 1;
               }
            }
            param->last_resolved = now;

            // any request fulfilled
            param->request = 0;

            // determine next resolver repetition (shorter time if resolver errors occured)
            rep_time = N2N_RESOLVE_INTERVAL;
            HASH_ITER(hh, param->list, entry, tmp_entry) {
                if(entry->error_code) {
                    rep_time = N2N_RESOLVE_INTERVAL / 10;
                    break;
                }
            }
        }

        // unlock access
        pthread_mutex_unlock(&param->access);
    }
}
#endif


int resolve_create_thread (n2n_resolve_parameter_t **param, struct peer_info *sn_list) {

#ifdef HAVE_LIBPTHREAD
    struct peer_info        *sn, *tmp_sn;
    n2n_resolve_ip_sock_t   *entry;
    int                     ret;

    // create parameter structure
    *param = (n2n_resolve_parameter_t*)calloc(1, sizeof(n2n_resolve_parameter_t));
    if(*param) {
        HASH_ITER(hh, sn_list, sn, tmp_sn) {
            // create entries for those peers that come with ip_addr string (from command-line)
            if(sn->ip_addr) {
                entry = (n2n_resolve_ip_sock_t*)calloc(1, sizeof(n2n_resolve_ip_sock_t));
                if(entry) {
                    entry->org_ip = sn->ip_addr;
                    entry->org_sock = &(sn->sock);
                    memcpy(&(entry->sock), &(sn->sock), sizeof(n2n_sock_t));
                    HASH_ADD(hh, (*param)->list, org_ip, sizeof(char*), entry);
                } else
                    traceEvent(TRACE_WARNING, "resolve_create_thread was unable to add list entry for supernode '%s'", sn->ip_addr);
            }
        }
        (*param)->check_interval = N2N_RESOLVE_CHECK_INTERVAL;
    } else {
        traceEvent(TRACE_WARNING, "resolve_create_thread was unable to create list of supernodes");
        return -1;
    }

    // create thread
    ret = pthread_create(&((*param)->id), NULL, resolve_thread, (void *)*param);
    if(ret) {
        traceEvent(TRACE_WARNING, "resolve_create_thread failed to create resolver thread with error number %d", ret);
        return -1;
    }

    pthread_mutex_init(&((*param)->access), NULL);

    return 0;
#else
    return -1;
#endif
}


void resolve_cancel_thread (n2n_resolve_parameter_t *param) {

#ifdef HAVE_LIBPTHREAD
    pthread_cancel(param->id);
    free(param);
#endif
}


uint8_t resolve_check (n2n_resolve_parameter_t *param, uint8_t requires_resolution, time_t now) {

    uint8_t ret = requires_resolution; /* if trylock fails, it still requires resolution */

#ifdef HAVE_LIBPTHREAD
    n2n_resolve_ip_sock_t   *entry, *tmp_entry;
    n2n_sock_str_t sock_buf;

    if(NULL == param)
        return ret;

    // check_interval and last_check do not need to be guarded by the mutex because
    // their values get changed and evaluated only here

    if((now - param->last_checked > param->check_interval) || (requires_resolution)) {
        // try to lock access
        if(pthread_mutex_trylock(&param->access) == 0) {
            // any changes?
            if(param->changed) {
                // reset flag
                param->changed = 0;
                // unselectively copy all socks (even those with error code, that would be the old one because
                // sockets do not get overwritten in case of error in resolve_thread) from list to supernode list
                HASH_ITER(hh, param->list, entry, tmp_entry) {
                    memcpy(entry->org_sock, &entry->sock, sizeof(n2n_sock_t));
                    traceEvent(TRACE_INFO, "resolve_check renews ip address of supernode '%s' to %s",
                                           entry->org_ip, sock_to_cstr(sock_buf, &(entry->sock)));
               }
            }

            // let the resolver thread know eventual difficulties in reaching the supernode
            if(requires_resolution) {
                param->request = 1;
                ret = 0;
            }

            param->last_checked = now;

            // next appointment
            if(param->request)
                // earlier if resolver still working on fulfilling a request
                param->check_interval = N2N_RESOLVE_CHECK_INTERVAL / 10;
            else
                param->check_interval = N2N_RESOLVE_CHECK_INTERVAL;

            // unlock access
            pthread_mutex_unlock(&param->access);
        }
    }
#endif

    return ret;
}


/* ************************************** */


struct peer_info* add_sn_to_list_by_mac_or_sock (struct peer_info **sn_list, n2n_sock_t *sock, const n2n_mac_t mac, int *skip_add) {

    struct peer_info *scan, *tmp, *peer = NULL;

    if(!is_null_mac(mac)) { /* not zero MAC */
        HASH_FIND_PEER(*sn_list, mac, peer);
    }

    if(peer == NULL) { /* zero MAC, search by socket */
        HASH_ITER(hh, *sn_list, scan, tmp) {
            if(sock_equal(&(scan->sock), sock)) {
                // update mac if appropriate, needs to be deleted first because it is key to the hash list
                if(!is_null_mac(mac)) {
                    HASH_DEL(*sn_list, scan);
                    memcpy(scan->mac_addr, mac, sizeof(n2n_mac_t));
                    HASH_ADD_PEER(*sn_list, scan);
                }
                peer = scan;
                break;
            }
        }

        if((peer == NULL) && (*skip_add == SN_ADD)) {
            peer = (struct peer_info*)calloc(1, sizeof(struct peer_info));
            if(peer) {
                sn_selection_criterion_default(&(peer->selection_criterion));
                peer->last_valid_time_stamp = initial_time_stamp();
                peer->purgeable = true;
                memcpy(&(peer->sock), sock, sizeof(n2n_sock_t));
                memcpy(peer->mac_addr, mac, sizeof(n2n_mac_t));
                HASH_ADD_PEER(*sn_list, peer);
                *skip_add = SN_ADD_ADDED;
            }
        }
    }

    return peer;
}

/* ************************************************ */


/* http://www.faqs.org/rfcs/rfc908.html */
uint8_t is_multi_broadcast (const n2n_mac_t dest_mac) {

    int is_broadcast = (memcmp(broadcast_mac, dest_mac, N2N_MAC_SIZE) == 0);
    int is_multicast = (memcmp(multicast_mac, dest_mac, 3) == 0) && !(dest_mac[3] >> 7);
    int is_ipv6_multicast = (memcmp(ipv6_multicast_mac, dest_mac, 2) == 0);

    return is_broadcast || is_multicast || is_ipv6_multicast;
}


uint8_t is_broadcast (const n2n_mac_t dest_mac) {

    int is_broadcast = (memcmp(broadcast_mac, dest_mac, N2N_MAC_SIZE) == 0);

    return is_broadcast;
}


uint8_t is_null_mac (const n2n_mac_t dest_mac) {

    int is_null_mac = (memcmp(null_mac, dest_mac, N2N_MAC_SIZE) == 0);

    return is_null_mac;
}


/* *********************************************** */

char* msg_type2str (uint16_t msg_type) {

    switch(msg_type) {
        case MSG_TYPE_REGISTER: return("MSG_TYPE_REGISTER");
        case MSG_TYPE_DEREGISTER: return("MSG_TYPE_DEREGISTER");
        case MSG_TYPE_PACKET: return("MSG_TYPE_PACKET");
        case MSG_TYPE_REGISTER_ACK: return("MSG_TYPE_REGISTER_ACK");
        case MSG_TYPE_REGISTER_SUPER: return("MSG_TYPE_REGISTER_SUPER");
        case MSG_TYPE_REGISTER_SUPER_ACK: return("MSG_TYPE_REGISTER_SUPER_ACK");
        case MSG_TYPE_REGISTER_SUPER_NAK: return("MSG_TYPE_REGISTER_SUPER_NAK");
        case MSG_TYPE_FEDERATION: return("MSG_TYPE_FEDERATION");
        default: return("???");
    }

    return("???");
}

/* *********************************************** */

void hexdump (const uint8_t *buf, size_t len) {

    size_t i;

    if(0 == len) {
        return;
    }

    printf("-----------------------------------------------\n");
    for(i = 0; i < len; i++) {
        if((i > 0) && ((i % 16) == 0)) {
            printf("\n");
        }
        printf("%02X ", buf[i] & 0xFF);
    }
    printf("\n");
    printf("-----------------------------------------------\n");
}


/* *********************************************** */

void print_n2n_version () {

    printf("Welcome to n2n v.%s\n"
           "Built on %s\n"
           "Copyright 2007-2022 - ntop.org and contributors\n\n",
           PACKAGE_VERSION, PACKAGE_BUILDDATE);
}

/* *********************************************** */

size_t purge_expired_nodes (struct peer_info **peer_list,
                            SOCKET socket_not_to_close,
                            n2n_tcp_connection_t **tcp_connections,
                            time_t *p_last_purge,
                            int frequency, int timeout) {

    time_t now = time(NULL);
    size_t num_reg = 0;

    if((now - (*p_last_purge)) < frequency) {
        return 0;
    }

    traceEvent(TRACE_DEBUG, "Purging old registrations");

    num_reg = purge_peer_list(peer_list, socket_not_to_close, tcp_connections, now - timeout);

    (*p_last_purge) = now;
    traceEvent(TRACE_DEBUG, "Remove %ld registrations", num_reg);

    return num_reg;
}

/** Purge old items from the peer_list, eventually close the related socket, and
  * return the number of items that were removed. */
size_t purge_peer_list (struct peer_info **peer_list,
                        SOCKET socket_not_to_close,
                        n2n_tcp_connection_t **tcp_connections,
                        time_t purge_before) {

    struct peer_info *scan, *tmp;
    n2n_tcp_connection_t *conn;
    size_t retval = 0;

    HASH_ITER(hh, *peer_list, scan, tmp) {
        if(scan->purgeable && scan->last_seen < purge_before) {
            if((scan->socket_fd >=0) && (scan->socket_fd != socket_not_to_close)) {
                if(tcp_connections) {
                    HASH_FIND_INT(*tcp_connections, &scan->socket_fd, conn);
                    if(conn) {
                        HASH_DEL(*tcp_connections, conn);
                        free(conn);
                    }
                    shutdown(scan->socket_fd, SHUT_RDWR);
                    closesocket(scan->socket_fd);
                }
            }
            HASH_DEL(*peer_list, scan);
            mgmt_event_post(N2N_EVENT_PEER,N2N_EVENT_PEER_PURGE,scan);
            /* FIXME: generates events for more than just p2p */
            retval++;
            free(scan);
        }
    }

    return retval;
}

/** Purge all items from the peer_list and return the number of items that were removed. */
size_t clear_peer_list (struct peer_info ** peer_list) {

    struct peer_info *scan, *tmp;
    size_t retval = 0;

    HASH_ITER(hh, *peer_list, scan, tmp) {
        if (!scan->purgeable && scan->ip_addr) {
            free(scan->ip_addr);
        }
        HASH_DEL(*peer_list, scan);
        mgmt_event_post(N2N_EVENT_PEER,N2N_EVENT_PEER_CLEAR,scan);
        /* FIXME: generates events for more than just p2p */
        retval++;
        free(scan);
    }

    return retval;
}

static uint8_t hex2byte (const char * s) {

    char tmp[3];
    tmp[0] = s[0];
    tmp[1] = s[1];
    tmp[2] = 0; /* NULL term */

    return((uint8_t)strtol(tmp, NULL, 16));
}

extern int str2mac (uint8_t * outmac /* 6 bytes */, const char * s) {

    size_t i;

    /* break it down as one case for the first "HH", the 5 x through loop for
     * each ":HH" where HH is a two hex nibbles in ASCII. */

    *outmac = hex2byte(s);
    ++outmac;
    s += 2; /* don't skip colon yet - helps generalise loop. */

    for(i = 1; i < 6; ++i) {
        s += 1;
        *outmac = hex2byte(s);
        ++outmac;
        s += 2;
    }

    return 0; /* ok */
}

extern char * sock_to_cstr (n2n_sock_str_t out,
                            const n2n_sock_t * sock) {


    if(NULL == out) {
        return NULL;
    }
    memset(out, 0, N2N_SOCKBUF_SIZE);

    if(AF_INET6 == sock->family) {
        char tmp[INET6_ADDRSTRLEN+1];

        tmp[0] = '\0';
        inet_ntop(AF_INET6, sock->addr.v6, tmp, sizeof(tmp));
        snprintf(out, N2N_SOCKBUF_SIZE, "[%s]:%hu", tmp[0] ? tmp : "", sock->port);
        return out;
    } else {
        const uint8_t * a = sock->addr.v4;

        snprintf(out, N2N_SOCKBUF_SIZE, "%hu.%hu.%hu.%hu:%hu",
                 (unsigned short)(a[0] & 0xff),
                 (unsigned short)(a[1] & 0xff),
                 (unsigned short)(a[2] & 0xff),
                 (unsigned short)(a[3] & 0xff),
                 (unsigned short)sock->port);
        return out;
    }
}

char *ip_subnet_to_str (dec_ip_bit_str_t buf, const n2n_ip_subnet_t *ipaddr) {

    snprintf(buf, sizeof(dec_ip_bit_str_t), "%hhu.%hhu.%hhu.%hhu/%hhu",
             (uint8_t) ((ipaddr->net_addr >> 24) & 0xFF),
             (uint8_t) ((ipaddr->net_addr >> 16) & 0xFF),
             (uint8_t) ((ipaddr->net_addr >> 8) & 0xFF),
             (uint8_t) (ipaddr->net_addr & 0xFF),
             ipaddr->net_bitlen);

    return buf;
}


/* @return 1 if the two sockets are equivalent. */
int sock_equal (const n2n_sock_t * a,
                const n2n_sock_t * b) {

    if(a->port != b->port) {
        return(0);
    }

    if(a->family != b->family) {
        return(0);
    }

    switch(a->family) {
        case AF_INET:
            if(memcmp(a->addr.v4, b->addr.v4, IPV4_SIZE)) {
                return(0);
            }
            break;

        default:
            if(memcmp(a->addr.v6, b->addr.v6, IPV6_SIZE)) {
                return(0);
            }
            break;
    }

    /* equal */
    return(1);
}


/* *********************************************** */

// fills a specified memory area with random numbers
int memrnd (uint8_t *address, size_t len) {

    for(; len >= 4; len -= 4) {
        *(uint32_t*)address = n2n_rand();
        address += 4;
    }

    for(; len > 0; len--) {
        *address = n2n_rand();
        address++;
    }

    return 0;
}


// exclusive-ors a specified memory area with another
int memxor (uint8_t *destination, const uint8_t *source, size_t len) {

    for(; len >= 4; len -= 4) {
        *(uint32_t*)destination ^= *(uint32_t*)source;
        source += 4;
        destination += 4;
    }

    for(; len > 0; len--) {
        *destination ^= *source;
        source++;
        destination++;
    }

    return 0;
}

/* *********************************************** */

#ifdef _WIN32
int gettimeofday (struct timeval *tp, void *tzp) {

    time_t clock;
    struct tm tm;
    SYSTEMTIME wtm;

    GetLocalTime(&wtm);
    tm.tm_year = wtm.wYear - 1900;
    tm.tm_mon = wtm.wMonth - 1;
    tm.tm_mday = wtm.wDay;
    tm.tm_hour = wtm.wHour;
    tm.tm_min = wtm.wMinute;
    tm.tm_sec = wtm.wSecond;
    tm.tm_isdst = -1;
    clock = mktime(&tm);
    tp->tv_sec = clock;
    tp->tv_usec = wtm.wMilliseconds * 1000;

    return 0;
}
#endif


// stores the previously issued time stamp
static uint64_t previously_issued_time_stamp = 0;


// returns a time stamp for use with replay protection (branchless code)
//
// depending on the self-detected accuracy, it has the following format
//
// MMMMMMMMCCCCCCCF or
//
// MMMMMMMMSSSSSCCF
//
// with M being the 32-bit second time stamp
//      S       the 20-bit sub-second (microsecond) time stamp part, if applicable
//      C       a counter (8 bit or 24 bit) reset to 0 with every MMMMMMMM(SSSSS) turn-over
//      F       a 4-bit flag field with
//      ...c    being the accuracy indicator (if set, only counter and no sub-second accuracy)
//
uint64_t time_stamp (void) {

    struct timeval tod;
    uint64_t micro_seconds;
    uint64_t co, mask_lo, mask_hi, hi_unchanged, counter, new_co;

    gettimeofday(&tod, NULL);

    // (roughly) calculate the microseconds since 1970, leftbound
    micro_seconds = ((uint64_t)(tod.tv_sec) << 32) + ((uint64_t)tod.tv_usec << 12);
    // more exact but more costly due to the multiplication:
    // micro_seconds = ((uint64_t)(tod.tv_sec) * 1000000ULL + tod.tv_usec) << 12;

    // extract "counter only" flag (lowest bit)
    co = (previously_issued_time_stamp << 63) >> 63;
    // set mask accordingly
    mask_lo   = -co;
    mask_lo >>= 32;
    // either 0x00000000FFFFFFFF (if co flag set) or 0x0000000000000000 (if co flag not set)

    mask_lo  |= (~mask_lo) >> 52;
    // either 0x00000000FFFFFFFF (unchanged)      or 0x0000000000000FFF (lowest 12 bit set)

    mask_hi   = ~mask_lo;

    hi_unchanged = ((previously_issued_time_stamp & mask_hi) == (micro_seconds & mask_hi));
    // 0 if upper bits unchanged (compared to previous stamp), 1 otherwise

    // read counter and shift right for flags
    counter   = (previously_issued_time_stamp & mask_lo) >> 4;

    counter  += hi_unchanged;
    counter  &= -hi_unchanged;
    // either counter++ if upper part of timestamp unchanged, 0 otherwise

    // back to time stamp format
    counter <<= 4;

    // set new co flag if counter overflows while upper bits unchanged or if it was set before
    new_co   = (((counter & mask_lo) == 0) & hi_unchanged) | co;

    // in case co flag changed, masks need to be recalculated
    mask_lo   = -new_co;
    mask_lo >>= 32;
    mask_lo  |= (~mask_lo) >> 52;
    mask_hi   = ~mask_lo;

    // assemble new timestamp
    micro_seconds &= mask_hi;
    micro_seconds |= counter;
    micro_seconds |= new_co;

    previously_issued_time_stamp = micro_seconds;

    return micro_seconds;
}


// returns an initial time stamp for use with replay protection
uint64_t initial_time_stamp (void) {

    return time_stamp() - TIME_STAMP_FRAME;
}


// checks if a provided time stamp is consistent with current time and previously valid time stamps
// and, in case of validity, updates the "last valid time stamp"
int time_stamp_verify_and_update (uint64_t stamp, uint64_t *previous_stamp, int allow_jitter) {

    int64_t diff; /* do not change to unsigned */
    uint64_t co;  /* counter only mode (for sub-seconds) */

    co = (stamp << 63) >> 63;

    // is it around current time (+/- allowed deviation TIME_STAMP_FRAME)?
    diff = stamp - time_stamp();
    // abs()
    diff = (diff < 0 ? -diff : diff);
    if(diff >= TIME_STAMP_FRAME) {
        traceEvent(TRACE_DEBUG, "time_stamp_verify_and_update found a timestamp out of allowed frame.");
        return 0; // failure
    }

    // if applicable: is it higher than previous time stamp (including allowed deviation of TIME_STAMP_JITTER)?
    if(NULL != previous_stamp) {
        diff = stamp - *previous_stamp;
        if(allow_jitter) {
            // 8 times higher jitter allowed for counter-only flagged timestamps ( ~ 1.25 sec with 160 ms default jitter)
            diff += TIME_STAMP_JITTER << (co << 3);
        }

        if(diff <= 0) {
            traceEvent(TRACE_DEBUG, "time_stamp_verify_and_update found a timestamp too old compared to previous.");
            return 0; // failure
        }
        // for not allowing to exploit the allowed TIME_STAMP_JITTER to "turn the clock backwards",
        // set the higher of the values
        *previous_stamp = (stamp > *previous_stamp ? stamp : *previous_stamp);
    }

    return 1; // success
}
