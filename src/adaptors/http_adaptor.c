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
ALLOC_DEFINE(qd_http2_session_data_t);
ALLOC_DEFINE(qd_http2_stream_data_t);

typedef struct qdr_http_adaptor_t {
    qdr_core_t              *core;
    qdr_protocol_adaptor_t  *adaptor;
    qd_http_lsnr_list_t      listeners;
    qd_http_connector_list_t connectors;
    qd_log_source_t         *log_source;
} qdr_http_adaptor_t;


static qdr_http_adaptor_t *http_adaptor;



/**
 * HTTP :path is mapped to the AMQP 'to' field.
 */
void qd_message_compose_10(qd_message_t *msg, const char *to, const char *reply_to)
{
    qd_composed_field_t  *field   = qd_compose(QD_PERFORMATIVE_HEADER, 0);
    qd_message_content_t *content = MSG_CONTENT(msg);
    if (!content)
        return;

    qd_compose_start_list(field);
    qd_compose_insert_bool(field, 0);     // durable
    qd_compose_insert_null(field);        // priority
    //qd_compose_insert_null(field);        // ttl
    //qd_compose_insert_boolean(field, 0);  // first-acquirer
    //qd_compose_insert_uint(field, 0);     // delivery-count
    qd_compose_end_list(field);

    qd_buffer_list_t out_ma;
    qd_buffer_list_t out_ma_trailer;
    DEQ_INIT(out_ma);
    DEQ_INIT(out_ma_trailer);
    //compose_message_annotations((qd_message_pvt_t*)msg, &out_ma, &out_ma_trailer, false);
    qd_compose_insert_buffers(field, &out_ma);
    // TODO: user annotation blob goes here
    qd_compose_insert_buffers(field, &out_ma_trailer);

    field = qd_compose(QD_PERFORMATIVE_PROPERTIES, field);
    qd_compose_start_list(field);
    qd_compose_insert_null(field);          // message-id
    qd_compose_insert_null(field);          // user-id
    qd_compose_insert_string(field, to);    // to
    qd_compose_insert_null(field);          // subject
    qd_compose_insert_string(field, reply_to); // reply-to
    //qd_compose_insert_null(field);          // correlation-id
    //qd_compose_insert_null(field);          // content-type
    //qd_compose_insert_null(field);          // content-encoding
    //qd_compose_insert_timestamp(field, 0);  // absolute-expiry-time
    //qd_compose_insert_timestamp(field, 0);  // creation-time
    //qd_compose_insert_null(field);          // group-id
    //qd_compose_insert_uint(field, 0);       // group-sequence
    //qd_compose_insert_null(field);          // reply-to-group-id
    qd_compose_end_list(field);



}


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


static void qdr_http_connection_copy_reply_to(qdr_http_connection_t* http_conn, qd_iterator_t* reply_to)
{
    int length = qd_iterator_length(reply_to);
    http_conn->reply_to = malloc(length + 1);
    qd_iterator_strncpy(reply_to, http_conn->reply_to, length + 1);
    qd_log(http_adaptor->log_source, QD_LOG_INFO, "reply-to for %s is %s", http_conn->in_dlv ? "ingress" : "egress", http_conn->reply_to);
}


static void qdr_http_second_attach(void *context, qdr_link_t *link,
                                  qdr_terminus_t *source, qdr_terminus_t *target)
{
    qd_log(http_adaptor->log_source, QD_LOG_INFO, "qdr_http_second_attach for %s", qdr_link_name(link));
    void* link_context = qdr_link_get_context(link);
    if (link_context && qdr_link_direction(link) == QD_OUTGOING) {
        qdr_http_connection_t* http_conn = (qdr_http_connection_t*) link_context;
        if (http_conn->ingress) {
            qdr_http_connection_copy_reply_to(http_conn, qdr_terminus_get_address(source));
        }
        pn_raw_connection_give_read_buffers(http_conn->pn_raw_conn, http_conn->read_buffers, READ_BUFFERS);
        qdr_link_flow(http_adaptor->core, link, 1, false);
    }
}

static int qdr_http_push(void *context, qdr_link_t *link, int limit)
{
    return 1;
}

static uint64_t qdr_http_deliver(void *context, qdr_link_t *link, qdr_delivery_t *delivery, bool settled)
{
    return 0;
}

void qd_http_connector_decref(qd_http_connector_t* c)
{
    if (c && sys_atomic_dec(&c->ref_count) == 1) {
        free_bridge_config(&c->config);
        free_qd_http_connector_t(c);
    }
}


void qd_dispatch_delete_http_connector(qd_dispatch_t *qd, void *impl)
{
    qd_http_connector_t *connector = (qd_http_connector_t*) impl;
    if (connector) {
        //TODO: cleanup and close any associated active connections
        DEQ_REMOVE(http_adaptor->connectors, connector);
        qd_http_connector_decref(connector);
    }
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


static qd_http2_stream_data_t *create_http2_stream_data(qd_http2_session_data_t *session_data, int32_t stream_id)
{
  qd_http2_stream_data_t *stream_data = new_qd_http2_stream_data_t();
  ZERO(stream_data);
  stream_data->stream_id = stream_id;

  DEQ_INSERT_TAIL(session_data->streams, stream_data);

  return stream_data;
}

static int on_begin_headers_callback(nghttp2_session *session,
                                     const nghttp2_frame *frame,
                                     void *user_data)
{
    qd_http2_session_data_t *session_data = (qd_http2_session_data_t *)user_data;
    qd_http2_stream_data_t *stream_data;

    if (frame->hd.type == NGHTTP2_HEADERS &&
            session_data->stream_data->stream_id == frame->hd.stream_id) {
        if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE || frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
            int32_t stream_id = frame->hd.stream_id;
            stream_data = create_http2_stream_data(session_data, stream_id);
            nghttp2_session_set_stream_user_data(session, stream_id, stream_data);
        }

    }

    return 0;
}


/**
 *  nghttp2_on_header_callback: Called when nghttp2 library emits
 *  single header name/value pair.
 */
static int on_header_callback(nghttp2_session *session,
                              const nghttp2_frame *frame,
                              const uint8_t *name,
                              size_t namelen,
                              const uint8_t *value,
                              size_t valuelen,
                              uint8_t flags,
                              void *user_data)
{
    char* header_name = (char*)name;
    printf ("header_name is %s\n", header_name);

    qd_http2_stream_data_t *stream_data;
    qd_http2_session_data_t *session_data = (qd_http2_session_data_t *) user_data;

    switch (frame->hd.type) {
        case NGHTTP2_HEADERS: {
            if (frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
                //The HEADERS frame is NOT opening new stream
                break;
            }
            stream_data = nghttp2_session_get_stream_user_data(session_data->session, frame->hd.stream_id);
            if (!stream_data || stream_data->request_path) {
                break;
            }
            const char PATH[] = ":path";
            const char METHOD[] = ":method";
            if (namelen == sizeof(PATH) - 1 && memcmp(PATH, name, namelen) == 0) {
                // We have the path
                //qd_message_compose_stream(session_data->message, (char *)value, session_data->conn->reply_to);
                session_data->path = malloc(valuelen + 1);
                strncpy(session_data->path, (char *)value, valuelen + 1);
            }
            if (namelen == sizeof(METHOD) - 1 && memcmp(METHOD, name, namelen) == 0) {
                session_data->method = malloc(valuelen + 1);
                strncpy(session_data->method, (char *)value,  valuelen + 1);
            }
        }
        break;

        default:
            break;
    }
    return 0;
}


static void handle_incoming_http(qdr_http_connection_t *conn, pn_raw_buffer_t rawbuf)
{
    printf ("handle_incoming_http \n");
    qd_buffer_list_t   buffers;
    qd_buffer_t       *buf;
    DEQ_INIT(buffers);
    buf = qd_buffer();
    char *insert = (char*) qd_buffer_cursor(buf);
    strncpy(insert, rawbuf.bytes, rawbuf.size);
    qd_buffer_insert(buf, rawbuf.size);
    DEQ_INSERT_HEAD(buffers, buf);

    if (conn->in_dlv) {

    }
    else {

    }

    ssize_t readlen = nghttp2_session_mem_recv(conn->session_data->session, (uint8_t *)qd_buffer_base(buf), qd_buffer_size(buf));

    if (readlen == 0) {

    }

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
    ingress_http_conn->session_data = new_qd_http2_session_data_t();
    ZERO(ingress_http_conn->session_data);
    DEQ_INIT(ingress_http_conn->session_data->streams);
    ingress_http_conn->session_data->conn = ingress_http_conn;
    ingress_http_conn->session_data->message = qd_message();
    ingress_http_conn->session_data->is_request = true;

    nghttp2_session_callbacks *callbacks = 0;
    nghttp2_session_callbacks_new(&callbacks);
    //nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback);
    nghttp2_session_server_new(&(ingress_http_conn->session_data->session), callbacks, ingress_http_conn->session_data);
    ingress_http_conn->session_data->message = qd_message();

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
                                                      "HttpAdaptor",    //const char      *container,
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
            qd_log(log, QD_LOG_INFO, "Accepting *****HTTP****** connection on %s", host_port);
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


static qd_http_connector_t *qd_http_connector(qd_server_t *server)
{
    qd_http_connector_t *c = new_qd_http_connector_t();
    if (!c) return 0;
    ZERO(c);
    sys_atomic_init(&c->ref_count, 1);
    c->server      = server;
    return c;
}

static void on_activate(void *context)
{
    qdr_http_connection_t* conn = (qdr_http_connection_t*) context;

    while (qdr_connection_process(conn->qdr_conn)) {}
}


static int on_stream_close_callback(nghttp2_session *session,
                                    int32_t stream_id,
                                    nghttp2_error_code error_code,
                                    void *user_data)
{
    return 0;
}

qdr_http_connection_t *qdr_http_connection_egress(qd_http_connector_t *connector)
{
    qdr_http_connection_t* egress_conn = NEW(qdr_http_connection_t);
    ZERO(egress_conn);
    //FIXME: this is only needed while waiting for raw_connection_wake
    //functionality in proton
    egress_conn->activate_timer = qd_timer(http_adaptor->core->qd, on_activate, egress_conn);

    egress_conn->ingress = false;
    egress_conn->context.context = egress_conn;
    egress_conn->context.handler = &handle_connection_event;
    egress_conn->config = &(connector->config);
    egress_conn->server = connector->server;
    egress_conn->pn_raw_conn = pn_raw_connection();

    nghttp2_session_callbacks *callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);


    pn_raw_connection_set_context(egress_conn->pn_raw_conn, egress_conn);
    qdr_connection_info_t *info = qdr_connection_info(false, //bool             is_encrypted,
                                                      false, //bool             is_authenticated,
                                                      true,  //bool             opened,
                                                      "",   //char            *sasl_mechanisms,
                                                      QD_OUTGOING, //qd_direction_t   dir,
                                                      egress_conn->config->host_port,    //const char      *host,
                                                      "",    //const char      *ssl_proto,
                                                      "",    //const char      *ssl_cipher,
                                                      "",    //const char      *user,
                                                      "httpAdaptor",    //const char      *container,
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
                                                   qd_server_allocate_connection_id(egress_conn->server),
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
    egress_conn->qdr_conn = conn;
    qdr_connection_set_context(conn, egress_conn);

    qdr_terminus_t *source = qdr_terminus(0);
    qdr_terminus_set_address(source, egress_conn->config->address);

    egress_conn->out_link = qdr_link_first_attach(conn,
                          QD_OUTGOING,
                          source,           //qdr_terminus_t   *source,
                          qdr_terminus(0),  //qdr_terminus_t   *target,
                          "http.egress.out", //const char       *name,
                          0,                //const char       *terminus_addr,
                          &(egress_conn->outgoing_id));
    qdr_link_set_context(egress_conn->out_link, egress_conn);
    //the incoming link for egress is created once we receive the
    //message which has the reply to address

    int i = READ_BUFFERS;
    for (; i; --i) {
        pn_raw_buffer_t *buff = &egress_conn->read_buffers[READ_BUFFERS-i];
        buff->bytes = (char*) malloc(1024);
        buff->capacity = 1024;
        buff->size = 0;
        buff->offset = 0;
    }

    return egress_conn;
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
    qdr_http_connection_egress(c);
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
