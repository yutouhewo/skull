#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <iostream>

#include "skull/api.h"
#include "skullcpp/api.h"
#include "skull_metrics.h"
#include "skull_protos.h"
#include "config.h"

extern "C" {
void module_init(skull_config_t* config);
void module_release();
size_t module_unpack(skullcpp::Txn& txn, const void* data, size_t data_sz);
int module_run(skullcpp::Txn& txn);
void module_pack(skullcpp::Txn& txn, skullcpp::TxnData& txndata);
}

void module_init(skull_config_t* config)
{
    printf("module(test): init\n");
    SKULL_LOG_TRACE("skull trace log test %d", 1);
    SKULL_LOG_DEBUG("skull debug log test %d", 2);
    SKULL_LOG_INFO("1", "skull info log test %d", 3);
    SKULL_LOG_WARN("1", "skull warn log test %d", 4, "ignore, this is test");
    SKULL_LOG_ERROR("1", "skull error log test %d", 5, "ignore, this is test");
    SKULL_LOG_FATAL("1", "skull fatal log test %d", 6, "ignore, this is test");

    // Convert skull_config to skull_static_config
    skull_static_config_convert(config);

    SKULL_LOG_DEBUG("config test_item: %d", skull_static_config()->test_item);
    SKULL_LOG_DEBUG("config test_rate: %f", skull_static_config()->test_rate);
    SKULL_LOG_DEBUG("config test_name: %s", skull_static_config()->test_name);
}

void module_release()
{
    printf("module(test): released\n");
    SKULL_LOG_INFO("5", "module(test): released");

    skull_static_config_destroy();
}

size_t module_unpack(skullcpp::Txn& txn, const void* data, size_t data_sz)
{
    skull_metrics_module.request.inc(1);
    printf("module_unpack(test): data sz:%zu\n", data_sz);
    SKULL_LOG_INFO("2", "module_unpack(test): data sz:%zu", data_sz);

    // deserialize data to transcation data
    skull::example& example = (skull::example&)txn.data();
    example.set_data(data, data_sz);
    return data_sz;
}

static
int svc_api_callback(skullcpp::Txn& txn, const std::string& apiName,
                     const google::protobuf::Message& request,
                     const google::protobuf::Message& response)
{
    std::cout << "svc_api_callback...." << apiName << std::endl;
    std::cout << "svc_api_callback.... request: " << ((s1::get_req&)request).name() << std::endl;
    std::cout << "svc_api_callback.... response: " << ((s1::get_resp&)response).response() << std::endl;
    return 0;
}

int module_run(skullcpp::Txn& txn)
{
    skull::example& example = (skull::example&)txn.data();

    printf("receive data: %s\n", example.data().c_str());
    SKULL_LOG_INFO("3", "receive data: %s", example.data().c_str());

    // Call service
    s1::get_req req;
    req.set_name("hello service");
    skullcpp::Service::Status ret = skullcpp::ServiceCall(txn, "s1", "get",
                                                          req, svc_api_callback,
                                                          0);
    std::cout << "ServiceCall ret: " << ret << std::endl;
    return 0;
}

void module_pack(skullcpp::Txn& txn, skullcpp::TxnData& txndata)
{
    skull::example& example = (skull::example&)txn.data();

    skull_metrics_module.response.inc(1);
    printf("module_pack(test): data sz:%zu\n", example.data().length());
    SKULL_LOG_INFO("4", "module_pack(test): data sz:%zu", example.data().length());
    txndata.append(example.data().c_str(), example.data().length());
}