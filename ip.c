/*

  Copyright (c) 2015 Martin Sustrik

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom
  the Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  IN THE SOFTWARE.

*/

#if defined __linux__
#define _GNU_SOURCE
#include <netdb.h>
#include <sys/eventfd.h>
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#if !defined __sun
#include <ifaddrs.h>
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "dns/dns.h"

#include "ip.h"
#include "libpill.h"
#include "utils.h"
#include "fd.h"

MILL_CT_ASSERT(sizeof(ipaddr) >= sizeof(struct sockaddr_in));
MILL_CT_ASSERT(sizeof(ipaddr) >= sizeof(struct sockaddr_in6));

static struct dns_resolv_conf *mill_dns_conf = NULL;
static struct dns_hosts *mill_dns_hosts = NULL;
static struct dns_hints *mill_dns_hints = NULL;
static struct dns_resolver *mill_dns_resolver = NULL;

static int mill_ipany(ipaddr *addr, int port, int mode)
{
    if(mill_slow(port < 0 || port > 0xffff)) {
        errno = EINVAL;
        return -1;
    }
    if (mode == 0 || mode == IPADDR_IPV4 || mode == IPADDR_PREF_IPV4) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in*)addr;
        ipv4->sin_family = AF_INET;
        ipv4->sin_addr.s_addr = htonl(INADDR_ANY);
        ipv4->sin_port = htons((uint16_t)port);
    }
    else {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6*)addr;
        ipv6->sin6_family = AF_INET6;
        memcpy(&ipv6->sin6_addr, &in6addr_any, sizeof(in6addr_any));
        ipv6->sin6_port = htons((uint16_t)port);
    }
    return 0;
}

/* Convert literal IPv4 address to a binary one. */
static int mill_ipv4_literal(ipaddr *addr, const char *name, int port) {
    struct sockaddr_in *ipv4 = (struct sockaddr_in*)addr;
    int rc = inet_pton(AF_INET, name, &ipv4->sin_addr);
    mill_assert(rc >= 0);
    if(rc == 1) {
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons((uint16_t)port);
        return 0;
    }
    errno = EINVAL;
    return -1;
}

/* Convert literal IPv6 address to a binary one. */
static int mill_ipv6_literal(ipaddr *addr, const char *name, int port) {
    struct sockaddr_in6 *ipv6 = (struct sockaddr_in6*)addr;
    int rc = inet_pton(AF_INET6, name, &ipv6->sin6_addr);
    mill_assert(rc >= 0);
    if(rc == 1) {
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons((uint16_t)port);
        return 0;
    }
    errno = EINVAL;
    return -1;
}

/* Convert literal IPv4 or IPv6 address to a binary one. */
static int mill_ipliteral(ipaddr *addr, const char *name, int port, int mode) {
    struct sockaddr *sa = (struct sockaddr*)addr;
    if(mill_slow(!name || port < 0 || port > 0xffff)) {
        errno = EINVAL;
        return -1;
    }
    int rc;
    switch(mode) {
    case IPADDR_IPV4:
        return mill_ipv4_literal(addr, name, port);
    case IPADDR_IPV6:
        return mill_ipv6_literal(addr, name, port);
    case 0:
    case IPADDR_PREF_IPV4:
        rc = mill_ipv4_literal(addr, name, port);
        if(rc == 0)
            return 0;
        return mill_ipv6_literal(addr, name, port);
    case IPADDR_PREF_IPV6:
        rc = mill_ipv6_literal(addr, name, port);
        if(rc == 0)
            return 0;
        return mill_ipv4_literal(addr, name, port);
    default:
        mill_assert(0);
    }
}

int ipfamily(const ipaddr *addr) {
    return ((struct sockaddr*) addr)->sa_family;
}

int iplen(const ipaddr *addr) {
    return ipfamily(addr) == AF_INET ?
        sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
}

int ipport(const ipaddr *addr) {
    return ntohs(ipfamily(addr) == AF_INET ?
        ((struct sockaddr_in*)addr)->sin_port :
        ((struct sockaddr_in6*)addr)->sin6_port);
}

/* Convert IP address from network format to ASCII dot notation. */
const char *ipaddrstr(const ipaddr *addr, char *ipstr) {
    if(ipfamily(addr) == AF_INET) {
        return inet_ntop(AF_INET, &(((struct sockaddr_in*) addr)->sin_addr),
            ipstr, INET_ADDRSTRLEN);
    }
    else {
        return inet_ntop(AF_INET6, &(((struct sockaddr_in6*) addr)->sin6_addr),
            ipstr, INET6_ADDRSTRLEN);
    }
}

int iplocal(ipaddr *addr, const char *name, int port, int mode) {
    if(!name)
        return mill_ipany(addr, port, mode);
    int rc = mill_ipliteral(addr, name, port, mode);
#if defined __sun
    return rc;
#else
    if(rc == 0)
       return 0;
    /* Address is not a literal. It must be an interface name then. */
    struct ifaddrs *ifaces = NULL;
    rc = getifaddrs (&ifaces);
    mill_assert (rc == 0);
    mill_assert (ifaces);
    /*  Find first IPv4 and first IPv6 address. */
    struct ifaddrs *ipv4 = NULL;
    struct ifaddrs *ipv6 = NULL;
    struct ifaddrs *it;
    for(it = ifaces; it != NULL; it = it->ifa_next) {
        if(!it->ifa_addr)
            continue;
        if(strcmp(it->ifa_name, name) != 0)
            continue;
        switch(it->ifa_addr->sa_family) {
        case AF_INET:
            mill_assert(!ipv4);
            ipv4 = it;
            break;
        case AF_INET6:
            mill_assert(!ipv6);
            ipv6 = it;
            break;
        }
        if(ipv4 && ipv6)
            break;
    }
    /* Choose the correct address family based on mode. */
    switch(mode) {
    case IPADDR_IPV4:
        ipv6 = NULL;
        break;
    case IPADDR_IPV6:
        ipv4 = NULL;
        break;
    case 0:
    case IPADDR_PREF_IPV4:
        if(ipv4)
           ipv6 = NULL;
        break;
    case IPADDR_PREF_IPV6:
        if(ipv6)
           ipv4 = NULL;
        break;
    default:
        mill_assert(0);
    }
    if(ipv4) {
        struct sockaddr_in *inaddr = (struct sockaddr_in*)addr;
        memcpy(inaddr, ipv4->ifa_addr, sizeof (struct sockaddr_in));
        inaddr->sin_port = htons(port);
        freeifaddrs(ifaces);
        return 0;
    }
    if(ipv6) {
        struct sockaddr_in6 *inaddr = (struct sockaddr_in6*)addr;
        memcpy(inaddr, ipv6->ifa_addr, sizeof (struct sockaddr_in6));
        inaddr->sin6_port = htons(port);
        freeifaddrs(ifaces);
        return 0;
    }
    freeifaddrs(ifaces);
    errno = ENODEV;
    return -1;
#endif
}

/* Load DNS config files. */
void mill_dns_init(void) {
    int rc;
    /* TODO: Maybe re-read the configuration once in a while? */
    if (mill_dns_resolver)
        return;
    mill_dns_conf = dns_resconf_local(&rc);
    mill_assert(mill_dns_conf);
    mill_dns_hosts = dns_hosts_local(&rc);
    mill_assert(mill_dns_hosts);
    mill_dns_hints = dns_hints_local(mill_dns_conf, &rc);
    mill_assert(mill_dns_hints);
}

int ipremote(ipaddr *addr, const char *name, int port, int mode, int64_t deadline) {
    int rc = mill_ipliteral(addr, name, port, mode);
    if(rc == 0)
       return 0;
    /* Let's do asynchronous DNS query here. */
    mill_assert(port >= 0 && port <= 0xffff);
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    struct dns_resolver *resolver = dns_res_open(mill_dns_conf, mill_dns_hosts,
            mill_dns_hints, NULL, dns_opts(), &rc);
    mill_assert(resolver);

    struct dns_addrinfo *ai = dns_ai_open(name, portstr, DNS_T_A, &hints,
            resolver, &rc);
    mill_assert(ai);
    struct addrinfo *ipv4 = NULL;
    struct addrinfo *ipv6 = NULL;
    struct addrinfo *it = NULL;
    while(1) {
        rc = dns_ai_nextent(&it, ai);
        if(rc == EAGAIN) {
            int fd = dns_ai_pollfd(ai);
            mill_assert(fd >= 0);
            int events = mill_fdevent(fd, FDW_IN, deadline);
            if(mill_slow(!events)) {
                dns_ai_close(ai);
                dns_res_close(resolver);
                errno = ETIMEDOUT;
                return -1;
            }
            if(events != FDW_IN)
                break;
            continue;
        }
        if(rc == ENOENT)
            break;
        if(!ipv4 && it && it->ai_family == AF_INET)
            ipv4 = it;
        if(!ipv6 && it && it->ai_family == AF_INET6)
            ipv6 = it;
        if(ipv4 && ipv6)
            break;
    }
    switch(mode) {
    case IPADDR_IPV4:
        if(ipv6)
            free(ipv6);
        ipv6 = NULL;
        break;
    case IPADDR_IPV6:
        if(ipv4)
            free(ipv4);
        ipv4 = NULL;
        break;
    case 0:
    case IPADDR_PREF_IPV4:
        if(ipv4) {
            if(ipv6)
                free(ipv6);
            ipv6 = NULL;
        }
        break;
    case IPADDR_PREF_IPV6:
        if(ipv6) {
            if(ipv4)
                free(ipv4);
            ipv4 = NULL;
        }
        break;
    default:
        mill_assert(0);
    }
    dns_res_close(resolver);
    if(ipv4) {
        struct sockaddr_in *inaddr = (struct sockaddr_in*)addr;
        memcpy(inaddr, ipv4->ai_addr, sizeof (struct sockaddr_in));
        inaddr->sin_port = htons(port);
        free(ipv4);
    }
    else if(ipv6) {
        struct sockaddr_in6 *inaddr = (struct sockaddr_in6*)addr;
        memcpy(inaddr, ipv6->ai_addr, sizeof (struct sockaddr_in6));
        inaddr->sin6_port = htons(port);
        free(ipv6);
    }
    else {
        dns_ai_close(ai);
        errno = EADDRNOTAVAIL;
        return -1;
    }
    dns_ai_close(ai);
    return 0;
}

