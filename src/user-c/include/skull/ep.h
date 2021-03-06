#ifndef SKULL_EP_H
#define SKULL_EP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <netinet/in.h>

#include "skull/service.h"

typedef enum skull_ep_type_t {
    SKULL_EP_TCP = 0,
    SKULL_EP_UDP = 1
} skull_ep_type_t;

typedef enum skull_ep_status_t {
    SKULL_EP_OK          = 0,
    SKULL_EP_ERROR       = 1,
    SKULL_EP_TIMEOUT     = 2
} skull_ep_status_t;

typedef struct skull_ep_ret_t {
    skull_ep_status_t status;
    int               latency;
} skull_ep_ret_t;

typedef struct skull_ep_handler_t {
    skull_ep_type_t type;
    in_port_t    port;
    uint16_t     _reserved;

    const char*  ip;

    // unit: millisecond
    // <= 0: means no timeout
    // >  0: after x milliseconds, the ep call would time out
    int          timeout;

    // Reserved
    int          flags;


    // return < 0: Error occurred
    // return = 0: The response data has not finished yet
    // return > 0: The response data has finished
    ssize_t      (*unpack)  (void* ud, const void* data, size_t len);
    void         (*release) (void* ud);
} skull_ep_handler_t;

typedef void (*skull_ep_cb_t) (const skull_service_t*, skull_ep_ret_t,
                               const void* response, size_t len, void* ud,
                               const void* api_req, size_t api_req_sz,
                               void* api_resp, size_t api_resp_sz);

typedef void (*skull_ep_np_cb_t) (const skull_service_t*, skull_ep_ret_t,
                               const void* response, size_t len, void* ud);

/**
 * This api would pend the service call until it's callback be finished
 *
 * @note It only can be called in a service api call. If this be called in
 *        non-service api call, it would return ERROR status
 */
skull_ep_status_t
skull_ep_send(const skull_service_t*, const skull_ep_handler_t handler,
              const void* data, size_t count, skull_ep_cb_t cb, void* ud);

/**
 * This api would not pend the service call
 */
skull_ep_status_t
skull_ep_send_np(const skull_service_t*, const skull_ep_handler_t handler,
              const void* data, size_t count, skull_ep_np_cb_t cb, void* ud);

#ifdef __cplusplus
}
#endif

#endif

