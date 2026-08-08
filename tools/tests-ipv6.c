/*
 * IPv6 transport tests for n2n socket parsing and dual-stack UDP I/O.
 */

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>

#include "n2n.h"
#include "n2n_wire.h"


static int wait_readable (SOCKET sock) {

    fd_set readfds;
    struct timeval timeout;

    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    return select(sock + 1, &readfds, NULL, NULL, &timeout);
}


static int receive_family (SOCKET sock, int expected_family) {

    char byte;
    struct sockaddr_storage sender;
    socklen_t sender_len = sizeof(sender);
    n2n_sock_t sender_sock;

    if(wait_readable(sock) != 1)
        return -1;
    if(recvfrom(sock, &byte, sizeof(byte), 0,
                (struct sockaddr *)&sender, &sender_len) != sizeof(byte))
        return -1;
    if(fill_n2nsock(&sender_sock, (struct sockaddr *)&sender) != 0)
        return -1;
    return (sender_sock.family == expected_family) ? 0 : -1;
}


int main (void) {

    n2n_sock_t sock;
    n2n_sock_t mapped;
    n2n_sock_t server_bind;
    n2n_sock_t server_address;
    n2n_sock_t client6_bind;
    struct sockaddr_storage address;
    struct sockaddr_storage actual;
    struct sockaddr_in destination4;
    socklen_t address_len;
    socklen_t actual_len;
    SOCKET server = -1;
    SOCKET client4 = -1;
    SOCKET client6 = -1;
    SOCKET reuse1 = -1;
    SOCKET reuse2 = -1;
    char byte = 'x';
    int failed = 0;

    setTraceLevel(TRACE_ERROR);

    if((supernode2sock(&sock, "127.0.0.1:7654") != 0)
       || (sock.family != AF_INET) || (sock.port != 7654)) {
        printf("IPv4 endpoint parsing: FAIL\n");
        failed = 1;
    } else
        printf("IPv4 endpoint parsing: PASS\n");

    if((supernode2sock(&sock, "[::1]:7654") != 0)
       || (sock.family != AF_INET6) || (sock.port != 7654)) {
        printf("IPv6 endpoint parsing: FAIL\n");
        failed = 1;
    } else
        printf("IPv6 endpoint parsing: PASS\n");

    if(supernode2sock(&sock, "::1:7654") != -3) {
        printf("IPv6 bracket validation: FAIL\n");
        failed = 1;
    } else
        printf("IPv6 bracket validation: PASS\n");

    if((parse_bind_address(&sock, "[::1]:7654", 0, AF_INET) != 0)
       || (sock.family != AF_INET6) || (sock.port != 7654)) {
        printf("IPv6 bind parsing: FAIL\n");
        failed = 1;
    } else
        printf("IPv6 bind parsing: PASS\n");

    memset(&mapped, 0, sizeof(mapped));
    mapped.family = AF_INET;
    mapped.port = 7654;
    inet_pton(AF_INET, "192.0.2.1", mapped.addr.v4);
    address_len = fill_sockaddr_for_family((struct sockaddr *)&address,
                                           sizeof(address), &mapped, AF_INET6);
    if((address_len != sizeof(struct sockaddr_in6))
       || (fill_n2nsock(&sock, (struct sockaddr *)&address) != 0)
       || !sock_equal(&sock, &mapped)) {
        printf("IPv4-mapped IPv6 conversion: FAIL\n");
        failed = 1;
    } else
        printf("IPv4-mapped IPv6 conversion: PASS\n");

    memset(&server_bind, 0, sizeof(server_bind));
    server_bind.family = AF_INET6;
    server_bind.port = 0;
    server = open_socket_bind(&server_bind, 0);
    if(server < 0) {
        printf("Dual-stack UDP receive: FAIL\n");
        return 1;
    }

    actual_len = sizeof(actual);
    if((getsockname(server, (struct sockaddr *)&actual, &actual_len) != 0)
       || (fill_n2nsock(&server_address, (struct sockaddr *)&actual) != 0)) {
        printf("Dual-stack UDP receive: FAIL\n");
        closesocket(server);
        return 1;
    }

    client4 = open_socket(0, INADDR_LOOPBACK, 0);
    memset(&destination4, 0, sizeof(destination4));
    destination4.sin_family = AF_INET;
    destination4.sin_port = htons(server_address.port);
    destination4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if((client4 < 0)
       || (sendto(client4, &byte, sizeof(byte), 0,
                  (struct sockaddr *)&destination4, sizeof(destination4)) != sizeof(byte))
       || (receive_family(server, AF_INET) != 0)) {
        printf("Dual-stack UDP receive: FAIL\n");
        failed = 1;
    } else
        printf("Dual-stack UDP receive: PASS\n");

    memset(&client6_bind, 0, sizeof(client6_bind));
    client6_bind.family = AF_INET6;
    client6 = open_socket_bind(&client6_bind, 0);
    server_address.family = AF_INET6;
    memset(server_address.addr.v6, 0, IPV6_SIZE);
    server_address.addr.v6[15] = 1;
    address_len = fill_sockaddr((struct sockaddr *)&address,
                                sizeof(address), &server_address) == 0
                  ? sizeof(struct sockaddr_in6) : 0;
    if((client6 < 0) || !address_len
       || (sendto(client6, &byte, sizeof(byte), 0,
                  (struct sockaddr *)&address, address_len) != sizeof(byte))
       || (receive_family(server, AF_INET6) != 0)) {
        printf("IPv6 UDP receive: FAIL\n");
        failed = 1;
    } else
        printf("IPv6 UDP receive: PASS\n");

    reuse1 = open_socket_reuse_port(0, INADDR_ANY, 0);
    actual_len = sizeof(actual);
    if((reuse1 < 0)
       || (getsockname(reuse1, (struct sockaddr *)&actual, &actual_len) != 0)
       || (fill_n2nsock(&sock, (struct sockaddr *)&actual) != 0)
       || ((reuse2 = open_socket_reuse_port(sock.port, INADDR_ANY, 0)) < 0)) {
        printf("Reusable UDP port binding: FAIL\n");
        failed = 1;
    } else
        printf("Reusable UDP port binding: PASS\n");

    if(reuse2 >= 0)
        closesocket(reuse2);
    if(reuse1 >= 0)
        closesocket(reuse1);
    if(client6 >= 0)
        closesocket(client6);
    if(client4 >= 0)
        closesocket(client4);
    closesocket(server);
    return failed;
}
