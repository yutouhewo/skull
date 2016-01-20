#include <stdlib.h>
#include <string.h>

#include "api/sk_utils.h"
#include "api/sk_txn.h"
#include "api/sk_env.h"
#include "api/sk_ep_pool.h"
#include "api/sk_service.h"
#include "idl_internal.h"
#include "srv_types.h"
#include "skull/ep.h"

typedef struct ep_job_t {
    skull_ep_handler_t handler;
    skull_service_t    service;
    skull_ep_cb_t      cb;
    void*              ud;
} ep_job_t;

static
size_t _unpack(void* ud, const void* data, size_t len)
{
    ep_job_t* job = ud;
    return job->handler.unpack(job->ud, data, len);
}

static
void _release(void* ud)
{
    if (!ud) return;

    ep_job_t* job = ud;
    job->handler.release(job->ud);
    free(job);
}

static
void _ep_cb(sk_ep_ret_t ret, const void* response, size_t len, void* ud)
{
    sk_print("prepare to run ep callback\n");

    // 1. Call user callback
    ep_job_t* job = ud;
    skull_ep_ret_t skull_ret;

    skull_service_t* service = &job->service;

    // TODO: Tricky here, should manually convert the fields one by one
    memcpy(&skull_ret, &ret, sizeof(ret));

    // 2. Restore the request
    char req_proto_name[SKULL_SRV_PROTO_MAXLEN];
    snprintf(req_proto_name, SKULL_SRV_PROTO_MAXLEN, "%s.%s_req",
             sk_service_name(service->service), service->task->api_name);

    const ProtobufCMessageDescriptor* req_desc =
        skull_srv_idl_descriptor(req_proto_name);

    ProtobufCMessage* req_msg = NULL;
    req_msg = protobuf_c_message_unpack(
            req_desc, NULL, service->task->request_sz, service->task->request);

    job->cb(skull_ret, response, len, job->ud, service->task->request,
            service->task->response_pb_msg);

    // 3. clean up the req
    protobuf_c_message_free_unpacked(req_msg, NULL);

    // 4. Reduce pending tasks counts
    job->service.task->pendings--;
    sk_print("service task pending cnt: %u\n", service->task->pendings);

    // 5. Try to call api callback
    sk_service_api_complete(service->service, service->txn, service->task,
                            service->task->api_name);
}

static
ep_job_t* _ep_job_create(skull_service_t* service,
                         const skull_ep_handler_t* handler,
                         skull_ep_cb_t cb, const void* ud,
                         sk_ep_handler_t* sk_handler)
{
    // Create ep_job
    ep_job_t* job = calloc(1, sizeof(*job));
    job->handler = *handler;
    job->service = *service;
    job->cb      = cb;
    job->ud      = (void*) ud;

    // Construct sk_ep_handler
    memset(sk_handler, 0, sizeof(*sk_handler));

    // TODO: Tricky here, should manually convert the fields one by one
    memcpy(sk_handler, handler, sizeof(*handler));
    sk_handler->unpack  = _unpack;
    sk_handler->release = _release;
    return job;
}

skull_ep_status_t
skull_ep_send(skull_service_t* service, const skull_ep_handler_t handler,
              const void* data, size_t count,
              skull_ep_cb_t cb, void* ud)
{
    sk_print("calling ep_send...\n");

    // Construct ep job
    sk_ep_handler_t sk_handler;
    ep_job_t* job = _ep_job_create(service, &handler, cb, ud, &sk_handler);
    const sk_entity_t* ett = sk_txn_entity(service->txn);

    // Send job to ep pool
    sk_ep_status_t ret = sk_ep_send(SK_ENV_EP, ett, sk_handler, data, count, _ep_cb, job);
    if (ret == SK_EP_OK) {
        service->task->pendings++;
    }

    sk_print("service task pending cnt: %u\n", service->task->pendings);

    switch (ret) {
    case SK_EP_OK:          return SKULL_EP_OK;
    case SK_EP_ERROR:       return SKULL_EP_ERROR;
    case SK_EP_TIMEOUT:     return SKULL_EP_TIMEOUT;
    default: SK_ASSERT(0);  return SKULL_EP_ERROR;
    }
}