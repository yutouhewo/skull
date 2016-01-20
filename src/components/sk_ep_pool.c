#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "flibs/fhash.h"
#include "flibs/flist.h"
#include "flibs/fdlist.h"
#include "flibs/fev.h"
#include "flibs/fev_buff.h"
#include "flibs/fnet.h"
#include "flibs/ftime.h"

#include "api/sk_const.h"
#include "api/sk_utils.h"
#include "api/sk_env.h"
#include "api/sk_metrics.h"
#include "api/sk_entity.h"
#include "api/sk_entity_mgr.h"
#include "api/sk_entity_util.h"
#include "api/sk_timer_service.h"
#include "api/sk_ep_pool.h"

#define SK_TIME_NOW (ftime_gettime() / 1000L)

#define SK_METRICS_EP_OK() { \
    sk_metrics_global.ep_ok.inc(1); \
    sk_metrics_worker.ep_ok.inc(1); \
}

#define SK_METRICS_EP_ERROR() { \
    sk_metrics_global.ep_error.inc(1); \
    sk_metrics_worker.ep_error.inc(1); \
}

#define SK_METRICS_EP_TIMEOUT() { \
    sk_metrics_global.ep_timeout.inc(1); \
    sk_metrics_worker.ep_timeout.inc(1); \
}

#define SK_METRICS_EP_CREATE() { \
    sk_metrics_global.ep_create.inc(1); \
    sk_metrics_worker.ep_create.inc(1); \
}

#define SK_METRICS_EP_SEND() { \
    sk_metrics_global.ep_send.inc(1); \
    sk_metrics_worker.ep_send.inc(1); \
}

#define SK_METRICS_EP_RECV() { \
    sk_metrics_global.ep_recv.inc(1); \
    sk_metrics_worker.ep_recv.inc(1); \
}

typedef enum sk_ep_st_t {
    SK_EP_ST_INIT       = 0,
    SK_EP_ST_CONNECTING = 1,
    SK_EP_ST_CONNECTED  = 2
} sk_ep_st_t;

typedef enum sk_ep_nst_t {
    SK_EP_NST_INIT  = 0,
    SK_EP_NST_SENT  = 1,
} sk_ep_nst_t;

typedef struct sk_ep_timerdata_t {
    sk_entity_t*   timer_entity;
    fdlist_node_t* ep_node;
} sk_ep_timerdata_t;

struct sk_ep_t;
typedef struct sk_ep_data_t {
    struct sk_ep_t*    owner;
    sk_ep_handler_t    handler;
    sk_ep_cb_t         cb;
    void*              ud;
    unsigned long long start;
    const void*        data;
    size_t             count;
    const sk_entity_t* e;       // sk_txn's entity

    sk_timer_t*        conn_timer;
    sk_timer_t*        recv_timer;

    sk_ep_nst_t        status;

#if __WORDSIZE == 64
    int                _padding;
#endif
} sk_ep_data_t;

typedef struct sk_ep_readarg_t {
    const void*   data;     // in
    size_t        len;      // in
    size_t        consumed; // out
} sk_ep_readarg_t;

struct sk_ep_mgr_t;
typedef struct sk_ep_t {
    struct sk_ep_mgr_t* owner;
    uint64_t            ekey;
    char                ipkey[SK_EP_KEY_MAX];
    sk_ep_type_t        type;
    sk_ep_st_t          status;
    sk_entity_t*        ep;
    fdlist*             txns;  // ep txns, type: sk_ep_data_t

    int                 ntxn;
    int                 flags;
} sk_ep_t;

struct sk_ep_pool_t;
typedef struct sk_ep_mgr_t {
    struct sk_ep_pool_t* owner;
    sk_entity_mgr_t* eps;

    // mapping
    fhash* ee;   // entity to (ip.port <--> ep) mapping
    flist* tmp;

    int    max;
    int    nep;
} sk_ep_mgr_t;

struct sk_ep_pool_t {
    // SK_EP_TCP
    sk_ep_mgr_t*   tcp;

    // evlp
    fev_state*     evlp;

    // timer service
    sk_timersvc_t* tmsvc;
};

static
void _recv_timeout(sk_entity_t* entity, int valid, sk_obj_t* ud);

static
void _ep_mgr_del(sk_ep_mgr_t* mgr, sk_ep_t* ep);

static
int  sk_ep_timeout(sk_ep_data_t* ep_data)
{
    sk_print("sk_ep_timeout: timeout value: %d\n", ep_data->handler.timeout);
    if (ep_data->handler.timeout == 0) {
        return 0;
    }

    unsigned long long consumed = SK_TIME_NOW - ep_data->start;
    sk_print("sk_ep_timeout: consumed: %llu, timeout value: %d\n",
             consumed, ep_data->handler.timeout);

    if (consumed >= ep_data->handler.timeout) {
        return 1;
    } else {
        return 0;
    }
}

static
unsigned long long sk_ep_time_consumed(sk_ep_data_t* ep_data)
{
    return SK_TIME_NOW - ep_data->start;
}

static
unsigned long long sk_ep_time_left(sk_ep_data_t* ep_data)
{
    unsigned long long consumed = SK_TIME_NOW - ep_data->start;
    if (consumed >= ep_data->handler.timeout) {
        return 0;
    } else {
        return ep_data->handler.timeout - consumed;
    }
}

static
void _ep_data_release(sk_ep_data_t* ep_data)
{
    if (ep_data->conn_timer) {
        sk_timer_cancel(ep_data->conn_timer);
        ep_data->conn_timer = NULL;
    }

    if (ep_data->recv_timer) {
        sk_timer_cancel(ep_data->recv_timer);
        ep_data->recv_timer = NULL;
    }

    if (ep_data->handler.release) {
        ep_data->handler.release(ep_data->ud);
    }

    free(ep_data);
}

static
void _ep_node_destroy(fdlist_node_t* ep_node)
{
    if (!ep_node) return;

    sk_ep_data_t* ep_data = fdlist_get_nodedata(ep_node);
    sk_ep_t*      ep      = ep_data->owner;
    _ep_data_release(ep_data);

    fdlist_delete_node(ep_node);
    fdlist_destroy_node(ep_node);
    ep->ntxn--;
}

static
void _ep_destroy(sk_ep_t* ep)
{
    if (!ep) return;

    // release unfinished user resources
    fdlist_node_t* node = NULL;
    while ((node = fdlist_pop(ep->txns))) {
        _ep_node_destroy(node);
    }
    fdlist_destroy(ep->txns);
    free(ep);
}

static
sk_ep_mgr_t* sk_ep_mgr_create(sk_ep_pool_t* owner, int max)
{
    if (max <= 0) return NULL;

    sk_ep_mgr_t* mgr = calloc(1, sizeof(*mgr));
    mgr->owner = owner;
    mgr->eps   = sk_entity_mgr_create(0);
    mgr->ee    = fhash_u64_create(0, FHASH_MASK_AUTO_REHASH);
    mgr->tmp   = flist_create();
    mgr->max   = max;

    return mgr;
}

static
void sk_ep_mgr_destroy(sk_ep_mgr_t* mgr)
{
    if (!mgr) return;

    // Destroy mapping
    fhash_u64_iter eiter = fhash_u64_iter_new(mgr->ee);
    fhash* ipm = NULL;
    while ((ipm = fhash_u64_next(&eiter))) {
        fhash_str_iter ipiter = fhash_str_iter_new(ipm);
        flist* ep_list = NULL;
        while ((ep_list = fhash_str_next(&ipiter))) {
            sk_ep_t* ep = NULL;
            while ((ep = flist_pop(ep_list))) {
                sk_print("destroy ep: %p\n", (void*)ep);
                _ep_destroy(ep);
            }
            flist_delete(ep_list);
        }
        fhash_str_iter_release(&ipiter);
        fhash_str_delete(ipm);
    }
    fhash_u64_iter_release(&eiter);
    fhash_u64_delete(mgr->ee);

    // destroy entity manager
    sk_entity_mgr_destroy(mgr->eps);
    flist_delete(mgr->tmp);
    free(mgr);
}

// TODO: optimize this part of performance
static
void _remove_ep(sk_ep_mgr_t* mgr, flist* ep_list, sk_ep_t* ep)
{
    if (!ep_list) return;
    if (flist_empty(ep_list)) return;

    sk_print("_remove_ep\n");
    sk_ep_t* t = NULL;
    while ((t = flist_pop(ep_list))) {
        sk_print("_remove_ep: t: %p, ep: %p\n", (void*)t, (void*)ep);
        if (t != ep) {
            sk_print("  _remove_ep: move t :%p to tmp\n", (void*)t);
            flist_push(mgr->tmp, t);
            continue;
        }
    }

    while ((t = flist_pop(mgr->tmp))) {
        flist_push(ep_list, t);
    }

    SK_ASSERT(flist_empty(mgr->tmp));
}

static
sk_ep_t* _find_ep(sk_ep_mgr_t* mgr, flist* ep_list,
                  const sk_ep_handler_t* handler)
{
    if (!ep_list) return NULL;
    if (flist_empty(ep_list)) return NULL;

    sk_ep_t* t = NULL;
    flist_iter iter = flist_new_iter(ep_list);
    while ((t = flist_each(&iter))) {
        if (handler->flags == t->flags) {
            if (t->flags & SK_EP_F_CONCURRENT) {
                return t;
            } else if (t->status == SK_EP_ST_CONNECTED) {
                SK_ASSERT(t->ntxn <= 1);

                if (fdlist_empty(t->txns)) {
                    return t;
                }
            }
        }
    }

    return NULL;
}

static
void _ep_mgr_del(sk_ep_mgr_t* mgr, sk_ep_t* ep)
{
    if (!ep) return;

    sk_entity_mgr_t* emgr = sk_entity_owner(ep->ep);

    SK_ASSERT(emgr);
    SK_ASSERT(ep->owner == mgr);

    // 2.1. update mapping
    fhash* ipm = fhash_u64_get(mgr->ee, ep->ekey);
    if (ipm) {
        flist* ep_list = fhash_str_get(ipm, ep->ipkey);
        if (ep_list) {
            _remove_ep(mgr, ep_list, ep);
        }
    }

    // 2.2 Destroy ep entity
    mgr->nep--;
    sk_entity_mgr_del(emgr, ep->ep);
    sk_entity_mgr_clean_dead(emgr);
    ep->ep = NULL;

    // 2.3 Destroy ep
    _ep_destroy(ep);
}

static
void _timer_data_destroy(sk_ud_t ud)
{
    sk_print("ep connecting timer data destroying...\n");
    sk_ep_timerdata_t* timerdata = ud.ud;
    sk_entity_t* timer_entity = timerdata->timer_entity;

    // Clean the entity if it's still no owner
    if (NULL == sk_entity_owner(timer_entity)) {
        sk_entity_safe_destroy(timer_entity);
    }

    free(timerdata);
}

static
sk_timer_t* _ep_node_create_timer(fdlist_node_t* ep_node,
                                  sk_timer_triggered timer_cb,
                                  uint32_t timeout)
{
    if (timeout == 0) return NULL;

    sk_ep_data_t* ep_data = fdlist_get_nodedata(ep_node);
    sk_ep_mgr_t*  mgr     = ep_data->owner->owner;

    sk_ep_timerdata_t* timerdata = calloc(1, sizeof(*timerdata));
    timerdata->timer_entity = sk_entity_create(NULL);
    timerdata->ep_node = ep_node;

    sk_ud_t cb_data  = {.ud = timerdata};
    sk_obj_opt_t opt = {.preset = NULL, .destroy = _timer_data_destroy};
    sk_obj_t* param_obj = sk_obj_create(opt, cb_data);

    sk_timer_t* timer =
        sk_timersvc_timer_create(mgr->owner->tmsvc, timerdata->timer_entity,
            timeout, timer_cb, param_obj);
    SK_ASSERT(timer);

    return timer;
}

static
sk_ep_status_t _ep_send(fdlist_node_t* ep_node, const void* data, size_t len)
{
    sk_ep_data_t* ep_data = fdlist_get_nodedata(ep_node);
    sk_ep_t* ep = ep_data->owner;

    if (ep->status != SK_EP_ST_CONNECTED) {
        SK_ASSERT(ep->status == SK_EP_ST_CONNECTING);
        // ep still not ready (under connecting), just return
        sk_print("ep still not ready (under connecting), just return\n");
        return SK_EP_OK;
    }

    sk_print("ep_send: send data to %s, len: %zu\n", ep->ipkey, len);
    sk_entity_write(ep->ep, data, len);
    ep_data->status = SK_EP_NST_SENT;

    SK_METRICS_EP_SEND();
    SK_LOG_TRACE(SK_ENV_LOGGER, "ep send");

    // Set up a timeout timer if needed
    if (ep_data->handler.timeout && ep_data->handler.unpack) {
        unsigned long long time_left = sk_ep_time_left(ep_data);
        if (time_left == 0) {
            SK_METRICS_EP_TIMEOUT();
            return SK_EP_TIMEOUT;
        }

        ep_data->recv_timer =
            _ep_node_create_timer(ep_node, _recv_timeout, (uint32_t)time_left);
        SK_ASSERT(ep_data->recv_timer);

        sk_print("_ep_send: setup recv timer, timeout = %u ms\n",
                 (uint32_t)time_left);
    }

    return SK_EP_OK;
}

static
void _handle_timeout(fdlist_node_t* ep_node, int latency)
{
    sk_print("handle timeout\n");
    sk_ep_data_t* ep_data = fdlist_get_nodedata(ep_node);

    sk_ep_ret_t ret = {SK_EP_TIMEOUT, latency};
    ep_data->cb(ret, NULL, 0, ep_data->ud);

    _ep_node_destroy(ep_node);

    SK_METRICS_EP_TIMEOUT();
}

static
void _handle_error(sk_ep_t* ep)
{
    fdlist_node_t* ep_node = NULL;
    while ((ep_node = fdlist_pop(ep->txns))) {
        sk_ep_data_t* ep_data = fdlist_get_nodedata(ep_node);
        int latency = (int)sk_ep_time_consumed(ep_data);

        sk_print("handle_error, ep: %p, latency: %d\n", (void*)ep, latency);
        sk_ep_ret_t ret = {SK_EP_ERROR, latency};
        ep_data->cb(ret, NULL, 0, ep_data->ud);

        _ep_node_destroy(ep_node);

        SK_METRICS_EP_ERROR();
    }

    _ep_mgr_del(ep->owner, ep);
}

static
void _handle_ok(fdlist_node_t* ep_node, const void* data, size_t len)
{
    sk_ep_data_t* ep_data = fdlist_get_nodedata(ep_node);

    sk_ep_ret_t ret = {SK_EP_OK, (int)sk_ep_time_consumed(ep_data)};
    ep_data->cb(ret, data, len, ep_data->ud);

    _ep_node_destroy(ep_node);

    SK_METRICS_EP_OK();
}

static
void _recv_timeout(sk_entity_t* entity, int valid, sk_obj_t* ud)
{
    sk_print("recv_timeout, valid: %d\n", valid);
    if (!valid) {
        sk_print("recv_timeout: timer invalid, skip it\n");
        return;
    }

    sk_ep_timerdata_t* timerdata = sk_obj_get(ud).ud;
    fdlist_node_t* ep_node = timerdata->ep_node;
    sk_ep_data_t*  ep_data = fdlist_get_nodedata(ep_node);

    ep_data->recv_timer = NULL;
    int latency = (int)sk_ep_time_consumed(ep_data);
    _handle_timeout(ep_node, latency);
}

static
int _try_unpack(fdlist_node_t* ep_node, void* ud)
{
    sk_ep_data_t* ep_data = fdlist_get_nodedata(ep_node);
    if (!ep_data->handler.unpack) {
        sk_print("no ep unpack function, skip it\n");
        return 0;
    }

    sk_ep_readarg_t* readarg = ud;
    const void*      data    = readarg->data;
    size_t           len     = readarg->len;

    size_t consumed = ep_data->handler.unpack(ep_data->ud, data, len);
    if (consumed == 0) {
        // means user need more data, re-try in next round
        sk_print("user need more data, current data size=%zu\n", len);
        return 0;
    }

    // Set return consumed value
    readarg->consumed = consumed;

    // cancel the timer if needed
    if (ep_data->recv_timer) {
        sk_timer_cancel(ep_data->recv_timer);
        ep_data->recv_timer = NULL;
    }
    return 1;
}

static
void _read_cb(fev_state* fev, fev_buff* evbuff, void* arg)
{
    sk_ep_t* ep = arg;
    SK_ASSERT(ep->status == SK_EP_ST_CONNECTED);

    size_t read_len = 0;
    size_t used_len = fevbuff_get_usedlen(evbuff, FEVBUFF_TYPE_READ);
    if (used_len == 0) {
        read_len = 1024;
    } else {
        read_len = used_len * 2;
    }

    ssize_t bytes = sk_entity_read(ep->ep, NULL, read_len);
    sk_print("ep entity read bytes: %ld\n", bytes);
    if (bytes <= 0) {
        sk_print("ep entity buffer cannot read\n");
        return;
    }

    const void* data = fevbuff_rawget(evbuff);

    sk_ep_readarg_t readarg = {
        .data     = data,
        .len      = (size_t)bytes,
        .consumed = 0
    };

    fdlist_node_t* succeed_node =
        fdlist_foreach(ep->txns, _try_unpack, &readarg);

    if (!succeed_node) {
        sk_print("There is no ep txn unpack succeed, waiting for next time\n");
        return;
    }

    size_t consumed = readarg.consumed;
    sk_print("ep txn unpack succeed, consumed: %zu\n", consumed);
    fevbuff_pop(evbuff, consumed);

    _handle_ok(succeed_node, data, consumed);
    SK_METRICS_EP_RECV();
    SK_LOG_TRACE(SK_ENV_LOGGER, "ep recv");
}

static
void _error(fev_state* fev, fev_buff* evbuff, void* arg)
{
    sk_ep_t* ep = arg;
    _handle_error(ep);
}

static
int _send_each(fdlist_node_t* ep_node, void* ud)
{
    sk_print("send_each\n");
    flist* timeout_nodes = ud;
    sk_ep_data_t* ep_data = fdlist_get_nodedata(ep_node);
    if (ep_data->status != SK_EP_NST_INIT) {
        sk_print("ep txn node status is not init, skip to send data to it."
                 "status: %d\n", ep_data->status);
        return 0;
    }

    if (ep_data->conn_timer) {
        sk_timer_cancel(ep_data->conn_timer);
        ep_data->conn_timer = NULL;
    }

    sk_ep_status_t ret = _ep_send(ep_node, ep_data->data, ep_data->count);
    if (ret == SK_EP_TIMEOUT) {
        flist_push(timeout_nodes, ep_node);
    }

    return 0;
}

static
void _on_connect(fev_state* fev, int fd, int mask, void* arg)
{
    sk_print("_on_connect\n");
    SK_LOG_TRACE(SK_ENV_LOGGER, "_on_connect");

    sk_ep_t* ep = arg;
    int sockfd = fd;
    fev_del_event(fev, sockfd, FEV_READ | FEV_WRITE);

    if (mask & FEV_ERROR) {
        sk_print("error ocurred\n");
        goto CONN_ERROR;
    }

    if (ep->status != SK_EP_ST_CONNECTING) {
        SK_ASSERT(0);
        _ep_destroy(ep);
        return;
    }

    int err = 0;
    socklen_t len = sizeof(int);
    if ((0 == getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &len))) {
        if (0 == err) {
            sk_print("target %s connected\n", ep->ipkey);
            sk_ep_mgr_t* mgr = ep->owner;
            fev_buff* evbuff =
                fevbuff_new(mgr->owner->evlp, sockfd, _read_cb, _error, ep);
            SK_ASSERT(evbuff);
            sk_entity_net_create(ep->ep, evbuff);
            ep->status = SK_EP_ST_CONNECTED;
            SK_LOG_TRACE(SK_ENV_LOGGER, "ep connected (async)");

            sk_print("prepare send data to target, txns size: %zu\n", fdlist_size(ep->txns));
            flist* timeout_nodes = ep->owner->tmp;
            fdlist_foreach(ep->txns, _send_each, timeout_nodes);

            fdlist_node_t* timeout_node = NULL;
            while ((timeout_node = flist_pop(timeout_nodes))) {
                sk_ep_data_t* ep_data = fdlist_get_nodedata(timeout_node);
                _handle_timeout(timeout_node, (int)sk_ep_time_consumed(ep_data));
            }
            return;
        }
    }

CONN_ERROR:
    _handle_error(ep);
}

static
void _conn_timeout(sk_entity_t* entity, int valid, sk_obj_t* ud)
{
    sk_print("conn_timeout, valid: %d\n", valid);
    if (!valid) {
        sk_print("connection timer not valid, skip it\n");
        return;
    }

    sk_ep_timerdata_t* timerdata = sk_obj_get(ud).ud;
    fdlist_node_t* ep_node = timerdata->ep_node;
    sk_ep_data_t*  ep_data = fdlist_get_nodedata(ep_node);
    sk_ep_t*       ep      = ep_data->owner;

    if (ep->status != SK_EP_ST_CONNECTING) {
        sk_print("ep is not in connecting, skip it, status=%d\n", ep->status);
        return;
    }

    ep_data->conn_timer = NULL;
    _handle_timeout(ep_node, (int)sk_ep_time_consumed(ep_data));
}

// Currently only support ipv4
static
int _create_entity_tcp(sk_ep_mgr_t* mgr, const sk_ep_handler_t* handler,
                       sk_ep_t* ep, unsigned long long start,
                       fdlist_node_t* ep_node)
{
    sk_ep_data_t* ep_data = fdlist_get_nodedata(ep_node);
    if (sk_ep_timeout(ep_data)) {
        sk_print("create_entity_tcp: timed out, won't connect to target\n");
        return 1;
    }

    ep->status = SK_EP_ST_CONNECTING;

    sk_print("create connection to %s, ep status: %d\n", ep->ipkey, ep->status);
    int sockfd = -1;
    int s = fnet_conn_async(handler->ip, handler->port, &sockfd);
    if (s == 0) {
        sk_entity_t* net_entity = ep->ep;

        fev_buff* evbuff =
            fevbuff_new(mgr->owner->evlp, sockfd, _read_cb, _error, ep);
        SK_ASSERT(evbuff);
        sk_entity_net_create(net_entity, evbuff);

        ep->status = SK_EP_ST_CONNECTED;
        SK_LOG_TRACE(SK_ENV_LOGGER, "ep connected (sync)");
        return 0;
    } else if (s > 0) {
        if (ep_data->handler.timeout > 0) {
            unsigned long long time_left = sk_ep_time_left(ep_data);
            if (time_left == 0) {
                sk_print("create_entity_tcp: timed out1, won't connect to target\n");
                return -1;
            }

            // Setup a connection timeout timer
            ep_data->conn_timer =
                _ep_node_create_timer(ep_node, _conn_timeout,
                    (uint32_t)time_left);
            SK_ASSERT(ep_data->conn_timer);

            sk_print("connection setup timer: timeout: %u\n",
                     (uint32_t)time_left);
        }

        // Setup connection socket to evlp
        int ret = fev_reg_event(mgr->owner->evlp, sockfd, FEV_WRITE, NULL,
                                _on_connect, ep);
        SK_ASSERT(!ret);

        return 0;
    } else {
        return -1;
    }
}

static
sk_ep_t* _ep_create(sk_ep_mgr_t* mgr, const sk_ep_handler_t* handler,
                    uint64_t ekey, const char* ipkey,
                    unsigned long long start)
{
    sk_ep_t* ep = calloc(1, sizeof(*ep));
    ep->ekey    = ekey;
    strncpy(ep->ipkey, ipkey, SK_EP_KEY_MAX);
    ep->type    = handler->type;
    ep->status  = SK_EP_ST_INIT;
    ep->owner   = mgr;
    ep->ep      = sk_entity_create(NULL);
    ep->txns    = fdlist_create();
    ep->ntxn    = 0;
    ep->flags   = handler->flags;
    SK_ASSERT(ep->ep);

    return ep;
}

static
fdlist_node_t* _ep_mgr_get_or_create(sk_ep_mgr_t*           mgr,
                                     const sk_entity_t*     entity,
                                     const sk_ep_handler_t* handler,
                                     unsigned long long     start,
                                     sk_ep_cb_t             cb,
                                     void*                  ud,
                                     const void*            data,
                                     size_t                 count)
{
    fhash*   ipm     = NULL;
    flist*   ep_list = NULL;
    sk_ep_t* ep      = NULL;
    uint64_t ekey    = 0;

    int flags = handler->flags;
    if (flags & SK_EP_F_ORPHAN) {
        ekey = 0;
    } else {
        ekey = (uint64_t) (uintptr_t) entity;
    }

    ipm = fhash_u64_get(mgr->ee, ekey);
    if (!ipm) {
        ipm = fhash_str_create(0, FHASH_MASK_AUTO_REHASH);
        fhash_u64_set(mgr->ee, ekey, ipm);
    }

    char ipkey[SK_EP_KEY_MAX];
    snprintf(ipkey, SK_EP_KEY_MAX, "%s.%d", handler->ip, handler->port);
    ep_list = fhash_str_get(ipm, ipkey);
    if (!ep_list) {
        ep_list = flist_create();
        fhash_str_set(ipm, ipkey, ep_list);
    }

    ep = _find_ep(mgr, ep_list, handler);
    if (!ep) {
        ep = _ep_create(mgr, handler, ekey, ipkey, start);
        if (!ep) return NULL;

        sk_entity_mgr_add(mgr->eps, ep->ep);
        flist_push(ep_list, ep);
        mgr->nep++;

        SK_METRICS_EP_CREATE();
        SK_LOG_TRACE(SK_ENV_LOGGER, "ep create");
    }

    // Create new ep data
    sk_ep_data_t* ep_data = calloc(1, sizeof(*ep_data));
    ep_data->owner   = ep;
    ep_data->handler = *handler;
    ep_data->cb      = cb;
    ep_data->ud      = ud;
    ep_data->start   = start;
    ep_data->data    = data;
    ep_data->count   = count;
    ep_data->e       = entity;
    ep_data->status  = SK_EP_NST_INIT;

    fdlist_node_t* ep_node = fdlist_make_node(ep_data, sizeof(*ep_data));
    fdlist_push(ep->txns, ep_node);
    ep->ntxn++;

    // Connect to target if needed
    if (ep->status == SK_EP_ST_INIT) {
        int r = _create_entity_tcp(mgr, handler, ep, start, ep_node);
        if (r) {
            _ep_node_destroy(ep_node);
            _ep_mgr_del(mgr, ep);
            return NULL;
        }
    }

    return ep_node;
}

// Public APIs
sk_ep_pool_t* sk_ep_pool_create(void* evlp, sk_timersvc_t* tmsvc, int max)
{
    sk_ep_pool_t* pool = calloc(1, sizeof(*pool));
    pool->tcp   = sk_ep_mgr_create(pool, max);
    pool->evlp  = evlp;
    pool->tmsvc = tmsvc;

    return pool;
}

void sk_ep_pool_destroy(sk_ep_pool_t* pool)
{
    if (!pool) return;

    sk_ep_mgr_destroy(pool->tcp);
    free(pool);
}

sk_ep_status_t sk_ep_send(sk_ep_pool_t* pool, const sk_entity_t* entity,
               const sk_ep_handler_t handler,
               const void* data, size_t count,
               sk_ep_cb_t cb, void* ud)
{
    SK_ASSERT(pool);
    unsigned long long start = SK_TIME_NOW;

    // 1. check
    if (!data || !count) {
        sk_print("sk_ep_send: error -> no data or data_len\n");
        SK_METRICS_EP_ERROR();
        return SK_EP_ERROR;
    }

    if (!handler.ip || handler.port == 0) {
        sk_print("sk_ep_send: error -> invalid ip or port\n");
        SK_METRICS_EP_ERROR();
        return SK_EP_ERROR;
    }

    // 2. get ep mgr
    sk_ep_mgr_t* mgr = NULL;
    sk_ep_type_t et = handler.type;
    if (et == SK_EP_TCP) {
        mgr = pool->tcp;
    } else {
        sk_print("sk_ep_send: error -> invalid protocol\n");
        SK_METRICS_EP_ERROR();
        return SK_EP_ERROR;
    }

    // 3. pick up one ep from ep mgr
    if (mgr->nep == mgr->max) {
        SK_METRICS_EP_ERROR();
        return SK_EP_ERROR;
    }

    fdlist_node_t* ep_node =
        _ep_mgr_get_or_create(mgr, entity, &handler, start, cb, ud, data, count);
    if (!ep_node) {
        sk_print("sk_ep_send: error -> create ep failed\n");
        SK_METRICS_EP_ERROR();
        return SK_EP_ERROR;
    }

    // 4. send via ep
    return _ep_send(ep_node, data, count);
}