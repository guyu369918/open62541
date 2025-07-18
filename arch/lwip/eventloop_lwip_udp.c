/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2021-2022 (c) Fraunhofer IOSB (Author: Julius Pfrommer)
 *    Copyright 2021 (c) Fraunhofer IOSB (Author: Jan Hermes)
 *    Copyright 2025 (c) Fraunhofer IOSB (Author: Noel Graf)
 */

#include "open62541/types.h"

#include "eventloop_lwip.h"
#include "../common/eventloop_common.h"

#define IPV4_PREFIX_MASK 0xF0
#define IPV4_MULTICAST_PREFIX 0xE0
#if UA_IPV6
# define IPV6_PREFIX_MASK 0xFF
# define IPV6_MULTICAST_PREFIX 0xFF
#endif

/* Configuration parameters */
#define UDP_PARAMETERSSIZE 10
#define UDP_PARAMINDEX_RECVBUF 0
#define UDP_PARAMINDEX_LISTEN 1
#define UDP_PARAMINDEX_ADDR 2
#define UDP_PARAMINDEX_PORT 3
#define UDP_PARAMINDEX_INTERFACE 4
#define UDP_PARAMINDEX_TTL 5
#define UDP_PARAMINDEX_LOOPBACK 6
#define UDP_PARAMINDEX_REUSE 7
#define UDP_PARAMINDEX_SOCKPRIO 8
#define UDP_PARAMINDEX_VALIDATE 9

static UA_KeyValueRestriction UDPConfigParameters[UDP_PARAMETERSSIZE] = {
    {{0, UA_STRING_STATIC("recv-bufsize")}, &UA_TYPES[UA_TYPES_UINT32], false, true, false},
    {{0, UA_STRING_STATIC("listen")}, &UA_TYPES[UA_TYPES_BOOLEAN], false, true, false},
    {{0, UA_STRING_STATIC("address")}, &UA_TYPES[UA_TYPES_STRING], false, true, true},
    {{0, UA_STRING_STATIC("port")}, &UA_TYPES[UA_TYPES_UINT16], true, true, false},
    {{0, UA_STRING_STATIC("interface")}, &UA_TYPES[UA_TYPES_STRING], false, true, false},
    {{0, UA_STRING_STATIC("ttl")}, &UA_TYPES[UA_TYPES_UINT32], false, true, false},
    {{0, UA_STRING_STATIC("loopback")}, &UA_TYPES[UA_TYPES_BOOLEAN], false, true, false},
    {{0, UA_STRING_STATIC("reuse")}, &UA_TYPES[UA_TYPES_BOOLEAN], false, true, false},
    {{0, UA_STRING_STATIC("sockpriority")}, &UA_TYPES[UA_TYPES_UINT32], false, true, false},
    {{0, UA_STRING_STATIC("validate")}, &UA_TYPES[UA_TYPES_BOOLEAN], false, true, false}
};

/* A registered file descriptor with an additional method pointer */
typedef struct {
    UA_RegisteredFD rfd;

    UA_ConnectionManager_connectionCallback applicationCB;
    void *application;
    void *context;

    struct sockaddr_storage sendAddr;
    socklen_t sendAddrLength;
} UDP_FD;

typedef enum {
    MULTICASTTYPE_NONE = 0,
    MULTICASTTYPE_IPV4,
    MULTICASTTYPE_IPV6
} MultiCastType;

typedef union {
    struct ip_mreq ipv4;
#if UA_IPV6
    struct ipv6_mreq ipv6;
#endif
} MulticastRequest;

static UA_Boolean
isMulticastAddress(const UA_Byte *address, UA_Byte mask, UA_Byte prefix) {
    return (address[0] & mask) == prefix;
}

static MultiCastType
multiCastType(struct addrinfo *info) {
    const UA_Byte *address;
    if(info->ai_family == AF_INET) {
        address = (UA_Byte *)&((struct sockaddr_in *)info->ai_addr)->sin_addr;
        if(isMulticastAddress(address, IPV4_PREFIX_MASK, IPV4_MULTICAST_PREFIX))
            return MULTICASTTYPE_IPV4;
#if UA_IPV6
    } else if(info->ai_family == AF_INET6) {
        address = (UA_Byte *)&((struct sockaddr_in6 *)info->ai_addr)->sin6_addr;
        if(isMulticastAddress(address, IPV6_PREFIX_MASK, IPV6_MULTICAST_PREFIX))
            return MULTICASTTYPE_IPV6;
#endif
    }
    return MULTICASTTYPE_NONE;
}

static UA_StatusCode
setMulticastInterface(const char *netif_name, struct addrinfo *info,
                      MulticastRequest *req, const UA_Logger *logger) {
    struct netif *netif = NULL;
    u8_t netif_index = 0;

#if LWIP_SINGLE_NETIF
    /* If only one network interface is available, use netif_default */
    netif = netif_default;
    if(!netif || !netif_is_up(netif)) {
        UA_LOG_ERROR(logger, UA_LOGCATEGORY_SERVER,
                        "UDP\t| No active network interface found.");
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    /* Check if the interface name matches */
    if(strcmp(netif->name, netif_name) == 0) {
        netif_index = netif_get_index(netif);
    } else {
    /* Convert IP to string and compare */
    char ip_str[INET_ADDRSTRLEN];
#if LWIP_IPV6
    char ip6_str[INET6_ADDRSTRLEN];
#endif

    if(info->ai_family == AF_INET) {
        ipaddr_ntoa_r(&netif->ip_addr, ip_str, sizeof(ip_str));
        if(strcmp(ip_str, netif_name) == 0) {
            netif_index = netif_get_index(netif);
        }
    }
#if LWIP_IPV6
    else if(info->ai_family == AF_INET6) {
        ipaddr_ntoa_r(&netif->ip6_addr[0], ip6_str, sizeof(ip6_str));
        if(strcmp(ip6_str, netif_name) == 0) {
            netif_index = netif_get_index(netif);
        }
    }
#endif
    }

#else
    /* Iterate over available network interfaces */
    NETIF_FOREACH(netif) {
        if(!netif || !netif_is_up(netif))
            continue;

        /* Check if the interface name matches */
        if(strcmp(netif->name, netif_name) == 0) {
            netif_index = netif_get_index(netif);
            break;
        }

        /* Convert IP to string and compare */
        char ip_str[INET_ADDRSTRLEN];
#if LWIP_IPV6
        char ip6_str[INET6_ADDRSTRLEN];
#endif
    
        if(info->ai_family == AF_INET) {
            ipaddr_ntoa_r(&netif->ip_addr, ip_str, sizeof(ip_str));
            if(strcmp(ip_str, netif_name) == 0) {
                netif_index = netif_get_index(netif);
                break;
            }
        }
#if LWIP_IPV6
        else if(info->ai_family == AF_INET6) {
            ipaddr_ntoa_r(&netif->ip6_addr[0], ip6_str, sizeof(ip6_str));
            if(strcmp(ip6_str, netif_name) == 0) {
                netif_index = netif_get_index(netif);
                break;
            }
        }
#endif
    }
#endif

    /* If no interface was found */
    if(!netif || netif_index == 0) {
        UA_LOG_ERROR(logger, UA_LOGCATEGORY_SERVER,
                     "UDP\t| No matching network interface found.");
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    /* Set the interface index for multicast request */
    if(info->ai_family == AF_INET) {
#if LWIP_IGMP
        req->ipv4.imr_interface.s_addr = ip4_addr_get_u32(ip_2_ip4(&netif->ip_addr));
#else
        UA_LOG_ERROR(logger, UA_LOGCATEGORY_SERVER,
                     "UDP\t| IGMP (IPv4 multicast) is not enabled in lwIP.");
        return UA_STATUSCODE_BADINTERNALERROR;
#endif
    }
#if UA_IPV6 && LWIP_IPV6
    else if (info->ai_family == AF_INET6) {
#if LWIP_IPV6_MLD
        req->ipv6.ipv6mr_interface = netif_index;
#else
        UA_LOG_ERROR(logger, UA_LOGCATEGORY_SERVER,
                        "UDP\t| MLD (IPv6 multicast) is not enabled in lwIP.");
        return UA_STATUSCODE_BADINTERNALERROR;
#endif
    }
#endif /* UA_IPV6 && LWIP_IPV6 */
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
setupMulticastRequest(UA_FD socket, MulticastRequest *req, const UA_KeyValueMap *params,
                      struct addrinfo *info, const UA_Logger *logger) {
    /* Initialize the address information */
    if(info->ai_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)info->ai_addr;
        req->ipv4.imr_multiaddr = sin->sin_addr;
        req->ipv4.imr_interface.s_addr = htonl(INADDR_ANY); /* default ANY */
#if UA_IPV6
    } else if(info->ai_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)info->ai_addr;
        req->ipv6.ipv6mr_multiaddr = sin6->sin6_addr;
        req->ipv6.ipv6mr_interface = 0; /* default ANY interface */
#endif
    } else {
        UA_LOG_ERROR(logger, UA_LOGCATEGORY_SERVER,
                     "UDP\t| Multicast configuration failed: Unknown protocol family");
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    /* Was an interface (or local IP address) defined? */
    const UA_String *netif = (const UA_String*)
        UA_KeyValueMap_getScalar(params, UDPConfigParameters[UDP_PARAMINDEX_INTERFACE].name,
                                 &UA_TYPES[UA_TYPES_STRING]);
    if(!netif) {
        UA_LOG_WARNING(logger, UA_LOGCATEGORY_NETWORK,
                       "UDP %u\t| No network interface defined for multicast. "
                       "The first suitable network interface is used.",
                       (unsigned)socket);
        return UA_STATUSCODE_GOOD;
    }

    /* Set the interface index */
    UA_STACKARRAY(char, interfaceAsChar, sizeof(char) * netif->length + 1);
    memcpy(interfaceAsChar, netif->data, netif->length);
    interfaceAsChar[netif->length] = 0;
    return setMulticastInterface(interfaceAsChar, info, req, logger);
}

/* Retrieves hostname and port from given key value parameters.
 *
 * @param[in] params the parameter map to retrieve from
 * @param[out] hostname the retrieved hostname when present, NULL otherwise
 * @param[out] portStr the retrieved port when present, NULL otherwise
 * @param[in] logger the logger to log information
 * @return -1 upon error, 0 if there was no host or port parameter, 1 if
 *         host and port are present */
static int
getHostAndPortFromParams(const UA_KeyValueMap *params, char *hostname,
                         char *portStr, const UA_Logger *logger) {
    /* Prepare the port parameter as a string */
    const UA_UInt16 *port = (const UA_UInt16*)
        UA_KeyValueMap_getScalar(params, UDPConfigParameters[UDP_PARAMINDEX_PORT].name,
                                 &UA_TYPES[UA_TYPES_UINT16]);
    UA_assert(port); /* checked before */
    mp_snprintf(portStr, UA_MAXPORTSTR_LENGTH, "%d", *port);

    /* Prepare the hostname string */
    const UA_String *host = (const UA_String*)
        UA_KeyValueMap_getScalar(params, UDPConfigParameters[UDP_PARAMINDEX_ADDR].name,
                                 &UA_TYPES[UA_TYPES_STRING]);
    if(!host) {
        UA_LOG_DEBUG(logger, UA_LOGCATEGORY_NETWORK,
                     "UDP\t| No address configured");
        return -1;
    }
    if(host->length >= UA_MAXHOSTNAME_LENGTH) {
        UA_LOG_ERROR(logger, UA_LOGCATEGORY_EVENTLOOP,
                     "UDP\t| Open UDP Connection: Hostname too long, aborting");
        return -1;
    }
    strncpy(hostname, (const char*)host->data, host->length);
    hostname[host->length] = 0;
    return 1;
}

#if LWIP_DNS || defined(UA_ARCHITECTURE_POSIX)
static int
getConnectionInfoFromParams(const UA_KeyValueMap *params,
                            char *hostname, char *portStr,
                            struct addrinfo **info, const UA_Logger *logger) {
    int foundParams = getHostAndPortFromParams(params, hostname, portStr, logger);
    if(foundParams < 0)
        return -1;

    /* Create the socket description from the connectString
     * TODO: Make this non-blocking */
    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
#if UA_IPV6
    hints.ai_family = AF_UNSPEC; /* Allow IPv4 and IPv6 */
#else
    hints.ai_family = AF_INET;   /* IPv4 only */
#endif
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    int error = UA_getaddrinfo(hostname, portStr, &hints, info);
    if(error != 0) {
        UA_LOG_SOCKET_ERRNO_GAI_WRAP(
            UA_LOG_WARNING(logger, UA_LOGCATEGORY_NETWORK,
                           "UDP\t| Lookup of %s failed with error %d - %s",
                           hostname, error, errno_str));
        return -1;
    }
    return 1;
}
#endif

/* Set loop back data to your host */
static UA_StatusCode
setLoopBackData(UA_SOCKET sockfd, UA_Boolean enableLoopback,
                int ai_family, const UA_Logger *logger) {
    /* The Linux Kernel IPv6 socket code checks for optlen to be at least the
     * size of an integer. However, channelDataUDPMC->enableLoopback is a
     * boolean. In order for the code to work for IPv4 and IPv6 propagate it to
     * a temporary integer here. */
    UA_Int32 enable = enableLoopback;
#if UA_IPV6
    if(UA_setsockopt(sockfd,
                     ai_family == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP,
                     ai_family == AF_INET6 ? IPV6_MULTICAST_LOOP : IP_MULTICAST_LOOP,
                     (const char *)&enable,
                     sizeof (enable)) < 0)
#else
        if(UA_setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_LOOP,
                     (const char *)&enable,
                     sizeof (enable)) < 0)
#endif
    {
        UA_LOG_SOCKET_ERRNO_WRAP(
            UA_LOG_ERROR(logger, UA_LOGCATEGORY_NETWORK,
                         "UDP %u\t| Loopback setup failed: "
                         "Cannot set socket option IP_MULTICAST_LOOP. Error: %s",
                         (unsigned)sockfd, errno_str));
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
setTimeToLive(UA_SOCKET sockfd, UA_UInt32 messageTTL,
              int ai_family, const UA_Logger *logger) {
    /* Set Time to live (TTL). Value of 1 prevent forward beyond the local network. */
#if UA_IPV6
    if(UA_setsockopt(sockfd,
                     ai_family == PF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP,
                     ai_family == PF_INET6 ? IPV6_MULTICAST_HOPS : IP_MULTICAST_TTL,
                     (const char *)&messageTTL,
                     sizeof(messageTTL)) < 0)
#else
    if(UA_setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL,
                     (const char *)&messageTTL,
                     sizeof(messageTTL)) < 0)
#endif
    {
        UA_LOG_SOCKET_ERRNO_WRAP(
            UA_LOG_WARNING(logger, UA_LOGCATEGORY_NETWORK,
                           "UDP %u\t| Time to live setup failed: "
                           "Cannot set socket option IP_MULTICAST_TTL. Error: %s",
                           (unsigned)sockfd, errno_str));
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_GOOD;
}

#ifdef __linux__
static UA_StatusCode
setSocketPriority(UA_SOCKET sockfd, UA_UInt32 socketPriority,
                  const UA_Logger *logger) {
    int prio = (int)socketPriority;
    if(UA_setsockopt(sockfd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(int)) < 0) {
        UA_LOG_SOCKET_ERRNO_WRAP(
            UA_LOG_ERROR(logger, UA_LOGCATEGORY_NETWORK,
                         "UDP %u\t| Socket priority setup failed: "
                         "Cannot set socket option SO_PRIORITY. Error: %s",
                         (unsigned)sockfd, errno_str));
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_GOOD;
}
#endif

static UA_StatusCode
setConnectionConfig(UA_FD socket, const UA_KeyValueMap *params,
                    int ai_family, const UA_Logger *logger) {
    /* Set socket config that is always set */
    UA_StatusCode res = UA_STATUSCODE_GOOD;
    res |= UA_EventLoopLWIP_setNonBlocking(socket);
    res |= UA_EventLoopLWIP_setNoSigPipe(socket);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    /* Some Linux distributions have net.ipv6.bindv6only not activated. So
     * sockets can double-bind to IPv4 and IPv6. This leads to problems. Use
     * AF_INET6 sockets only for IPv6. */
#if UA_IPV6
    int optval = 1;
    if(ai_family == AF_INET6 &&
       UA_setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY,
                     (const char*)&optval, sizeof(optval)) == -1) {
        UA_LOG_WARNING(logger, UA_LOGCATEGORY_NETWORK,
                       "UDP %u\t| Could not set an IPv6 socket to IPv6 only, closing",
                       (unsigned)socket);
        return UA_STATUSCODE_BADCONNECTIONREJECTED;
    }
#endif

    /* Set socket settings from the parameters */
    const UA_UInt32 *messageTTL = (const UA_UInt32*)
        UA_KeyValueMap_getScalar(params, UDPConfigParameters[UDP_PARAMINDEX_TTL].name,
                                 &UA_TYPES[UA_TYPES_UINT32]);
    if(messageTTL) {
        res |= setTimeToLive(socket, *messageTTL, ai_family, logger);
    } else {
        /* Set the default ttl value to 1 */
        res |= setTimeToLive(socket, 1, ai_family, logger);
    }

    const UA_Boolean *enableLoopback = (const UA_Boolean*)
        UA_KeyValueMap_getScalar(params, UDPConfigParameters[UDP_PARAMINDEX_LOOPBACK].name,
                                 &UA_TYPES[UA_TYPES_BOOLEAN]);
    if(enableLoopback)
        res |= setLoopBackData(socket, *enableLoopback, ai_family, logger);

    const UA_Boolean *enableReuse = (const UA_Boolean*)
        UA_KeyValueMap_getScalar(params, UDPConfigParameters[UDP_PARAMINDEX_REUSE].name,
                                 &UA_TYPES[UA_TYPES_BOOLEAN]);
    if(enableReuse)
        res |=  UA_EventLoopLWIP_setReusable(socket);

#ifdef __linux__
    const UA_UInt32 *socketPriority = (const UA_UInt32*)
        UA_KeyValueMap_getScalar(params, UDPConfigParameters[UDP_PARAMINDEX_SOCKPRIO].name,
                                 &UA_TYPES[UA_TYPES_UINT32]);
    if(socketPriority)
        res |= setSocketPriority(socket, *socketPriority, logger);
#endif

    if(res != UA_STATUSCODE_GOOD) {
        UA_LOG_SOCKET_ERRNO_WRAP(
            UA_LOG_WARNING(logger, UA_LOGCATEGORY_NETWORK,
                           "UDP\t| Could not set socket options: %s", errno_str));
    }
    return res;
}

static UA_StatusCode
setupListenMultiCast(UA_FD fd, struct addrinfo *info, const UA_KeyValueMap *params,
                     MultiCastType multiCastType, const UA_Logger *logger) {
    MulticastRequest req;
    UA_StatusCode res = setupMulticastRequest(fd, &req, params, info, logger);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    int result = -1;
    if(info->ai_family == AF_INET && multiCastType == MULTICASTTYPE_IPV4) {
        result = UA_setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                               &req.ipv4, sizeof(req.ipv4));
#if UA_IPV6
    } else if(info->ai_family == AF_INET6 && multiCastType == MULTICASTTYPE_IPV6) {
        result = UA_setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                               &req.ipv6, sizeof(req.ipv6));
#endif
    }

    if(result < 0) {
        UA_LOG_SOCKET_ERRNO_WRAP(
            UA_LOG_ERROR(logger, UA_LOGCATEGORY_NETWORK,
                         "UDP %u\t| Cannot set socket for multicast receiving. Error: %s",
                         (unsigned)fd, errno_str));
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
setupSendMultiCast(UA_FD fd, struct addrinfo *info, const UA_KeyValueMap *params,
                   MultiCastType multiCastType, const UA_Logger *logger) {
    MulticastRequest req;
    UA_StatusCode res = setupMulticastRequest(fd, &req, params, info, logger);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    int result = -1;
    if(info->ai_family == AF_INET && multiCastType == MULTICASTTYPE_IPV4) {
        result = UA_setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
                            &req.ipv4, sizeof(req.ipv4));
#if UA_IPV6
    } else if(info->ai_family == AF_INET6 && multiCastType == MULTICASTTYPE_IPV6) {
        result = UA_setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF,
                            &req.ipv6.ipv6mr_interface,
                            sizeof(req.ipv6.ipv6mr_interface));
#endif
    }

    if(result < 0) {
        UA_LOG_SOCKET_ERRNO_WRAP(
            UA_LOG_ERROR(logger, UA_LOGCATEGORY_NETWORK,
                         "UDP %u\t| Cannot set socket for multicast sending. Error: %s",
                         (unsigned)fd, errno_str));
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_GOOD;
}

/* Test if the ConnectionManager can be stopped */
static void
UDP_checkStopped(UA_LWIPConnectionManager *pcm) {
    UA_LOCK_ASSERT(&((UA_EventLoopLWIP*)pcm->cm.eventSource.eventLoop)->elMutex);

    if(pcm->fdsSize == 0 &&
       pcm->cm.eventSource.state == UA_EVENTSOURCESTATE_STOPPING) {
        UA_LOG_DEBUG(pcm->cm.eventSource.eventLoop->logger, UA_LOGCATEGORY_NETWORK,
                     "UDP\t| All sockets closed, the EventLoop has stopped");
        pcm->cm.eventSource.state = UA_EVENTSOURCESTATE_STOPPED;
    }
}

/* This method must not be called from the application directly, but from within
 * the EventLoop. Otherwise we cannot be sure whether the file descriptor is
 * still used after calling close. */
static void
UDP_close(UA_LWIPConnectionManager *pcm, UDP_FD *conn) {
    UA_EventLoopLWIP *el = (UA_EventLoopLWIP*)pcm->cm.eventSource.eventLoop;
    UA_LOCK_ASSERT(&el->elMutex);

    UA_LOG_DEBUG(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                 "UDP %u\t| Closing connection",
                 (unsigned)conn->rfd.fd);

    /* Deregister from the EventLoop */
    UA_EventLoopLWIP_deregisterFD(el, &conn->rfd);

    /* Deregister internally */
    ZIP_REMOVE(UA_FDTree, &pcm->fds, &conn->rfd);
    UA_assert(pcm->fdsSize > 0);
    pcm->fdsSize--;

    /* Signal closing to the application */
    UA_UNLOCK(&el->elMutex);
    conn->applicationCB(&pcm->cm, (uintptr_t)conn->rfd.fd,
                        conn->application, &conn->context,
                        UA_CONNECTIONSTATE_CLOSING,
                        &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);
    UA_LOCK(&el->elMutex);

    /* Close the socket */
    int ret = UA_close(conn->rfd.fd);
    if(ret == 0) {
        UA_LOG_INFO(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                    "UDP %u\t| Socket closed", (unsigned)conn->rfd.fd);
    } else {
        UA_LOG_SOCKET_ERRNO_WRAP(
           UA_LOG_WARNING(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                          "UDP %u\t| Could not close the socket (%s)",
                          (unsigned)conn->rfd.fd, errno_str));
    }

    UA_free(conn);

    /* Stop if the ucm is stopping and this was the last open socket */
    UDP_checkStopped(pcm);
}

static void
UDP_delayedClose(void *application, void *context) {
    UA_LWIPConnectionManager *pcm = (UA_LWIPConnectionManager*)application;
    UA_EventLoopLWIP *el = (UA_EventLoopLWIP*)pcm->cm.eventSource.eventLoop;
    UDP_FD *conn = (UDP_FD*)context;
    UA_LOG_DEBUG(el->eventLoop.logger, UA_LOGCATEGORY_EVENTLOOP,
                 "UDP %u\t| Delayed closing of the connection",
                 (unsigned)conn->rfd.fd);
    UA_LOCK(&el->elMutex);
    UDP_close(pcm, conn);
    UA_UNLOCK(&el->elMutex);
}

/* Gets called when a socket receives data or closes */
static void
UDP_connectionSocketCallback(UA_LWIPConnectionManager *pcm, UDP_FD *conn,
                             short event) {
    UA_EventLoopLWIP *el = (UA_EventLoopLWIP*)pcm->cm.eventSource.eventLoop;
    UA_LOCK_ASSERT(&el->elMutex);

    UA_LOG_DEBUG(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                 "UDP %u\t| Activity on the socket",
                 (unsigned)conn->rfd.fd);

    if(event == UA_FDEVENT_ERR) {
        UA_LOG_SOCKET_ERRNO_WRAP(
           UA_LOG_DEBUG(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                        "UDP %u\t| recv signaled the socket was shutdown (%s)",
                        (unsigned)conn->rfd.fd, errno_str));
        UDP_close(pcm, conn);
        return;
    }

    UA_LOG_DEBUG(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                 "UDP %u\t| Allocate receive buffer", (unsigned)conn->rfd.fd);

    /* Use the already allocated receive-buffer */
    UA_ByteString response = pcm->rxBuffer;

    /* Receive */
    struct sockaddr_storage source;
#ifndef _WIN32
    socklen_t sourceSize = (socklen_t)sizeof(struct sockaddr_storage);
    ssize_t ret = UA_recvfrom(conn->rfd.fd, (char*)response.data, response.length,
                           MSG_DONTWAIT, (struct sockaddr*)&source, &sourceSize);
#else
    int sourceSize = (int)sizeof(struct sockaddr_storage);
    int ret = UA_recvfrom(conn->rfd.fd, (char*)response.data, (int)response.length,
                       MSG_DONTWAIT, (struct sockaddr*)&source, &sourceSize);
#endif

    /* Receive has failed */
    if(ret <= 0) {
        if(UA_ERRNO == UA_INTERRUPTED)
            return;

        /* Orderly shutdown of the socket. We can immediately close as no method
         * "below" in the call stack will use the socket in this iteration of
         * the EventLoop. */
        UA_LOG_SOCKET_ERRNO_WRAP(
           UA_LOG_DEBUG(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                        "UDP %u\t| recv signaled the socket was shutdown (%s)",
                        (unsigned)conn->rfd.fd, errno_str));
        UDP_close(pcm, conn);
        return;
    }

    response.length = (size_t)ret; /* Set the length of the received buffer */

    /* Extract message source and port */
    char sourceAddr[64];
    UA_UInt16 sourcePort;
    switch(source.ss_family) {
        case AF_INET:
            UA_inet_ntop(AF_INET, &((struct sockaddr_in *)&source)->sin_addr,
                    sourceAddr, 64);
            sourcePort = htons(((struct sockaddr_in *)&source)->sin_port);
            break;
#if UA_IPV6
        case AF_INET6:
            UA_inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)&source)->sin6_addr),
                    sourceAddr, 64);
            sourcePort = htons(((struct sockaddr_in6 *)&source)->sin6_port);
            break;
#endif
        default:
            sourceAddr[0] = 0;
            sourcePort = 0;
    }

    UA_String sourceAddrStr = UA_STRING(sourceAddr);
    UA_KeyValuePair kvp[2];
    kvp[0].key = UA_QUALIFIEDNAME(0, "remote-address");
    UA_Variant_setScalar(&kvp[0].value, &sourceAddrStr, &UA_TYPES[UA_TYPES_STRING]);
    kvp[1].key = UA_QUALIFIEDNAME(0, "remote-port");
    UA_Variant_setScalar(&kvp[1].value, &sourcePort, &UA_TYPES[UA_TYPES_UINT16]);
    UA_KeyValueMap kvm = {2, kvp};

    UA_LOG_DEBUG(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                 "UDP %u\t| Received message of size %u from %s on port %u",
                 (unsigned)conn->rfd.fd, (unsigned)ret,
                 sourceAddr, sourcePort);

    /* Callback to the application layer */
    UA_UNLOCK(&el->elMutex);
    conn->applicationCB(&pcm->cm, (uintptr_t)conn->rfd.fd,
                        conn->application, &conn->context,
                        UA_CONNECTIONSTATE_ESTABLISHED,
                        &kvm, response);
    UA_LOCK(&el->elMutex);
}

static UA_StatusCode
UDP_registerListenSocket(UA_LWIPConnectionManager *pcm, UA_UInt16 port,
                         struct addrinfo *info, const UA_KeyValueMap *params,
                         void *application, void *context,
                         UA_ConnectionManager_connectionCallback connectionCallback,
                         UA_Boolean validate) {
    UA_EventLoopLWIP *el = (UA_EventLoopLWIP*)pcm->cm.eventSource.eventLoop;
    UA_LOCK_ASSERT(&el->elMutex);

    /* Get logging information */
    char hoststr[UA_MAXHOSTNAME_LENGTH];
    int get_res = UA_getnameinfo(info->ai_addr, info->ai_addrlen,
                                 hoststr, sizeof(hoststr),
                                 NULL, 0, NI_NUMERICHOST);
    if(get_res != 0) {
        hoststr[0] = 0;
        UA_LOG_SOCKET_ERRNO_WRAP(
           UA_LOG_WARNING(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                          "UDP\t| getnameinfo(...) could not resolve the hostname (%s)",
                          errno_str));
        if(validate) {
            return UA_STATUSCODE_BADCONNECTIONREJECTED;
        }
    }

    /* Create the listen socket */
    UA_FD listenSocket = UA_socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    if(listenSocket == UA_INVALID_FD) {
        UA_LOG_SOCKET_ERRNO_WRAP(
           UA_LOG_WARNING(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                          "UDP %u\t| Error opening the listen socket for "
                          "\"%s\" on port %u (%s)",
                          (unsigned)listenSocket, hoststr, port, errno_str));
        return UA_STATUSCODE_BADCONNECTIONREJECTED;
    }

    /* Set the socket configuration per the parameters */
    UA_StatusCode res =
        setConnectionConfig(listenSocket, params,
                            info->ai_family, el->eventLoop.logger);
    if(res != UA_STATUSCODE_GOOD) {
        UA_close(listenSocket);
        return UA_STATUSCODE_BADCONNECTIONREJECTED;
    }

    /* Are we going to prepare a socket for multicast? */
    MultiCastType mc = multiCastType(info);

    /* Bind socket to the address */
    int ret = UA_bind(listenSocket, info->ai_addr, (socklen_t)info->ai_addrlen);

    /* Get the port being used if dynamic porting was used */
    if(port == 0) {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        socklen_t len = sizeof(sin);
        UA_getsockname(listenSocket, (struct sockaddr *)&sin, &len);
        port = ntohs(sin.sin_port);
    }

    UA_LOG_INFO(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
            "UDP %u\t| New listen socket for \"%s\" on port %u",
            (unsigned)listenSocket, hoststr, port);

    if(ret < 0) {
        UA_LOG_SOCKET_ERRNO_WRAP(
           UA_LOG_WARNING(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                          "UDP %u\t| Error binding the socket to the address (%s), closing",
                          (unsigned)listenSocket, errno_str));
        UA_close(listenSocket);
        return UA_STATUSCODE_BADCONNECTIONREJECTED;
    }

    /* Enable multicast if this is a multicast address */
    if(mc != MULTICASTTYPE_NONE) {
        res = setupListenMultiCast(listenSocket, info, params,
                                   mc, el->eventLoop.logger);
        if(res != UA_STATUSCODE_GOOD) {
            UA_close(listenSocket);
            return res;
        }
    }

    /* Validation is complete - close and return */
    if(validate) {
        UA_close(listenSocket);
        return UA_STATUSCODE_GOOD;
    }

    /* Allocate the UA_RegisteredFD */
    UDP_FD *newudpfd = (UDP_FD*)UA_calloc(1, sizeof(UDP_FD));
    if(!newudpfd) {
        UA_LOG_WARNING(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                       "UDP %u\t| Error allocating memory for the socket, closing",
                       (unsigned)listenSocket);
        UA_close(listenSocket);
        return UA_STATUSCODE_BADCONNECTIONREJECTED;
    }

    newudpfd->rfd.fd = listenSocket;
    newudpfd->rfd.es = &pcm->cm.eventSource;
    newudpfd->rfd.listenEvents = UA_FDEVENT_IN;
    newudpfd->rfd.eventSourceCB = (UA_FDCallback)UDP_connectionSocketCallback;
    newudpfd->applicationCB = connectionCallback;
    newudpfd->application = application;
    newudpfd->context = context;

    /* Register in the EventLoop */
    res = UA_EventLoopLWIP_registerFD(el, &newudpfd->rfd);
    if(res != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                       "UDP %u\t| Error registering the socket, closing",
                       (unsigned)listenSocket);
        UA_free(newudpfd);
        UA_close(listenSocket);
        return res;
    }

    /* Register internally in the EventSource */
    ZIP_INSERT(UA_FDTree, &pcm->fds, &newudpfd->rfd);
    pcm->fdsSize++;

    /* Register the listen socket in the application */
    UA_UNLOCK(&el->elMutex);
    connectionCallback(&pcm->cm, (uintptr_t)newudpfd->rfd.fd,
                       application, &newudpfd->context,
                       UA_CONNECTIONSTATE_ESTABLISHED,
                       &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);
    UA_LOCK(&el->elMutex);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
UDP_registerListenSockets(UA_LWIPConnectionManager *pcm, const char *hostname,
                          UA_UInt16 port, const UA_KeyValueMap *params,
                          void *application, void *context,
                          UA_ConnectionManager_connectionCallback connectionCallback,
                          UA_Boolean validate) {
    UA_LOCK_ASSERT(&((UA_EventLoopLWIP*)pcm->cm.eventSource.eventLoop)->elMutex);

#if LWIP_DNS || defined(UA_ARCHITECTURE_POSIX)
    /* Get all the interface and IPv4/6 combinations for the configured hostname */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
#if UA_IPV6
    hints.ai_family = AF_UNSPEC; /* Allow IPv4 and IPv6 */
#else
    hints.ai_family = AF_INET;   /* IPv4 only */
#endif
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_PASSIVE;

    /* Set up the port string */
    char portstr[6];
    mp_snprintf(portstr, 6, "%d", port);

    int retcode = UA_getaddrinfo(hostname, portstr, &hints, &res);
    if(retcode != 0) {
        UA_LOG_SOCKET_ERRNO_GAI_WRAP(
           UA_LOG_WARNING(pcm->cm.eventSource.eventLoop->logger,
                          UA_LOGCATEGORY_NETWORK,
                          "UDP\t| getaddrinfo lookup for \"%s\" on port %u failed (%s)",
                          hostname, port, errno_str));
        return UA_STATUSCODE_BADCONNECTIONREJECTED;
    }

    /* Add listen sockets */
    struct addrinfo *ai = res;
    UA_StatusCode rv = UA_STATUSCODE_GOOD;
    while(ai) {
        rv = UDP_registerListenSocket(pcm, port, ai, params, application,
                                      context, connectionCallback, validate);
        if(rv != UA_STATUSCODE_GOOD)
            break;
        ai = ai->ai_next;
    }
    UA_freeaddrinfo(res);
    return rv;
#else
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
#if UA_IPV6
    hints.ai_family = AF_UNSPEC; /* Allow IPv4 and IPv6 */
#else
    hints.ai_family = AF_INET;   /* IPv4 only */
#endif
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_PASSIVE;

    /* TODO: What happens with IPv6 and specific ip? */
    /*Set up the sockaddr_in structure for IPv4 */
    struct sockaddr_in addr4;
    memset(&addr4, 0, sizeof addr4);
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(port);
    if(inet_pton(AF_INET, hostname, &addr4.sin_addr) <= 0) {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    /* Manually set ai_addr and ai_addrlen */
    hints.ai_addr = (struct sockaddr *)&addr4;
    hints.ai_addrlen = sizeof(addr4);

    return UDP_registerListenSocket(pcm, port, &hints, params, application, context,
                                    connectionCallback, validate);
#endif
}

/* Close the connection via a delayed callback */
static void
UDP_shutdown(UA_ConnectionManager *cm, UA_RegisteredFD *rfd) {
    UA_EventLoopLWIP *el = (UA_EventLoopLWIP *)cm->eventSource.eventLoop;
    UA_LOCK_ASSERT(&el->elMutex);

    if(rfd->dc.callback) {
        UA_LOG_INFO(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                    "UDP %u\t| Cannot close - already closing",
                    (unsigned)rfd->fd);
        return;
    }

    /* Shutdown the socket to cancel the current select/epoll */
    UA_shutdown(rfd->fd, UA_SHUT_RDWR);

    UA_LOG_DEBUG(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                 "UDP %u\t| Shutdown called", (unsigned)rfd->fd);

    UA_DelayedCallback *dc = &rfd->dc;
    dc->callback = UDP_delayedClose;
    dc->application = cm;
    dc->context = rfd;

    /* Adding a delayed callback does not take a lock */
    UA_EventLoopLWIP_addDelayedCallback((UA_EventLoop*)el, dc);
}

static UA_StatusCode
UDP_shutdownConnection(UA_ConnectionManager *cm, uintptr_t connectionId) {
    UA_LWIPConnectionManager *pcm = (UA_LWIPConnectionManager*)cm;
    UA_EventLoopLWIP *el = (UA_EventLoopLWIP *)cm->eventSource.eventLoop;
    UA_FD fd = (UA_FD)connectionId;

    UA_LOCK(&el->elMutex);
    UA_RegisteredFD *rfd = ZIP_FIND(UA_FDTree, &pcm->fds, &fd);
    if(!rfd) {
        UA_LOG_WARNING(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                       "UDP\t| Cannot close UDP connection %u - not found",
                       (unsigned)connectionId);
        UA_UNLOCK(&el->elMutex);
        return UA_STATUSCODE_BADNOTFOUND;
    }
    UDP_shutdown(cm, rfd);
    UA_UNLOCK(&el->elMutex);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
UDP_sendWithConnection(UA_ConnectionManager *cm, uintptr_t connectionId,
                       const UA_KeyValueMap *params,
                       UA_ByteString *buf) {
    UA_LWIPConnectionManager *pcm = (UA_LWIPConnectionManager*)cm;
    UA_EventLoopLWIP *el = (UA_EventLoopLWIP*)cm->eventSource.eventLoop;

    UA_LOCK(&el->elMutex);

    /* Look up the registered UDP socket */
    UA_FD fd = (UA_FD)connectionId;
    UDP_FD *conn = (UDP_FD*)ZIP_FIND(UA_FDTree, &pcm->fds, &fd);
    if(!conn) {
        UA_UNLOCK(&el->elMutex);
        UA_ByteString_clear(buf);
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    /* Send the full buffer. This may require several calls to send */
    size_t nWritten = 0;
    do {
        ssize_t n = 0;
        do {
            UA_LOG_DEBUG(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                         "UDP %u\t| Attempting to send", (unsigned)connectionId);

            /* Prevent OS signals when sending to a closed socket */
            int flags = MSG_NOSIGNAL;
            size_t bytes_to_send = buf->length - nWritten;
            n = UA_sendto((UA_FD)connectionId, (const char*)buf->data + nWritten,
                          bytes_to_send, flags, (struct sockaddr*)&conn->sendAddr,
                          conn->sendAddrLength);
            if(n < 0) {
                /* An error we cannot recover from? */
                if(UA_ERRNO != UA_INTERRUPTED &&
                   UA_ERRNO != UA_WOULDBLOCK &&
                   UA_ERRNO != UA_AGAIN) {
                    UA_LOG_SOCKET_ERRNO_WRAP(
                       UA_LOG_ERROR(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                                    "UDP %u\t| Send failed with error %s",
                                    (unsigned)connectionId, errno_str));
                    UA_UNLOCK(&el->elMutex);
                    UDP_shutdownConnection(cm, connectionId);
                    UA_ByteString_clear(buf);
                    return UA_STATUSCODE_BADCONNECTIONCLOSED;
                }

                /* Poll for the socket resources to become available and retry
                 * (blocking) */
                int poll_ret;
                struct pollfd tmp_poll_fd;
                tmp_poll_fd.fd = (UA_FD)connectionId;
                tmp_poll_fd.events = UA_POLLOUT;
                do {
                    poll_ret = UA_poll(&tmp_poll_fd, 1, 100);
                    if(poll_ret < 0 && UA_ERRNO != UA_INTERRUPTED) {
                        UA_LOG_SOCKET_ERRNO_WRAP(
                           UA_LOG_ERROR(el->eventLoop.logger,
                                        UA_LOGCATEGORY_NETWORK,
                                        "UDP %u\t| Send failed with error %s",
                                        (unsigned)connectionId, errno_str));
                        UA_UNLOCK(&el->elMutex);
                        UDP_shutdownConnection(cm, connectionId);
                        UA_ByteString_clear(buf);
                        return UA_STATUSCODE_BADCONNECTIONCLOSED;
                    }
                } while(poll_ret <= 0);
            }
        } while(n < 0);
        nWritten += (size_t)n;
    } while(nWritten < buf->length);

    /* Free the buffer */
    UA_UNLOCK(&el->elMutex);
    UA_ByteString_clear(buf);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
registerSocketAndDestinationForSend(const UA_KeyValueMap *params,
                                    const char *hostname, struct addrinfo *info,
                                    int error, UDP_FD *ufd, UA_FD *sock,
                                    const UA_Logger *logger) {
    UA_FD newSock = UA_socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    *sock = newSock;
    if(newSock == UA_INVALID_FD) {
        UA_LOG_SOCKET_ERRNO_WRAP(
            UA_LOG_WARNING(logger, UA_LOGCATEGORY_NETWORK,
                           "UDP\t| Could not create socket to connect to %s (%s)",
                           hostname, errno_str));
        return UA_STATUSCODE_BADDISCONNECT;
    }
    UA_StatusCode res = setConnectionConfig(newSock, params, info->ai_family, logger);
    if(res != UA_STATUSCODE_GOOD) {
        UA_close(newSock);
        return res;
    }

    /* Prepare socket for multicast */
    MultiCastType mc = multiCastType(info);
    if(mc != MULTICASTTYPE_NONE) {
        res = setupSendMultiCast(newSock, info, params, mc, logger);
        if(res != UA_STATUSCODE_GOOD) {
            UA_close(newSock);
            return res;
        }
    }

    memcpy(&ufd->sendAddr, info->ai_addr, info->ai_addrlen);
    ufd->sendAddrLength = info->ai_addrlen;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
UDP_openSendConnection(UA_LWIPConnectionManager *pcm, const UA_KeyValueMap *params,
                       void *application, void *context,
                       UA_ConnectionManager_connectionCallback connectionCallback,
                       UA_Boolean validate) {
    UA_EventLoopLWIP *el = (UA_EventLoopLWIP *)pcm->cm.eventSource.eventLoop;
    UA_LOCK_ASSERT(&el->elMutex);

    /* Get the connection parameters */
    char hostname[UA_MAXHOSTNAME_LENGTH];
    char portStr[UA_MAXPORTSTR_LENGTH];
    struct addrinfo *info = NULL;

#if LWIP_DNS || defined(UA_ARCHITECTURE_POSIX)
    int error = getConnectionInfoFromParams(params, hostname,
                                            portStr, &info, el->eventLoop.logger);
    if(error < 0 || info == NULL) {
        if(info != NULL) {
            UA_freeaddrinfo(info);
        }
        UA_LOG_ERROR(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                     "UDP\t| Opening a connection failed");
        return UA_STATUSCODE_BADCONNECTIONREJECTED;
    }
    UA_LOG_DEBUG(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                 "UDP\t| Open a connection to \"%s\" on port %s", hostname, portStr);

#else
    int error = getHostAndPortFromParams(params, hostname, portStr, el->eventLoop.logger);
    if(error < 0) {
        UA_LOG_ERROR(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                     "UDP\t| Opening a connection failed");
        return UA_STATUSCODE_BADCONNECTIONREJECTED;
    }
    UA_LOG_DEBUG(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
             "UDP\t| Open a connection to \"%s\" on port %s", hostname, portStr);

    info = (struct addrinfo*)UA_calloc(1, sizeof(struct addrinfo));
#if UA_IPV6
    info->ai_family = AF_UNSPEC; /* Allow IPv4 and IPv6 */
#else
    info->ai_family = AF_INET;   /* IPv4 only */
#endif
    info->ai_socktype = SOCK_DGRAM;
    info->ai_protocol = IPPROTO_UDP;

    char *endptr;
    unsigned long port = strtoul(portStr, &endptr, 10);
    if(*endptr != '\0' || port > UINT16_MAX) {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    struct sockaddr_in addr4;
    memset(&addr4, 0, sizeof addr4);
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(port);
    if(inet_pton(AF_INET, hostname, &addr4.sin_addr) <= 0) {
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    /* Manually set ai_addr and ai_addrlen */
    info->ai_addr = (struct sockaddr *)&addr4;
    info->ai_addrlen = sizeof(addr4);
#endif

    /* Allocate the UA_RegisteredFD */
    UDP_FD *conn = (UDP_FD*)UA_calloc(1, sizeof(UDP_FD));
    if(!conn) {
        UA_LOG_WARNING(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                       "UDP\t| Error allocating memory for the socket, closing");
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    /* Create a socket and register the destination address from the provided parameters */
    UA_FD newSock = UA_INVALID_FD;
    UA_StatusCode res =
        registerSocketAndDestinationForSend(params, hostname, info,
                                            error, conn, &newSock,
                                            el->eventLoop.logger);
#if LWIP_DNS || defined(UA_ARCHITECTURE_POSIX)
    UA_freeaddrinfo(info);
#else
    UA_free(info);
#endif
    if(validate && res == UA_STATUSCODE_GOOD) {
        UA_LOG_INFO(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                    "UDP %u\t| Connection validated to \"%s\" on port %s",
                    (unsigned)newSock, hostname, portStr);
        UA_close(newSock);
        UA_free(conn);
        return UA_STATUSCODE_GOOD;
    }
    if(res != UA_STATUSCODE_GOOD) {
        UA_free(conn);
        return res;
    }

    conn->rfd.fd = newSock;
    conn->rfd.listenEvents = 0;
    conn->rfd.es = &pcm->cm.eventSource;
    conn->rfd.eventSourceCB = (UA_FDCallback)UDP_connectionSocketCallback;
    conn->applicationCB = connectionCallback;
    conn->application = application;
    conn->context = context;

    /* Register the fd to trigger when output is possible (the connection is open) */
    res = UA_EventLoopLWIP_registerFD(el, &conn->rfd);
    if(res != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                       "UDP\t| Registering the socket for %s failed", hostname);
        UA_close(newSock);
        UA_free(conn);
        return res;
    }

    /* Register internally in the EventSource */
    ZIP_INSERT(UA_FDTree, &pcm->fds, &conn->rfd);
    pcm->fdsSize++;

    UA_LOG_INFO(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                "UDP %u\t| New connection to \"%s\" on port %s",
                (unsigned)newSock, hostname, portStr);

    /* Signal the connection as opening. The connection fully opens in the next
     * iteration of the EventLoop */
    UA_UNLOCK(&el->elMutex);
    connectionCallback(&pcm->cm, (uintptr_t)newSock, application,
                       &conn->context, UA_CONNECTIONSTATE_ESTABLISHED,
                       &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);
    UA_LOCK(&el->elMutex);

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
UDP_openReceiveConnection(UA_LWIPConnectionManager *pcm, const UA_KeyValueMap *params,
                          void *application, void *context,
                          UA_ConnectionManager_connectionCallback connectionCallback,
                          UA_Boolean validate) {
    UA_EventLoopLWIP *el = (UA_EventLoopLWIP*)pcm->cm.eventSource.eventLoop;
    UA_LOCK_ASSERT(&el->elMutex);

    /* Get the port */
    const UA_UInt16 *port = (const UA_UInt16*)
        UA_KeyValueMap_getScalar(params, UDPConfigParameters[UDP_PARAMINDEX_PORT].name,
                                 &UA_TYPES[UA_TYPES_UINT16]);
    UA_assert(port); /* checked before */

    /* Get the hostname configuration */
    const UA_Variant *addrs =
        UA_KeyValueMap_get(params, UDPConfigParameters[UDP_PARAMINDEX_ADDR].name);
    size_t addrsSize = 0;
    if(addrs) {
        UA_assert(addrs->type == &UA_TYPES[UA_TYPES_STRING]);
        if(UA_Variant_isScalar(addrs))
            addrsSize = 1;
        else
            addrsSize = addrs->arrayLength;
    }

    /* No hostname configured -> listen on all interfaces */
    if(addrsSize == 0) {
        UA_LOG_DEBUG(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                     "UDP\t| Listening on all interfaces");
        return UDP_registerListenSockets(pcm, NULL, *port, params, application,
                                         context, connectionCallback, validate);
    }

    /* Iterate over the configured hostnames */
    UA_String *hostStrings = (UA_String*)addrs->data;
    for(size_t i = 0; i < addrsSize; i++) {
        char hn[UA_MAXHOSTNAME_LENGTH];
        if(hostStrings[i].length >= sizeof(hn))
            continue;
        memcpy(hn, hostStrings[i].data, hostStrings->length);
        hn[hostStrings->length] = '\0';
        UA_StatusCode rv =
            UDP_registerListenSockets(pcm, hn, *port, params, application,
                                      context, connectionCallback, validate);
        if(rv != UA_STATUSCODE_GOOD)
            return rv;
    }

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
UDP_openConnection(UA_ConnectionManager *cm, const UA_KeyValueMap *params,
                   void *application, void *context,
                   UA_ConnectionManager_connectionCallback connectionCallback) {
    UA_LWIPConnectionManager *pcm = (UA_LWIPConnectionManager*)cm;
    UA_EventLoopLWIP *el = (UA_EventLoopLWIP*)cm->eventSource.eventLoop;
    UA_LOCK(&el->elMutex);

    if(cm->eventSource.state != UA_EVENTSOURCESTATE_STARTED) {
        UA_LOG_ERROR(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                     "UDP\t| Cannot open a connection for a "
                     "ConnectionManager that is not started");
        UA_UNLOCK(&el->elMutex);
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    /* Check the parameters */
    UA_StatusCode res =
        UA_KeyValueRestriction_validate(el->eventLoop.logger, "UDP",
                                        &UDPConfigParameters[1],
                                        UDP_PARAMETERSSIZE-1, params);
    if(res != UA_STATUSCODE_GOOD) {
        UA_UNLOCK(&el->elMutex);
        return res;
    }

    UA_Boolean validate = false;
    const UA_Boolean *validationValue = (const UA_Boolean*)
        UA_KeyValueMap_getScalar(params, UDPConfigParameters[UDP_PARAMINDEX_VALIDATE].name,
                                 &UA_TYPES[UA_TYPES_BOOLEAN]);
    if(validationValue)
        validate = *validationValue;

    UA_Boolean listen = false;
    const UA_Boolean *listenValue = (const UA_Boolean*)
        UA_KeyValueMap_getScalar(params, UDPConfigParameters[UDP_PARAMINDEX_LISTEN].name,
                                 &UA_TYPES[UA_TYPES_BOOLEAN]);
    if(listenValue)
        listen = *listenValue;

    if(listen) {
        res = UDP_openReceiveConnection(pcm, params, application, context,
                                        connectionCallback, validate);
    } else {
        res = UDP_openSendConnection(pcm, params, application, context,
                                     connectionCallback, validate);
    }
    UA_UNLOCK(&el->elMutex);
    return res;
}

static UA_StatusCode
UDP_eventSourceStart(UA_ConnectionManager *cm) {
    UA_LWIPConnectionManager *pcm = (UA_LWIPConnectionManager*)cm;
    UA_EventLoopLWIP *el = (UA_EventLoopLWIP*)cm->eventSource.eventLoop;
    if(!el)
        return UA_STATUSCODE_BADINTERNALERROR;

    UA_LOCK(&el->elMutex);

    /* Check the state */
    if(cm->eventSource.state != UA_EVENTSOURCESTATE_STOPPED) {
        UA_LOG_ERROR(el->eventLoop.logger, UA_LOGCATEGORY_NETWORK,
                     "UDP\t| To start the ConnectionManager, "
                     "it has to be registered in an EventLoop and not started");
        UA_UNLOCK(&el->elMutex);
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    /* Check the parameters */
    UA_StatusCode res =
        UA_KeyValueRestriction_validate(el->eventLoop.logger, "UDP",
                                        UDPConfigParameters, 1,
                                        &cm->eventSource.params);
    if(res != UA_STATUSCODE_GOOD)
        goto finish;

    /* Allocate the rx buffer */
    res = UA_EventLoopLWIP_allocateStaticBuffers(pcm);
    if(res != UA_STATUSCODE_GOOD)
        goto finish;

    /* Set the EventSource to the started state */
    cm->eventSource.state = UA_EVENTSOURCESTATE_STARTED;

 finish:
    UA_UNLOCK(&el->elMutex);
    return res;
}

static void *
UDP_shutdownCB(void *application, UA_RegisteredFD *rfd) {
    UA_ConnectionManager *cm = (UA_ConnectionManager*)application;
    UDP_shutdown(cm, rfd);
    return NULL;
}

static void
UDP_eventSourceStop(UA_ConnectionManager *cm) {
    UA_LWIPConnectionManager *pcm = (UA_LWIPConnectionManager*)cm;
    UA_EventLoopLWIP *el = (UA_EventLoopLWIP*)cm->eventSource.eventLoop;
    (void)el;
    UA_LOCK(&el->elMutex);

    UA_LOG_INFO(cm->eventSource.eventLoop->logger, UA_LOGCATEGORY_NETWORK,
                "UDP\t| Shutting down the ConnectionManager");

    /* Prevent new connections to open */
    cm->eventSource.state = UA_EVENTSOURCESTATE_STOPPING;

    /* Shutdown all existing connection */
    ZIP_ITER(UA_FDTree, &pcm->fds, UDP_shutdownCB, cm);

    /* Check if stopped once more (also checking inside UDP_close, but there we
     * don't check if there is no rfd at all) */
    UDP_checkStopped(pcm);

    UA_UNLOCK(&el->elMutex);
}

static UA_StatusCode
UDP_eventSourceDelete(UA_ConnectionManager *cm) {
    UA_LWIPConnectionManager *pcm = (UA_LWIPConnectionManager*)cm;
    if(cm->eventSource.state >= UA_EVENTSOURCESTATE_STARTING) {
        UA_LOG_ERROR(cm->eventSource.eventLoop->logger, UA_LOGCATEGORY_EVENTLOOP,
                     "UDP\t| The EventSource must be stopped before it can be deleted");
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    UA_ByteString_clear(&pcm->rxBuffer);
    UA_KeyValueMap_clear(&cm->eventSource.params);
    UA_String_clear(&cm->eventSource.name);
    UA_free(cm);

    return UA_STATUSCODE_GOOD;
}

static const char *udpName = "udp";

UA_ConnectionManager *
UA_ConnectionManager_new_LWIP_UDP(const UA_String eventSourceName) {
    UA_LWIPConnectionManager *cm = (UA_LWIPConnectionManager*)
        UA_calloc(1, sizeof(UA_LWIPConnectionManager));
    if(!cm)
        return NULL;

    cm->cm.eventSource.eventSourceType = UA_EVENTSOURCETYPE_CONNECTIONMANAGER;
    UA_String_copy(&eventSourceName, &cm->cm.eventSource.name);
    cm->cm.eventSource.start = (UA_StatusCode (*)(UA_EventSource *))UDP_eventSourceStart;
    cm->cm.eventSource.stop = (void (*)(UA_EventSource *))UDP_eventSourceStop;
    cm->cm.eventSource.free = (UA_StatusCode (*)(UA_EventSource *))UDP_eventSourceDelete;
    cm->cm.protocol = UA_STRING((char*)(uintptr_t)udpName);
    cm->cm.openConnection = UDP_openConnection;
    cm->cm.allocNetworkBuffer = UA_EventLoopLWIP_allocNetworkBuffer;
    cm->cm.freeNetworkBuffer = UA_EventLoopLWIP_freeNetworkBuffer;
    cm->cm.sendWithConnection = UDP_sendWithConnection;
    cm->cm.closeConnection = UDP_shutdownConnection;
    return &cm->cm;
}
