include Makefile.inc

# dependency library list
SKULL_LIBS = \
    -lyaml \
    -lflibs \
    $(JEMALLOC_STATIC) \
    -lprotobuf-c \
    -pthread \
    -ldl \
    -lrt

# prefix
prefix ?= /usr/local

# binaries
SKULL_ENGINE = skull-engine

# ut library
SKULL_UTLIB_C = libskull-unittest-c.so

# static macros
EXTRA_MACRO += $(shell ../scripts/tools/static_macros.sh)
EXTRA_MACRO += -DSKULL_COMPILER_OPT=\"$(OPT)\"

# Objs
COMMON_OBJS = \
    common/sk_loader.o \
    common/sk_srv_loader.o \
    common/sk_user_loader.o \
    common/sk_utils.o \
    common/sk_io.o \
    common/sk_eventloop.o \
    common/sk_event.o \
    common/sk_config_loader.o \
    common/sk_txn.o \
    common/sk_env.o \
    common/sk_workflow.o \
    common/sk_log.o \
    common/sk_log_tpl.o \
    common/sk_mon.o \
    common/sk_metrics.o \
    common/sk_queue.o \
    common/sk_service_data.o \
    common/sk_timer_service.o \
    common/sk_object.o \
    common/sk_mbuf.o \
    common/sk_module.o

ENTITY_OBJS = \
    entities/sk_entity.o \
    entities/sk_entity_net.o \
    entities/sk_entity_stdin.o \
    entities/sk_entity_util.o

TRIGGER_OBJS = \
    triggers/sk_trigger.o \
    triggers/sk_trigger_immedia.o \
    triggers/sk_trigger_sock.o \
    triggers/sk_trigger_stdin.o

COMPONENTS_OBJS = \
    components/sk_sched.o \
    components/sk_core.o \
    components/sk_entity_mgr.o \
    components/sk_config.o \
    components/sk_engine.o \
    components/sk_service.o \
    components/sk_admin.o \
    components/sk_ep_pool.o

PTO_OBJS = \
    pto/sk_pto.o \
    pto/sk_pto_net_accept.o \
    pto/sk_pto_entity_destroy.o \
    pto/sk_pto_workflow_run.o \
    pto/sk_pto_service_iocall.o \
    pto/sk_pto_service_task_run.o \
    pto/sk_pto_service_task_complete.o \
    pto/sk_pto_timer_triggered.o \
    pto/sk_pto_timer_emit.o \
    pto/sk_pto_stdin_start.o \
    pto/sk_pto_service_timer_run.o \
    pto/sk_pto_service_timer_complete.o \
    pto/sk_pto_service_task_cb.o

PTO_IDLS_OBJS = \
    pto/idls/net_accept.pb-c.o \
    pto/idls/entity_destroy.pb-c.o \
    pto/idls/workflow_run.pb-c.o \
    pto/idls/service_iocall.pb-c.o \
    pto/idls/service_task.pb-c.o \
    pto/idls/timer_triggered.pb-c.o \
    pto/idls/timer_emit.pb-c.o \
    pto/idls/stdin_start.pb-c.o \
    pto/idls/service_timer.pb-c.o

USER_C_OBJS = \
    user-c/src/logger_private.o \
    user-c/src/metrics_utils.o \
    user-c/src/module_loader.o \
    user-c/src/txn.o \
    user-c/src/txn_iocall.o \
    user-c/src/txn_utils.o \
    user-c/src/config.o \
    user-c/src/module_executor.o \
    user-c/src/service_loader.o \
    user-c/src/srv_executor.o \
    user-c/src/service.o \
    user-c/src/ep.o

SKULL_MAIN_OBJ = main.o

SKULL_OBJS = \
    $(COMMON_OBJS) \
    $(ENTITY_OBJS) \
    $(TRIGGER_OBJS) \
    $(COMPONENTS_OBJS) \
    $(SKULL_MAIN_OBJ) \
    $(PTO_OBJS) \
    $(PTO_IDLS_OBJS) \
    $(USER_C_OBJS)

PROTO_IDLS = \
    net_accept.proto \
    entity_destroy.proto \
    workflow_run.proto \
    service_iocall.proto \
    service_task.proto \
    timer_triggered.proto \
    timer_emit.proto \
    stdin_start.proto \
    service_timer.proto

UT_OBJS = \
    user-c/src/unittest.o \
    user-c/src/module_executor.o \
    user-c/src/module_loader.o \
    user-c/src/service_loader.o \
    user-c/src/srv_executor.o \
    user-c/src/txn.o \
    user-c/src/txn_utils.o \
    user-c/src/config.o \
    common/sk_utils.o \
    common/sk_loader.o \
    common/sk_srv_loader.o \
    common/sk_txn.o \
    common/sk_workflow.o \
    common/sk_config_loader.o \
    common/sk_queue.o \
    common/sk_service_data.o \
    common/sk_metrics.o \
    common/sk_object.o \
    common/sk_mbuf.o \
    common/sk_module.o \
    entities/sk_entity.o \
    entities/sk_entity_net.o \
    entities/sk_entity_stdin.o \
    pto/idls/service_iocall.pb-c.o \
    pto/idls/service_task.pb-c.o

# target rules
all: $(SKULL_ENGINE) $(SKULL_UTLIB_C)

# compile proto idls
protos: $(PROTO_IDLS)

install:
	test -d $(prefix)/bin || mkdir -p $(prefix)/bin
	test -x $(SKULL_ENGINE) && cp $(SKULL_ENGINE) $(prefix)/bin
	test -f $(SKULL_UTLIB_C) && cp $(SKULL_UTLIB_C) $(prefix)/lib

clean:
	rm -f $(SKULL_ENGINE) $(SKULL_UTLIB_C) $(SKULL_OBJS) $(UT_OBJS)

clean-protos:
	rm -f pto/idls/*.pb-c.*

.PHONY: skull clean install protos clean-protos

# sub targets
$(SKULL_ENGINE): $(SKULL_OBJS)
	$(SKULL_LD) -o $@ $^ $(SKULL_LIBS)

# compile UT lib
$(SKULL_UTLIB_C): $(UT_OBJS)
	$(SKULL_UTLD) -o $(SKULL_UTLIB_C) $(UT_OBJS) $(SKULL_LIBS)

# compile protocol file without -Wpadded
%.pb-c.o: %.pb-c.c
	$(SKULL_TP_CC) -c $< -o $@

# This file needs extra_macro to build static core information
common/sk_utils.o: common/sk_utils.c
	$(SKULL_CC) $(EXTRA_MACRO) -c $< -o $@

%.o: %.c
	$(SKULL_CC) -c $< -o $@

%.proto:
	cd pto/idls && protoc-c --c_out=. $@
