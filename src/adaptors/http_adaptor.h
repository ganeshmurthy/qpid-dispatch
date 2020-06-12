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
#include <qpid/dispatch/server.h>
#include <qpid/dispatch/threading.h>
#include <qpid/dispatch/atomic.h>
#include <qpid/dispatch/alloc.h>
#include <qpid/dispatch/ctools.h>
#include <qpid/dispatch/log.h>


// We already have a qd_http_listener_t defined in http-libwebsockets.c
// We will call this as qd_http_lsnr_t in order to avoid a clash.
// At a later point in time, we will handle websocket here as well
// and get rid of http-libwebsockets.c and rename this as qd_http_listener_t
typedef struct qd_http_lsnr_t     qd_http_lsnr_t;
typedef struct qd_http_connector_t     qd_http_connector_t;
typedef struct qd_bridge_config_t qd_bridge_config_t;

struct qd_bridge_config_t {
    char *name;
    char *host;
    char *port;
    char *address;
    char *host_port;
};

struct qd_http_lsnr_t {
    qd_handler_context_t       context;
    sys_atomic_t               ref_count;
    qd_server_t               *server;
    qd_bridge_config_t         config;
    pn_listener_t             *pn_listener;
    DEQ_LINKS(qd_http_lsnr_t);
};

struct qd_http_connector_t
{
    sys_atomic_t              ref_count;
    qd_server_t              *server;
    qd_bridge_config_t        config;
    qd_timer_t               *timer;
    long                      delay;

    DEQ_LINKS(qd_http_connector_t);
};


DEQ_DECLARE(qd_http_lsnr_t, qd_http_lsnr_list_t);
ALLOC_DECLARE(qd_http_lsnr_t);

DEQ_DECLARE(qd_http_connector_t, qd_http_connector_list_t);
ALLOC_DECLARE(qd_http_connector_t);
