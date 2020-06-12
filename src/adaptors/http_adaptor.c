/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <stdio.h>
#include <inttypes.h>

#include <proton/condition.h>
#include <proton/listener.h>
#include <proton/proactor.h>
#include <proton/raw_connection.h>
#include <nghttp2/nghttp2.h>

#include "qpid/dispatch/protocol_adaptor.h"
#include "delivery.h"
#include "http_adaptor.h"

#define READ_BUFFERS 4

ALLOC_DEFINE(qd_http_lsnr_t);
ALLOC_DEFINE(qd_http_connector_t);

typedef struct qdr_http_adaptor_t {
    qdr_core_t              *core;
    qdr_protocol_adaptor_t  *adaptor;
    qd_http_lsnr_list_t      listeners;
    qd_http_connector_list_t connectors;
    qd_log_source_t         *log_source;
} qdr_http_adaptor_t;


static qdr_http_adaptor_t *http_adaptor;

typedef struct qdr_http_connection_t {
    qd_handler_context_t  context;
    char                 *reply_to;
    qdr_connection_t     *qdr_conn;
    qdr_link_t           *in_link;
    qdr_link_t           *out_link;
    uint64_t              incoming_id;
    uint64_t              outgoing_id;
    pn_raw_connection_t  *pn_raw_conn;
    pn_raw_buffer_t       read_buffers[4];
    qdr_delivery_t       *in_dlv;
    qdr_delivery_t       *out_dlv;
    bool                  ingress;
    qd_timer_t           *activate_timer;
    qd_bridge_config_t   *config;
    qd_server_t          *server;

} qdr_http_connection_t;


static void free_bridge_config(qd_bridge_config_t *config)
{
    if (!config) {
        return;
    }
    free(config->host);
    free(config->port);
    free(config->name);
    free(config->address);
    free(config->host_port);
}
void qd_http_listener_decref(qd_http_lsnr_t* li)
{
    if (li && sys_atomic_dec(&li->ref_count) == 1) {
        free_bridge_config(&li->config);
        free_qd_http_lsnr_t(li);
    }
}

static void qdr_http_detach(void *context, qdr_link_t *link, qdr_error_t *error, bool first, bool close)
{
}


static void qdr_http_flow(void *context, qdr_link_t *link, int credit)
{
}


static void qdr_http_offer(void *context, qdr_link_t *link, int delivery_count)
{
}


static void qdr_http_drained(void *context, qdr_link_t *link)
{
}


static void qdr_http_drain(void *context, qdr_link_t *link, bool mode)
{
}

static int qdr_http_get_credit(void *context, qdr_link_t *link)
{
    return 1;
}


static void qdr_http_delivery_update(void *context, qdr_delivery_t *dlv, uint64_t disp, bool settled)
{
}


static void qdr_http_conn_close(void *context, qdr_connection_t *conn, qdr_error_t *error)
{
}


static void qdr_http_conn_trace(void *context, qdr_connection_t *conn, bool trace)
{
}

static void qdr_http_activate(void *context, qdr_connection_t *c)
{

}

static void qdr_http_first_attach(void *context, qdr_connection_t *conn, qdr_link_t *link,
                                 qdr_terminus_t *source, qdr_terminus_t *target,
                                 qd_session_class_t session_class)
{
}

static void qdr_http_second_attach(void *context, qdr_link_t *link,
                                  qdr_terminus_t *source, qdr_terminus_t *target)
{

}

static int qdr_http_push(void *context, qdr_link_t *link, int limit)
{
    return 1;
}

static uint64_t qdr_http_deliver(void *context, qdr_link_t *link, qdr_delivery_t *delivery, bool settled)
{
    return 0;
}

/**
 * This initialization function will be invoked when the router core is ready for the protocol
 * adaptor to be created.  This function must:
 *
 *   1) Register the protocol adaptor with the router-core.
 *   2) Prepare the protocol adaptor to be configured.
 */
static void qdr_http_adaptor_init(qdr_core_t *core, void **adaptor_context)
{
    qdr_http_adaptor_t *adaptor = NEW(qdr_http_adaptor_t);
    adaptor->core    = core;
    adaptor->adaptor = qdr_protocol_adaptor(core,
                                            "http",                // name
                                            adaptor,              // context
                                            qdr_http_activate,                    // activate
                                            qdr_http_first_attach,
                                            qdr_http_second_attach,
                                            qdr_http_detach,
                                            qdr_http_flow,
                                            qdr_http_offer,
                                            qdr_http_drained,
                                            qdr_http_drain,
                                            qdr_http_push,
                                            qdr_http_deliver,
                                            qdr_http_get_credit,
                                            qdr_http_delivery_update,
                                            qdr_http_conn_close,
                                            qdr_http_conn_trace);
    adaptor->log_source  = qd_log_source("HTTP_ADAPTOR");
    *adaptor_context = adaptor;
    DEQ_INIT(adaptor->listeners);

    //FIXME: I need adaptor when handling configuration, need to
    //figure out right way to do that. Just hold on to a pointer as
    //temporary hack for now.
    http_adaptor = adaptor;
}


static void handle_incoming_http(qdr_http_connection_t *conn, pn_raw_buffer_t rawbuf)
{
    qd_buffer_list_t   buffers;
    qd_buffer_t       *buf;
    DEQ_INIT(buffers);
    buf = qd_buffer();
    char *insert = (char*) qd_buffer_cursor(buf);
    strncpy(insert, rawbuf.bytes, rawbuf.size);
    qd_buffer_insert(buf, rawbuf.size);
    DEQ_INSERT_HEAD(buffers, buf);


    nghttp2_session_callbacks *callbacks = 0;
    if (callbacks) {

    }
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_del(callbacks);
}


static void handle_connection_event(pn_event_t *e, qd_server_t *qd_server, void *context)
{
    qdr_http_connection_t *conn = (qdr_http_connection_t*) context;
    qd_log_source_t *log = http_adaptor->log_source;
    switch (pn_event_type(e)) {
    case PN_RAW_CONNECTION_CONNECTED: {
        qd_log(log, QD_LOG_NOTICE, "Connected on %s %p", conn->ingress ? "ingress" : "egress", conn->pn_raw_conn);
        qdr_connection_process(conn->qdr_conn);
        break;
    }
    case PN_RAW_CONNECTION_CLOSED_READ: {
        qd_log(log, QD_LOG_NOTICE, "Closed for reading on %s %p", conn->ingress ? "ingress" : "egress", conn->pn_raw_conn);
        break;
    }
    case PN_RAW_CONNECTION_CLOSED_WRITE: {
        qd_log(log, QD_LOG_NOTICE, "Closed for writing on %s %p", conn->ingress ? "ingress" : "egress", conn->pn_raw_conn);
        break;
    }
    case PN_RAW_CONNECTION_DISCONNECTED: {
        qd_log(log, QD_LOG_NOTICE, "Disconnected on %s %p", conn->ingress ? "ingress" : "egress", conn->pn_raw_conn);
        break;
    }
    case PN_RAW_CONNECTION_NEED_WRITE_BUFFERS: {
        qd_log(log, QD_LOG_NOTICE, "Need write buffers on %s %p", conn->ingress ? "ingress" : "egress", conn->pn_raw_conn);
        qdr_connection_process(conn->qdr_conn);
        break;
    }
    case PN_RAW_CONNECTION_NEED_READ_BUFFERS:
    case PN_RAW_CONNECTION_WAKE:
        qdr_connection_process(conn->qdr_conn);
        break;
    case PN_RAW_CONNECTION_READ: {
        qd_log(log, QD_LOG_NOTICE, "Data read for %s %p", conn->ingress ? "ingress" : "egress", conn->pn_raw_conn);
        pn_raw_buffer_t buffs[READ_BUFFERS];
        size_t n;
        while ( (n = pn_raw_connection_take_read_buffers(conn->pn_raw_conn, buffs, READ_BUFFERS)) ) {
            unsigned i;
            for (i=0; i<n && buffs[i].bytes; ++i) {
                handle_incoming_http(conn, buffs[i]);
            }

            if (!pn_raw_connection_is_read_closed(conn->pn_raw_conn)) { //TODO: probably want to check credit here as well
                pn_raw_connection_give_read_buffers(conn->pn_raw_conn, buffs, n);
            }
        }
        qdr_connection_process(conn->qdr_conn);
        break;
    }
    case PN_RAW_CONNECTION_WRITTEN:
        qd_log(log, QD_LOG_NOTICE, "Data written for %s %p", conn->ingress ? "ingress" : "egress", conn->pn_raw_conn);
        break;
    default:
        break;
    }
}

qdr_http_connection_t *qdr_http_connection_ingress(qd_http_lsnr_t* listener)
{
    qdr_http_connection_t* ingress_http_conn = NEW(qdr_http_connection_t);
    ingress_http_conn->ingress = true;
    ingress_http_conn->context.context = ingress_http_conn;
    ingress_http_conn->context.handler = &handle_connection_event;
    ingress_http_conn->config = &(listener->config);
    ingress_http_conn->server = listener->server;
    ingress_http_conn->pn_raw_conn = pn_raw_connection();

    pn_raw_connection_set_context(ingress_http_conn->pn_raw_conn, ingress_http_conn);

    qdr_connection_info_t *info = qdr_connection_info(false, //bool             is_encrypted,
                                                      false, //bool             is_authenticated,
                                                      true,  //bool             opened,
                                                      "",   //char            *sasl_mechanisms,
                                                      QD_INCOMING, //qd_direction_t   dir,
                                                      listener->config.host_port,    //const char      *host,
                                                      "",    //const char      *ssl_proto,
                                                      "",    //const char      *ssl_cipher,
                                                      "",    //const char      *user,
                                                      "TcpAdaptor",    //const char      *container,
                                                      pn_data(0),     //pn_data_t       *connection_properties,
                                                      0,     //int              ssl_ssf,
                                                      false, //bool             ssl,
                                                      // set if remote is a qdrouter
                                                      0);    //const qdr_router_version_t *version)

    qdr_connection_t *conn = qdr_connection_opened(http_adaptor->core,
                                                   http_adaptor->adaptor,
                                                   true,
                                                   QDR_ROLE_NORMAL,
                                                   1,
                                                   qd_server_allocate_connection_id(ingress_http_conn->server),
                                                   0,
                                                   0,
                                                   false,
                                                   false,
                                                   false,
                                                   false,
                                                   250,
                                                   0,
                                                   info,
                                                   0,
                                                   0);

    ingress_http_conn->qdr_conn = conn;
    qdr_connection_set_context(conn, ingress_http_conn);

    qdr_terminus_t *dynamic_source = qdr_terminus(0);
    qdr_terminus_set_dynamic(dynamic_source);
    qdr_terminus_t *target = qdr_terminus(0);
    qdr_terminus_set_address(target, ingress_http_conn->config->address);

    ingress_http_conn->out_link = qdr_link_first_attach(conn,
                                         QD_OUTGOING,
                                         dynamic_source,   //qdr_terminus_t   *source,
                                         qdr_terminus(0),  //qdr_terminus_t   *target,
                                         "tcp.ingress.out",        //const char       *name,
                                         0,                //const char       *terminus_addr,
                                         &(ingress_http_conn->outgoing_id));
    qdr_link_set_context(ingress_http_conn->out_link, ingress_http_conn);

    ingress_http_conn->in_link = qdr_link_first_attach(conn,
                                         QD_INCOMING,
                                         qdr_terminus(0),  //qdr_terminus_t   *source,
                                         target,           //qdr_terminus_t   *target,
                                         "tcp.ingress.in",         //const char       *name,
                                         0,                //const char       *terminus_addr,
                                         &(ingress_http_conn->incoming_id));
    qdr_link_set_context(ingress_http_conn->in_link, ingress_http_conn);

    int i = READ_BUFFERS;
    for (; i; --i) {
        pn_raw_buffer_t *buff = &ingress_http_conn->read_buffers[READ_BUFFERS-i];
        buff->bytes = (char*) malloc(1024);
        buff->capacity = 1024;
        buff->size = 0;
        buff->offset = 0;
    }

    return ingress_http_conn;


}

static void handle_listener_event(pn_event_t *e, qd_server_t *qd_server, void *context) {
    qd_log_source_t *log = http_adaptor->log_source;

    qd_http_lsnr_t *li = (qd_http_lsnr_t*) context;
    const char *host_port = li->config.host_port;

    switch (pn_event_type(e)) {
        case PN_LISTENER_OPEN: {
            qd_log(log, QD_LOG_NOTICE, "Listening on ******************** %s", host_port);
        }
        break;

        case PN_LISTENER_ACCEPT: {
            qd_log(log, QD_LOG_INFO, "Accepting HTTP connection on %s", host_port);
            qdr_http_connection_t *http_conn = qdr_http_connection_ingress(li);
            if (http_conn) {
                pn_listener_raw_accept(pn_event_listener(e), http_conn->pn_raw_conn);
            }
        }
        break;

        case PN_LISTENER_CLOSE:
            break;

        default:
            break;
    }
}


static qd_http_lsnr_t *qd_http_lsnr(qd_server_t *server)
{
    qd_http_lsnr_t *li = new_qd_http_lsnr_t();
    if (!li)
        return 0;
    ZERO(li);
    sys_atomic_init(&li->ref_count, 1);
    li->server = server;
    li->context.context = li;
    li->context.handler = &handle_listener_event;
    return li;
}


#define CHECK() if (qd_error_code()) goto error


static const int BACKLOG = 50;  /* Listening backlog */

static bool http_listener_listen(qd_http_lsnr_t *li) {
   li->pn_listener = pn_listener();
    if (li->pn_listener) {
        pn_listener_set_context(li->pn_listener, &li->context);
        pn_proactor_listen(qd_server_proactor(li->server), li->pn_listener, li->config.host_port, BACKLOG);
        sys_atomic_inc(&li->ref_count); /* In use by proactor, PN_LISTENER_CLOSE will dec */
        /* Listen is asynchronous, log "listening" message on PN_LISTENER_OPEN event */
    } else {
        qd_log(http_adaptor->log_source, QD_LOG_CRITICAL, "Failed to create listener for %s",
               li->config.host_port);
     }
    return li->pn_listener;
}

static qd_error_t load_bridge_config(qd_dispatch_t *qd, qd_bridge_config_t *config, qd_entity_t* entity)
{
    qd_error_clear();
    ZERO(config);

    config->name                 = qd_entity_get_string(entity, "name");              CHECK();
    config->host                 = qd_entity_get_string(entity, "host");              CHECK();
    config->port                 = qd_entity_get_string(entity, "port");              CHECK();

    int hplen = strlen(config->host) + strlen(config->port) + 2;
    config->host_port = malloc(hplen);
    snprintf(config->host_port, hplen, "%s:%s", config->host, config->port);

    return QD_ERROR_NONE;

 error:
    free_bridge_config(config);
    return qd_error_code();
}


qd_http_lsnr_t *qd_dispatch_configure_http_lsnr(qd_dispatch_t *qd, qd_entity_t *entity)
{
    qd_http_lsnr_t *li = qd_http_lsnr(qd->server);
    if (!li || load_bridge_config(qd, &li->config, entity) != QD_ERROR_NONE) {
        qd_log(http_adaptor->log_source, QD_LOG_ERROR, "Unable to create http listener: %s", qd_error_message());
        qd_http_listener_decref(li);
        return 0;
    }
    //DEQ_ITEM_INIT(li);
    DEQ_INSERT_TAIL(http_adaptor->listeners, li);
    qd_log(http_adaptor->log_source, QD_LOG_INFO, "Configured HTTP adaptor listener on %s", (&li->config)->host_port);
    http_listener_listen(li);
    return li;
}

void qd_http_connector_decref(qd_http_connector_t* c)
{
    if (c && sys_atomic_dec(&c->ref_count) == 1) {
        free_bridge_config(&c->config);
        free_qd_http_connector_t(c);
    }
}


static qd_http_connector_t *qd_http_connector(qd_server_t *server)
{
    qd_http_connector_t *c = new_qd_http_connector_t();
    if (!c) return 0;
    ZERO(c);
    sys_atomic_init(&c->ref_count, 1);
    c->server      = server;
    return c;
}


qd_http_connector_t *qd_dispatch_configure_http_connector(qd_dispatch_t *qd, qd_entity_t *entity)
{
    qd_http_connector_t *c = qd_http_connector(qd->server);
    if (!c || load_bridge_config(qd, &c->config, entity) != QD_ERROR_NONE) {
        qd_log(http_adaptor->log_source, QD_LOG_ERROR, "Unable to create tcp connector: %s", qd_error_message());
        qd_http_connector_decref(c);
        return 0;
    }
    DEQ_ITEM_INIT(c);
    DEQ_INSERT_TAIL(http_adaptor->connectors, c);
    //log_tcp_bridge_config(http_adaptor->log_source, &c->config, "TcpConnector");
    //TODO: probably want a pool of egress connections, ready to handle incoming 'connection' streamed messages
    //qdr_tcp_connection_egress(c);
    return c;

}

static void qdr_http_adaptor_final(void *adaptor_context)
{
    qdr_http_adaptor_t *adaptor = (qdr_http_adaptor_t*) adaptor_context;
    qdr_protocol_adaptor_free(adaptor->core, adaptor->adaptor);
    free(adaptor);
    http_adaptor =  NULL;
}

qd_error_t qd_entity_refresh_httpListener(qd_entity_t* entity, void *impl)
{
    return QD_ERROR_NONE;
}


qd_error_t qd_entity_refresh_httpConnector(qd_entity_t* entity, void *impl)
{
    return QD_ERROR_NONE;
}

/**
 * Declare the adaptor so that it will self-register on process startup.
 */
QDR_CORE_ADAPTOR_DECLARE("http-adaptor", qdr_http_adaptor_init, qdr_http_adaptor_final)
