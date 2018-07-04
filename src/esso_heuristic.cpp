#include <limits>

//#include "esso_heuristic.hpp"
#include "iz_topology.hpp"
#include "problem_instance.hpp"
#include "iz_timer.hpp"

using namespace std;
using namespace izlib;

template <typename T>
using matrix = vector<vector<T>>;

const char* join_path(string& dir, const string& file) {
  if (dir.back() != '/') dir += '/';
  return (dir+file).c_str();
}

iz_path stage_one(const sfc_request& sfc, const int k, const int timeslot,
    problem_instance& prob_inst) {
  // find path set
  iz_path_list paths;
  auto& inter_co_topo = prob_inst.topology.inter_co_topo;
  inter_co_topo.k_shortest_paths(sfc.ingress_co, sfc.egress_co, k, paths,
      sfc.bandwidth);
  
  // find the path with max energy
  int max_energy_path_index{-1};
  double max_energy{-1.0};
  const auto& cos = prob_inst.topology.cos;
  for (int i = 0; i < paths.size(); ++i) {
    const auto& path = paths[i];
    if (path.latency > sfc.latency) continue;
    double path_energy{0.0};
    for (auto& co_id : path.nodes) {
      path_energy += cos[co_id].green_residual[timeslot];
    }
    if (path_energy > max_energy) {
      max_energy_path_index = i;
      max_energy = path_energy;
    }
  }

  // return either the path with max energy or
  // return an empty path is all paths' latency
  // is greater than the sfc's latency bound
  iz_path path;
  if (max_energy_path_index != -1)
    path = move(paths[max_energy_path_index]);
  return path;
}

void stage_two(const int co_id, const sfc_request& sfc, const iz_path& path, 
    const int timeslot, 
    vector<vector<double>>& cost_matrix, vector<vector<int>>& node_matrix,
    problem_instance& prob_inst) {
  auto& cos = prob_inst.topology.cos;
  cos[co_id].compute_embedding_cost(sfc.cpu_reqs, sfc.bandwidth,
      timeslot, cost_matrix, node_matrix);
}

bool first_fit(const sfc_request& sfc, const iz_path& path, 
    const vector<vector<vector<double>>>& cost_matrices,
    vector<vector<int>>& embedding_table) {
  int next_vnf{0}, curr_co_idx{0};
  while (next_vnf < sfc.vnf_count && curr_co_idx < path.size()) {
    for (int i = sfc.vnf_count-1; i >= 0; --i) {
      if (cost_matrices[curr_co_idx][next_vnf][i] != -1) {
        for (int j = next_vnf; j <= i; ++j) {
          embedding_table[curr_co_idx][j] = 1;
        }
        next_vnf = i+1;
        break;
      }
    }
    if (next_vnf == sfc.vnf_count) return true;
    ++curr_co_idx;
  }
  return false;
}

// compute the embedding cost of an embedding table
double embedding_cost(const sfc_request& sfc, const iz_path& path,
    const vector<vector<vector<double>>>& cost_matrices,
    const vector<vector<int>>& embedding_table) {
  double cost{0.0};
  for (int c = 0; c < path.size(); ++c) {
    // find starting 1 in embedding table
    int sfc_start{0}, sfc_end{sfc.vnf_count-1};
    while(sfc_start < sfc.vnf_count && 
        embedding_table[c][sfc_start] == 0) 
      ++sfc_start;
    while(sfc_end >= 0 && 
        embedding_table[c][sfc_end] == 0)
      --sfc_end;
    if (sfc_start <= sfc_end) {
      cost += cost_matrices[c][sfc_start][sfc_end];
    }
  }
  return cost;
}

// returns the nodes selected in an embedding table
vector<int> embedding_nodes(const sfc_request& sfc, const iz_path& path,
    const vector<vector<vector<int>>>& node_matrices,
    const vector<vector<int>>& embedding_table,
    vector<int>& emb_cos, vector<int>& emb_co_nodes) {
  vector<int> nodes;
  for (int c = 0; c < path.size(); ++c) {
    // find starting 1 in embedding table
    int sfc_start{0}, sfc_end{sfc.vnf_count-1};
    while(sfc_start < sfc.vnf_count && 
        embedding_table[c][sfc_start] == 0) 
      ++sfc_start;
    while(sfc_end >= 0 && 
        embedding_table[c][sfc_end] == 0)
      --sfc_end;
    for (int i = sfc_start; i <= sfc_end; ++i) {
      nodes.push_back(path.nodes[c] * 9 + 
          node_matrices[c][sfc_start][i]);
      emb_cos.push_back(path.nodes[c]);
      emb_co_nodes.push_back(node_matrices[c][sfc_start][i]);
    }
  }
  return nodes;
}

// checks whether an embedding table is valid
bool is_valid_embedding(const sfc_request& sfc,const iz_path& path,
    const vector<vector<vector<double>>>& cost_matrices,
    const vector<vector<int>>& embedding_table) {
  // vnf_co stores the co_ids for the vnfs in the sfc
  // according to the provided embedding table
  vector<int> vnf_co(sfc.vnf_count);
  // loop for each vnf
  for (int vnf_idx = 0; vnf_idx < sfc.vnf_count; ++vnf_idx) {
    int sum{0}, non_zero_count{0};
    // chech each co for vnf vnf_inx
    for (int co_idx = 0; co_idx < path.size(); ++co_idx) {
      sum += embedding_table[co_idx][vnf_idx];
      if (embedding_table[co_idx][vnf_idx]) {
        ++non_zero_count;
        // here the entries in the vnf_co are updated to store
        // the co_ids on the path
        vnf_co[vnf_idx] = path.nodes[co_idx];
      }
    }
    // both sum and non_zero_count must be equal to one
    if (sum != 1 || non_zero_count != 1) return false;
  }

  // check to ensure vnf embedding always moves in the 
  // forward direction
  for (int v = 1; v < sfc.vnf_count; ++v) {
    if (vnf_co[v-1] > vnf_co[v]) {
      return false;
    }
  }

  // check to see if the 1's in the embedding table
  // are really valid costs in the cost_matrices
  for (int c = 0; c < path.size(); ++c) {
    // find starting 1 in embedding table
    int sfc_start{0}, sfc_end{sfc.vnf_count-1};
    while(sfc_start < sfc.vnf_count && 
        embedding_table[c][sfc_start] == 0) 
      ++sfc_start;
    // find ending 1 in the embedding table
    while(sfc_end >= 0 && 
        embedding_table[c][sfc_end] == 0)
      --sfc_end;
    if (sfc_start <= sfc_end) {
      if (cost_matrices[c][sfc_start][sfc_end] == -1)
        return false;
    }
  }


  return true;
}

bool tabu_search(const sfc_request& sfc, const iz_path& path,
    const vector<vector<vector<double>>>& cost_matrices,
    vector<vector<int>>& best_solution,
    double& best_solution_cost) {
    // initial solution from first-fit
    vector<vector<int>> current_solution = vector<vector<int>>(
        path.size(), vector<int>(sfc.vnf_count, 0));
    auto res = first_fit(sfc, path, cost_matrices, 
        current_solution);
    // if no fist-fit solution, then return false
    if (!res) return false;

    // overall best solution and cost
    best_solution_cost = -1.0;
    // set best solution = current solution
    best_solution = current_solution;
    best_solution_cost = embedding_cost(sfc, path, cost_matrices, 
        best_solution);

    // tabu search specific data strutures
    set<pair<int, int>> tabu_set;
    vector<vector<int>> tabu_timers(path.size(), 
        vector<int>(sfc.vnf_count, 0));
    const int tabu_period = 500;
    const int max_iterations = 100000;
    const int max_no_improvement_iterations = 1500;
    int best_cost_update_timestamp = 0;

    // variables for random number
    default_random_engine rnd_engine;
    uniform_int_distribution<> uni_dist(0,99);
    // main loop for tabu search
    int iter = 0;
    for (; iter < max_iterations; ++iter) {
      // datastructure to keep track of best neighbour
      double best_nbr_cost = numeric_limits<double>::max();
      vector<vector<int>> best_nbr;
      pair<int, int> potential_tabu_move(-1, -1);

      // generate neighbors to find the best neighbor for this iteration
      for (int j = 0; j < sfc.vnf_count; ++j) {
        // initialize the nbr solution with current solution
        auto nbr(current_solution);
        // now change one vnf assignment 
        // find the current index of co for vnf j
        int curr_co_idx{0}, next_co_idx{0};
        while (nbr[curr_co_idx][j] == 0 && 
            curr_co_idx < path.size()) 
          ++curr_co_idx;

        int rnd_num = uni_dist(rnd_engine);
        if (rnd_num%2) { // if odd then move up (-1)
          next_co_idx = (curr_co_idx + path.size() - 1) % path.size();
        }
        else { // even, so move down (+1)
          next_co_idx = (curr_co_idx + 1) % path.size();
        }
        // update current and next co assignment to generate the neighbor
        nbr[curr_co_idx][j] = 0;
        nbr[next_co_idx][j] = 1;

        // if nbr_solution is not valid then continue
        if (!is_valid_embedding(sfc, path, cost_matrices, nbr))
          continue;

        // find cost of neighbor solution
        auto nbr_cost = embedding_cost(sfc, path, cost_matrices, nbr);

        // update best neighbor and potential tabu move 
        if (nbr_cost < best_nbr_cost) {
          best_nbr_cost = nbr_cost;
          best_nbr = nbr;
          potential_tabu_move.first = j;
          potential_tabu_move.second = next_co_idx;
        }
      } // end of for loop for generating neigbor solutions

      // now, update best_solution with the best neighbor so far
      if (best_nbr_cost < best_solution_cost) {
        best_solution_cost = best_nbr_cost;
        best_solution = best_nbr;
        tabu_set.insert(potential_tabu_move);
        tabu_timers[potential_tabu_move.first][potential_tabu_move.second] =
            tabu_period;
        best_cost_update_timestamp = iter;
      }

      // update tabu list
      for(auto itr = tabu_set.begin(); itr != tabu_set.end();) {
        if (tabu_timers[itr->first][itr->second] > 0)
          --tabu_timers[itr->first][itr->second];
        if (tabu_timers[itr->first][itr->second] == 0)
          itr = tabu_set.erase(itr);
        else
          ++itr;
      }

      // if best cost is not updated in the last 
      // max_no_improvement_iterations then break
      if (iter - best_cost_update_timestamp > 
          max_no_improvement_iterations)
        break;

    } // end of tabu search iterations
    return true;
}

// build the entire inter + intra co topology 
// this topology is used to generate the final
// optimizer output from the embedding table
// produced by tabu search

void generate_full_topology(problem_instance& prob_inst,
    izlib::iz_topology& topo, 
    vector<char>& node_info, vector<int>& server_ids) {
  //---------------
  auto& inter_co_topo = prob_inst.topology.inter_co_topo;
  auto& cos = prob_inst.topology.cos;

  int total_node_count{0}, total_edge_count{0};
  int max_inodes{0};
  //total_node_count += cos.size();
  total_edge_count += inter_co_topo.edge_count;
  for (auto& co : cos) {
    total_node_count += co.intra_topo.node_count;
    total_edge_count += 2*co.intra_topo.edge_count;
    max_inodes = max(max_inodes, co.intra_topo.node_count);
  }
  // resize the node_info vector
  node_info.resize(total_node_count);
  // ds to hold id mappings
  vector<vector<int>> id_map(cos.size(), vector<int>(max_inodes));
  int node_id{0}; // node_id for the merged graph
  for (auto& co : cos) {
    // output the switches 
    for (int i = 0; i < co.server_ids[0]; ++i) {
      id_map[co.id][i] = node_id;
      node_info[node_id] = 's';
      ++node_id;
    }
    // now the servers
    for (auto i : co.server_ids) {
      id_map[co.id][i] = node_id;
      node_info[node_id] = 'c';
      server_ids.push_back(node_id);
      auto server_ptr = dynamic_pointer_cast<esso_server>(
          co.intra_nodes[i]);
      ++node_id;
    }
  }
  // initialize topo
  topo.init(total_node_count);
  int edge_id{0};
  // output inter co edges
  for (auto& e : inter_co_topo.edges()) {
    topo.add_edge(id_map[e.u][0], id_map[e.v][0], e.latency, e.capacity);
    topo.add_edge(id_map[e.v][0], id_map[e.u][0], e.latency, e.capacity);
  }
  // output intra co edges
  for (auto& co : cos) {
    // output edges
    for (auto& e : co.intra_topo.edges()) {
      topo.add_edge(id_map[co.id][e.u], id_map[co.id][e.v], 
          e.latency, e.capacity);
      topo.add_edge(id_map[co.id][e.v], id_map[co.id][e.u], 
          e.latency, e.capacity);
    }
  }
  //---------------
}

// prints the 404... message when no embedding is found
void print_404_message(const sfc_request& sfc) {
  cout << "404 " << sfc << endl;
}

bool read_res_topology_file(const string& res_topology_filename,
    problem_instance& prob_inst) {
  fstream fin(res_topology_filename);
  int co_count, co_id;
  fin >> co_count;
  auto& cos = prob_inst.topology.cos;
  for (int i = 0; i < co_count; ++i) {
    fin >> co_id;
    fin >> cos[co_id].carbon;
    for (int j = 0; j < 24; ++j) fin >> cos[co_id].green_residual[j];
  }
  int node_count, edge_count;
  fin >> node_count >> edge_count;
  int node_id, cpu_count;
  char type;
  double sleep_power, base_power, per_cpu_power;
  for (int i = 0; i < node_count; ++i) {
    fin >> node_id >> type >> co_id;
    if (type == 'c') {
      fin >> sleep_power >> base_power >> cpu_count >> per_cpu_power;
      cos[co_id].set_residual_cpu(node_id%9, cpu_count);
    }
    else {
      // for switch just read data, no need to update any state
      fin >> sleep_power >> base_power;
    }
  }
  int edge_id, node_u, node_v, capacity, latency;
  for (int i = 0; i < edge_count; ++i) {
    fin >> edge_id >> node_u >> node_v >> type >> co_id >> 
        capacity >> latency;
    if (type == 'b') {
      prob_inst.topology.set_residual_bandwidth(node_u/9, node_v/9, capacity);
    }
    else {
      cos[co_id].set_residual_bandwidth(node_u%9, node_v%9, capacity);
    }
  }
  fin >> type;
  if (!fin.eof()) cout << "all data not read" << endl;
  fin.close();
}

int main(int argc, char **argv) {

  if (argc != 3) {
    cerr << "usage: ./esso_heuristic.o <relative-path-to co_topology.dat> " << 
        "<relative-path-to res_topology.dat>" << endl;
    return -1;
  }
  // directory for the dataset
  //string dataset_dir {argv[1]};
  string co_topology_filename {argv[1]};
  string res_topology_filename {argv[2]};

  // prob_inst contains all the input data
  problem_instance prob_inst;
  problem_input prob_input;
  //prob_input.vnf_info_filename = join_path(dataset_dir, "vnf_types.dat");
  //prob_input.time_slot_filename = join_path(dataset_dir, "timeslots.dat");
  //prob_input.topology_filename = join_path(dataset_dir, "co_topology.dat");
  prob_input.topology_filename = co_topology_filename; 

  // if all input read successfully, then call the heuristic
  if (prob_inst.read_input(prob_input)) {

    if(!read_res_topology_file(res_topology_filename, prob_inst)) {
      cerr << "failed to read res topology file" << endl;
      return -1;
    }

    sfc_request sfc;
    int timeslot;
    double current_cost, migration_threshold;
    cin >> timeslot >> sfc >> current_cost >> migration_threshold;

    iz_timer htimer;

    // Stage-1: find a path for embedding sfc
    int k = 3; // number of candidate paths
    auto embedding_path = stage_one(sfc, k, timeslot, prob_inst);
    // if no path found then 
    // OUTPUT 404 message
    if (!embedding_path.is_valid()) {
      print_404_message(sfc);
      cerr << "no embedding path" << endl;
      return -1;
    }
    
    // Stage-2: compute the cost matrix for all co's on the embedding path
    vector<vector<vector<double>>> cost_matrices(embedding_path.size());
    vector<vector<vector<int>>> node_matrices(embedding_path.size());
    for (int i = 0; i < embedding_path.size(); ++i) {
      int co_id = embedding_path.nodes[i];
      stage_two(co_id, sfc, embedding_path, timeslot, 
          cost_matrices[i], node_matrices[i], prob_inst);
    }

    // 1 1 3 1 4 1 2 2 1 100 100 0 0.2
    // Stage-3: call tabu search
    vector<vector<int>> best_solution = vector<vector<int>>(
        embedding_path.size(), vector<int>(sfc.vnf_count, 0));
    double best_cost{};
    auto res = tabu_search(sfc, embedding_path, cost_matrices, 
        best_solution, best_cost);
    
    // no soution found then 
    // OUTPUT 404 message
    if (!res) {
      print_404_message(sfc);
      cerr << "failed to find any solution" << endl;
      return -1;
    }

    double time = htimer.time();

    //OUTPUT sfc and cost
    cout << "200 " << sfc << " ";

    vector<int> emb_cos, emb_co_nodes;
    auto emb_nodes = embedding_nodes(sfc, embedding_path, 
        node_matrices, best_solution, emb_cos, emb_co_nodes);

    //cout << endl << "---------------------------" << endl;
    // compute carbon footprint
    // if the first co is not the ingress, then find a path
    // from the ingress to the first co and allocate bandwidth
    // in esso_topology class
    if (sfc.ingress_co != emb_cos.front()) {
      iz_path path;
      prob_inst.topology.inter_co_topo.shortest_path(sfc.ingress_co, 
          emb_cos.front(), path, sfc.bandwidth);
      prob_inst.topology.allocate_bandwidth(path, 
          sfc.bandwidth);
    }
    // allocate bandwidth for the next backbone links
    int co_u = emb_cos.front();
    for(int i = 1; i < emb_cos.size(); ++i) {
      int co_v = emb_cos.at(i);
      if(co_u != co_v) {
        iz_path path;
        prob_inst.topology.inter_co_topo.shortest_path(co_u, co_v,
            path, sfc.bandwidth);
        prob_inst.topology.allocate_bandwidth(path, 
            sfc.bandwidth);
      }
      co_u = co_v;
    }
    // now for the last backbone link
    if (sfc.egress_co != emb_cos.back()) {
      iz_path path;
      prob_inst.topology.inter_co_topo.shortest_path(emb_cos.back(), 
          sfc.egress_co, path, sfc.bandwidth);
      prob_inst.topology.allocate_bandwidth(path, 
          sfc.bandwidth);
    }
    // now process the intra-co links and servers (and switches)
    vector<int> same_co_nodes;
    int last_co = emb_cos.front();
    same_co_nodes.push_back(emb_co_nodes.front());
    for(int i = 1; i < emb_cos.size(); ++i) {
      if (emb_cos.at(i) == last_co) {
        if(same_co_nodes.back() != emb_co_nodes.at(i)) {
          same_co_nodes.push_back(emb_co_nodes.at(i));
        }
      }
      else {
        // now we allocate bandwidth and start a fresh
        // same_co_nodes
        int u = 0;
        for (int v : same_co_nodes) {
          if (u == v) continue;
          iz_path path;
          prob_inst.topology.cos[last_co].intra_topo.shortest_path(u, v,
              path, sfc.bandwidth);
          prob_inst.topology.cos[last_co].allocate_bandwidth(path, 
              sfc.bandwidth);
          u = v;
        }
        iz_path path;
        prob_inst.topology.cos[last_co].intra_topo.shortest_path(u, 0,
            path, sfc.bandwidth);
        prob_inst.topology.cos[last_co].allocate_bandwidth(path, 
            sfc.bandwidth);

        same_co_nodes.clear();
        same_co_nodes.push_back(emb_co_nodes.at(i));
      }
      last_co = emb_cos.at(i);
    }
    if (!same_co_nodes.empty()) {
      int u = 0;
      for (int v : same_co_nodes) {
        if (u == v) continue;
        iz_path path;
        prob_inst.topology.cos[last_co].intra_topo.shortest_path(u, v,
            path, sfc.bandwidth);
        prob_inst.topology.cos[last_co].allocate_bandwidth(path, 
            sfc.bandwidth);
        u = v;
      }
      iz_path path;
      prob_inst.topology.cos[last_co].intra_topo.shortest_path(u, 0,
          path, sfc.bandwidth);
      prob_inst.topology.cos[last_co].allocate_bandwidth(path, 
          sfc.bandwidth);
    }
    // allocate cpu energy
    for (int i = 0; i < emb_cos.size(); ++i) {
      prob_inst.topology.cos[emb_cos[i]].allocate_cpu(emb_co_nodes[i],
          sfc.cpu_reqs[i]);
    }

    cout << prob_inst.topology.get_carbon_fp(timeslot) << " ";
    //cout << endl << "---------------------------" << endl;

    // OUTPUT the number of nodes followed by the nodes
    cout << sfc.vnf_count << " "; 
    for (auto node : emb_nodes) cout << node << " ";

    // generate full topology
    iz_topology full_topo;
    vector<char> node_info;
    vector<int> server_ids;
    generate_full_topology(prob_inst, full_topo, node_info, server_ids);

    // generate path info for optimizer output
    // a vector to hold the ingress + emb_nodes + egress
    vector<int> full_path_nodes;
    int inter_co_node_count = prob_inst.topology.cos[0].inter_co_node_count;
    full_path_nodes.push_back(sfc.ingress_co * inter_co_node_count);
    copy(emb_nodes.begin(), emb_nodes.end(), back_inserter(full_path_nodes));
    full_path_nodes.push_back(sfc.egress_co * inter_co_node_count);
    // OUTPUT the number of partial paths on the full path
    // always equal to sfc.vnf_count + 1
    cout << sfc.vnf_count + 1 << " ";
    // iterate the nodes in the full path
    int u = full_path_nodes[0];
    for (int v_idx = 1; v_idx < full_path_nodes.size(); ++v_idx) {
      int v = full_path_nodes[v_idx];
      // if u == v then just OUTPUT 
      if (u == v) {
        cout << 2 << " " << u << " " << v << " ";
      }
      else {
        // find the shortest path between u and v
        iz_path p;
        full_topo.shortest_path(u, v, p, sfc.bandwidth);
        // OUTPUT the path
        // number of nodes, then the nodes
        cout << p.size() << " ";
        for (auto n : p.nodes) cout << n << " ";
      }
      u = v;
    }
    cout << time << endl;
  }
  else {
    cerr << "failed to read input files for porblem instance" << endl;
    return -1;
  }

  return 0;
}
