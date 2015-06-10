// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "clustering/table_contract/coordinator/calculate_contracts.hpp"

#include "clustering/table_contract/cpu_sharding.hpp"
#include "logger.hpp"

/* A `contract_ack_t` is not necessarily homogeneous. It may have different `version_t`s
for different regions, and a region with a single `version_t` may need to be split
further depending on the branch history. Since `calculate_contract()` assumes it's
processing a homogeneous input, we need to break the `contract_ack_t` into homogeneous
pieces. `contract_ack_frag_t` is like a homogeneous version of `contract_ack_t`; in place
of the `region_map_t<version_t>` it has a single `state_timestamp_t`. Use
`break_ack_into_fragments()` to convert a `contract_ack_t` into a
`region_map_t<contract_ack_frag_t>`. */

class contract_ack_frag_t {
public:
    bool operator==(const contract_ack_frag_t &x) const {
        return state == x.state && version == x.version && branch == x.branch;
    }
    bool operator!=(const contract_ack_frag_t &x) const {
        return !(*this == x);
    }

    contract_ack_t::state_t state;
    boost::optional<state_timestamp_t> version;
    boost::optional<branch_id_t> branch;
};

region_map_t<contract_ack_frag_t> break_ack_into_fragments(
        const region_t &region,
        const contract_ack_t &ack,
        const region_map_t<branch_id_t> &current_branches,
        const branch_history_reader_t *raft_branch_history) {
    contract_ack_frag_t base_frag;
    base_frag.state = ack.state;
    base_frag.branch = ack.branch;
    if (!static_cast<bool>(ack.version)) {
        return region_map_t<contract_ack_frag_t>(region, base_frag);
    } else {
        branch_history_combiner_t combined_branch_history(
            raft_branch_history, &ack.branch_history);
        /* Fragment over branches and then over versions within each branch. */
        return current_branches.map_multi(region,
            [&](const region_t &branch_reg, const branch_id_t &branch) {
                return ack.version->map_multi(branch_reg,
                    [&](const region_t &reg, const version_t &vers) {
                        region_map_t<version_t> points_on_canonical_branch =
                            version_find_branch_common(&combined_branch_history,
                                vers, branch, reg);
                        return points_on_canonical_branch.map(reg,
                            [&](const version_t &common_vers) {
                                base_frag.version = boost::make_optional(common_vers.timestamp);
                                return base_frag;
                            });
                    });
            });
    }
}

/* `invisible_to_majority_of_set()` returns `true` if `target` definitely cannot be seen
by a majority of the servers in `judges`. If we can't see one of the servers in `judges`,
we'll assume it can see `target` to reduce spurious failoves. */
bool invisible_to_majority_of_set(
        const server_id_t &target,
        const std::set<server_id_t> &judges,
        watchable_map_t<std::pair<server_id_t, server_id_t>, empty_value_t> *
            connections_map) {
    size_t count = 0;
    for (const server_id_t &s : judges) {
        if (static_cast<bool>(connections_map->get_key(std::make_pair(s, target))) ||
                !static_cast<bool>(connections_map->get_key(std::make_pair(s, s)))) {
            ++count;
        }
    }
    return !(count > judges.size() / 2);
}

/* `calculate_contract()` calculates a new contract for a region. Whenever any of the
inputs changes, the coordinator will call `update_contract()` to compute a contract for
each range of keys. The new contract will often be the same as the old, in which case it
doesn't get a new contract ID. */
contract_t calculate_contract(
        /* The old contract that contains this region. */
        const contract_t &old_c,
        /* The user-specified configuration for the shard containing this region. */
        const table_config_t::shard_t &config,
        /* Contract acks from replicas regarding `old_c`. If a replica hasn't sent us an
        ack *specifically* for `old_c`, it won't appear in this map; we don't include
        acks for contracts that were in the same region before `old_c`. */
        const std::map<server_id_t, contract_ack_frag_t> &acks,
        /* This `watchable_map_t` will have an entry for (X, Y) if we can see server X
        and server X can see server Y. */
        watchable_map_t<std::pair<server_id_t, server_id_t>, empty_value_t> *
            connections_map,
        /* We'll print log messages of the form `<log prefix>: <message>`, unless
        `log_prefix` is empty, in which case we won't print anything. */
        const std::string &log_prefix) {

    contract_t new_c = old_c;

    /* If there are new servers in `config.all_replicas`, add them to `c.replicas` */
    new_c.replicas.insert(config.all_replicas.begin(), config.all_replicas.end());

    /* If there is a mismatch between `config.voting_replicas()` and `c.voters`, then
    correct it */
    std::set<server_id_t> config_voting_replicas = config.voting_replicas();
    if (!static_cast<bool>(old_c.temp_voters) &&
            old_c.voters != config_voting_replicas) {
        size_t num_streaming = 0;
        for (const server_id_t &server : config_voting_replicas) {
            auto it = acks.find(server);
            if (it != acks.end() &&
                    (it->second.state == contract_ack_t::state_t::secondary_streaming ||
                    (static_cast<bool>(old_c.primary) &&
                        old_c.primary->server == server))) {
                ++num_streaming;
            }
        }

        /* We don't want to initiate the change until a majority of the new replicas are
        already streaming, or else we'll lose write availability as soon as we set
        `temp_voters`. */
        if (num_streaming > config_voting_replicas.size() / 2) {
            /* OK, we're ready to go */
            new_c.temp_voters = boost::make_optional(config_voting_replicas);
            if (!log_prefix.empty()) {
                logINF("%s: Beginning replica set change.", log_prefix.c_str());
            }
        }
    }

    /* If we already initiated a voter change by setting `temp_voters`, it might be time
    to commit that change by setting `voters` to `temp_voters`. */
    if (static_cast<bool>(old_c.temp_voters)) {
        /* Before we change `voters`, we have to make sure that we'll preserve the
        invariant that every acked write is on a majority of `voters`. This is mostly the
        job of the primary; it will not report `primary_running` unless it is requiring
        acks from a majority of both `voters` and `temp_voters` before acking writes to
        the client, *and* it has ensured that every write that was acked before that
        policy was implemented has been backfilled to a majority of `temp_voters`. So we
        can't switch voters unless the primary reports `primary_running`. */
        if (static_cast<bool>(old_c.primary) &&
                acks.count(old_c.primary->server) == 1 &&
                acks.at(old_c.primary->server).state ==
                    contract_ack_t::state_t::primary_ready) {
            /* OK, it's safe to commit. */
            new_c.voters = *new_c.temp_voters;
            new_c.temp_voters = boost::none;
            if (!log_prefix.empty()) {
                logINF("%s: Committed replica set change.", log_prefix.c_str());
            }
        }
    }

    /* `visible_voters` includes all members of `voters` and `temp_voters` which could be
    visible to a majority of `voters` (and `temp_voters`, if `temp_voters` exists). Note
    that if the coordinator can't see server X, it will assume server X can see every
    other server; this reduces spurious failovers when the coordinator loses contact with
    other servers. */
    std::set<server_id_t> visible_voters;
    for (const server_id_t &server : new_c.replicas) {
        if (new_c.voters.count(server) == 0 &&
                (!static_cast<bool>(new_c.temp_voters) ||
                    new_c.temp_voters->count(server) == 0)) {
            continue;
        }
        if (invisible_to_majority_of_set(server, new_c.voters, connections_map)) {
            continue;
        }
        if (static_cast<bool>(new_c.temp_voters)) {
            if (invisible_to_majority_of_set(
                    server, *new_c.temp_voters, connections_map)) {
                continue;
            }
        }
        visible_voters.insert(server);
    }

    /* If a server was removed from `config.replicas` and `c.voters` but it's still in
    `c.replicas`, then remove it. And if it's primary, then make it not be primary. */
    bool should_kill_primary = false;
    for (const server_id_t &server : old_c.replicas) {
        if (config.all_replicas.count(server) == 0 &&
                new_c.voters.count(server) == 0 &&
                (!static_cast<bool>(new_c.temp_voters) ||
                    new_c.temp_voters->count(server) == 0)) {
            new_c.replicas.erase(server);
            if (static_cast<bool>(old_c.primary) && old_c.primary->server == server) {
                /* Actual killing happens further down */
                should_kill_primary = true;
                if (!log_prefix.empty()) {
                    logINF("%s: Stopping server %s as primary because it's no longer a "
                        "voter.", log_prefix.c_str(), uuid_to_str(server).c_str());
                }
            }
        }
    }

    /* If we don't have a primary, choose a primary. Servers are not eligible to be a
    primary unless they are carrying every acked write. There will be at least one
    eligible server if and only if we have reports from a majority of `new_c.voters`.

    In addition, we must choose `config.primary_replica` if it is eligible. If
    `config.primary_replica` has not sent an ack, we must wait for the failover timeout
    to elapse before electing a different replica. This is to make sure that we won't
    elect the wrong replica simply because the user's designated primary took a little
    longer to send the ack. */
    if (!static_cast<bool>(old_c.primary)) {
        /* We have an invariant that every acked write must be on the path from the root
        of the branch history to `old_c.branch`. So we project each voter's state onto
        that path, then sort them by position along the path. Any voter that is at least
        as up to date, according to that metric, as more than half of the voters
        (including itself) is eligible. We also take into account whether a server is
        visible to its peers when deciding which server to select. */

        /* First, collect the states from the servers, and sort them by how up-to-date
        they are. Note that we use the server ID as a secondary sorting key. This mean we
        tend to pick the same server if we run the algorithm twice; this helps to reduce
        unnecessary fragmentation. */
        std::vector<std::pair<state_timestamp_t, server_id_t> > sorted_candidates;
        for (const server_id_t &server : new_c.voters) {
            if (acks.count(server) == 1 && acks.at(server).state ==
                    contract_ack_t::state_t::secondary_need_primary) {
                sorted_candidates.push_back(
                    std::make_pair(*(acks.at(server).version), server));
            }
        }
        std::sort(sorted_candidates.begin(), sorted_candidates.end());

        /* Second, determine which servers are eligible to become primary on the basis of
        their data and their visibility to their peers. */
        std::vector<server_id_t> eligible_candidates;
        for (size_t i = 0; i < sorted_candidates.size(); ++i) {
            server_id_t server = sorted_candidates[i].second;
            /* If the server is not visible to more than half of its peers, then it is
            not eligible to be primary */
            if (visible_voters.count(server) == 0) {
                continue;
            }
            /* `up_to_date_count` is the number of servers that `server` is at least as
            up-to-date as. We know `server` must be at least as up-to-date as itself and
            all of the servers that are earlier in the list. */
            size_t up_to_date_count = i + 1;
            /* If there are several servers with the same timestamp, they will appear
            together in the list. So `server` may be at least as up-to-date as some of
            the servers that appear after it in the list. */
            while (up_to_date_count < sorted_candidates.size() &&
                    sorted_candidates[up_to_date_count].first ==
                        sorted_candidates[i].first) {
                ++up_to_date_count;
            }
            /* OK, now `up_to_date_count` is the number of servers that this server is
            at least as up-to-date as. */
            if (up_to_date_count > new_c.voters.size() / 2) {
                eligible_candidates.push_back(server);
            }
        }

        /* OK, now we can pick a primary. */
        auto it = std::find(eligible_candidates.begin(), eligible_candidates.end(),
                config.primary_replica);
        if (it != eligible_candidates.end()) {
            /* The user's designated primary is eligible, so use it. */
            contract_t::primary_t p;
            p.server = config.primary_replica;
            new_c.primary = boost::make_optional(p);
        } else if (!eligible_candidates.empty()) {
            /* The user's designated primary is ineligible. We have to decide if we'll
            wait for the user's designated primary to become eligible, or use one of the
            other eligible candidates. */
            if (!config.primary_replica.is_nil() &&
                    visible_voters.count(config.primary_replica) == 1 &&
                    acks.count(config.primary_replica) == 0) {
                /* The user's designated primary is visible to a majority of its peers,
                and the only reason it was disqualified is because we haven't seen an ack
                from it yet. So we'll wait for it to send in an ack rather than electing
                a different primary. */
            } else {
                /* We won't wait for it. */
                contract_t::primary_t p;
                /* `eligible_candidates` is ordered by how up-to-date they are */
                p.server = eligible_candidates.back();
                new_c.primary = boost::make_optional(p);
            }
        }

        if (static_cast<bool>(new_c.primary)) {
            if (!log_prefix.empty()) {
                logINF("%s: Selected server %s as primary.", log_prefix.c_str(),
                    uuid_to_str(new_c.primary->server).c_str());
            }
        }
    }

    /* Sometimes we already have a primary, but we need to pick a different one. There
    are three such situations:
    - The existing primary is disconnected
    - The existing primary isn't `config.primary_replica`, and `config.primary_replica`
        is ready to take over the role
    - `config.primary_replica` isn't ready to take over the role, but the existing
        primary isn't even supposed to be a replica anymore.
    In the first situation, we'll simply remove `c.primary`. In the second and third
    situations, we'll first set `c.primary->warm_shutdown` to `true`, and then only
    once the primary acknowledges that, we'll remove `c.primary`. Either way, once the
    replicas acknowledge the contract in which we removed `c.primary`, the logic earlier
    in this function will select a new primary. Note that we can't go straight from the
    old primary to the new one; we need a majority of replicas to promise to stop
    receiving updates from the old primary before it's safe to elect a new one. */
    if (static_cast<bool>(old_c.primary)) {
        /* Note we already checked for the case where the old primary wasn't supposed to
        be a replica. If this is so, then `should_kill_primary` will already be set to
        `true`. */

        /* Check if we need to do an auto-failover. The precise form of this condition
        isn't important for correctness. If we do an auto-failover when the primary isn't
        actually dead, or don't do an auto-failover when the primary is actually dead,
        the worst that will happen is we'll lose availability. */
        if (!should_kill_primary && visible_voters.count(old_c.primary->server) == 0) {
            should_kill_primary = true;
            if (!log_prefix.empty()) {
                logINF("%s: Stopping server %s as primary because a majority of voters "
                    "cannot reach it.", log_prefix.c_str(),
                    uuid_to_str(old_c.primary->server).c_str());
            }
        }

        if (should_kill_primary) {
            new_c.primary = boost::none;
        } else if (old_c.primary->server != config.primary_replica) {
            /* The old primary is still a valid replica, but it isn't equal to
            `config.primary_replica`. So we have to do a hand-over to ensure that after
            we kill the primary, `config.primary_replica` will be a valid candidate. */

            if (old_c.primary->hand_over !=
                    boost::make_optional(config.primary_replica)) {
                /* We haven't started the hand-over yet, or we're in the middle of a
                hand-over to a different primary. */
                if (acks.count(config.primary_replica) == 1 &&
                        acks.at(config.primary_replica).state ==
                            contract_ack_t::state_t::secondary_streaming &&
                        visible_voters.count(config.primary_replica) == 1) {
                    /* The new primary is ready, so begin the hand-over. */
                    new_c.primary->hand_over =
                        boost::make_optional(config.primary_replica);
                    if (!log_prefix.empty()) {
                        logINF("%s: Handing over primary from %s to %s to match table "
                            "config.", log_prefix.c_str(),
                            uuid_to_str(old_c.primary->server).c_str(),
                            uuid_to_str(config.primary_replica).c_str());
                    }
                } else {
                    /* We're not ready to switch to the new primary yet. */
                    if (static_cast<bool>(old_c.primary->hand_over)) {
                        /* We were in the middle of a hand over to a different primary,
                        and then the user changed `config.primary_replica`. But the new
                        primary isn't ready yet, so cancel the old hand-over. (This is
                        very uncommon.) */
                        new_c.primary->hand_over = boost::none;
                    }
                }
            } else {
                /* We're already in the process of handing over to the new primary. */
                if (acks.count(old_c.primary->server) == 1 &&
                        acks.at(old_c.primary->server).state ==
                            contract_ack_t::state_t::primary_ready) {
                    /* The hand over is complete. Now it's safe to stop the old primary.
                    The new primary will be started later, after a majority of the
                    replicas acknowledge that they are no longer listening for writes
                    from the old primary. */
                    new_c.primary = boost::none;
                } else if (visible_voters.count(config.primary_replica) == 0) {
                    /* Something went wrong with the new primary before the hand-over was
                    complete. So abort the hand-over. */
                    new_c.primary->hand_over = boost::none;
                }
            }
        } else {
            if (static_cast<bool>(old_c.primary->hand_over)) {
                /* We were in the middle of a hand over, but then the user changed
                `config.primary_replica` back to what it was before. (This is very
                uncommon.) */
                new_c.primary->hand_over = boost::none;
            }
        }
    }

    return new_c;
}

/* `calculate_all_contracts()` is sort of like `calculate_contract()` except that it
applies to the whole set of contracts instead of to a single contract. It takes the
inputs that `calculate_contract()` needs, but in sharded form; then breaks the key space
into small enough chunks that the inputs are homogeneous across each chunk; then calls
`calculate_contract()` on each chunk.

The output is in the form of a diff instead of a set of new contracts. We need a diff to
put in the `table_raft_state_t::change_t::new_contracts_t`, and we need to compute the
diff anyway in order to reuse contract IDs for contracts that haven't changed, so it
makes sense to combine those two diff processes. */
void calculate_all_contracts(
        const table_raft_state_t &old_state,
        watchable_map_t<std::pair<server_id_t, contract_id_t>, contract_ack_t> *acks,
        watchable_map_t<std::pair<server_id_t, server_id_t>, empty_value_t>
            *connections_map,
        const std::string &log_prefix,
        std::set<contract_id_t> *remove_contracts_out,
        std::map<contract_id_t, std::pair<region_t, contract_t> > *add_contracts_out,
        std::map<region_t, branch_id_t> *register_current_branches_out) {

    ASSERT_FINITE_CORO_WAITING;

    std::vector<region_t> new_contract_region_vector;
    std::vector<contract_t> new_contract_vector;

    /* We want to break the key-space into sub-regions small enough that the contract,
    table config, and ack versions are all constant across the sub-region. First we
    iterate over all contracts: */
    for (const std::pair<contract_id_t, std::pair<region_t, contract_t> > &cpair :
            old_state.contracts) {
        /* Next iterate over all shards of the table config and find the ones that
        overlap the contract in question: */
        for (size_t shard_index = 0; shard_index < old_state.config.config.shards.size();
                ++shard_index) {
            region_t region = region_intersection(
                cpair.second.first,
                region_t(old_state.config.shard_scheme.get_shard_range(shard_index)));
            if (region_is_empty(region)) {
                continue;
            }
            /* Now collect the acks for this contract into `ack_frags`. `ack_frags` is
            homogeneous at first and then it gets fragmented as we iterate over `acks`.
            */
            region_map_t<std::map<server_id_t, contract_ack_frag_t> > frags_by_server(
                region);
            acks->read_all([&](
                    const std::pair<server_id_t, contract_id_t> &key,
                    const contract_ack_t *value) {
                if (key.second != cpair.first) {
                    return;
                }
                region_map_t<contract_ack_frag_t> frags = break_ack_into_fragments(
                    region, *value, old_state.current_branches,
                    &old_state.branch_history);
                frags.visit(region,
                [&](const region_t &reg, const contract_ack_frag_t &frag) {
                    frags_by_server.visit_mutable(reg,
                    [&](const region_t &,
                            std::map<server_id_t, contract_ack_frag_t> *acks_map) {
                        auto res = acks_map->insert(std::make_pair(key.first, frag));
                        guarantee(res.second);
                    });
                });
            });
            size_t subshard_index = 0;
            frags_by_server.visit(region,
            [&](const region_t &reg,
                    const std::map<server_id_t, contract_ack_frag_t> &acks_map) {
                /* We've finally collected all the inputs to `calculate_contract()` and
                broken the key space into regions across which the inputs are
                homogeneous. So now we can actually call it. */

                /* Compute a shard identifier for logging, of the form:
                    "shard <user shard>.<subshard>.<hash shard>"
                This relies on the fact that `visit()` goes first in subshard order and
                then in hash shard order; it increments `subshard_index` whenever it
                finds a region with `reg.beg` equal to zero. */
                std::string log_subprefix;
                if (!log_prefix.empty()) {
                    log_subprefix = strprintf("%s: shard %zu.%zu.%d",
                        log_prefix.c_str(),
                        shard_index, subshard_index, get_cpu_shard_approx_number(reg));
                    if (reg.end == HASH_REGION_HASH_SIZE) {
                        ++subshard_index;
                    }
                }

                const contract_t &old_contract = cpair.second.second;

                contract_t new_contract = calculate_contract(
                    old_contract,
                    old_state.config.config.shards[shard_index],
                    acks_map,
                    connections_map,
                    log_subprefix);

                /* Register a branch if a primary is asking us to */
                if (static_cast<bool>(old_contract.primary) &&
                        static_cast<bool>(new_contract.primary) &&
                        old_contract.primary->server ==
                            new_contract.primary->server &&
                        acks_map.count(old_contract.primary->server) == 1 &&
                        acks_map.at(old_contract.primary->server).state ==
                            contract_ack_t::state_t::primary_need_branch) {
                    auto res = register_current_branches_out->insert(
                        std::make_pair(
                            reg,
                            *acks_map.at(old_contract.primary->server).branch));
                    guarantee(res.second);
                }

                new_contract_region_vector.push_back(reg);
                new_contract_vector.push_back(new_contract);
            });
        }
    }

    /* Put the new contracts into a `region_map_t` to coalesce adjacent regions that have
    identical contracts */
    region_map_t<contract_t> new_contract_region_map =
        region_map_t<contract_t>::from_unordered_fragments(
            std::move(new_contract_region_vector), std::move(new_contract_vector));

    /* Slice the new contracts by CPU shard and by user shard, so that no contract spans
    more than one CPU shard or user shard. */
    std::map<region_t, contract_t> new_contract_map;
    for (size_t cpu = 0; cpu < CPU_SHARDING_FACTOR; ++cpu) {
        region_t region = cpu_sharding_subspace(cpu);
        for (size_t shard = 0; shard < old_state.config.config.shards.size(); ++shard) {
            region.inner = old_state.config.shard_scheme.get_shard_range(shard);
            new_contract_region_map.visit(region,
            [&](const region_t &reg, const contract_t &contract) {
                guarantee(reg.beg == region.beg && reg.end == region.end);
                new_contract_map.insert(std::make_pair(reg, contract));
            });
        }
    }

    /* Diff the new contracts against the old contracts */
    for (const auto &cpair : old_state.contracts) {
        auto it = new_contract_map.find(cpair.second.first);
        if (it != new_contract_map.end() && it->second == cpair.second.second) {
            /* The contract was unchanged. Remove it from `new_contract_map` to signal
            that we don't need to assign it a new ID. */
            new_contract_map.erase(it);
        } else {
            /* The contract was changed. So delete the old one. */
            remove_contracts_out->insert(cpair.first);
        }
    }
    for (const auto &pair : new_contract_map) {
        /* The contracts remaining in `new_contract_map` are actually new; whatever
        contracts used to cover their region have been deleted. So assign them contract
        IDs and export them. */
        add_contracts_out->insert(std::make_pair(generate_uuid(), pair));
    }
}
