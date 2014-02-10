/*********************************************************************
   PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
   See LICENSE and COPYING for usage.


   Authors: Daniele Lacamera
 *********************************************************************/


#include "pico_config.h"
#include "pico_queue.h"
#include "pico_socket.h"
#include "pico_ipv4.h"
#include "pico_ipv6.h"
#include "pico_udp.h"
#include "pico_tcp.h"
#include "pico_stack.h"
#include "pico_icmp4.h"
#include "pico_nat.h"
#include "pico_tree.h"
#include "pico_device.h"
#include "pico_socket_multicast.h"
#include "pico_socket_tcp.h"

#if defined (PICO_SUPPORT_IPV4) || defined (PICO_SUPPORT_IPV6)
#if defined (PICO_SUPPORT_TCP) || defined (PICO_SUPPORT_UDP)

#define UDP_FRAME_OVERHEAD (sizeof(struct pico_frame))

#define PROTO(s) ((s)->proto->proto_number)
#define PICO_SOCKET4_MTU 1480 /* Ethernet MTU(1500) - IP header size(20) */
#define PICO_SOCKET6_MTU 1460 /* Ethernet MTU(1500) - IP header size(40) */
#define TCP_STATE(s) (s->state & PICO_SOCKET_STATE_TCP)

#ifdef PICO_SUPPORT_MUTEX
static void *Mutex = NULL;
#define PICOTCP_MUTEX_LOCK(x) { \
        if (x == NULL) \
            x = pico_mutex_init(); \
        pico_mutex_lock(x); \
}
#define PICOTCP_MUTEX_UNLOCK(x) pico_mutex_unlock(x)

#else
#define PICOTCP_MUTEX_LOCK(x) do {} while(0)
#define PICOTCP_MUTEX_UNLOCK(x) do {} while(0)
#endif


#define PROTO(s) ((s)->proto->proto_number)

#define PICO_SOCKET_MTU 1480 /* Ethernet MTU(1500) - IP header size(20) */

#ifdef PICO_SUPPORT_IPV4
# define IS_SOCK_IPV4(s) ((s->net == &pico_proto_ipv4))
#else
# define IS_SOCK_IPV4(s) (0)
#endif

#ifdef PICO_SUPPORT_IPV6
# define IS_SOCK_IPV6(s) ((s->net == &pico_proto_ipv6))
#else
# define IS_SOCK_IPV6(s) (0)
#endif

#ifdef PICO_SUPPORT_IPFRAG
# define frag_dbg(...) do {} while(0)
#endif


static struct pico_sockport *sp_udp = NULL, *sp_tcp = NULL;

struct pico_frame *pico_socket_frame_alloc(struct pico_socket *s, uint16_t len);

static int32_t socket_cmp(void *ka, void *kb)
{
    struct pico_socket *a = ka, *b = kb;
    uint32_t a_is_ip6 = is_sock_ipv6(a);
    uint32_t b_is_ip6 = is_sock_ipv6(b);

    int32_t diff;

    /* First, order by network ver */
    if (a_is_ip6 < b_is_ip6)
        return -1;

    if (a_is_ip6 > b_is_ip6)
        return 1;

    /* If either socket is PICO_IPV4_INADDR_ANY mode, skip local address comparison */

    /* At this point, sort by local host */

    if (0) {
#ifdef PICO_SUPPORT_IPV6
    } else if (a_is_ip6) {
        if ((memcmp(a->local_addr.ip6.addr, PICO_IP6_ANY, PICO_SIZE_IP6) == 0) || memcmp((b->local_addr.ip6.addr, PICO_IP6_ANY, PICO_SIZE_IP6) == 0))
            diff = 0;
        else
            diff = memcmp(a->local_addr.ip6.addr, b->local_addr.ip6.addr, PICO_SIZE_IP6);

#endif
    } else {
        if ((a->local_addr.ip4.addr == PICO_IP4_ANY) || (b->local_addr.ip4.addr == PICO_IP4_ANY))
            diff = 0;
        else
            diff = (int32_t)(a->local_addr.ip4.addr - b->local_addr.ip4.addr);
    }

    if (diff)
        return diff;


    /* Sort by remote host */
    if (a_is_ip6)
        diff = memcmp(a->remote_addr.ip6.addr, b->remote_addr.ip6.addr, PICO_SIZE_IP6);
    else
        diff = (int32_t)(a->remote_addr.ip4.addr - b->remote_addr.ip4.addr);

    if (diff)
        return diff;

    /* And finally by remote port. The two sockets are coincident if the quad is the same. */
    return b->remote_port - a->remote_port;
}

struct pico_sockport
{
    struct pico_tree socks; /* how you make the connection ? */
    uint16_t number;
    uint16_t proto;
};

#define INIT_SOCKPORT { {&LEAF, socket_cmp}, 0, 0 }

int sockport_cmp(void *ka, void *kb)
{
    struct pico_sockport *a = ka, *b = kb;
    if (a->number < b->number)
        return -1;

    if (a->number > b->number)
        return 1;

    return 0;
}

PICO_TREE_DECLARE(UDPTable, sockport_cmp);
PICO_TREE_DECLARE(TCPTable, sockport_cmp);

static struct pico_sockport *pico_get_sockport(uint16_t proto, uint16_t port)
{
    struct pico_sockport test = INIT_SOCKPORT;
    test.number = port;

    if (proto == PICO_PROTO_UDP)
        return pico_tree_findKey(&UDPTable, &test);

    else if (proto == PICO_PROTO_TCP)
        return pico_tree_findKey(&TCPTable, &test);

    else return NULL;
}

int pico_is_port_free(uint16_t proto, uint16_t port, void *addr, void *net)
{
    struct pico_sockport *sp;
    struct pico_ip4 ip;
    sp = pico_get_sockport(proto, port);

    if (!net)
        net = &pico_proto_ipv4;

    /** IPv6 (wip) ***/
    if (net != &pico_proto_ipv4) {
        dbg("IPV6!!!!!\n");
        return (!sp);
    }

    /* IPv4 */
#ifdef PICO_SUPPORT_NAT
    if (pico_ipv4_nat_find(port, NULL, 0, (uint8_t)proto)) {
        dbg("In use by nat....\n");
        return 0;
    }

#endif
    if (addr)
        ip.addr = ((struct pico_ip4 *)addr)->addr;
    else
        ip.addr = PICO_IPV4_INADDR_ANY;

    if (ip.addr == PICO_IPV4_INADDR_ANY) {
        if (!sp) return 1;
        else {
            dbg("In use, and asked for ANY\n");
            return 0;
        }
    }

    if (sp) {
        struct pico_ip4 *s_local;
        struct pico_tree_node *idx;
        struct pico_socket *s;
        pico_tree_foreach(idx, &sp->socks) {
            s = idx->keyValue;
            if (s->net == &pico_proto_ipv4) {
                s_local = (struct pico_ip4*) &s->local_addr;
                if ((s_local->addr == PICO_IPV4_INADDR_ANY) || (s_local->addr == ip.addr))
                    return 0;
            }
        }
    }

    return 1;
}

static int pico_check_socket(struct pico_socket *s)
{
    struct pico_sockport *test;
    struct pico_socket *found;
    struct pico_tree_node *index;

    test = pico_get_sockport(PROTO(s), s->local_port);

    if (!test) {
        return -1;
    }

    pico_tree_foreach(index, &test->socks){
        found = index->keyValue;
        if (s == found) {
            return 0;
        }
    }

    return -1;
}

struct pico_socket*pico_sockets_find(uint16_t local, uint16_t remote)
{
    struct pico_socket *sock = NULL;
    struct pico_tree_node *index = NULL;
    struct pico_sockport *sp = NULL;

    sp = pico_get_sockport(PICO_PROTO_TCP, local);
    if(sp)
    {
        pico_tree_foreach(index, &sp->socks)
        {
            if(((struct pico_socket *)index->keyValue)->remote_port == remote)
            {
                sock = (struct pico_socket *)index->keyValue;
                break;
            }
        }
    }

    return sock;
}


int8_t pico_socket_add(struct pico_socket *s)
{
    struct pico_sockport *sp = pico_get_sockport(PROTO(s), s->local_port);
    PICOTCP_MUTEX_LOCK(Mutex);
    if (!sp) {
        /* dbg("Creating sockport..%04x\n", s->local_port); / * In comment due to spam during test * / */
        sp = pico_zalloc(sizeof(struct pico_sockport));

        if (!sp) {
            pico_err = PICO_ERR_ENOMEM;
            PICOTCP_MUTEX_UNLOCK(Mutex);
            return -1;
        }

        sp->proto = PROTO(s);
        sp->number = s->local_port;
        sp->socks.root = &LEAF;
        sp->socks.compare = socket_cmp;

        if (PROTO(s) == PICO_PROTO_UDP)
        {
            pico_tree_insert(&UDPTable, sp);
        }
        else if (PROTO(s) == PICO_PROTO_TCP)
        {
            pico_tree_insert(&TCPTable, sp);
        }
    }

    pico_tree_insert(&sp->socks, s);
    s->state |= PICO_SOCKET_STATE_BOUND;
    PICOTCP_MUTEX_UNLOCK(Mutex);
#if DEBUG_SOCKET_TREE
    {
        struct pico_tree_node *index;
        /* RB_FOREACH(s, socket_tree, &sp->socks) { */
        pico_tree_foreach(index, &sp->socks){
            s = index->keyValue;
            dbg(">>>> List Socket lc=%hu rm=%hu\n", short_be(s->local_port), short_be(s->remote_port));
        }

    }
#endif
    return 0;
}

static void socket_clean_queues(struct pico_socket *sock)
{
    struct pico_frame *f_in = pico_dequeue(&sock->q_in);
    struct pico_frame *f_out = pico_dequeue(&sock->q_out);
    while(f_in || f_out)
    {
        if(f_in)
        {
            pico_frame_discard(f_in);
            f_in = pico_dequeue(&sock->q_in);
        }

        if(f_out)
        {
            pico_frame_discard(f_out);
            f_out = pico_dequeue(&sock->q_out);
        }
    }
#ifdef PICO_SUPPORT_TCP
    /* for tcp sockets go further and clean the sockets inside queue */
    if(sock->proto == &pico_proto_tcp)
        pico_tcp_cleanup_queues(sock);
#endif

}

static void socket_garbage_collect(pico_time now, void *arg)
{
    struct pico_socket *s = (struct pico_socket *) arg;
    IGNORE_PARAMETER(now);

    socket_clean_queues(s);
    pico_free(s);
}

int8_t pico_socket_del(struct pico_socket *s)
{
    struct pico_sockport *sp = pico_get_sockport(PROTO(s), s->local_port);

    if (!sp) {
        pico_err = PICO_ERR_ENXIO;
        return -1;
    }

    PICOTCP_MUTEX_LOCK(Mutex);
    pico_tree_delete(&sp->socks, s);
    s->net = NULL;
    if(pico_tree_empty(&sp->socks)) {
        if (PROTO(s) == PICO_PROTO_UDP)
        {
            pico_tree_delete(&UDPTable, sp);
        }
        else if (PROTO(s) == PICO_PROTO_TCP)
        {
            pico_tree_delete(&TCPTable, sp);
        }

        if(sp_tcp == sp) sp_tcp = NULL;

        if(sp_udp == sp) sp_udp = NULL;

        pico_free(sp);

    }
    pico_multicast_delete(s);

#ifdef PICO_SUPPORT_TCP
    if(s->parent)
        s->parent->number_of_pending_conn--;

#endif

    s->state = PICO_SOCKET_STATE_CLOSED;
    pico_timer_add(3000, socket_garbage_collect, s);
    PICOTCP_MUTEX_UNLOCK(Mutex);
    return 0;
}

static int8_t pico_socket_alter_state(struct pico_socket *s, uint16_t more_states, uint16_t less_states, uint16_t tcp_state)
{
    struct pico_sockport *sp;
    if (more_states & PICO_SOCKET_STATE_BOUND)
        return pico_socket_add(s);

    if (less_states & PICO_SOCKET_STATE_BOUND)
        return pico_socket_del(s);

    sp = pico_get_sockport(PROTO(s), s->local_port);
    if (!sp) {
        pico_err = PICO_ERR_ENXIO;
        return -1;
    }

    s->state |= more_states;
    s->state = (uint16_t)(s->state & (~less_states));
    if (tcp_state) {
        s->state &= 0x00FF;
        s->state |= tcp_state;
    }

    return 0;
}

static int pico_socket_deliver(struct pico_protocol *p, struct pico_frame *f, uint16_t localport)
{
    struct pico_frame *cpy = NULL;
    struct pico_sockport *sp = NULL;
    struct pico_socket *s = NULL;
    struct pico_tree_node *index = NULL;
    struct pico_tree_node *_tmp;
    struct pico_trans *tr = (struct pico_trans *) f->transport_hdr;
  #ifdef PICO_SUPPORT_IPV4
    struct pico_ipv4_hdr *ip4hdr;
  #endif
  #ifdef PICO_SUPPORT_IPV6
    struct pico_ipv6_hdr *ip6hdr;
  #endif

#ifdef PICO_SUPPORT_TCP
    struct pico_socket *found = NULL;
#endif

    if (!tr)
        return -1;

    sp = pico_get_sockport(p->proto_number, localport);

    if (!sp) {
        dbg("No such port %d\n", short_be(localport));
        return -1;
    }

  #ifdef PICO_SUPPORT_TCP
    if (p->proto_number == PICO_PROTO_TCP) {
        pico_tree_foreach_safe(index, &sp->socks, _tmp){
            s = index->keyValue;
            /* 4-tuple identification of socket (port-IP) */
      #ifdef PICO_SUPPORT_IPV4
            if (IS_IPV4(f)) {
                struct pico_ip4 s_local, s_remote, p_src, p_dst;
                ip4hdr = (struct pico_ipv4_hdr*)(f->net_hdr);
                s_local.addr = s->local_addr.ip4.addr;
                s_remote.addr = s->remote_addr.ip4.addr;
                p_src.addr = ip4hdr->src.addr;
                p_dst.addr = ip4hdr->dst.addr;
                if ((s->remote_port == tr->sport) && /* remote port check */
                    (s_remote.addr == p_src.addr) && /* remote addr check */
                    ((s_local.addr == PICO_IPV4_INADDR_ANY) || (s_local.addr == p_dst.addr))) { /* Either local socket is ANY, or matches dst */
                    found = s;
                    break;
                } else if ((s->remote_port == 0)  && /* not connected... listening */
                           ((s_local.addr == PICO_IPV4_INADDR_ANY) || (s_local.addr == p_dst.addr))) { /* Either local socket is ANY, or matches dst */
                    /* listen socket */
                    found = s;
                }
            }

      #endif
      #ifdef PICO_SUPPORT_IPV6    /* XXX TODO make compare for ipv6 addresses */
            if (IS_IPV6(f)) {
                ip6hdr = (struct pico_ipv6_hdr*)(f->net_hdr);
                if ((s->remote_port == localport)) { /* && (((struct pico_ip6) s->remote_addr.ip6).addr == ((struct pico_ip6)(ip6hdr->src)).addr) ) { */
                    found = s;
                    break;
                } else if (s->remote_port == 0) {
                    /* listen socket */
                    found = s;
                }
            }

      #endif
        } /* FOREACH */
        if (found != NULL) {
            pico_tcp_input(found, f);
            if ((found->ev_pending) && found->wakeup) {
                found->wakeup(found->ev_pending, found);
                if(!found->parent)
                    found->ev_pending = 0;
            }

            return 0;
        } else {
            dbg("SOCKET> mmm something wrong (prob sockport)\n");
            return -1;
        }
    } /* TCP CASE */

#endif

#ifdef PICO_SUPPORT_UDP
    if (p->proto_number == PICO_PROTO_UDP) {
        pico_tree_foreach_safe(index, &sp->socks, _tmp){
            s = index->keyValue;
            if (IS_IPV4(f)) { /* IPV4 */
                struct pico_ip4 s_local, p_dst;
                ip4hdr = (struct pico_ipv4_hdr*)(f->net_hdr);
                s_local.addr = s->local_addr.ip4.addr;
                p_dst.addr = ip4hdr->dst.addr;
                if ((pico_ipv4_is_broadcast(p_dst.addr)) || pico_ipv4_is_multicast(p_dst.addr)) {
                    struct pico_device *dev = pico_ipv4_link_find(&s->local_addr.ip4);
                    if (pico_ipv4_is_multicast(p_dst.addr) && (pico_socket_mcast_filter(s, &ip4hdr->dst, &ip4hdr->src) < 0))
                        return -1;

                    if ((s_local.addr == PICO_IPV4_INADDR_ANY) || /* If our local ip is ANY, or.. */
                        (dev == f->dev)) { /* the source of the bcast packet is a neighbor... */
                        cpy = pico_frame_copy(f);
                        if (!cpy)
                            return -1;

                        if (pico_enqueue(&s->q_in, cpy) > 0) {
                            if (s->wakeup)
                                s->wakeup(PICO_SOCK_EV_RD, s);
                        }
                        else
                            pico_frame_discard(cpy);

                    }
                } else if ((s_local.addr == PICO_IPV4_INADDR_ANY) || (s_local.addr == p_dst.addr))
                { /* Either local socket is ANY, or matches dst */
                    cpy = pico_frame_copy(f);
                    if (!cpy)
                        return -1;

                    if (pico_enqueue(&s->q_in, cpy) > 0) {
                        if (s->wakeup)
                            s->wakeup(PICO_SOCK_EV_RD, s);
                    }
                    else
                        pico_frame_discard(cpy);
                }
            } else {
                /*... IPv6 */
            }
        } /* FOREACH */
        pico_frame_discard(f);
        if (s)
            return 0;
        else
            return -1;
    }

#endif
    return -1;
}

struct pico_socket *pico_socket_open(uint16_t net, uint16_t proto, void (*wakeup)(uint16_t ev, struct pico_socket *))
{

    struct pico_socket *s = NULL;

#ifdef PICO_SUPPORT_UDP
    if (proto == PICO_PROTO_UDP) {
        s = pico_udp_open();
        s->proto = &pico_proto_udp;
        s->q_in.overhead = s->q_out.overhead = UDP_FRAME_OVERHEAD;
    }

#endif

#ifdef PICO_SUPPORT_TCP
    if (proto == PICO_PROTO_TCP) {
        s = pico_tcp_open();
        s->proto = &pico_proto_tcp;
        /*check if Nagle enabled */
        /*
           if (!IS_NAGLE_ENABLED(s))
            dbg("ERROR Nagle should be enabled here\n\n");
         */
    }

#endif

    if (!s) {
        pico_err = PICO_ERR_EPROTONOSUPPORT;
        return NULL;
    }

#ifdef PICO_SUPPORT_IPV4
    if (net == PICO_PROTO_IPV4)
        s->net = &pico_proto_ipv4;

#endif

#ifdef PICO_SUPPORT_IPV6
    if (net == PICO_PROTO_IPV6)
        s->net = &pico_proto_ipv6;

#endif

    s->q_in.max_size = PICO_DEFAULT_SOCKETQ;
    s->q_out.max_size = PICO_DEFAULT_SOCKETQ;

    s->wakeup = wakeup;

    if (!s->net) {
        pico_free(s);
        pico_err = PICO_ERR_ENETUNREACH;
        return NULL;
    }

    return s;
}


struct pico_socket *pico_socket_clone(struct pico_socket *facsimile)
{
    struct pico_socket *s = NULL;

#ifdef PICO_SUPPORT_UDP
    if (facsimile->proto->proto_number == PICO_PROTO_UDP) {
        s = pico_udp_open();
        s->proto = &pico_proto_udp;
        s->q_in.overhead = s->q_out.overhead = UDP_FRAME_OVERHEAD;
    }

#endif

#ifdef PICO_SUPPORT_TCP
    if (facsimile->proto->proto_number == PICO_PROTO_TCP) {
        s = pico_tcp_open();
        s->proto = &pico_proto_tcp;
    }

#endif

    if (!s) {
        pico_err = PICO_ERR_EPROTONOSUPPORT;
        return NULL;
    }

    s->local_port = facsimile->local_port;
    s->remote_port = facsimile->remote_port;
    s->state = facsimile->state;

#ifdef PICO_SUPPORT_IPV4
    if (facsimile->net == &pico_proto_ipv4) {
        s->net = &pico_proto_ipv4;
        memcpy(&s->local_addr, &facsimile->local_addr, sizeof(struct pico_ip4));
        memcpy(&s->remote_addr, &facsimile->remote_addr, sizeof(struct pico_ip4));
    }

#endif

#ifdef PICO_SUPPORT_IPV6
    if (net == &pico_proto_ipv6) {
        s->net = &pico_proto_ipv6;
        memcpy(&s->local_addr, &facsimile->local_addr, sizeof(struct pico_ip6));
        memcpy(&s->remote_addr, &facsimile->remote_addr, sizeof(struct pico_ip6));
    }

#endif
    s->q_in.max_size = PICO_DEFAULT_SOCKETQ;
    s->q_out.max_size = PICO_DEFAULT_SOCKETQ;
    s->wakeup = NULL;
    if (!s->net) {
        pico_free(s);
        pico_err = PICO_ERR_ENETUNREACH;
        return NULL;
    }

    return s;
}

int pico_socket_read(struct pico_socket *s, void *buf, int len)
{
    if (!s || buf == NULL) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    } else {
        /* check if exists in tree */
        /* See task #178 */
        if (pico_check_socket(s) != 0) {
            pico_err = PICO_ERR_EINVAL;
            return -1;
        }
    }

    if ((s->state & PICO_SOCKET_STATE_BOUND) == 0) {
        pico_err = PICO_ERR_EIO;
        return -1;
    }

#ifdef PICO_SUPPORT_UDP
    if (PROTO(s) == PICO_PROTO_UDP)
        return pico_udp_recv(s, buf, (uint16_t)len, NULL, NULL);

#endif

#ifdef PICO_SUPPORT_TCP
    if (PROTO(s) == PICO_PROTO_TCP) {
        /* check if in shutdown state and if no more data in tcpq_in */
        if ((s->state & PICO_SOCKET_STATE_SHUT_REMOTE) && pico_tcp_queue_in_is_empty(s)) {
            pico_err = PICO_ERR_ESHUTDOWN;
            return -1;
        } else {
            return (int)pico_tcp_read(s, buf, (uint32_t)len);
        }
    }

#endif
    return 0;
}

int pico_socket_write(struct pico_socket *s, const void *buf, int len)
{
    if (!s || buf == NULL) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    } else {
        /* check if exists in tree */
        /* See task #178 */
        if (pico_check_socket(s) != 0) {
            pico_err = PICO_ERR_EINVAL;
            return -1;
        }
    }

    if ((s->state & PICO_SOCKET_STATE_BOUND) == 0) {
        pico_err = PICO_ERR_EIO;
        return -1;
    }

    if ((s->state & PICO_SOCKET_STATE_CONNECTED) == 0) {
        pico_err = PICO_ERR_ENOTCONN;
        return -1;
    } else if (s->state & PICO_SOCKET_STATE_SHUT_LOCAL) { /* check if in shutdown state */
        pico_err = PICO_ERR_ESHUTDOWN;
        return -1;
    } else {
        return pico_socket_sendto(s, buf, len, &s->remote_addr, s->remote_port);
    }
}

uint16_t pico_socket_high_port(uint16_t proto)
{
    uint16_t port;
    if (0 ||
#ifdef PICO_SUPPORT_TCP
        (proto == PICO_PROTO_TCP) ||
#endif
#ifdef PICO_SUPPORT_TCP
        (proto == PICO_PROTO_UDP) ||
#endif
        0) {
        do {
            uint32_t rand = pico_rand();
            port = (uint16_t) (rand & 0xFFFFU);
            port = (uint16_t)((port % (65535 - 1024)) + 1024U);
            if (pico_is_port_free(proto, port, NULL, NULL)) {
                return short_be(port);
            }
        } while(1);
    }
    else return 0U;
}


int pico_socket_sendto(struct pico_socket *s, const void *buf, const int len, void *dst, uint16_t remote_port)
{
    struct pico_frame *f;
    struct pico_remote_duple *remote_duple = NULL;
    int socket_mtu = PICO_SOCKET4_MTU;
    int header_offset = 0;
    int total_payload_written = 0;

#ifdef PICO_SUPPORT_IPV4
    struct pico_ip4 *src4;
#endif

#ifdef PICO_SUPPORT_IPV6
    struct pico_ip6 *src6;
#endif
    if (len == 0) {
        return 0;
    } else if (len < 0) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    if (buf == NULL || s == NULL) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    if (!dst || !remote_port) {
        pico_err = PICO_ERR_EADDRNOTAVAIL;
        return -1;
    }

    if ((s->state & PICO_SOCKET_STATE_CONNECTED) != 0) {
        if (remote_port != s->remote_port) {
            pico_err = PICO_ERR_EINVAL;
            return -1;
        }
    }

#ifdef PICO_SUPPORT_IPV4
    if (IS_SOCK_IPV4(s)) {
        socket_mtu = PICO_SOCKET4_MTU;
        if ((s->state & PICO_SOCKET_STATE_CONNECTED)) {
            if  (s->remote_addr.ip4.addr != ((struct pico_ip4 *)dst)->addr ) {
                pico_err = PICO_ERR_EADDRNOTAVAIL;
                return -1;
            }
        } else {
            src4 = pico_ipv4_source_find(dst);
            if (!src4) {
                pico_err = PICO_ERR_EHOSTUNREACH;
                return -1;
            }

            if (src4->addr != PICO_IPV4_INADDR_ANY)
                s->local_addr.ip4.addr = src4->addr;

#     ifdef PICO_SUPPORT_UDP
            /* socket remote info could change in a consecutive call, make persistent */
            if (PROTO(s) == PICO_PROTO_UDP) {
                remote_duple = pico_zalloc(sizeof(struct pico_remote_duple));
                remote_duple->remote_addr.ip4.addr = ((struct pico_ip4 *)dst)->addr;
                remote_duple->remote_port = remote_port;
            }

#     endif
        }

    #endif
}
else if (IS_SOCK_IPV6(s)) {
    socket_mtu = PICO_SOCKET6_MTU;
    #ifdef PICO_SUPPORT_IPV6
    if (s->state & PICO_SOCKET_STATE_CONNECTED) {
        if (memcmp(&s->remote_addr, dst, PICO_SIZE_IP6))
            return -1;
    } else {
        src6 = pico_ipv6_source_find(dst);
        if (!src6) {
            pico_err = PICO_ERR_EHOSTUNREACH;
            return -1;
        }

        memcpy(&s->local_addr, src6, PICO_SIZE_IP6);
        memcpy(&s->remote_addr, dst, PICO_SIZE_IP6);
#     ifdef PICO_SUPPORT_UDP
        if (PROTO(s) == PICO_PROTO_UDP) {
            remote_duple = pico_zalloc(sizeof(struct pico_remote_duple));
            remote_duple->remote_addr.ip6.addr = ((struct pico_ip6 *)dst)->addr;
            remote_duple->remote_port = remote_port;
        }

#     endif
    }

#endif
}

if ((s->state & PICO_SOCKET_STATE_BOUND) == 0) {
    s->local_port = pico_socket_high_port(s->proto->proto_number);
    if (s->local_port == 0) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }
}

if ((s->state & PICO_SOCKET_STATE_CONNECTED) == 0) {
    s->remote_port = remote_port;
}

#ifdef PICO_SUPPORT_TCP
if (PROTO(s) == PICO_PROTO_TCP)
    header_offset = pico_tcp_overhead(s);

#endif

#ifdef PICO_SUPPORT_UDP
if (PROTO(s) == PICO_PROTO_UDP)
    header_offset = sizeof(struct pico_udp_hdr);

#endif

while (total_payload_written < len) {
    int transport_len = (len - total_payload_written) + header_offset;
    if (transport_len > socket_mtu)
        transport_len = socket_mtu;

    #ifdef PICO_SUPPORT_IPFRAG
    else {
        if (total_payload_written)
            transport_len -= header_offset; /* last fragment, do not allocate memory for transport header */

    }
#endif /* PICO_SUPPORT_IPFRAG */

    f = pico_socket_frame_alloc(s, (uint16_t)transport_len);
    if (!f) {
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    f->payload += header_offset;
    f->payload_len = (uint16_t)(f->payload_len - header_offset);
    f->sock = s;
#ifdef PICO_SUPPORT_TCP
    transport_flags_update(f, s);
#endif
    if (remote_duple) {
        f->info = pico_zalloc(sizeof(struct pico_remote_duple));
        memcpy(f->info, remote_duple, sizeof(struct pico_remote_duple));
    }

    #ifdef PICO_SUPPORT_IPFRAG
    #ifdef PICO_SUPPORT_UDP
    if (PROTO(s) == PICO_PROTO_UDP && ((len + header_offset) > socket_mtu)) {
        /* hacking way to identify fragmentation frames: payload != transport_hdr -> first frame */
        if (!total_payload_written) {
            frag_dbg("FRAG: first fragmented frame %p | len = %u offset = 0\n", f, f->payload_len);
            /* transport header length field contains total length + header length */
            f->transport_len = (uint16_t)(len + header_offset);
            f->frag = short_be(PICO_IPV4_MOREFRAG);
        } else {
            /* no transport header in fragmented IP */
            f->payload = f->transport_hdr;
            f->payload_len = (uint16_t)(f->payload_len + header_offset);
            /* set offset in octets */
            f->frag = short_be((uint16_t)((total_payload_written + header_offset) / 8));
            if (total_payload_written + f->payload_len < len) {
                frag_dbg("FRAG: intermediate fragmented frame %p | len = %u offset = %u\n", f, f->payload_len, short_be(f->frag));
                f->frag |= short_be(PICO_IPV4_MOREFRAG);
            } else {
                frag_dbg("FRAG: last fragmented frame %p | len = %u offset = %u\n", f, f->payload_len, short_be(f->frag));
                f->frag &= short_be(PICO_IPV4_FRAG_MASK);
            }
        }
    } else {
        f->frag = short_be(PICO_IPV4_DONTFRAG);
    }

#  endif /* PICO_SUPPORT_UDP */
#endif /* PICO_SUPPORT_IPFRAG */

    if (f->payload_len <= 0) {
        pico_frame_discard(f);
        if (remote_duple)
            pico_free(remote_duple);

        return total_payload_written;
    }

    memcpy(f->payload, (const uint8_t *)buf + total_payload_written, f->payload_len);
    /* dbg("Pushing segment, hdr len: %d, payload_len: %d\n", header_offset, f->payload_len); */

    if (s->proto->push(s->proto, f) > 0) {
        total_payload_written += (int)f->payload_len;
    } else {
        pico_frame_discard(f);
        pico_err = PICO_ERR_EAGAIN;
        break;
    }
}
if (remote_duple)
    pico_free(remote_duple);

return total_payload_written;
}

int pico_socket_send(struct pico_socket *s, const void *buf, int len)
{
    if (!s || buf == NULL) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    } else {
        /* check if exists in tree */
        /* See task #178 */
        if (pico_check_socket(s) != 0) {
            pico_err = PICO_ERR_EINVAL;
            return -1;
        }
    }

    if ((s->state & PICO_SOCKET_STATE_CONNECTED) == 0) {
        pico_err = PICO_ERR_ENOTCONN;
        return -1;
    }

    return pico_socket_sendto(s, buf, len, &s->remote_addr, s->remote_port);
}

int pico_socket_recvfrom(struct pico_socket *s, void *buf, int len, void *orig, uint16_t *remote_port)
{
    if (!s || buf == NULL) { /* / || orig == NULL || remote_port == NULL) { */
        pico_err = PICO_ERR_EINVAL;
        return -1;
    } else {
        /* check if exists in tree */
        if (pico_check_socket(s) != 0) {
            pico_err = PICO_ERR_EINVAL;
            /* See task #178 */
            return -1;
        }
    }

    if ((s->state & PICO_SOCKET_STATE_BOUND) == 0) {
        pico_err = PICO_ERR_EADDRNOTAVAIL;
        return -1;
    }

#ifdef PICO_SUPPORT_UDP
    if (PROTO(s) == PICO_PROTO_UDP) {
        return pico_udp_recv(s, buf, (uint16_t)len, orig, remote_port);
    }

#endif
#ifdef PICO_SUPPORT_TCP
    if (PROTO(s) == PICO_PROTO_TCP) {
        /* check if in shutdown state and if tcpq_in empty */
        if ((s->state & PICO_SOCKET_STATE_SHUT_REMOTE) && pico_tcp_queue_in_is_empty(s)) {
            pico_err = PICO_ERR_ESHUTDOWN;
            return -1;
        } else {
            /* dbg("socket tcp recv\n"); */
            return (int)pico_tcp_read(s, buf, (uint32_t)len);
        }
    }

#endif
    /* dbg("socket return 0\n"); */
    return 0;
}

int pico_socket_recv(struct pico_socket *s, void *buf, int len)
{
    return pico_socket_recvfrom(s, buf, len, NULL, NULL);
}


int pico_socket_bind(struct pico_socket *s, void *local_addr, uint16_t *port)
{
    if (!s || !local_addr || !port) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    if (!is_sock_ipv6(s)) {
        struct pico_ip4 *ip = (struct pico_ip4 *)local_addr;
        if (ip->addr != PICO_IPV4_INADDR_ANY) {
            if (!pico_ipv4_link_find(local_addr)) {
                pico_err = PICO_ERR_EINVAL;
                return -1;
            }
        }
    } else {
        /*... IPv6 */
    }


    /* When given port = 0, get a random high port to bind to. */
    if (*port == 0) {
        *port = pico_socket_high_port(PROTO(s));
        if (*port == 0) {
            pico_err = PICO_ERR_EINVAL;
            return -1;
        }
    }

    if (pico_is_port_free(PROTO(s), *port, local_addr, s->net) == 0) {
        pico_err = PICO_ERR_EADDRINUSE;
        return -1;
    }

    s->local_port = *port;

    if (is_sock_ipv6(s)) {
        struct pico_ip6 *ip = (struct pico_ip6 *) local_addr;
        memcpy(s->local_addr.ip6.addr, ip, PICO_SIZE_IP6);
        /* XXX: port ipv4 functionality to ipv6 */
        /* Check for port already in use */
        if (pico_is_port_free(PROTO(s), *port, &local_addr, s->net)) {
            pico_err = PICO_ERR_EADDRINUSE;
            return -1;
        }
    } else if (is_sock_ipv4(s)) {
        struct pico_ip4 *ip = (struct pico_ip4 *) local_addr;
        s->local_addr.ip4.addr = ip->addr;
    }

    return pico_socket_alter_state(s, PICO_SOCKET_STATE_BOUND, 0, 0);
}

int pico_socket_connect(struct pico_socket *s, const void *remote_addr, uint16_t remote_port)
{
    int ret = -1;
    pico_err = PICO_ERR_EPROTONOSUPPORT;
    if (!s || remote_addr == NULL || remote_port == 0) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    s->remote_port = remote_port;

    if (s->local_port == 0) {
        s->local_port = pico_socket_high_port(PROTO(s));
        if (!s->local_port) {
            pico_err = PICO_ERR_EINVAL;
            return -1;
        }
    }

    if (is_sock_ipv6(s)) {
        const struct pico_ip6 *ip = (const struct pico_ip6 *) remote_addr;
        memcpy(s->remote_addr.ip6.addr, ip, PICO_SIZE_IP6);
    } else if (is_sock_ipv4(s)) {
        const struct pico_ip4 *local, *ip = (const struct pico_ip4 *) remote_addr;
        s->remote_addr.ip4.addr = ip->addr;
        local = pico_ipv4_source_find(ip);
        if (local) {
            s->local_addr.ip4.addr = local->addr;
        } else {
            pico_err = PICO_ERR_EHOSTUNREACH;
            return -1;
        }
    }

    pico_socket_alter_state(s, PICO_SOCKET_STATE_BOUND, 0, 0);

#ifdef PICO_SUPPORT_UDP
    if (PROTO(s) == PICO_PROTO_UDP) {
        pico_socket_alter_state(s, PICO_SOCKET_STATE_CONNECTED, 0, 0);
        pico_err = PICO_ERR_NOERR;
        ret = 0;
    }

#endif

#ifdef PICO_SUPPORT_TCP
    if (PROTO(s) == PICO_PROTO_TCP) {
        if (pico_tcp_initconn(s) == 0) {
            pico_socket_alter_state(s, PICO_SOCKET_STATE_CONNECTED | PICO_SOCKET_STATE_TCP_SYN_SENT, 0, 0);
            pico_err = PICO_ERR_NOERR;
            ret = 0;
        } else {
            pico_err = PICO_ERR_EHOSTUNREACH;
        }
    }

#endif

    return ret;
}

#ifdef PICO_SUPPORT_TCP

int pico_socket_listen(struct pico_socket *s, int backlog)
{
    if (!s || backlog < 1) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    } else {
        /* check if exists in tree */
        /* See task #178 */
        if (pico_check_socket(s) != 0) {
            pico_err = PICO_ERR_EINVAL;
            return -1;
        }
    }

    if (PROTO(s) == PICO_PROTO_UDP) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    if ((s->state & PICO_SOCKET_STATE_BOUND) == 0) {
        pico_err = PICO_ERR_EISCONN;
        return -1;
    }

    if (backlog < 1) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    if (PROTO(s) == PICO_PROTO_TCP)
        pico_socket_alter_state(s, PICO_SOCKET_STATE_TCP_SYN_SENT, 0, PICO_SOCKET_STATE_TCP_LISTEN);

    s->max_backlog = backlog;

    return 0;
}

struct pico_socket *pico_socket_accept(struct pico_socket *s, void *orig, uint16_t *port)
{
    if (!s || !orig || !port) {
        pico_err = PICO_ERR_EINVAL;
        return NULL;
    }

    pico_err = PICO_ERR_EINVAL;

    if ((s->state & PICO_SOCKET_STATE_BOUND) == 0) {
        return NULL;
    }

    if (PROTO(s) == PICO_PROTO_UDP) {
        return NULL;
    }

    if (TCPSTATE(s) == PICO_SOCKET_STATE_TCP_LISTEN) {
        struct pico_sockport *sp = pico_get_sockport(PICO_PROTO_TCP, s->local_port);
        struct pico_socket *found;
        /* If at this point no incoming connection socket is found,
         * the accept call is valid, but no connection is established yet.
         */
        pico_err = PICO_ERR_EAGAIN;
        if (sp) {
            struct pico_tree_node *index;
            /* RB_FOREACH(found, socket_tree, &sp->socks) { */
            pico_tree_foreach(index, &sp->socks){
                found = index->keyValue;
                if ((s == found->parent) && ((found->state & PICO_SOCKET_STATE_TCP) == PICO_SOCKET_STATE_TCP_ESTABLISHED)) {
                    found->parent = NULL;
                    pico_err = PICO_ERR_NOERR;
                    memcpy(orig, &found->remote_addr, sizeof(struct pico_ip4));
                    *port = found->remote_port;
                    s->number_of_pending_conn--;
                    return found;
                }
            }
        }
    }

    return NULL;
}

#else

int pico_socket_listen(struct pico_socket *s, int backlog)
{
    IGNORE_PARAMETER(s);
    IGNORE_PARAMETER(backlog);
    pico_err = PICO_ERR_EINVAL;
    return -1;
}

struct pico_socket *pico_socket_accept(struct pico_socket *s, void *orig, uint16_t *local_port)
{
    IGNORE_PARAMETER(s);
    IGNORE_PARAMETER(orig);
    IGNORE_PARAMETER(local_port);
    pico_err = PICO_ERR_EINVAL;
    return NULL;
}

#endif


int pico_socket_setoption(struct pico_socket *s, int option, void *value) /* XXX no check against proto (vs setsockopt) or implicit by socket? */
{

    if (s == NULL) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    pico_err = PICO_ERR_NOERR;

    if ( (pico_setsockopt_tcp(s, option, value) < 0) &&
         (pico_setsockopt_mcast(s, option, value) < 0) )
      return -1;
    else return 0;
}


int pico_socket_getoption(struct pico_socket *s, int option, void *value)
{
    if (s == NULL) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    pico_err = PICO_ERR_NOERR;

    if ( (pico_getsockopt_tcp(s, option, value) < 0) &&
         (pico_getsockopt_mcast(s, option, value) < 0) )
      return -1;
    else return 0;
}


int pico_socket_shutdown(struct pico_socket *s, int mode)
{
    if (!s) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    } else {
        /* check if exists in tree */
        /* See task #178 */
        if (pico_check_socket(s) != 0) {
            pico_free(s); /* close socket after bind or connect failed */
            return 0;
        }
    }

#ifdef PICO_SUPPORT_UDP
    if (PROTO(s) == PICO_PROTO_UDP) {
        if (mode & PICO_SHUT_RDWR)
            pico_socket_alter_state(s, PICO_SOCKET_STATE_CLOSED, PICO_SOCKET_STATE_CLOSING | PICO_SOCKET_STATE_BOUND | PICO_SOCKET_STATE_CONNECTED, 0);
        else if (mode & PICO_SHUT_RD)
            pico_socket_alter_state(s, 0, PICO_SOCKET_STATE_BOUND, 0);
    }

#endif
#ifdef PICO_SUPPORT_TCP
    if (PROTO(s) == PICO_PROTO_TCP) {
        if(mode & PICO_SHUT_RDWR)
        {
            pico_socket_alter_state(s, PICO_SOCKET_STATE_SHUT_LOCAL | PICO_SOCKET_STATE_SHUT_REMOTE, 0, 0);
            pico_tcp_notify_closing(s);
        }
        else if (mode & PICO_SHUT_WR)
            pico_socket_alter_state(s, PICO_SOCKET_STATE_SHUT_LOCAL, 0, 0);
        else if (mode & PICO_SHUT_RD)
            pico_socket_alter_state(s, PICO_SOCKET_STATE_SHUT_REMOTE, 0, 0);

    }

#endif
    return 0;
}

int pico_socket_close(struct pico_socket *s)
{
    return pico_socket_shutdown(s, PICO_SHUT_RDWR);
}

#ifdef PICO_SUPPORT_CRC
static inline int pico_transport_crc_check(struct pico_frame *f)
{
    struct pico_ipv4_hdr *net_hdr = (struct pico_ipv4_hdr *) f->net_hdr;
    struct pico_udp_hdr *udp_hdr = NULL;
    uint16_t checksum_invalid = 1;

    switch (net_hdr->proto)
    {
    case PICO_PROTO_TCP:
        checksum_invalid = short_be(pico_tcp_checksum_ipv4(f));
        /* dbg("TCP CRC validation == %u\n", checksum_invalid); */
        if (checksum_invalid) {
            /* dbg("TCP CRC: validation failed!\n"); */
            pico_frame_discard(f);
            return 0;
        }

        break;

    case PICO_PROTO_UDP:
        udp_hdr = (struct pico_udp_hdr *) f->transport_hdr;
        if (short_be(udp_hdr->crc)) {
            checksum_invalid = short_be(pico_udp_checksum_ipv4(f));
            /* dbg("UDP CRC validation == %u\n", checksum_invalid); */
            if (checksum_invalid) {
                /* dbg("UDP CRC: validation failed!\n"); */
                pico_frame_discard(f);
                return 0;
            }
        }

        break;

    default:
        /* Do nothing */
        break;
    }
    return 1;
}
#else
static inline int pico_transport_crc_check(struct pico_frame *f)
{
    IGNORE_PARAMETER(f);
    return 1;
}
#endif /* PICO_SUPPORT_CRC */

int pico_transport_process_in(struct pico_protocol *self, struct pico_frame *f)
{
    struct pico_trans *hdr = (struct pico_trans *) f->transport_hdr;
    int ret = 0;

    if (!hdr) {
        pico_err = PICO_ERR_EFAULT;
        return -1;
    }

    ret = pico_transport_crc_check(f);
    if (ret < 1)
        return ret;
    else
        ret = 0;

    if ((hdr) && (pico_socket_deliver(self, f, hdr->dport) == 0))
        return ret;

    if (!IS_BCAST(f)) {
        dbg("Socket not found... \n");
        pico_notify_socket_unreachable(f);
#ifdef PICO_SUPPORT_TCP
        /* if tcp protocol send RST segment */
        /* if (self->proto_number == PICO_PROTO_TCP) */
        /*  pico_tcp_reply_rst(f); */
#endif
        ret = -1;
        pico_err = PICO_ERR_ENOENT;
    }

    pico_frame_discard(f);
    return ret;
}

#define SL_LOOP_MIN 1

#ifdef PICO_SUPPORT_TCP
static int checkSocketSanity(struct pico_socket *s)
{

/* checking for pending connections */
    if(TCP_STATE(s) == PICO_SOCKET_STATE_TCP_SYN_RECV) {
        if((pico_time)(PICO_TIME_MS() - s->timestamp) >= PICO_SOCKET_BOUND_TIMEOUT)
            return -1;
    }

    if((pico_time)(PICO_TIME_MS() - s->timestamp) >= PICO_SOCKET_TIMEOUT) {
        /* checking for hanging sockets */
        if((TCP_STATE(s) != PICO_SOCKET_STATE_TCP_LISTEN) && (TCP_STATE(s) != PICO_SOCKET_STATE_TCP_ESTABLISHED))
            return -1;

        /* if no activity, force the socket into closing state */
        /* DLA: Why? This seems off-specs. */
#ifdef TCP_OFF_SPECS
        if( TCP_STATE(s) == PICO_SOCKET_STATE_TCP_ESTABLISHED )
        {
            s->wakeup(PICO_SOCK_EV_CLOSE, s);
            pico_socket_close(s);
            s->timestamp = PICO_TIME_MS();
        }

#endif
    }

    return 0;
}
#endif

int pico_sockets_loop(int loop_score)
{
#ifdef PICO_SUPPORT_UDP
    static struct pico_tree_node *index_udp;
#endif

#ifdef PICO_SUPPORT_TCP
    static struct pico_tree_node*index_tcp;
#endif

    struct pico_sockport *start;
    struct pico_socket *s;

#ifdef PICO_SUPPORT_UDP
    struct pico_frame *f;

    if (sp_udp == NULL)
    {
        index_udp = pico_tree_firstNode(UDPTable.root);
        sp_udp = index_udp->keyValue;
    }

    /* init start node */
    start = sp_udp;

    /* round-robin all transport protocols, break if traversed all protocols */
    while (loop_score > SL_LOOP_MIN && sp_udp != NULL) {
        struct pico_tree_node *index;

        pico_tree_foreach(index, &sp_udp->socks){
            s = index->keyValue;
            f = pico_dequeue(&s->q_out);
            while (f && (loop_score > 0)) {
                pico_proto_udp.push(&pico_proto_udp, f);
                loop_score -= 1;
                f = pico_dequeue(&s->q_out);
            }
        }

        index_udp = pico_tree_next(index_udp);
        sp_udp = index_udp->keyValue;

        if (sp_udp == NULL)
        {
            index_udp = pico_tree_firstNode(UDPTable.root);
            sp_udp = index_udp->keyValue;
        }

        if (sp_udp == start)
            break;
    }
#endif

#ifdef PICO_SUPPORT_TCP
    if (sp_tcp == NULL)
    {
        index_tcp = pico_tree_firstNode(TCPTable.root);
        sp_tcp = index_tcp->keyValue;
    }

    /* init start node */
    start = sp_tcp;

    while (loop_score > SL_LOOP_MIN && sp_tcp != NULL) {
        struct pico_tree_node *index = NULL, *safe_index = NULL;
        pico_tree_foreach_safe(index, &sp_tcp->socks, safe_index){
            s = index->keyValue;
            loop_score = pico_tcp_output(s, loop_score);
            if ((s->ev_pending) && s->wakeup) {
                s->wakeup(s->ev_pending, s);
                if(!s->parent)
                    s->ev_pending = 0;
            }

            if (loop_score <= 0) {
                loop_score = 0;
                break;
            }

            if(checkSocketSanity(s) < 0)
            {
                pico_socket_del(s);
                index_tcp = NULL; /* forcing the restart of loop */
                sp_tcp = NULL;
                break;
            }
        }

        /* check if RB_FOREACH ended, if not, break to keep the cur sp_tcp */
        if (!index_tcp || (index && index->keyValue))
            break;

        index_tcp = pico_tree_next(index_tcp);
        sp_tcp = index_tcp->keyValue;

        if (sp_tcp == NULL)
        {
            index_tcp = pico_tree_firstNode(TCPTable.root);
            sp_tcp = index_tcp->keyValue;
        }

        if (sp_tcp == start)
            break;
    }
#endif

    return loop_score;
}


struct pico_frame *pico_socket_frame_alloc(struct pico_socket *s, uint16_t len)
{
    struct pico_frame *f = NULL;

#ifdef PICO_SUPPORT_IPV6
    if (IS_SOCK_IPV6(s))
        f = pico_proto_ipv6.alloc(&pico_proto_ipv6, len);

#endif

#ifdef PICO_SUPPORT_IPV4
    if (IS_SOCK_IPV4(s))
        f = pico_proto_ipv4.alloc(&pico_proto_ipv4, (uint16_t)len);

#endif
    if (!f) {
        pico_err = PICO_ERR_ENOMEM;
        return f;
    }

    f->payload = f->transport_hdr;
    f->payload_len = len;
    f->sock = s;
    return f;
}

int pico_transport_error(struct pico_frame *f, uint8_t proto, int code)
{
    int ret = -1;
    struct pico_trans *trans = (struct pico_trans*) f->transport_hdr;
    struct pico_sockport *port = NULL;
    struct pico_socket *s = NULL;
    switch (proto) {


#ifdef PICO_SUPPORT_UDP
    case PICO_PROTO_UDP:
        port = pico_get_sockport(proto, trans->sport);
        break;
#endif

#ifdef PICO_SUPPORT_TCP
    case PICO_PROTO_TCP:
        port = pico_get_sockport(proto, trans->sport);
        break;
#endif

    default:
        /* Protocol not available */
        ret = -1;
    }
    if (port) {
        struct pico_tree_node *index;
        ret = 0;

        pico_tree_foreach(index, &port->socks) {
            s = index->keyValue;
            if (trans->dport == s->remote_port) {
                if (s->wakeup) {
                    /* dbg("SOCKET ERROR FROM ICMP NOTIFICATION. (icmp code= %d)\n\n", code); */
                    switch(code) {
                    case PICO_ICMP_UNREACH_NET:
                        pico_err = PICO_ERR_ENETUNREACH;
                        break;

                    case PICO_ICMP_UNREACH_HOST:
                        pico_err = PICO_ERR_EHOSTUNREACH;
                        break;

                    case PICO_ICMP_UNREACH_PROTOCOL:
                        pico_err = PICO_ERR_ENOPROTOOPT;
                        break;

                    case PICO_ICMP_UNREACH_PORT:
                        pico_err = PICO_ERR_ECONNREFUSED;
                        break;

                    case PICO_ICMP_UNREACH_NET_PROHIB:
                    case PICO_ICMP_UNREACH_NET_UNKNOWN:
                        pico_err = PICO_ERR_ENETUNREACH;
                        break;

                    default:
                        pico_err = PICO_ERR_EOPNOTSUPP;
                    }
                    s->state |= PICO_SOCKET_STATE_SHUT_REMOTE;
                    s->wakeup(PICO_SOCK_EV_ERR, s);

                }

                break;
            }
        }
    }

    pico_frame_discard(f);
    return ret;
}
#endif
#endif
