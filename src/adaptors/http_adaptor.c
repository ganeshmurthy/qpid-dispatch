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
#include <proton/netaddr.h>
#include <proton/raw_connection.h>
#include <nghttp2/nghttp2.h>

#include <qpid/dispatch/buffer.h>

#include "qpid/dispatch/protocol_adaptor.h"
#include "delivery.h"
#include "http_adaptor.h"

const char *PATH = ":path";
const char *METHOD = ":method";
const char *STATUS = ":status";
const char *CONTENT_TYPE = "content-type";
const char *CONTENT_ENCODING = "content-encoding";

#define READ_BUFFERS 4
#define WRITE_BUFFERS 4
#define DEBUGBUILD 1
#define ARRLEN(x) (sizeof(x) / sizeof(x[0]))

#define MAKE_NV2(NAME, VALUE)                                                  \
{                                                                              \
    (uint8_t *)NAME, (uint8_t *)VALUE, sizeof(NAME) - 1, sizeof(VALUE) - 1,    \
        NGHTTP2_NV_FLAG_NONE                                                   \
}

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

static void handle_connection_event(pn_event_t *e, qd_server_t *qd_server, void *context);

/**
 * HTTP :path is mapped to the AMQP 'to' field.
 */
qd_composed_field_t  *qd_message_compose_amqp(qd_message_t *msg,
                                              const char *to,
                                              const char *subject,
                                              const char *reply_to,
                                              const char *content_type,
                                              const char *content_encoding)
{
    qd_composed_field_t  *field   = qd_compose(QD_PERFORMATIVE_HEADER, 0);
    qd_message_content_t *content = MSG_CONTENT(msg);
    if (!content)
        return 0;
    //
    // Header
    //
    qd_compose_start_list(field);
    qd_compose_insert_bool(field, 0);     // durable
    qd_compose_insert_null(field);        // priority
    //qd_compose_insert_null(field);        // ttl
    //qd_compose_insert_bool(field, 0);     // first-acquirer
    //qd_compose_insert_uint(field, 0);     // delivery-count
    qd_compose_end_list(field);

    //
    // Properties
    //
    field = qd_compose(QD_PERFORMATIVE_PROPERTIES, field);
    qd_compose_start_list(field);
    qd_compose_insert_null(field);          // message-id
    qd_compose_insert_null(field);          // user-id
    if (to) {
        qd_compose_insert_string(field, to);    // to
    }
    else {
        qd_compose_insert_null(field);
    }

    if (subject) {
        qd_compose_insert_string(field, subject);      // subject
    }
    else {
        qd_compose_insert_null(field);
    }

    if (reply_to) {
        qd_compose_insert_string(field, reply_to); // reply-to
    }
    else {
        qd_compose_insert_null(field);
    }

    //qd_compose_insert_null(field);          // correlation-id
    if (content_type) {
        qd_compose_insert_string(field, content_type);        // content-type
    }
    else {
        qd_compose_insert_null(field);
    }
    if (content_encoding) {
        qd_compose_insert_string(field, content_encoding);               // content-encoding
    }
    else {
        qd_compose_insert_null(field);
    }
    qd_compose_end_list(field);

    //
    // Application properties and Body will be added later
    //
    return field;
}

static char *get_address_string(pn_raw_connection_t *pn_raw_conn)
{
    const pn_netaddr_t *netaddr = pn_raw_connection_remote_addr(pn_raw_conn);
    char buffer[1024];
    int len = pn_netaddr_str(netaddr, buffer, 1024);
    if (len <= 1024) {
        return strdup(buffer);
    } else {
        return strndup(buffer, 1024);
    }
}

void free_qdr_http_connection(qdr_http_connection_t* http_conn)
{
    if (http_conn->reply_to) {
        free(http_conn->reply_to);
    }
    if(http_conn->remote_address) {
        free(http_conn->remote_address);
    }
    if (http_conn->activate_timer) {
        //qd_timer_free(http_conn->activate_timer);
    }
    free(http_conn);
}

static qd_http2_stream_data_t *create_http2_stream_data(qd_http2_session_data_t *session_data, int32_t stream_id)
{
  qd_http2_stream_data_t *stream_data = new_qd_http2_stream_data_t();
  ZERO(stream_data);
  stream_data->stream_id = stream_id;

  DEQ_INSERT_TAIL(session_data->streams, stream_data);

  return stream_data;
}


static int on_data_chunk_recv_callback(nghttp2_session *session,
                                                   uint8_t flags,
                                                   int32_t stream_id,
                                                   const uint8_t *data,
                                                   size_t len, void *user_data)
{
    printf ("on_data_chunk_recv_callback ************ %i\n", (int)len);

    qd_buffer_list_t buffers;
    DEQ_INIT(buffers);
    qd_buffer_t *buff = qd_buffer();
    memcpy(qd_buffer_cursor(buff), data, len);
    qd_buffer_insert(buff, len);
    DEQ_INSERT_TAIL (buffers, buff);


    qd_http2_session_data_t *session_data = (qd_http2_session_data_t *)user_data;

    qd_message_extend(session_data->message, &buffers);

    return 0;
}

/* nghttp2_send_callback. Here we transmit the |data|, |length| bytes,
   to the network. */
static ssize_t send_callback(nghttp2_session *session, const uint8_t *data,
                             size_t length, int flags, void *user_data) {
    qd_http2_session_data_t *session_data = (qd_http2_session_data_t *)user_data;

    printf ("send_callback length**************** %i\n", (int)length);

//    char *local_data = malloc(length);
//    memcpy(local_data, data, length);
//
//    pn_raw_buffer_t buffers[1];
//    buffers[0].bytes = (char *)local_data;
//    buffers[0].capacity = length;
//    buffers[0].size = length;
//    buffers[0].offset = 0;

    qd_buffer_t *buff = qd_buffer();
    memcpy(qd_buffer_cursor(buff), data, length);
    qd_buffer_insert(buff, length);
    DEQ_INSERT_TAIL (session_data->buff_list, buff);

    //pn_raw_connection_write_buffers(session_data->conn->pn_raw_conn, buffers, 1);

    //
    // We will copy this data into our buffers now and will send it out
    // when we get a chance to send it.
    //

//    qd_buffer_t *buff = DEQ_TAIL(session_data->out_buffs);
//    if (!buff) {
//        buff = qd_buffer();
//        DEQ_INSERT_TAIL(session_data->out_buffs, buff);
//        session_data->cursor.cursor = qd_buffer_base(buff);
//        session_data->cursor.remaining = qd_buffer_capacity(buff);
//        session_data->cursor.buffer = buff;
//    }
//
//    while (length_remaining > 0) {
//        if (length_remaining > qd_buffer_capacity(buff)) {
//            printf ("if in send_callback *******\n");
//            memcpy(session_data->cursor.cursor, local_data, qd_buffer_capacity(buff));
//            local_data += qd_buffer_capacity(buff);
//            length_remaining = length_remaining - qd_buffer_capacity(buff);
//
//            buff = qd_buffer();
//            DEQ_INSERT_TAIL(session_data->out_buffs, buff);
//            session_data->cursor.cursor = qd_buffer_base(buff);
//            session_data->cursor.remaining = qd_buffer_capacity(buff);
//            session_data->cursor.buffer = buff;
//        }
//        else {
//            printf ("else in send_callback *******\n");
//            memcpy(local_data, session_data->cursor.cursor, length_remaining);
//            qd_buffer_insert(buff, length_remaining);
//            session_data->cursor.cursor = qd_buffer_base(buff) + length_remaining;
//            session_data->cursor.remaining = qd_buffer_capacity(buff);
//            local_data += length_remaining;
//            length_remaining = 0;
//            printf ("qd_buffer_size in send_callback *******%i\n", (int)qd_buffer_size(buff));
//        }
//    }

    //pn_raw_connection_wake(session_data->conn->pn_raw_conn);

    return (ssize_t)length;
}

/**
 * This callback function is invoked with the reception of header block in HEADERS or PUSH_PROMISE is started
 */
static int on_begin_headers_callback(nghttp2_session *session,
                                     const nghttp2_frame *frame,
                                     void *user_data)
{
    qd_http2_session_data_t *session_data = (qd_http2_session_data_t *)user_data;

    if (frame->hd.type == NGHTTP2_HEADERS) { //}&&
            //session_data->stream_data->stream_id == frame->hd.stream_id) {
        if (frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
            session_data->message = qd_message();
            int32_t stream_id = frame->hd.stream_id;
            session_data->stream_data = create_http2_stream_data(session_data, stream_id);
            nghttp2_session_set_stream_user_data(session, stream_id, session_data->stream_data);
        }
        else if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
            // How do you get the stream_data from the session data?
            session_data->message = qd_message();
            int32_t stream_id = frame->hd.stream_id;
            session_data->stream_data = create_http2_stream_data(session_data, stream_id);
            nghttp2_session_set_stream_user_data(session, stream_id, session_data->stream_data);
        }

    }

    return 0;
}

static qd_http2_stream_data_t *create_http2_client_stream_data(const char *uri, const char *host, const char *port)
{
    //size_t extra = 7;
    qd_http2_stream_data_t *stream_data = new_qd_http2_stream_data_t();
    stream_data->uri = uri;
    stream_data->stream_id = -1;
    //stream_data->authoritylen = u->field_data[UF_HOST].len;
    //stream_data->authority = malloc(stream_data->authoritylen + extra);

    stream_data->pathlen = 1;

    return stream_data;
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
    qd_http2_stream_data_t *stream_data;
    if (stream_data) {

    }

    qd_http2_session_data_t *session_data = (qd_http2_session_data_t *) user_data;

    switch (frame->hd.type) {
        case NGHTTP2_HEADERS: {
            stream_data = nghttp2_session_get_stream_user_data(session_data->session, frame->hd.stream_id);
            //if (!stream_data || stream_data->request_path) {
            //    break;
            //}
            if (!session_data->app_properties) {
                printf ("New properties\n");
                session_data->app_properties = qd_compose(QD_PERFORMATIVE_APPLICATION_PROPERTIES, 0);
                qd_compose_start_map(session_data->app_properties);
            }
            session_data->num_headers ++;
            printf ("header_name is ************** %s\n", (char *)name);
            printf ("on_header_callback session_data->app_properties is %p\n", (void *)session_data->app_properties);
            qd_compose_insert_string_n(session_data->app_properties, (const char *)name, namelen);
            qd_compose_insert_string_n(session_data->app_properties, (const char *)value, valuelen);
            printf ("header_value is ************** %s\n", (char *)value);
        }
        break;

        default:
            break;
    }
    return 0;
}


static int on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame, void *user_data)
{

    qd_http2_session_data_t *session_data = (qd_http2_session_data_t *)user_data;

    printf ("**********on_frame_recv_callback******************%p\n", (void *)session_data);

    switch (frame->hd.type) {
        case NGHTTP2_DATA: {
            if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                printf ("Receive complete is true NGHTTP2_DATA\n");
                MSG_CONTENT(session_data->message)->receive_complete = true;
            }
            qdr_delivery_continue(http_adaptor->core, session_data->conn->in_dlv, false);
            printf ("qdr_delivery_continue\n");
        }
        case NGHTTP2_HEADERS:{
            /* All the headers have been received. Send out the AMQP message */
            if (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) {
                printf ("******END HEADERS*************\n");
                // All header fields have been received
                // End the map.
                qd_compose_end_map(session_data->app_properties);

                qd_composed_field_t  *header_prop = qd_message_compose_amqp(session_data->message,
                                                                            session_data->path,
                                                                            session_data->method,
                                                                            session_data->conn->reply_to,
                                                                            session_data->content_type,
                                                                            session_data->content_encoding);

                bool receive_complete = false;
                session_data->app_properties = qd_compose(QD_PERFORMATIVE_BODY_DATA, session_data->app_properties);
                if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                    receive_complete = true;
                    qd_compose_insert_null(session_data->app_properties);
                }
                else {
                    receive_complete = false;
                }
                qd_message_compose_3(session_data->message, header_prop, session_data->app_properties);

                MSG_CONTENT(session_data->message)->receive_complete = receive_complete;

                printf ("session_data->num_headers is=%i\n", (int)session_data->num_headers);

                printf ("qdr_link_deliver MSG_CONTENT(session_data->message) %p\n", (void *)MSG_CONTENT(session_data->message));
                printf ("qdr_link_deliver session_data->app_properties %p\n", (void *)session_data->app_properties);
                session_data->conn->in_dlv = qdr_link_deliver(session_data->conn->in_link, session_data->message, 0, false, 0, 0);
                //session_data->message = 0;
            }
        }
    }

    return 0;
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
    //ingress_http_conn->session_data->message = qd_message();

    nghttp2_session_callbacks *callbacks = 0;
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_server_new(&(ingress_http_conn->session_data->session), callbacks, ingress_http_conn->session_data);
    //ingress_http_conn->session_data->stream_data = create_http2_client_stream_data(ingress_http_conn->config->host_port, ingress_http_conn->config->host, ingress_http_conn->config->port);

    pn_raw_connection_set_context(ingress_http_conn->pn_raw_conn, ingress_http_conn);
    pn_listener_raw_accept(listener->pn_listener, ingress_http_conn->pn_raw_conn);
    return ingress_http_conn;
}

static void grant_read_buffers(qdr_http_connection_t *conn)
{
    pn_raw_buffer_t raw_buffers[READ_BUFFERS];
    // Give proactor more read buffers for the pn_raw_conn
    if (!pn_raw_connection_is_read_closed(conn->pn_raw_conn)) {
        size_t desired = pn_raw_connection_read_buffers_capacity(conn->pn_raw_conn);
        while (desired) {
            size_t i;
            for (i = 0; i < desired && i < READ_BUFFERS; ++i) {
                qd_buffer_t *buf = qd_buffer();
                raw_buffers[i].bytes = (char*) qd_buffer_base(buf);
                raw_buffers[i].capacity = qd_buffer_capacity(buf);
                raw_buffers[i].size = 0;
                raw_buffers[i].offset = 0;
                raw_buffers[i].context = (uintptr_t) buf;
            }
            desired -= i;
            pn_raw_connection_give_read_buffers(conn->pn_raw_conn, raw_buffers, i);
        }
    }
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
    return 10;
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


static void qdr_http_first_attach(void *context, qdr_connection_t *conn, qdr_link_t *link,
                                 qdr_terminus_t *source, qdr_terminus_t *target,
                                 qd_session_class_t session_class)
{
}


static void qdr_http_connection_copy_reply_to(qdr_http_connection_t* http_conn, qd_iterator_t* reply_to)
{
    printf ("qdr_http_connection_copy_reply_to ingress=%i\n", http_conn->ingress);
    int length = qd_iterator_length(reply_to);
    http_conn->reply_to = malloc(length + 1);
    qd_iterator_strncpy(reply_to, http_conn->reply_to, length + 1);
    qd_log(http_adaptor->log_source, QD_LOG_INFO, "reply-to for %s is %s", http_conn->in_dlv ? "ingress" : "egress", http_conn->reply_to);
}


static void qdr_http_second_attach(void *context, qdr_link_t *link,
                                  qdr_terminus_t *source, qdr_terminus_t *target)
{
    void* link_context = qdr_link_get_context(link);
    if (link_context) {
        qdr_http_connection_t* tc = (qdr_http_connection_t*) link_context;
        if (qdr_link_direction(link) == QD_OUTGOING) {
            if (tc->ingress) {
                qdr_http_connection_copy_reply_to(tc, qdr_terminus_get_address(source));
                // for ingress, can start reading from socket once we have
                // a reply to address, as that is when we are able to send
                // out a message
                grant_read_buffers(tc);
            }
            qdr_link_flow(http_adaptor->core, link, 10, false);
        } else if (!tc->ingress) {
            //for egress we can start reading from the socket once we
            //have the link to send messages over
            grant_read_buffers(tc);
        }
    }
}

static void qdr_http_activate(void *notused, qdr_connection_t *c)
{
    printf("qdr_http_activate 1\n");

    void *context = qdr_connection_get_context(c);
    if (context) {
        qdr_http_connection_t* conn = (qdr_http_connection_t*) context;
        if (conn->pn_raw_conn) {
            printf("qdr_http_activate 2 %p\n", (void *)conn);
            qd_log(http_adaptor->log_source, QD_LOG_INFO, "[C%i] Activation triggered, calling pn_raw_connection_wake()", conn->conn_id);
            pn_raw_connection_wake(conn->pn_raw_conn);
        } else if (conn->activate_timer) {
            printf("qdr_http_activate 3\n");
            // On egress, the raw connection is only created once the
            // first part of the message encapsulating the
            // client->server half of the stream has been
            // received. Prior to that however a subscribing link (and
            // its associated connection must be setup), for which we
            // fake wakeup by using a timer.
            qd_log(http_adaptor->log_source, QD_LOG_INFO, "[C%i] Activation triggered, no socket yet so scheduling timer", conn->conn_id);
            qd_timer_schedule(conn->activate_timer, 0);
        } else {
            printf("qdr_http_activate 4\n");
            qd_log(http_adaptor->log_source, QD_LOG_ERROR, "[C%i] Cannot activate", conn->conn_id);
        }
    }
}

static int qdr_http_push(void *context, qdr_link_t *link, int limit)
{
    printf ("in qdr_http_push calling qdr_link_process_deliveries\n");
    return qdr_link_process_deliveries(http_adaptor->core, link, limit);
}


static void http_connector_establish(qdr_http_connection_t *conn)
{
    qd_log(http_adaptor->log_source, QD_LOG_INFO, "[C%i] Connecting to: %s", conn->conn_id, conn->config->host_port);
    conn->pn_raw_conn = pn_raw_connection();
    pn_raw_connection_set_context(conn->pn_raw_conn, conn);
    printf ("conn->pn_raw_conn is %p\n", (void *)conn->pn_raw_conn);
    printf ("conn->config->host_port %s\n", conn->config->host_port);
    pn_proactor_raw_connect(qd_server_proactor(conn->server), conn->pn_raw_conn, conn->config->host_port);
}

ssize_t read_callback(nghttp2_session *session,
                                  int32_t stream_id, uint8_t *buf,
                                  size_t length, uint32_t *data_flags,
                                  nghttp2_data_source *source,
                                  void *user_data)
{
    qd_buffer_t *qd_buff = source->ptr;
    printf ("read_callback qd_buff is %p\n", (void *)qd_buff);
    printf ("read_callback length is %i\n", (int)length);
    buf = qd_buffer_base(qd_buff);
    size_t ret_val = qd_buffer_size(qd_buff);
    printf ("read_callback ret_val is %i\n", (int)ret_val);
    return ret_val;
}

void handle_outgoing_http(qdr_http_connection_t *conn)
{
    printf ("handle_outgoing_http start\n");
    qd_http2_session_data_t *session_data = conn->session_data;
    qd_message_t *message = conn->session_data->message;
    qd_http2_stream_data_t *stream_data = session_data->stream_data;
    if (conn->out_dlv) {
        qd_http2_session_data_t *session_data = conn->session_data;

        if (!session_data->header_sent) {
            session_data->header_sent = true;
            // Compose the headers. We are not sending anything yet.
            nghttp2_settings_entry iv[1] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 1}};
            int rv;

            /* client 24 bytes (3.5.  HTTP/2 Connection Preface) will be sent by nghttp2 library */
            rv = nghttp2_submit_settings(session_data->session, NGHTTP2_FLAG_NONE, iv, ARRLEN(iv));

            printf ("nghttp2_submit_settings rv=%i\n", (int)rv);

            if (rv) {}

            qd_message_depth_status_t  depth_valid = qd_message_check_depth(message, QD_DEPTH_HEADER);
            printf ("qd_message_depth_status_t is %i\n", (int)depth_valid);

            // The HTTP Path is in the AMQP to field.
            qd_iterator_t *to = qd_message_field_iterator(message, QD_FIELD_TO);
            char *path = (char *)qd_iterator_copy(to);

            qd_iterator_t *subject = qd_message_field_iterator(message, QD_FIELD_SUBJECT);
            char *http_method = (char *)qd_iterator_copy(subject);

            qd_iterator_t *ct = qd_message_field_iterator(message, QD_FIELD_CONTENT_TYPE);
            char *content_type = (char *)qd_iterator_copy(ct);

            printf ("compose_http method = %s\n", http_method);
            printf ("compose_http path = %s\n", path);
            printf ("compose_http accept = %s\n", content_type);
            fflush(stdout);

            //const char *uri = stream_data->uri;

            qd_iterator_t *app_properties_iter = qd_message_field_iterator(session_data->message, QD_FIELD_APPLICATION_PROPERTIES);
            qd_parsed_field_t *app_properties_fld = qd_parse(app_properties_iter);
            if (qd_parse_is_map(app_properties_fld)) {
                printf ("app_properties_fld is a map ******%p\n", (void *)session_data->message);
            }

            uint32_t count = qd_parse_sub_count(app_properties_fld);

            nghttp2_nv hdrs_1[count];

            printf("app_properties_fld count=%i\n", (int)count);

            for (uint32_t idx = 0; idx < count; idx++) {
                qd_parsed_field_t *key = qd_parse_sub_key(app_properties_fld, idx);
                qd_parsed_field_t *val = qd_parse_sub_value(app_properties_fld, idx);
                qd_iterator_t *key_raw = qd_parse_raw(key);
                qd_iterator_t *val_raw = qd_parse_raw(val);

                hdrs_1[idx].name = (uint8_t *)qd_iterator_copy(key_raw);
                printf ("hdrs_1[idx].name=%s\n", (char *)hdrs_1[idx].name);

                hdrs_1[idx].value = (uint8_t *)qd_iterator_copy(val_raw);

                printf ("hdrs_1[idx].value=%s\n", (char *)hdrs_1[idx].value);
                hdrs_1[idx].namelen = qd_iterator_length(key_raw);

                printf ("hdrs_1[idx].namelen=%i\n", (int)hdrs_1[idx].namelen);

                hdrs_1[idx].valuelen = qd_iterator_length(val_raw);
                hdrs_1[idx].flags = NGHTTP2_NV_FLAG_NONE;
            }

            printf ("nghttp2_submit_headers stream_data->stream_id is %i\n", (int) stream_data->stream_id);

            // This does not really submit the request. We need to read the bytes
            stream_data->stream_id = nghttp2_submit_headers(session_data->session, 0,
                                                   stream_data->stream_id , NULL, hdrs_1,
                                                   count, stream_data);
            printf ("nghttp2_session_send before\n");
            nghttp2_session_send(session_data->session);
            printf ("nghttp2_session_send after\n");

            int num_buffs = DEQ_SIZE(session_data->buff_list);
            pn_raw_buffer_t raw_buffers[num_buffs];
            qd_buffer_t *qd_buff = DEQ_HEAD(session_data->buff_list);
            int i = 0;
            while (qd_buff) {
                raw_buffers[i].bytes = (char *)qd_buffer_base(qd_buff);
                raw_buffers[i].capacity = qd_buffer_size(qd_buff);
                raw_buffers[i].size = qd_buffer_size(qd_buff);
                raw_buffers[i].offset = 0;
                qd_buff = DEQ_NEXT(qd_buff);
                i ++;
            }

            pn_raw_connection_write_buffers(session_data->conn->pn_raw_conn, raw_buffers, num_buffs);
        }



        qd_buffer_t *head_buff = DEQ_HEAD(MSG_CONTENT(message)->buffers);
        printf ("head_buff outside is %p\n", (void *)head_buff);
        while (head_buff) {
            printf ("head_buff is %p\n", (void *)head_buff);
            printf ("head_buff size is %i\n", (int)qd_buffer_size(head_buff));
            nghttp2_data_provider data_prd;
            data_prd.source.ptr = head_buff;
            data_prd.read_callback = read_callback;
            if (DEQ_NEXT(head_buff) == 0 && MSG_CONTENT(session_data->message)->receive_complete) {
                printf ("NGHTTP2_FLAG_END_STREAM\n");
                nghttp2_submit_data(session_data->session, NGHTTP2_FLAG_END_STREAM, stream_data->stream_id, &data_prd);
            }
            else {
                printf ("not NGHTTP2_FLAG_END_STREAM\n");
                nghttp2_submit_data(session_data->session, 0, stream_data->stream_id, &data_prd);
            }
            DEQ_REMOVE_HEAD(MSG_CONTENT(message)->buffers);
            head_buff = DEQ_HEAD(MSG_CONTENT(message)->buffers);

            nghttp2_session_send(session_data->session);
        }

        printf ("handle_outgoing_http end\n");



        //pn_raw_buffer_t buffs[WRITE_BUFFERS];
        //populate the raw buffers from message buffers but only want the body
        //need to track where we got to
//        size_t n = qd_message_get_body_data(msg, buffs, WRITE_BUFFERS);
//        size_t used = pn_raw_connection_write_buffers(conn->pn_raw_conn, buffs, n);
//        int bytes_written = 0;
//        for (size_t i = 0; i < used; i++) {
//            if (buffs[i].bytes) {
//                bytes_written += buffs[i].size;
//            } else {
//                qd_log(http_adaptor->log_source, QD_LOG_ERROR, "[C%i] empty buffer can't be written", conn->conn_id);
//            }
//        }
//        qd_log(http_adaptor->log_source, QD_LOG_INFO, "[C%i] Writing %i bytes", conn->conn_id, bytes_written);
    }
}

static uint64_t qdr_http_deliver(void *context, qdr_link_t *link, qdr_delivery_t *delivery, bool settled)
{
    printf ("qdr_http_deliver *************\n");
    void* link_context = qdr_link_get_context(link);
    bool connector_establish = false;
    if (link_context) {
            qdr_http_connection_t* conn = (qdr_http_connection_t*) link_context;
            conn->session_data->message = qdr_delivery_message(delivery);
            qd_log(http_adaptor->log_source, QD_LOG_INFO, "[C%i][L%i] Delivery event", conn->conn_id, conn->outgoing_id);
            if (!conn->out_dlv) {
                conn->out_dlv = delivery;
                if (!conn->ingress) {
                    //on egress, can only set up link for the reverse
                    //direction once we receive the first part of the
                    //message from client to server
                    http_connector_establish(conn);
                    printf ("http_connector_establish true\n");
                    connector_establish = true;
                    qd_message_t *msg = qdr_delivery_message(delivery);
                    qdr_http_connection_copy_reply_to(conn, qd_message_field_iterator(msg, QD_FIELD_REPLY_TO));
                    qdr_terminus_t *target = qdr_terminus(0);
                    qdr_terminus_set_address(target, conn->reply_to);
                    conn->in_link = qdr_link_first_attach(conn->qdr_conn,
                                                         QD_INCOMING,
                                                         qdr_terminus(0),  //qdr_terminus_t   *source,
                                                         target, //qdr_terminus_t   *target,
                                                         "http.egress.in",  //const char       *name,
                                                         0,                //const char       *terminus_addr,
                                                         &(conn->incoming_id));
                    qdr_link_set_context(conn->in_link, conn);
                }
            }
            if (!connector_establish)
                handle_outgoing_http(conn);
    }
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
    DEQ_INIT(adaptor->connectors);

    //FIXME: I need adaptor when handling configuration, need to
    //figure out right way to do that. Just hold on to a pointer as
    //temporary hack for now.
    http_adaptor = adaptor;
}


static int handle_incoming_http(qdr_http_connection_t *conn)
{
    printf ("handle_incoming_http %p and incoming is %i\n", (void *)conn, conn->ingress);
    qd_buffer_list_t buffers;
    DEQ_INIT(buffers);
    pn_raw_buffer_t raw_buffers[READ_BUFFERS];
    size_t n;
    int count = 0;
    while ( (n = pn_raw_connection_take_read_buffers(conn->pn_raw_conn, raw_buffers, READ_BUFFERS)) ) {
        for (size_t i = 0; i < n && raw_buffers[i].bytes; ++i) {
            qd_buffer_t *buf = (qd_buffer_t*) raw_buffers[i].context;
            qd_buffer_insert(buf, raw_buffers[i].size);
            count += raw_buffers[i].size;
            DEQ_INSERT_TAIL(buffers, buf);
        }
    }
    grant_read_buffers(conn);

    //
    // Read each buffer in the buffer chain and call nghttp2_session_mem_recv with each buffer content
    //
    qd_buffer_t *buf = DEQ_HEAD(buffers);
    while (buf) {
        nghttp2_session_mem_recv(conn->session_data->session, qd_buffer_base(buf), qd_buffer_size(buf));
        buf = DEQ_NEXT(buf);
    }

    return count;
}


qdr_http_connection_t *qdr_http_connection_ingress_accept(qdr_http_connection_t* ingress_http_conn)
{
    ingress_http_conn->remote_address = get_address_string(ingress_http_conn->pn_raw_conn);
    qdr_connection_info_t *info = qdr_connection_info(false, //bool             is_encrypted,
                                                      false, //bool             is_authenticated,
                                                      true,  //bool             opened,
                                                      "",   //char            *sasl_mechanisms,
                                                      QD_INCOMING, //qd_direction_t   dir,
                                                      ingress_http_conn->remote_address,    //const char      *host,
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
    ingress_http_conn->out_link = qdr_link_first_attach(conn,
                                         QD_OUTGOING,
                                         dynamic_source,   //qdr_terminus_t   *source,
                                         qdr_terminus(0),  //qdr_terminus_t   *target,
                                         "tcp.ingress.out",        //const char       *name,
                                         0,                //const char       *terminus_addr,
                                         &(ingress_http_conn->outgoing_id));
    qdr_link_set_context(ingress_http_conn->out_link, ingress_http_conn);

    qdr_terminus_t *target = qdr_terminus(0);
    printf ("ingress_http_conn->config->address = %s\n", ingress_http_conn->config->address);
    qdr_terminus_set_address(target, ingress_http_conn->config->address);
    ingress_http_conn->in_link = qdr_link_first_attach(conn,
                                         QD_INCOMING,
                                         qdr_terminus(0),  //qdr_terminus_t   *source,
                                         target,           //qdr_terminus_t   *target,
                                         "tcp.ingress.in",         //const char       *name,
                                         0,                //const char       *terminus_addr,
                                         &(ingress_http_conn->incoming_id));
    printf ("ingress_http_conn->in_link is %p\n", (void *)ingress_http_conn->in_link);
    qdr_link_set_context(ingress_http_conn->in_link, ingress_http_conn);


    grant_read_buffers(ingress_http_conn);

    return ingress_http_conn;


}


static void handle_connection_event(pn_event_t *e, qd_server_t *qd_server, void *context)
{
    qdr_http_connection_t *conn = (qdr_http_connection_t*) context;
    qd_log_source_t *log = http_adaptor->log_source;
    switch (pn_event_type(e)) {
    case PN_RAW_CONNECTION_CONNECTED: {
        if (conn->ingress) {
            qdr_http_connection_ingress_accept(conn);
            qd_log(log, QD_LOG_INFO, "[C%i] Accepted from %s", conn->conn_id, conn->remote_address);
        } else {
            qd_log(log, QD_LOG_INFO, "[C%i] Connected", conn->conn_id);
            printf ("PN_RAW_CONNECTION_CONNECTED Calling handle_outgoing_http\n");
            handle_outgoing_http(conn);
            qdr_connection_process(conn->qdr_conn);
        }
        break;
    }
    case PN_RAW_CONNECTION_CLOSED_READ: {
        qd_log(log, QD_LOG_INFO, "[C%i] Closed for reading", conn->conn_id);
        break;
    }
    case PN_RAW_CONNECTION_CLOSED_WRITE: {
        qd_log(log, QD_LOG_INFO, "[C%i] Closed for writing", conn->conn_id);
        break;
    }
    case PN_RAW_CONNECTION_DISCONNECTED: {
        qdr_connection_closed(conn->qdr_conn);
        //free_qdr_http_connection(conn);
        qd_log(log, QD_LOG_INFO, "[C%i] Disconnected", conn->conn_id);
        break;
    }
    case PN_RAW_CONNECTION_NEED_WRITE_BUFFERS: {
        qd_log(log, QD_LOG_INFO, "[C%i] Need write buffers", conn->conn_id);
        while (qdr_connection_process(conn->qdr_conn)) {}
        break;
    }
    case PN_RAW_CONNECTION_NEED_READ_BUFFERS: {
        qd_log(log, QD_LOG_INFO, "[C%i] Need read buffers", conn->conn_id);
        while (qdr_connection_process(conn->qdr_conn)) {}
        break;
    }
    case PN_RAW_CONNECTION_WAKE: {
        qd_log(log, QD_LOG_INFO, "[C%i] Wake-up", conn->conn_id);
        printf ("PN_RAW_CONNECTION_WAKE\n");
        while (qdr_connection_process(conn->qdr_conn)) {}
        break;
    }
    case PN_RAW_CONNECTION_READ: {
        int read = handle_incoming_http(conn);
        qd_log(log, QD_LOG_INFO, "[C%i] Read %i bytes", conn->conn_id, read);
        while (qdr_connection_process(conn->qdr_conn)) {}
        break;
    }
    case PN_RAW_CONNECTION_WRITTEN: {
        pn_raw_buffer_t buffs[WRITE_BUFFERS];
        size_t pn_raw_connection_take_written_buffers(pn_raw_connection_t *connection, pn_raw_buffer_t *buffers, size_t num);
        size_t n;
        size_t written = 0;
        while ( (n = pn_raw_connection_take_written_buffers(conn->pn_raw_conn, buffs, WRITE_BUFFERS)) ) {

            for (size_t i = 0; i < n; ++i) {
                printf ("buffs[i].size is %i\n", (int)buffs[i].size);
                written += buffs[i].size;
            }
            //qd_message_release_body(qdr_delivery_message(conn->out_dlv), buffs, n);
        }
        qd_log(log, QD_LOG_INFO, "[C%i] Wrote %i bytes", conn->conn_id, written);
        while (qdr_connection_process(conn->qdr_conn)) {}
        break;
    }
    default:
        break;
    }
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
            qdr_http_connection_ingress(li);
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
    config->address              = qd_entity_get_string(entity, "address");           CHECK();

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

    qd_log(http_adaptor->log_source, QD_LOG_INFO, "[C%i] on_activate", conn->conn_id);
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
    printf ("qdr_http_connection_egress *********\n");
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
    //egress_conn->pn_raw_conn = pn_raw_connection();

    qd_http2_session_data_t *session_data = new_qd_http2_session_data_t();
    egress_conn->session_data = session_data;
    ZERO(egress_conn->session_data);
    DEQ_INIT(egress_conn->session_data->streams);
    egress_conn->session_data->conn = egress_conn;

    nghttp2_session_callbacks *callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_client_new(&session_data->session, callbacks, session_data);
    egress_conn->session_data->stream_data = create_http2_client_stream_data(egress_conn->config->host_port, egress_conn->config->host, egress_conn->config->port);


    //pn_raw_connection_set_context(egress_conn->pn_raw_conn, egress_conn);
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
    printf ("egress conn is %p\n", (void *)conn);
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

    printf("egress conn out_link is %p\n", (void *)egress_conn->out_link);

    qdr_link_set_context(egress_conn->out_link, egress_conn);
    //the incoming link for egress is created once we receive the
    //message which has the reply to address

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
