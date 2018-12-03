#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

#include <tcp/tcp.h>

#include "derecho_exception.h"
#include "derecho_internal.h"
#include "persistence_manager.h"
#include "raw_subgroup.h"
#include "replicated.h"
#include "rpc_manager.h"
#include "subgroup_info.h"
#include "view_manager.h"
#include "group_admin.h"

#include "conf/conf.hpp"
#include "mutils-containers/KindMap.hpp"
#include "mutils-containers/TypeMap2.hpp"
#include "spdlog/spdlog.h"

namespace derecho {
/**
 * The function that implements index_of_type, which is separate to hide the
 * "counter" template parameter (an implementation detail only used to maintain
 * state across recursive calls).
 */
template <uint32_t counter, typename TargetType, typename FirstType, typename... RestTypes>
constexpr uint32_t index_of_type_impl() {
    if constexpr(std::is_same<TargetType, FirstType>::value)
        return counter;
    else
        return index_of_type_impl<counter + 1, TargetType, RestTypes...>();
}

/**
 * A compile-time "function" that computes the index of a type within a template
 * parameter pack of types. The value of this constant is equal to the index of
 * TargetType within TypePack. (Behavior is undefined if TargetType is not
 * actually in TypePack).
 * @tparam TargetType The type to search for in the parameter pack
 * @tparam TypePack The template parameter pack that should be searched
 */
template <typename TargetType, typename... TypePack>
constexpr inline uint32_t index_of_type = index_of_type_impl<0, TargetType, TypePack...>();

//Type alias for a sparse-vector of Replicated, otherwise KindMap can't understand it's a template
template <typename T>
using replicated_index_map = std::map<uint32_t, Replicated<T>>;

class _Group {
private:
public:
    virtual ~_Group() = default;
    template <typename SubgroupType>
    auto& get_subgroup(uint32_t subgroup_num = 0);
};

template <typename ReplicatedType>
class GroupProjection : public virtual _Group {
protected:
    virtual void set_replicated_pointer(std::type_index, uint32_t, void**) = 0;

public:
    Replicated<ReplicatedType>& get_subgroup(uint32_t subgroup_num = 0);
};

template <>
class GroupProjection<RawObject> : public virtual _Group {
protected:
    virtual void set_replicated_pointer(std::type_index, uint32_t, void**) = 0;

public:
    RawSubgroup& get_subgroup(uint32_t subgroup_num = 0);
};

class GroupReference {
public:
    _Group* group;
    uint32_t subgroup_index;
    void set_group_pointers(_Group* group, uint32_t subgroup_index) {
        this->group = group;
        this->subgroup_index = subgroup_index;
    }
};

/**
 * Group is always a partial specialization of a template, because the first
 * Replicated Type must always be GroupAdmin. This unspecialized "base template"
 * is never defined but must be declared.
 */
template <typename GroupAdmin, typename...>
class Group;

/**
 * The top-level object for creating a Derecho group. This implements the group
 * management service (GMS) features and contains a MulticastGroup instance that
 * manages the actual sending and tracking of messages within the group.
 * @tparam ReplicatedTypes The types of user-provided objects that will represent
 * state and RPC functions for subgroups of this group.
 */
template <typename ExtendedAdmin, typename... ReplicatedTypes>
class Group : public virtual _Group, public GroupProjection<RawObject>, public GroupProjection<GroupAdmin>, public GroupProjection<ReplicatedTypes>... {
public:
    static_assert(std::is_base_of<GroupAdmin, ExtendedAdmin>::value,"Error: First parameter must extend GroupAdmin");
    void set_replicated_pointer(std::type_index type, uint32_t subgroup_num, void** ret);

private:
    using pred_handle = sst::Predicates<DerechoSST>::pred_handle;

    //Same thing for a sparse-vector of ExternalCaller
    template <typename T>
    using external_caller_index_map = std::map<uint32_t, ExternalCaller<T>>;

#ifndef NOLOG
    std::shared_ptr<spdlog::logger> logger;
#endif

    const node_id_t my_id;
    bool is_starting_leader;
    std::optional<tcp::socket> leader_connection;
    /** Persist the objects. Once persisted, persistence_manager updates the SST
     * so that the persistent progress is known by group members. */
    PersistenceManager<ExtendedAdmin, ReplicatedTypes...> persistence_manager;
    /** Contains a TCP connection to each member of the group, for the purpose
     * of transferring state information to new members during a view change.
     * This connection pool is shared between Group and ViewManager */
    std::shared_ptr<tcp::tcp_connections> tcp_sockets;
    /** Contains all state related to receiving and handling RPC function
     * calls for any Replicated objects implemented by this group. */
    rpc::RPCManager rpc_manager;
    std::unique_ptr<std::unique_ptr<ExtendedAdmin>>& admin_object_ptr;
    /** Contains all state related to managing Views, including the
     * MulticastGroup and SST (since those change when the view changes). */
    ViewManager view_manager;
    /** Maps a type to the Factory for that type. */
    mutils::KindMap<Factory, ExtendedAdmin, ReplicatedTypes...> factories;
    /** Maps each type T to a map of (index -> Replicated<T>) for that type's
     * subgroup(s). If this node is not a member of a subgroup for a type, the
     * map will have no entry for that type and index. (Instead, external_callers
     * will have an entry for that type-index pair). If this node is a member
     * of a subgroup, the Replicated<T> will refer to the one shard that this
     * node belongs to. */
    mutils::KindMap<replicated_index_map, ExtendedAdmin, ReplicatedTypes...> replicated_objects;
    /** Maps subgroup index -> RawSubgroup for the subgroups of type RawObject.
     * If this node is not a member of RawObject subgroup i, the RawSubgroup at
     * index i will be invalid; otherwise, the RawObject will refer to the one
     * shard of that subgroup that this node belongs to. */
    std::vector<RawSubgroup> raw_subgroups;
    /** Maps each type T to a map of (index -> ExternalCaller<T>) for the
     * subgroup(s) of that type that this node is not a member of. The
     * ExternalCaller for subgroup i of type T can be used to contact any member
     * of any shard of that subgroup, so shards are not indexed. */
    mutils::KindMap<external_caller_index_map, ReplicatedTypes...> external_callers;
    /** Alternate view of the Replicated<T>s, indexed by subgroup ID. The entry at
     * index X is a reference to the Replicated<T> for this node's shard of
     * subgroup X, which may or may not be valid. The references are the abstract
     * base type ReplicatedObject because they are only used for send/receive_object.
     * Note that this is a std::map solely so that we can initialize it out-of-order;
     * its keys are continuous integers starting at 0 and it should be a std::vector. */
    std::map<subgroup_id_t, std::reference_wrapper<ReplicatedObject>> objects_by_subgroup_id;

    /* get_subgroup is actually implemented in these two methods. This is an
     * ugly hack to allow us to specialize get_subgroup<RawObject> to behave differently than
     * get_subgroup<T>. The unnecessary unused parameter is for overload selection. */
    template <typename SubgroupType>
    Replicated<SubgroupType>& get_subgroup(SubgroupType*, uint32_t subgroup_index);
    RawSubgroup& get_subgroup(RawObject*, uint32_t subgroup_index);

    /** Type of a 2-dimensional vector used to store potential node IDs, or -1 */
    using vector_int64_2d = std::vector<std::vector<int64_t>>;

    /** Deserializes a vector of shard leader IDs sent over the given socket. */
    static std::unique_ptr<vector_int64_2d> receive_old_shard_leaders(tcp::socket& leader_socket);

#ifndef NOLOG
    /**
     * Constructs a spdlog::logger instance and registers it in spdlog's global
     * registry, so that dependent objects like ViewManager can retrieve a
     * pointer to the same logger without needing to pass it around.
     * @return A pointer to the logger that was created.
     */
    std::shared_ptr<spdlog::logger> create_logger() const;
#endif
    /**
     * Updates the state of the replicated objects that correspond to subgroups
     * identified in the provided map, by receiving serialized state from the
     * shard leader whose ID is paired with that subgroup ID.
     * @param subgroups_and_leaders Pairs of (subgroup ID, leader's node ID) for
     * subgroups that need to have their state initialized from the leader.
     */
    void receive_objects(const std::set<std::pair<subgroup_id_t, node_id_t>>& subgroups_and_leaders);

    /** A new-view callback that adds and removes TCP connections from the pool
     * of long-standing TCP connections to each member (used mostly by RPCManager). */
    void update_tcp_connections_callback(const View& new_view);

    std::unique_ptr<std::unique_ptr<ExtendedAdmin>>& construct_admin_object(Factory<ExtendedAdmin>& admin_factory);

    /** Constructor helper that wires together the component objects of Group. */
    void set_up_components();

    /**
     * Constructor helper that constructs RawSubgroup objects for each subgroup
     * of type RawObject; called to initialize the raw_subgroups map.
     * @param curr_view A reference to the current view as reported by View_manager
     * @return A vector containing a RawSubgroup for each "raw" subgroup the user
     * requested, at the index corresponding to that subgroup's index.
     */
    std::vector<RawSubgroup> construct_raw_subgroups(const View& curr_view);

    /**
     * Base case for the construct_objects template. Note that the neat "varargs
     * trick" (defining construct_objects(...) as the base case) doesn't work
     * because varargs can't match const references, and will force a copy
     * constructor on View. So std::enable_if is the only way to match an empty
     * template pack.
     */
    template <typename... Empty>
    std::enable_if_t<0 == sizeof...(Empty),
                            std::set<std::pair<subgroup_id_t, node_id_t>>>
    construct_objects(const View&, const vector_int64_2d&) {
        return std::set<std::pair<subgroup_id_t, node_id_t>>();
    }

    /**
     * Constructor helper that unpacks this Group's template parameter pack.
     * Constructs Replicated<T> wrappers for each object being replicated,
     * using the corresponding Factory<T> saved in Group::factories. If this
     * node is not a member of the subgroup for a type T, an "empty" Replicated<T>
     * will be constructed with no corresponding object. If this node is joining
     * an existing group and there was a previous leader for its shard of a
     * subgroup, an "empty" Replicated<T> will also be constructed for that
     * subgroup, since all object state will be received from the shard leader.
     *
     * @param curr_view A reference to the current view as reported by View_manager
     * @param old_shard_leaders The array of old shard leaders for each subgroup
     * (indexed by subgroup ID), which will contain -1 if there is no previous
     * leader for that shard.
     * @return The set of subgroup IDs that are un-initialized because this node is
     * joining an existing group and needs to receive initial object state, paired
     * with the ID of the node that should be contacted to receive that state.
     */
    template <typename FirstType, typename... RestTypes>
    std::set<std::pair<subgroup_id_t, node_id_t>> construct_objects(
            const View& curr_view, const vector_int64_2d& old_shard_leaders);

public:
    /**
     * Constructor that starts a new Derecho group. This can be called by both
     * the initial group leader and non-leader nodes, and the configuration
     * parameters (specified in the Derecho config file) will be used to
     * determine whether this node runs as a leader or non-leader.
     *
     * @param callbacks The set of callback functions for message delivery
     * events in this group.
     * @param subgroup_info The set of functions that define how membership in
     * each subgroup and shard will be determined in this group.
     * @param _view_upcalls A list of functions to be called when the group
     * experiences a View-Change event (optional).
     * @param factories A variable number of Factory functions, one for each
     * template parameter of Group, providing a way to construct instances of
     * each Replicated Object
     */
    Group(const CallbackSet& callbacks,
          const SubgroupInfo& subgroup_info,
          std::vector<view_upcall_t> _view_upcalls = {},
          Factory<ExtendedAdmin> admin_factory,
          Factory<ReplicatedTypes>... factories);

    ~Group();

    /**
     * Gets the "handle" for the subgroup of the specified type and index (which
     * is either a Replicated<T> or a RawSubgroup) assuming this node is a
     * member of the desired subgroup. If the subgroup has type RawObject, the
     * return value will be a RawSubgroup; otherwise it will be a Replicated<T>.
     * The Replicated<T> will contain the replicated state of an object of type
     * T and be usable to send multicasts to this node's shard of the subgroup.
     *
     * @param subgroup_index The index of the subgroup within the set of
     * subgroups that replicate the same type of object. Defaults to 0, so
     * if there is only one subgroup of type T, it can be retrieved with
     * get_subgroup<T>();
     * @tparam SubgroupType The object type identifying the subgroup
     * @return A reference to either a Replicated<SubgroupType> or a RawSubgroup
     * for this subgroup
     * @throws subgroup_provisioning_exception If there are no subgroups because
     * the current View is inadequately provisioned
     * @throws invalid_subgroup_exception If this node is not a member of the
     * requested subgroup.
     */
    template <typename SubgroupType>
    auto& get_subgroup(uint32_t subgroup_index = 0);

    /**
     * Gets the "handle" for a subgroup of the specified type and index,
     * assuming this node is not a member of the subgroup. The returned
     * ExternalCaller can be used to make peer-to-peer RPC calls to a specific
     * member of the subgroup.
     *
     * @param subgroup_index The index of the subgroup within the set of
     * subgroups that replicate the same type of object.
     * @tparam SubgroupType The object type identifying the subgroup
     * @return A reference to the ExternalCaller for this subgroup
     * @throws invalid_subgroup_exception If this node is actually a member of
     * the requested subgroup, or if no such subgroup exists
     */
    template <typename SubgroupType>
    ExternalCaller<SubgroupType>& get_nonmember_subgroup(uint32_t subgroup_index = 0);

    template <typename SubgroupType>
    ShardIterator<SubgroupType> get_shard_iterator(uint32_t subgroup_index = 0);

    /** Causes this node to cleanly leave the group by setting itself to "failed." */
    void leave();
    /** Creates and returns a vector listing the nodes that are currently members of the group. */
    std::vector<node_id_t> get_members();

    /** Reports to the GMS that the given node has failed. */
    void report_failure(const node_id_t who);
    /** Waits until all members of the group have called this function. */
    void barrier_sync();
    void debug_print_status() const;

#ifndef NOLOG
    void log_event(const std::string& event_text) {
        SPDLOG_DEBUG(logger, event_text);
    }
#endif
};

} /* namespace derecho */

#include "group_impl.h"
