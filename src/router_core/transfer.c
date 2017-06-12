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

#include "router_core_private.h"
#include <qpid/dispatch/amqp.h>
#include <stdio.h>

static void qdr_link_deliver_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_link_flow_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_send_to_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_update_delivery_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_delete_delivery_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_deliver_continue_CT(qdr_core_t *core, qdr_action_t *action, bool discard);

//==================================================================================
// Internal Functions
//==================================================================================

void qdr_delivery_read_extension_state(qdr_delivery_t *dlv, uint64_t disposition, pn_data_t* disposition_date, bool update_disposition);
void qdr_delivery_copy_extension_state(qdr_delivery_t *src, qdr_delivery_t *dest, bool update_disposition);


//==================================================================================
// Interface Functions
//==================================================================================

qdr_delivery_t *qdr_link_deliver(qdr_link_t *link, qd_message_t *msg, qd_iterator_t *ingress,
                                 bool settled, qd_bitmask_t *link_exclusion)
{
    qdr_action_t   *action = qdr_action(qdr_link_deliver_CT, "link_deliver");
    qdr_delivery_t *dlv    = new_qdr_delivery_t();

    ZERO(dlv);
    sys_atomic_init(&dlv->ref_count, 1); // referenced by the action
    dlv->link           = link;
    dlv->msg            = msg;
    dlv->to_addr        = 0;
    dlv->origin         = ingress;
    dlv->settled        = settled;
    dlv->presettled     = settled;
    dlv->link_exclusion = link_exclusion;
    dlv->error          = 0;

    action->args.connection.delivery = dlv;
    qdr_action_enqueue(link->core, action);
    return dlv;
}


qdr_delivery_t *qdr_link_deliver_to(qdr_link_t *link, qd_message_t *msg,
                                    qd_iterator_t *ingress, qd_iterator_t *addr,
                                    bool settled, qd_bitmask_t *link_exclusion)
{
    qdr_action_t   *action = qdr_action(qdr_link_deliver_CT, "link_deliver");
    qdr_delivery_t *dlv    = new_qdr_delivery_t();

    ZERO(dlv);
    sys_atomic_init(&dlv->ref_count, 1); // referenced by the action
    dlv->link           = link;
    dlv->msg            = msg;
    dlv->to_addr        = addr;
    dlv->origin         = ingress;
    dlv->settled        = settled;
    dlv->presettled     = settled;
    dlv->link_exclusion = link_exclusion;
    dlv->error          = 0;

    action->args.connection.delivery = dlv;
    qdr_action_enqueue(link->core, action);
    return dlv;
}


qdr_delivery_t *qdr_link_deliver_to_routed_link(qdr_link_t *link, qd_message_t *msg, bool settled,
                                                const uint8_t *tag, int tag_length,
                                                uint64_t disposition, pn_data_t* disposition_data)
{
    if (tag_length > 32)
        return 0;
    
    qdr_action_t   *action = qdr_action(qdr_link_deliver_CT, "link_deliver");
    qdr_delivery_t *dlv    = new_qdr_delivery_t();
    ZERO(dlv);
    sys_atomic_init(&dlv->ref_count, 1); // referenced by the action
    dlv->link       = link;
    dlv->msg        = msg;
    dlv->settled    = settled;
    dlv->presettled = settled;
    dlv->error      = 0;

    qdr_delivery_read_extension_state(dlv, disposition, disposition_data, true);

    action->args.connection.delivery   = dlv;
    action->args.connection.tag_length = tag_length;
    memcpy(action->args.connection.tag, tag, tag_length);
    qdr_action_enqueue(link->core, action);
    return dlv;
}

qdr_delivery_t *qdr_deliver_continue(qdr_delivery_t *in_dlv)
{
    qdr_action_t   *action = qdr_action(qdr_deliver_continue_CT, "deliver_continue");
    action->args.connection.delivery = in_dlv;
    qdr_action_enqueue(in_dlv->link->core, action);
    return in_dlv;
}

void qdr_link_process_deliveries(qdr_core_t *core, qdr_link_t *link, int credit)
{
    qdr_connection_t *conn = link->conn;
    qdr_delivery_t   *out_dlv;
    bool              drained = false;
    int               offer   = -1;
    bool              settled = false;
    bool              sent    = false;

    if (link->link_direction == QD_OUTGOING) {
        while (credit > 0 && !drained) {
            sys_mutex_lock(conn->work_lock);
            out_dlv = DEQ_HEAD(link->undelivered);
            if (out_dlv) {
                sent = qdr_delivery_sent(out_dlv);
                settled = out_dlv->settled;

                //
                // Remove from the undelivered and move to unsettled only if the entire message has been sent.
                //
                if (sent) {
                    DEQ_REMOVE_HEAD(link->undelivered);
                    out_dlv->link_work = 0;
                    if (!settled) {
                        DEQ_INSERT_TAIL(link->unsettled, out_dlv);
                        out_dlv->where = QDR_DELIVERY_IN_UNSETTLED;
                    } else
                        out_dlv->where = QDR_DELIVERY_NOWHERE;

                    credit--;
                    link->total_deliveries++;
                    offer = DEQ_SIZE(link->undelivered);
                }

            } else
                drained = true;
            sys_mutex_unlock(conn->work_lock);

            if (out_dlv) {
                link->credit_to_core--;
                if (!sent) {
                    core->deliver_handler(core->user_context, link, out_dlv, settled);
                }
                if (settled && sent)
                    qdr_delivery_decref(core, out_dlv);
            }
        }

        if (drained)
            core->drained_handler(core->user_context, link);
        else if (offer != -1)
            core->offer_handler(core->user_context, link, offer);
    }
}


void qdr_link_flow(qdr_core_t *core, qdr_link_t *link, int credit, bool drain_mode)
{
    qdr_action_t *action = qdr_action(qdr_link_flow_CT, "link_flow");

    //
    // Compute the number of credits now available that we haven't yet given
    // incrementally to the router core.  i.e. convert absolute credit to
    // incremental credit.
    //
    credit -= link->credit_to_core;
    if (credit < 0)
        credit = 0;
    link->credit_to_core += credit;

    action->args.connection.link   = link;
    action->args.connection.credit = credit;
    action->args.connection.drain  = drain_mode;

    qdr_action_enqueue(core, action);
}


void qdr_send_to1(qdr_core_t *core, qd_message_t *msg, qd_iterator_t *addr, bool exclude_inprocess, bool control)
{
    qdr_action_t *action = qdr_action(qdr_send_to_CT, "send_to");
    action->args.io.address           = qdr_field_from_iter(addr);
    action->args.io.message           = qd_message_copy(msg);
    action->args.io.exclude_inprocess = exclude_inprocess;
    action->args.io.control           = control;

    qdr_action_enqueue(core, action);
}


void qdr_send_to2(qdr_core_t *core, qd_message_t *msg, const char *addr, bool exclude_inprocess, bool control)
{
    qdr_action_t *action = qdr_action(qdr_send_to_CT, "send_to");
    action->args.io.address           = qdr_field(addr);
    action->args.io.message           = qd_message_copy(msg);
    action->args.io.exclude_inprocess = exclude_inprocess;
    action->args.io.control           = control;

    qdr_action_enqueue(core, action);
}


void qdr_delivery_update_disposition(qdr_core_t *core, qdr_delivery_t *delivery, uint64_t disposition,
                                     bool settled, qdr_error_t *error, pn_data_t *ext_state, bool ref_given)
{
    qdr_action_t *action = qdr_action(qdr_update_delivery_CT, "update_delivery");
    action->args.delivery.delivery    = delivery;
    action->args.delivery.disposition = disposition;
    action->args.delivery.settled     = settled;
    action->args.delivery.error       = error;

    // handle delivery-state extensions e.g. declared, transactional-state
    qdr_delivery_read_extension_state(delivery, disposition, ext_state, false);

    //
    // The delivery's ref_count must be incremented to protect its travels into the
    // core thread.  If the caller has given its reference to us, we can simply use
    // the given ref rather than increment a new one.
    //
    if (!ref_given)
        qdr_delivery_incref(delivery);

    qdr_action_enqueue(core, action);
}


void qdr_delivery_set_context(qdr_delivery_t *delivery, void *context)
{
    delivery->context = context;
}


void *qdr_delivery_get_context(qdr_delivery_t *delivery)
{
    return delivery->context;
}

void qdr_delivery_incref(qdr_delivery_t *delivery)
{
    sys_atomic_inc(&delivery->ref_count);
}


void qdr_delivery_decref(qdr_core_t *core, qdr_delivery_t *delivery)
{
    uint32_t ref_count = sys_atomic_dec(&delivery->ref_count);
    assert(ref_count > 0);

    if (ref_count == 1) {
        //
        // The delivery deletion must occur inside the core thread.
        // Queue up an action to do the work.
        //
        qdr_action_t *action = qdr_action(qdr_delete_delivery_CT, "delete_delivery");
        action->args.delivery.delivery = delivery;
        qdr_action_enqueue(core, action);
    }
}


void qdr_delivery_tag(const qdr_delivery_t *delivery, const char **tag, int *length)
{
    *tag    = (const char*) delivery->tag;
    *length = delivery->tag_length;
}


qd_message_t *qdr_delivery_message(const qdr_delivery_t *delivery)
{
    if (!delivery)
        return 0;

    return delivery->msg;
}

bool qdr_delivery_sent(const qdr_delivery_t *delivery)
{
    if (!delivery)
        return false;
    return qd_message_sent(delivery->msg);
}

uint64_t qdr_delivery_disposition(const qdr_delivery_t *delivery)
{
    return delivery->disposition;
}

bool qdr_delivery_released(const qdr_delivery_t *delivery)
{
    return qdr_delivery_disposition(delivery) == PN_RELEASED;
}

bool qdr_delivery_rejected(const qdr_delivery_t *delivery)
{
    return qdr_delivery_disposition(delivery) == PN_REJECTED;
}

qdr_error_t *qdr_delivery_error(const qdr_delivery_t *delivery)
{
    return delivery->error;
}

//==================================================================================
// In-Thread Functions
//==================================================================================

void qdr_delivery_release_CT(qdr_core_t *core, qdr_delivery_t *dlv)
{
    bool push = dlv->disposition != PN_RELEASED;

    dlv->disposition = PN_RELEASED;
    dlv->settled = true;
    bool moved = qdr_delivery_settled_CT(core, dlv);

    if (push || moved)
        qdr_delivery_push_CT(core, dlv);

    //
    // Remove the unsettled reference
    //
    if (moved)
        qdr_delivery_decref_CT(core, dlv);
}


void qdr_delivery_failed_CT(qdr_core_t *core, qdr_delivery_t *dlv)
{
    bool push = dlv->disposition != PN_MODIFIED;

    dlv->disposition = PN_MODIFIED;
    dlv->settled = true;
    bool moved = qdr_delivery_settled_CT(core, dlv);

    if (push || moved)
        qdr_delivery_push_CT(core, dlv);

    //
    // Remove the unsettled reference
    //
    if (moved)
       qdr_delivery_decref_CT(core, dlv);
}


bool qdr_delivery_settled_CT(qdr_core_t *core, qdr_delivery_t *dlv)
{
    //
    // Remove a delivery from its unsettled list.  Side effects include issuing
    // replacement credit and visiting the link-quiescence algorithm
    //
    qdr_link_t       *link  = dlv->link;
    qdr_connection_t *conn  = link ? link->conn : 0;
    bool              moved = false;

    if (!link || !conn)
        return false;

    //
    // The lock needs to be acquired only for outgoing links
    //
    if (link->link_direction == QD_OUTGOING)
        sys_mutex_lock(conn->work_lock);

    if (dlv->where == QDR_DELIVERY_IN_UNSETTLED) {
        DEQ_REMOVE(link->unsettled, dlv);
        dlv->where = QDR_DELIVERY_NOWHERE;
        moved = true;
    }

    if (link->link_direction == QD_OUTGOING)
        sys_mutex_unlock(conn->work_lock);

    if (dlv->tracking_addr) {
        dlv->tracking_addr->outstanding_deliveries[dlv->tracking_addr_bit]--;
        dlv->tracking_addr->tracked_deliveries--;

        if (dlv->tracking_addr->tracked_deliveries == 0)
            qdr_check_addr_CT(core, dlv->tracking_addr, false);

        dlv->tracking_addr = 0;
    }

    //
    // If this is an incoming link and it is not link-routed or inter-router, issue
    // one replacement credit on the link.  Note that credit on inter-router links is
    // issued immediately even for unsettled deliveries.
    //
    if (moved && link->link_direction == QD_INCOMING &&
        link->link_type != QD_LINK_ROUTER && !link->connected_link)
        qdr_link_issue_credit_CT(core, link, 1, false);

    return moved;
}


static void qdr_delete_delivery_internal_CT(qdr_core_t *core, qdr_delivery_t *delivery)
{
    qdr_link_t *link = delivery->link;

    if (delivery->msg)
        qd_message_free(delivery->msg);

    if (delivery->to_addr)
        qd_iterator_free(delivery->to_addr);

    if (delivery->tracking_addr) {
        delivery->tracking_addr->outstanding_deliveries[delivery->tracking_addr_bit]--;
        delivery->tracking_addr->tracked_deliveries--;

        if (delivery->tracking_addr->tracked_deliveries == 0)
            qdr_check_addr_CT(core, delivery->tracking_addr, false);

        delivery->tracking_addr = 0;
    }

    if (link) {
        if (delivery->presettled)
            link->presettled_deliveries++;
        else if (delivery->disposition == PN_ACCEPTED)
            link->accepted_deliveries++;
        else if (delivery->disposition == PN_REJECTED)
            link->rejected_deliveries++;
        else if (delivery->disposition == PN_RELEASED)
            link->released_deliveries++;
        else if (delivery->disposition == PN_MODIFIED)
            link->modified_deliveries++;
    }

    qd_bitmask_free(delivery->link_exclusion);
    qdr_error_free(delivery->error);

    free_qdr_delivery_t(delivery);
}


void qdr_delivery_decref_CT(qdr_core_t *core, qdr_delivery_t *dlv)
{
    uint32_t ref_count = sys_atomic_dec(&dlv->ref_count);
    assert(ref_count > 0);

    if (ref_count == 1)
        qdr_delete_delivery_internal_CT(core, dlv);
}


static void qdr_link_flow_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_link_t *link      = action->args.connection.link;
    int  credit           = action->args.connection.credit;
    bool drain            = action->args.connection.drain;
    bool activate         = false;
    bool drain_was_set    = !link->drain_mode && drain;
    qdr_link_work_t *work = 0;
    
    link->drain_mode = drain;

    //
    // If this is an attach-routed link, propagate the flow data downrange.
    // Note that the credit value is incremental.
    //
    if (link->connected_link) {
        qdr_link_t *clink = link->connected_link;

        if (clink->link_direction == QD_INCOMING)
            qdr_link_issue_credit_CT(core, link->connected_link, credit, drain);
        else {
            work = new_qdr_link_work_t();
            ZERO(work);
            work->work_type = QDR_LINK_WORK_FLOW;
            work->value     = credit;
            if (drain)
                work->drain_action = QDR_LINK_WORK_DRAIN_ACTION_DRAINED;
            qdr_link_enqueue_work_CT(core, clink, work);
        }

        return;
    }

    //
    // Handle the replenishing of credit outbound
    //
    if (link->link_direction == QD_OUTGOING && (credit > 0 || drain_was_set)) {
        if (drain_was_set) {
            work = new_qdr_link_work_t();
            ZERO(work);
            work->work_type    = QDR_LINK_WORK_FLOW;
            work->drain_action = QDR_LINK_WORK_DRAIN_ACTION_DRAINED;
        }

        sys_mutex_lock(link->conn->work_lock);
        if (work)
            DEQ_INSERT_TAIL(link->work_list, work);
        if (DEQ_SIZE(link->undelivered) > 0 || drain_was_set) {
            qdr_add_link_ref(&link->conn->links_with_work, link, QDR_LINK_LIST_CLASS_WORK);
            activate = true;
        }
        sys_mutex_unlock(link->conn->work_lock);
    }

    //
    // Activate the connection if we have deliveries to send or drain mode was set.
    //
    if (activate)
        qdr_connection_activate_CT(core, link->conn);
}


/**
 * Return the number of outbound paths to destinations that this address has.
 * Note that even if there are more than zero paths, the destination still may
 * be unreachable (e.g. an rnode next hop with no link).
 */
static long qdr_addr_path_count_CT(qdr_address_t *addr)
{
    return (long) DEQ_SIZE(addr->subscriptions) + (long) DEQ_SIZE(addr->rlinks) +
        (long) qd_bitmask_cardinality(addr->rnodes);
}


static void qdr_link_forward_CT(qdr_core_t *core, qdr_link_t *in_link, qdr_delivery_t *in_dlv, qdr_address_t *addr)
{
    bool more = qd_message_more(qdr_delivery_message(in_dlv));

    if (addr && addr == in_link->owning_addr && qdr_addr_path_count_CT(addr) == 0) {
        //
        // We are trying to forward a delivery on an address that has no outbound paths
        // AND the incoming link is targeted (not anonymous).  In this case, we must put
        // the delivery on the incoming link's undelivered list.  Note that it is safe
        // to do this because the undelivered list will be flushed once the number of
        // paths transitions from zero to one.
        //
        // Use the action-reference as the reference for undelivered rather
        // than decrementing and incrementing the delivery ref_count.
        //
        DEQ_INSERT_TAIL(in_link->undelivered, in_dlv);
        in_dlv->where = QDR_DELIVERY_IN_UNDELIVERED;
        return;
    }

    int fanout = 0;

    if (addr) {
        fanout = qdr_forward_message_CT(core, addr, in_dlv->msg, in_dlv, false, in_link->link_type == QD_LINK_CONTROL);
        if (in_link->link_type != QD_LINK_CONTROL && in_link->link_type != QD_LINK_ROUTER)
            addr->deliveries_ingress++;
        in_link->total_deliveries++;
    }

    if (fanout == 0) {
        //
        // Message was not delivered, drop the delivery.
        //
        // If the delivery is not settled, release it.
        //
        if (!in_dlv->settled) {
            qdr_delivery_release_CT(core, in_dlv);
            qd_message_set_discard(in_dlv->msg, true);
        }

        //
        // Decrementing the delivery ref count for the action
        //
        qdr_delivery_decref_CT(core, in_dlv);

        qdr_link_issue_credit_CT(core, in_link, 1, false);

    } else if (fanout > 0) {
        if (in_dlv->settled) {
            //
            // The delivery is settled.  Keep it off the unsettled list and issue
            // replacement credit for it now.
            //
            qdr_link_issue_credit_CT(core, in_link, 1, false);

            //
            // If the delivery has no more references, free it now.
            //
            if (!more) {
                //assert(!in_dlv->pe er);
                qdr_delivery_decref_CT(core, in_dlv);
            }
        } else {
            //
            // Again, don't bother decrementing then incrementing the ref_count
            //
            DEQ_INSERT_TAIL(in_link->unsettled, in_dlv);
            in_dlv->where = QDR_DELIVERY_IN_UNSETTLED;

            //
            // If the delivery was received on an inter-router link, issue the credit
            // now.  We don't want to tie inter-router link flow control to unsettled
            // deliveries because it increases the risk of credit starvation if there
            // are many addresses sharing the link.
            //
            if (in_link->link_type == QD_LINK_ROUTER)
                qdr_link_issue_credit_CT(core, in_link, 1, false);
        }
    }
}

static void qdr_deliver_continue_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_delivery_t *in_dlv  = action->args.connection.delivery;

    //
    // If it is already in the undelivered list or it has no peers, don't try to deliver this again.
    //
    if (in_dlv->where == QDR_DELIVERY_IN_UNDELIVERED || in_dlv->num_peers == 0)
        return;

    if (in_dlv->num_peers > INITIAL_PEER_SIZE) {
        qdr_delivery_ref_t *ref = DEQ_HEAD(in_dlv->peers.peer_ref_list);
        qdr_delivery_t *peer = ref->dlv;
        if (peer) {
            sys_mutex_lock(peer->link->conn->work_lock);
            qdr_forward_add_work_delivery_CT(core, peer->link, peer);
            sys_mutex_unlock(peer->link->conn->work_lock);

            //
            // Activate the outgoing connection for later processing.
            //
            qdr_connection_activate_CT(core, peer->link->conn);
        }
    }
    else {
        for (int i=0; i < INITIAL_PEER_SIZE; i++) {
            qdr_delivery_t *peer = in_dlv->peers.peer_list[i];
            if (peer) {
                sys_mutex_lock(peer->link->conn->work_lock);
                qdr_forward_add_work_delivery_CT(core, peer->link, peer);
                sys_mutex_unlock(peer->link->conn->work_lock);

                //
                // Activate the outgoing connection for later processing.
                //
                qdr_connection_activate_CT(core, peer->link->conn);
            }
        }
    }


    if (!qd_message_more(qdr_delivery_message(in_dlv))) {
        qdr_link_t *in_link = in_dlv->link;

        if (qdr_link_is_routed(in_link)) {
            if (in_dlv->settled) {
                // If the link-routed delivery is settled, decrement the ref_count on the delivery.
                // This count was the owned-by-action count.
                //
                qdr_delivery_decref_CT(core, in_dlv);
            }
            return;
        }

        // Non-link routed

        /*if (in_dlv->fanout == 0) {
            qdr_delivery_decref_CT(core, in_dlv);
        }
        else {
            if (in_dlv->settled) {
                qdr_delivery_decref_CT(core, in_dlv);
            }
        }*/

        //
        // The entire message has now been received. Check to see if there are in process subscriptions that need to
        // receive this message. in process subscriptions, at this time, can deal only with full messages.
        //
        qdr_subscription_t *sub = DEQ_HEAD(in_dlv->subscriptions);
        while (sub) {
            qdr_forward_on_message_CT(core, sub, in_dlv ? in_dlv->link : 0, in_dlv->msg);
            sub = DEQ_NEXT(sub);
        }
    }

}

static void qdr_link_deliver_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_delivery_t *in_dlv  = action->args.connection.delivery;
    qdr_link_t     *link = in_dlv->link;

    //
    // If this is an attach-routed link, put the delivery directly onto the peer link
    //
    if (link->connected_link) {
        qdr_delivery_t *peer = qdr_forward_new_delivery_CT(core, in_dlv, link->connected_link, in_dlv->msg);

        qdr_delivery_copy_extension_state(in_dlv, peer, true);

        //
        // Copy the delivery tag.  For link-routing, the delivery tag must be preserved.
        //
        peer->tag_length = action->args.connection.tag_length;
        memcpy(peer->tag, action->args.connection.tag, peer->tag_length);

        qdr_forward_deliver_CT(core, link->connected_link, peer);

        link->total_deliveries++;

        if (in_dlv->settled) {
                qdr_delivery_decref_CT(core, in_dlv);
        }
        else {
            //
            // Note, in this case the delivery ref_count is left unchanged as we are transferring
            // the action's reference to the unsettled list's reference.
            //
            DEQ_INSERT_TAIL(link->unsettled, in_dlv);
            in_dlv->where = QDR_DELIVERY_IN_UNSETTLED;

        }
        return;
    }

    //
    // NOTE: The link->undelivered list does not need to be protected by the
    //       connection's work lock for incoming links.  This protection is only
    //       needed for outgoing links.
    //

    if (DEQ_IS_EMPTY(link->undelivered)) {
        qdr_address_t *addr = link->owning_addr;
        if (!addr && in_dlv->to_addr) {
            qdr_connection_t *conn = link->conn;
            if (conn && conn->tenant_space)
                qd_iterator_annotate_space(in_dlv->to_addr, conn->tenant_space, conn->tenant_space_len);
            qd_hash_retrieve(core->addr_hash, in_dlv->to_addr, (void**) &addr);
        }

        //
        // Give the action reference to the qdr_link_forward function.
        //
        qdr_link_forward_CT(core, link, in_dlv, addr);


    } else {
        //
        // Take the action reference and use it for undelivered.  Don't decref/incref.
        //
        DEQ_INSERT_TAIL(link->undelivered, in_dlv);
        in_dlv->where = QDR_DELIVERY_IN_UNDELIVERED;
    }
}


static void qdr_send_to_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    qdr_field_t  *addr_field = action->args.io.address;
    qd_message_t *msg        = action->args.io.message;

    if (!discard) {
        qdr_address_t *addr = 0;

        qd_iterator_reset_view(addr_field->iterator, ITER_VIEW_ADDRESS_HASH);
        qd_hash_retrieve(core->addr_hash, addr_field->iterator, (void**) &addr);
        if (addr) {
            //
            // Forward the message.  We don't care what the fanout count is.
            //
            (void) qdr_forward_message_CT(core, addr, msg, 0, action->args.io.exclude_inprocess,
                                          action->args.io.control);
            addr->deliveries_from_container++;
        } else
            qd_log(core->log, QD_LOG_DEBUG, "In-process send to an unknown address");
    }

    qdr_field_free(addr_field);
    qd_message_free(msg);
}


static void qdr_update_delivery_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    qdr_delivery_t *dlv        = action->args.delivery.delivery;
    qdr_delivery_t *peer       = dlv->peers.peer_list[0];
    bool            push       = false;
    bool            peer_moved = false;
    bool            dlv_moved  = false;
    uint64_t        disp       = action->args.delivery.disposition;
    bool            settled    = action->args.delivery.settled;
    qdr_error_t    *error      = action->args.delivery.error;
    bool error_unassigned      = true;

    //
    // Logic:
    //
    // If disposition has changed and there is a peer link, set the disposition of the peer
    // If settled, the delivery must be unlinked and freed.
    // If settled and there is a peer, the peer shall be settled and unlinked.  It shall not
    //   be freed until the connection-side thread settles the PN delivery.
    //
    if (disp != dlv->disposition) {
        //
        // Disposition has changed, propagate the change to the peer delivery.
        //
        dlv->disposition = disp;
        if (peer) {

            peer->disposition = disp;
            peer->error       = error;
            push = true;
            error_unassigned = false;
            qdr_delivery_copy_extension_state(dlv, peer, false);
        }
    }

    if (settled) {
        if (peer) {
            peer->settled = true;
            peer->peers.peer_list[0] = 0;
            dlv->peers.peer_list[0]  = 0;

            if (peer->link) {
                peer_moved = qdr_delivery_settled_CT(core, peer);
                if (peer_moved)
                    push = true;
            }

            qdr_delivery_decref_CT(core, dlv);
            qdr_delivery_decref_CT(core, peer);
        }

        if (dlv->link)
            dlv_moved = qdr_delivery_settled_CT(core, dlv);
    }

    if (push)
        qdr_delivery_push_CT(core, peer);

    //
    // Release the action reference, possibly freeing the delivery
    //
    qdr_delivery_decref_CT(core, dlv);

    //
    // Release the unsettled references if the deliveries were moved
    //
    if (dlv_moved)
        qdr_delivery_decref_CT(core, dlv);
    if (peer_moved)
        qdr_delivery_decref_CT(core, peer);
    if (error_unassigned)
        qdr_error_free(error);
}


static void qdr_delete_delivery_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (!discard)
        qdr_delete_delivery_internal_CT(core, action->args.delivery.delivery);
}


/**
 * Add link-work to provide credit to the link in an IO thread
 */
void qdr_link_issue_credit_CT(qdr_core_t *core, qdr_link_t *link, int credit, bool drain)
{
    assert(link->link_direction == QD_INCOMING);

    bool drain_changed = link->drain_mode |= drain;
    link->drain_mode   = drain;

    if (!drain_changed && credit == 0)
        return;

    if (credit > 0)
        link->flow_started = true;

    qdr_link_work_t *work = new_qdr_link_work_t();
    ZERO(work);

    work->work_type = QDR_LINK_WORK_FLOW;
    work->value     = credit;

    if (drain_changed)
        work->drain_action = drain ? QDR_LINK_WORK_DRAIN_ACTION_SET : QDR_LINK_WORK_DRAIN_ACTION_CLEAR;

    qdr_link_enqueue_work_CT(core, link, work);
}


/**
 * This function should be called after adding a new destination (subscription, local link,
 * or remote node) to an address.  If this address now has exactly one destination (i.e. it
 * transitioned from unreachable to reachable), make sure any unstarted in-links are issued
 * initial credit.
 *
 * Also, check the inlinks to see if there are undelivered messages.  If so, drain them to
 * the forwarder.
 */
void qdr_addr_start_inlinks_CT(qdr_core_t *core, qdr_address_t *addr)
{
    //
    // If there aren't any inlinks, there's no point in proceeding.
    //
    if (DEQ_SIZE(addr->inlinks) == 0)
        return;

    if (qdr_addr_path_count_CT(addr) == 1) {
        qdr_link_ref_t *ref = DEQ_HEAD(addr->inlinks);
        while (ref) {
            qdr_link_t *link = ref->link;

            //
            // Issue credit to stalled links
            //
            if (!link->flow_started)
                qdr_link_issue_credit_CT(core, link, link->capacity, false);

            //
            // Drain undelivered deliveries via the forwarder
            //
            if (DEQ_SIZE(link->undelivered) > 0) {
                //
                // Move all the undelivered to a local list in case not all can be delivered.
                // We don't want to loop here forever putting the same messages on the undelivered
                // list.
                //
                qdr_delivery_list_t deliveries;
                DEQ_MOVE(link->undelivered, deliveries);

                qdr_delivery_t *dlv = DEQ_HEAD(deliveries);
                while (dlv) {
                    DEQ_REMOVE_HEAD(deliveries);
                    qdr_link_forward_CT(core, link, dlv, addr);
                    dlv = DEQ_HEAD(deliveries);
                }
            }

            ref = DEQ_NEXT(ref);
        }
    }
}


void qdr_delivery_push_CT(qdr_core_t *core, qdr_delivery_t *dlv)
{
    if (!dlv || !dlv->link)
        return;

    qdr_link_t *link = dlv->link;
    bool activate = false;

    sys_mutex_lock(link->conn->work_lock);
    if (dlv->where != QDR_DELIVERY_IN_UNDELIVERED) {
        qdr_delivery_incref(dlv);
        qdr_add_delivery_ref(&link->updated_deliveries, dlv);
        qdr_add_link_ref(&link->conn->links_with_work, link, QDR_LINK_LIST_CLASS_WORK);
        activate = true;
    }
    sys_mutex_unlock(link->conn->work_lock);

    //
    // Activate the connection
    //
    if (activate)
        qdr_connection_activate_CT(core, link->conn);
}

pn_data_t* qdr_delivery_extension_state(qdr_delivery_t *delivery)
{
    if (!delivery->extension_state) {
        delivery->extension_state = pn_data(0);
    }
    pn_data_rewind(delivery->extension_state);
    return delivery->extension_state;
}

void qdr_delivery_free_extension_state(qdr_delivery_t *delivery)
{
    if (delivery->extension_state) {
        pn_data_free(delivery->extension_state);
        delivery->extension_state = 0;
    }
}

void qdr_delivery_write_extension_state(qdr_delivery_t *dlv, pn_delivery_t* pdlv, bool update_disposition)
{
    if (dlv->disposition > PN_MODIFIED) {
        pn_data_copy(pn_disposition_data(pn_delivery_local(pdlv)), qdr_delivery_extension_state(dlv));
        if (update_disposition) pn_delivery_update(pdlv, dlv->disposition);
        qdr_delivery_free_extension_state(dlv);
    }
}

void qdr_delivery_export_transfer_state(qdr_delivery_t *dlv, pn_delivery_t* pdlv)
{
    qdr_delivery_write_extension_state(dlv, pdlv, true);
}

void qdr_delivery_export_disposition_state(qdr_delivery_t *dlv, pn_delivery_t* pdlv)
{
    qdr_delivery_write_extension_state(dlv, pdlv, false);
}

void qdr_delivery_copy_extension_state(qdr_delivery_t *src, qdr_delivery_t *dest, bool update_diposition)
{
    if (src->disposition > PN_MODIFIED) {
        pn_data_copy(qdr_delivery_extension_state(dest), qdr_delivery_extension_state(src));
        if (update_diposition) dest->disposition = src->disposition;
        qdr_delivery_free_extension_state(src);
    }
}

void qdr_delivery_read_extension_state(qdr_delivery_t *dlv, uint64_t disposition, pn_data_t* disposition_data, bool update_disposition)
{
    if (disposition > PN_MODIFIED) {
        pn_data_rewind(disposition_data);
        pn_data_copy(qdr_delivery_extension_state(dlv), disposition_data);
        if (update_disposition) dlv->disposition = disposition;
    }
}
