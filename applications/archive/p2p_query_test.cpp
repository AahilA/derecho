#include <iostream>
#include <sstream>

#include "derecho/derecho.h"
#include "test_objects.h"
#include "conf/conf.hpp"

using derecho::ExternalCaller;
using derecho::Replicated;
using std::cout;
using std::endl;
using namespace persistent;

int main(int argc, char** argv) {
    derecho::Conf::initialize(argc, argv);
    derecho::SubgroupInfo subgroup_info{
            {{std::type_index(typeid(Foo)), [](const derecho::View& curr_view, int& next_unassigned_rank) {
                  if(curr_view.num_members < 3) {
                      std::cout << "Foo function throwing subgroup_provisioning_exception" << std::endl;
                      throw derecho::subgroup_provisioning_exception();
                  }
                  derecho::subgroup_shard_layout_t subgroup_vector(1);
                  std::vector<node_id_t> first_3_nodes(&curr_view.members[0], &curr_view.members[0] + 3);
                  //Put the desired SubView at subgroup_vector[0][0] since there's one subgroup with one shard
                  subgroup_vector[0].emplace_back(curr_view.make_subview(first_3_nodes));
                  next_unassigned_rank = std::max(next_unassigned_rank, 3);
                  cout << "Foo function setting next_unassigned_rank to " << next_unassigned_rank << endl;
                  return subgroup_vector;
              }},
             {std::type_index(typeid(Bar)), [](const derecho::View& curr_view, int& next_unassigned_rank) {
                  if(curr_view.num_members < 3) {
                      std::cout << "Bar function throwing subgroup_provisioning_exception" << std::endl;
                      throw derecho::subgroup_provisioning_exception();
                  }
                  derecho::subgroup_shard_layout_t subgroup_vector(1);
                  std::vector<node_id_t> first_3_nodes(&curr_view.members[0], &curr_view.members[0] + 3);
                  subgroup_vector[0].emplace_back(curr_view.make_subview(first_3_nodes));
                  next_unassigned_rank = std::max(next_unassigned_rank, 3);
                  cout << "Bar function setting next_unassigned_rank to " << next_unassigned_rank << endl;
                  return subgroup_vector;
              }},
             {std::type_index(typeid(Cache)), [](const derecho::View& curr_view, int& next_unassigned_rank) {
                  if(curr_view.num_members < 6) {
                      std::cout << "Cache function throwing subgroup_provisioning_exception" << std::endl;
                      throw derecho::subgroup_provisioning_exception();
                  }
                  derecho::subgroup_shard_layout_t subgroup_vector(1);
                  std::vector<node_id_t> next_3_nodes(&curr_view.members[3], &curr_view.members[3] + 3);
                  subgroup_vector[0].emplace_back(curr_view.make_subview(next_3_nodes));
                  next_unassigned_rank = std::max(next_unassigned_rank, 5);
                  cout << "Cache function setting next_unassigned_rank to " << next_unassigned_rank << endl;
                  return subgroup_vector;
              }}},
            {std::type_index(typeid(Foo)), std::type_index(typeid(Bar)), std::type_index(typeid(Cache))}};

    //Each replicated type needs a factory; this can be used to supply constructor arguments
    //for the subgroup's initial state
    auto foo_factory = [](PersistentRegistry*) { return std::make_unique<Foo>(-1); };
    auto bar_factory = [](PersistentRegistry*) { return std::make_unique<Bar>(); };
    auto cache_factory = [](PersistentRegistry*) { return std::make_unique<Cache>(); };

    derecho::Group<Foo, Bar, Cache> group(
            {}, subgroup_info,
            std::vector<derecho::view_upcall_t>{},
            foo_factory, bar_factory, cache_factory);

    cout << "Finished constructing/joining Group" << endl;

    std::vector<node_id_t> members = group.get_members();
    const uint32_t node_id = derecho::getConfUInt32(CONF_DERECHO_LOCAL_ID);
    uint my_rank;
    for(my_rank = 0; my_rank < members.size(); ++my_rank) {
        if(members[my_rank] == node_id) {
            break;
        }
    }
    if(my_rank < 3) {
        Replicated<Foo>& foo_rpc_handle = group.get_subgroup<Foo>();
        Replicated<Bar>& bar_rpc_handle = group.get_subgroup<Bar>();
        //Make sure Foo and Bar have some initial state before doing P2P
        derecho::rpc::QueryResults<bool> results = foo_rpc_handle.ordered_send<RPC_NAME(change_state)>(99);
        bool results_total = true;
        for(auto& reply_pair : results.get()) {
            results_total = results_total && reply_pair.second.get();
        }
        bar_rpc_handle.ordered_send<RPC_NAME(append)>("Some stuff for Bar. ");
        group.barrier_sync();
        ExternalCaller<Cache>& cache_p2p_handle = group.get_nonmember_subgroup<Cache>();
        int p2p_target = 4;
        derecho::rpc::QueryResults<std::string> result = cache_p2p_handle.p2p_query<RPC_NAME(get)>(p2p_target, "Stuff");
        std::string response = result.get().get(p2p_target);
        cout << "Node " << p2p_target << " had cache entry Stuff = " << response << endl;
    } else {
        Replicated<Cache>& cache_rpc_handle = group.get_subgroup<Cache>();
        //Make sure Cache has some initial state
        cache_rpc_handle.ordered_send<RPC_NAME(put)>("Stuff", "Things");
        //This will wait for the Foo and Bar nodes
        if(node_id != 7) {
            group.barrier_sync();
        }
        if(node_id == 5) {
            return 0;
        }
        ExternalCaller<Foo>& foo_p2p_handle = group.get_nonmember_subgroup<Foo>();
        int foo_p2p_target = 1;
        derecho::rpc::QueryResults<int> foo_result = foo_p2p_handle.p2p_query<RPC_NAME(read_state)>(foo_p2p_target);
        cout << "Node " << foo_p2p_target << " returned Foo state = " << foo_result.get().get(foo_p2p_target) << endl;
        ExternalCaller<Bar>& bar_p2p_handle = group.get_nonmember_subgroup<Bar>();
        int bar_p2p_target = 0;
        derecho::rpc::QueryResults<std::string> bar_result = bar_p2p_handle.p2p_query<RPC_NAME(print)>(bar_p2p_target);
        cout << "Node " << bar_p2p_target << " has Bar log = " << bar_result.get().get(bar_p2p_target) << endl;
    }

    cout << "Reached end of main(), entering infinite loop so program doesn't exit" << std::endl;
    while(true) {
    }
}
