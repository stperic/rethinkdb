// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "clustering/immediate_consistency/remote_replicator_server.hpp"

remote_replicator_server_t::remote_replicator_server_t(
        mailbox_manager_t *_mailbox_manager,
        primary_query_router_t *_primary) :
    mailbox_manager(_mailbox_manager),
    primary(_primary),
    registrar(mailbox_manager, this)
    { }

remote_replicator_server_t::proxy_replica_t::proxy_replica_t(
        const remote_replicator_client_bcard_t &_client_bcard,
        remote_replicator_server_t *_parent) :
    client_bcard(_client_bcard), parent(_parent), is_ready(false)
{
    state_timestamp_t first_timestamp;
    registration = make_scoped<primary_query_router_t::dispatchee_registration_t>(
        parent->primary, this, client_bcard.server_id, 1.0, &first_timestamp);
    send(parent->mailbox_manager, client_bcard.intro_mailbox,
        remote_replica_client_intro_t { first_timestamp, ready_mailbox.get_address() });
}

void remote_replicator_server_t::proxy_replica_t::do_read(
        const read_t &read,
        state_timestamp_t min_timestamp,
        signal_t *interruptor,
        read_response_t *response_out) {
    guarantee(is_ready);
    cond_t got_response;
    mailbox_t<void(read_response_t)> response_mailbox(
        parent->mailbox_manager,
        [&](signal_t *, const read_response_t &response) {
            *response_out = response;
            got_response.pulse();
        });
    send(parent->mailbox_manager, client_bcard.read_mailbox,
        read, min_timestamp, reply_mailbox.get_address());
    wait_interruptible(&got_response, interruptor);
}

void remote_replicator_server_t::proxy_replica_t::do_write_sync(
        const write_t &write,
        state_timestamp_t timestamp,
        order_token_t order_token,
        write_durability_t durability,
        signal_t *interruptor,
        write_response_t *response_out) {
    guarantee(is_ready);
    cond_t got_response;
    mailbox_t<void(write_response_t)> response_mailbox(
        parent->mailbox_manager,
        [&](signal_t *, const write_response_t &response) {
            *response_out = response;
            got_response.pulse();
        });
    send(parent->mailbox_manager, client_bcard.write_sync_mailbox,
        write, timestamp, order_token, durability, response_mailbox.get_address());
    wait_interruptible(&got_response, interruptor);
}

void remote_replicator_server_t::proxy_replica_t::do_write_async(
        const write_t &write,
        state_timestamp_t timestamp,
        order_token_t order_token,
        UNUSED signal_t *interruptor) {
    send(parent->mailbox_manager, client_bcard.write_async_mailbox,
        write, timestamp, order_token);
}

void remote_replicator_server_t::proxy_replica_t::on_ready(signal_t *) {
    guarantee(!is_ready);
    is_ready = true;
    registration->mark_readable();
}
