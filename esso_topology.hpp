#ifndef ESSO_TOPOLOGY_H
#define ESSO_TOPOLOGY_H

#include <vector>
#include <iostream>
#include <memory>
#include <cmath>

#include "iz_topology.hpp"

using namespace std;

struct esso_node {
  int id;
  double base_power;
  double sleep_power;
  bool is_active;

  esso_node(int id, double base_power, double sleep_power) : 
      id(id), base_power(base_power), 
      sleep_power(sleep_power), is_active(false) {}
  
  virtual double get_power() {return 0.0;}
};

class esso_server : public esso_node {
public:
  int cpu_capacity;
  int cpu_residual;
  double per_cpu_power;

  esso_server(int id, int cpu_capacity, double per_cpu_power, 
      double base_power, double sleep_power) :
      esso_node(id, base_power, sleep_power), 
    cpu_capacity(cpu_capacity), cpu_residual(cpu_capacity), 
    per_cpu_power(per_cpu_power) {}

  void allocate_cpu(const int cpu_count) {
    if (!is_active) is_active = true;
    cpu_residual -= cpu_count;
    assert(cpu_residual >= 0);
  }

  void release_cpu(const int cpu_count) {
    cpu_residual += cpu_count;
    if (cpu_residual == cpu_capacity) is_active = false;
    assert(cpu_residual <= cpu_capacity);
  }

  double get_power() {
    if (!is_active) return sleep_power;
    return base_power + (cpu_capacity - cpu_residual) * per_cpu_power;
  }
};

struct esso_switch : public esso_node {

  esso_switch(int id, double base_power, double sleep_power) :
    esso_node(id, base_power, sleep_power) {}
  
  double get_port_power(int bandwidth) {
    return 0.003;
  }

  double get_power() {
    if (!is_active) return sleep_power;
    double total_power{base_power};
    //for (auto& arc : arcs) {
    //  totalPower += getPortPower(
    //    arc.bandwidthCapacity-arc.bandwidthResidual);
    //}
    return total_power;
  }
};

struct esso_co {
  int id;
  // renewable energy capacity and residual per timeslot
  vector<double> green_capacity;
  vector<double> green_residual;
  // carbon footpring pet kWh
  double carbon;

  vector<shared_ptr<esso_node>> intra_nodes;
  izlib::iz_topology intra_topo;
  // node 0 is always the gateway/border-router/top-switch
  int border_router;
  std::vector<int> server_ids; // these are the servers
  
  /*
  struct server_info {
    int id;
    int cpu_residual;
    server_info(int id, int cpu_residual) :
        id(id), cpu_residual(cpu_residual) {}
    bool operator<(const server_info& rhs) {
      return cpu_residual < rhs.cpu_residual;
    }
  };
  std::vector<server_info> server_list;
  std::vector<int> server_id_info_map;
  */

  void add_server_info(const int server_id) {
    server_ids.push_back(server_id);
    //server_id_info_map
    //server_list.emplace_back(server_id, 
    //    std::dynamic_pointer_cast<esso_server>(
    //    intra_nodes[server_id])->cpu_residual);
  }
  
  esso_co(int id, vector<double>& green_capacity, double carbon) :
    id(id), green_capacity(green_capacity), green_residual(green_capacity),
    carbon(carbon), border_router{0} {
    // create the internal network of one core switch, two aggregate
    // switch, four tor switch, and five servers (all in idle state) 
    int node_count = 27;
    intra_topo.init(node_count);
    int core_switch = add_switch();
    // add aggregate switches and link to core switch
    vector<int> aggr_switches(2);
    for (auto& agsw : aggr_switches) {
      agsw = add_switch();
      add_intra_edge(core_switch, agsw, 1, 100000);
    }
    // add tor switches and link them to the aggregate switches
    vector<int> tor_switches(4);
    for (auto& trsw : tor_switches) {
      trsw = add_switch();
      for (const auto& agsw : aggr_switches) {
        add_intra_edge(agsw, trsw, 1, 25000);
      } 
    }
    for (const auto& trsw : tor_switches) {
      add_servers_to_switch(trsw);
    }
  }

  int get_residual_cpu(const int server_id) {
    return dynamic_pointer_cast<esso_server>(
        intra_nodes[server_id])->cpu_residual;
  }

  void allocate_cpu(const int server_id, const int cpu_count) {
    std::dynamic_pointer_cast<esso_server>(
        intra_nodes[server_id])->allocate_cpu(cpu_count);
  }
  void release_cpu(const int server_id, const int cpu_count) {
    std::dynamic_pointer_cast<esso_server>(
        intra_nodes[server_id])->release_cpu(cpu_count);
  }

  void allocate_bandwidth(const izlib::iz_path& path, 
      const int bandwidth) {
    for(auto& edge : intra_topo.path_edges(path)) {
      intra_topo.allocate_bandwidth(edge.u, edge.v, bandwidth);
    }
  }
  void release_bandwidth(const izlib::iz_path& path, 
      const int bandwidth) {
    for(auto& edge : intra_topo.path_edges(path)) {
      intra_topo.release_bandwidth(edge.u, edge.v, bandwidth);
    }
  }

  double get_carbon_fp(int time_slot) const {
    double brown_power{0.0};
    for (const auto& node : intra_nodes) {
        brown_power += node->get_power();
    }
    for (const auto& edge : intra_topo.edges(0)) {
      if (edge.consumed_bandwidth() > 0) {
        brown_power += 0.3; // power per port
      }
    }
    brown_power = max(0.0, brown_power - green_capacity[time_slot]);
    return brown_power * carbon;
  }

  int add_server(int cpu_capacity = 16, double per_cpu_power = 0.165, 
                 double base_power = 0.0805, double sleep_power = 0.02415) {
    shared_ptr<esso_node> ptr = make_shared<esso_server>(intra_nodes.size(), 
        cpu_capacity, per_cpu_power, base_power, sleep_power);
    intra_nodes.push_back(ptr);
    return intra_nodes.back()->id;
  }

  int add_switch(double base_power = 0.25, double sleep_power = 0.08) {
    shared_ptr<esso_node> ptr = make_shared<esso_switch>(intra_nodes.size(), 
        base_power, sleep_power);
    intra_nodes.push_back(ptr);
    return intra_nodes.back()->id;
  }

  void add_intra_edge(int u, int v, int latency, int capacity) {
    intra_topo.add_edge(u, v, latency, capacity);
  }

  // add count number of servers to the switch with switch_id
  // using the default parameters for each server
  void add_servers_to_switch(int switch_id, int count = 5) {
    for (int i = 0; i < count; ++i) {
      int server_id = add_server();
      add_server_info(server_id);
      add_intra_edge(switch_id, server_id, 1, 10000); //10Gbps link
    }
  }

  void compute_embedding_cost(vector<int> cpu_reqs, 
    int bandwidth, int time_slot, vector<vector<double>>& cost_matrix) {

    // cost_matrix is used to hold the cost of all partial 
    // allocations
    cost_matrix.resize(cpu_reqs.size(), vector<double>(cpu_reqs.size(), -1));

    // used to save info about pseudo allocation
    vector<pair<int, int>> pseudo_cpu_alloc;
    vector<pair<izlib::iz_path, int>> pseudo_bandwidth_alloc;

    // temp variables
    izlib::iz_path f_path, r_path;

    // current cost
    double cost_before_alloc {get_carbon_fp(time_slot)}, new_cost;

    // loop to generate all possile partial embeddings
    for (size_t i = 0; i < cpu_reqs.size(); ++i) {
      // for each j, we can build on the previously computed cost
      // so pseudo alloc vectors are cleared only here
      pseudo_cpu_alloc.clear();
      pseudo_bandwidth_alloc.clear();
      // each j represent a new partial chain embedding, so
      // last_node is initialized to border_router
      int last_node{border_router};
      // loop to generate all possible partial embeddings
      for (size_t j = i; j < cpu_reqs.size(); ++j) {
        //embedding_found = true;
        // if the last node is not the border router then 
        // we have embedded at least one vnf, then the last
        // server is out first candidate
        vector<int> candidate_servers;
        if (last_node != border_router && 
            get_residual_cpu(last_node) >= cpu_reqs[j]) {
          candidate_servers.push_back(last_node);
        }
        // now we filter all other servers with enough capacity
        copy_if(server_ids.begin(), server_ids.end(),
            back_inserter(candidate_servers), 
            [&] (int server_id) {
                return get_residual_cpu(server_id) >= cpu_reqs[j];
            });
        if (candidate_servers.empty()) {
          break;
        }
        // for each candidate try to find a forward and return path
        // if both found then we have a valid embedding
        int server_id{-1};
        for (const auto& cs : candidate_servers) {
          // path from last node to candidate server
          if (cs != last_node) {
            intra_topo.shortest_path(last_node, cs, f_path, bandwidth);
            if (!f_path.is_valid()) continue;
          }
          // return path from candidate server to border router
          intra_topo.shortest_path(cs, border_router, r_path, bandwidth);
          if (!r_path.is_valid()) continue;
          // embedding found
          server_id = cs;
          break;
        }
        // if no server has a path with enough bandwidth then 
        // server_id will be -1.
        if (server_id == -1) {
          break;
        }
        // allocate server resources
        allocate_cpu(server_id, cpu_reqs[j]);
        pseudo_cpu_alloc.push_back(make_pair(server_id, cpu_reqs[j]));
        // allocate bandwidth resources
        if (last_node != server_id) {
          allocate_bandwidth(f_path, bandwidth);
          pseudo_bandwidth_alloc.push_back(make_pair(f_path, bandwidth));  
        }
        allocate_bandwidth(r_path, bandwidth);
        pseudo_bandwidth_alloc.push_back(make_pair(r_path, bandwidth));
        // now calculate the cost and update cost matrix
        new_cost = get_carbon_fp(time_slot);
        cost_matrix[i][j] = new_cost - cost_before_alloc;
        // remove and release the bandwidth for the return path
        auto return_path_pair = pseudo_bandwidth_alloc.back();
        pseudo_bandwidth_alloc.pop_back();
        release_bandwidth(return_path_pair.first, return_path_pair.second);
        // update last_node for the next iteration
        last_node = server_id;
      } // end of j's loop
      // release the pseudo allocation resources
      for (auto& server_cpu : pseudo_cpu_alloc) {
        release_cpu(server_cpu.first, server_cpu.second);
      }
      for (auto& path_bandwidth : pseudo_bandwidth_alloc) {
        release_bandwidth(path_bandwidth.first, path_bandwidth.second);
      }
    } // end of i's loop
  }
};

struct esso_topology {
  vector<esso_co> cos; 
  izlib::iz_topology inter_co_topo;
  
  void init(int node_count) {
    inter_co_topo.init(node_count);
  }

  int add_co(vector<double>& green_capacity, double carbon = 1.2) {
    esso_co co(cos.size(), green_capacity, carbon);
    cos.emplace_back(co);
    return co.id;
  }

  void add_edge(int u, int v, int latency, int capacity) {
    inter_co_topo.add_edge(u, v, latency, capacity);
  }

  double get_carbon_fp(int time_slot) {
    double carbon{0};
    for(const auto& c : cos) {
      carbon += c.get_carbon_fp(time_slot);
    }
    return carbon;
  }
};


#endif
