#include <omp.h>

#include <fstream>

#include <boost/format.hpp>

#include "parallel_local_push_yche.h"

using boost::format;

PRLP::PRLP(GraphYche &g, string name, double c_, double epsilon, size_t n_) : LP(g, name, c_, epsilon, n_) {
    P.add(n);
    R.add(n);
    marker.add(n);

    num_threads = 56u;
    thread_local_expansion_set_lst = vector<vector<int>>(num_threads);
    thread_local_expansion_set_lst[0].reserve(n);
    expansion_pair_lst.resize(n);

    for (int i = 0; i < n; i++) {
        NodePair np(i, i);
        R[np] = 1;
        marker[np] = true;
        thread_local_expansion_set_lst[0].emplace_back(i);
        expansion_pair_lst[i].emplace_back(i);
    }
}

void PRLP::local_push(GraphYche &g) {
    vector<std::unordered_map<int, vector<RLPTask>>> thread_local_task_hash_table_lst(num_threads);
    vector<vector<pair<int, vector<RLPTask>>>> thread_local_task_vec_lst(num_threads);
    vector<int> expansion_set_g;
    int counter = 0;
    bool is_go_on;

#pragma omp parallel
    {
#ifdef HAS_OPENMP
        auto thread_id = omp_get_thread_num();
#else
        auto thread_id = 0;
#endif
        auto &task_hash_table = thread_local_task_hash_table_lst[thread_id];
        auto &task_vec = thread_local_task_vec_lst[thread_id];
        auto &local_expansion_set = thread_local_expansion_set_lst[thread_id];

        while (true) {
            // 0th: initialize is_go_on flag
#pragma omp single
            {
                is_go_on = false;
                cout << "gen" << endl;
            }
            if (!local_expansion_set.empty()) { is_go_on = true; }
#pragma omp barrier
            if (!is_go_on) {
                break;
            }

#pragma omp single
            {
                // aggregation of v_a for expansion
                std::unordered_set<int> my_set;
                for (auto &expansion_set: thread_local_expansion_set_lst) {
                    for (auto u:expansion_set) {
                        my_set.emplace(u);
                    }
                }
                expansion_set_g.clear();
                std::copy(std::begin(my_set), std::end(my_set), back_inserter(expansion_set_g));
            }

            // 1st: generate tasks
#pragma omp for schedule(dynamic, 50)
            for (auto i = 0; i < expansion_set_g.size(); i++) {
                auto a = expansion_set_g[i];
                for (auto b:expansion_pair_lst[a]) {
                    bool is_singleton = (a == b);

                    NodePair np(a, b);
                    auto &residual_ref = R[np];
                    double residual_to_push = residual_ref;
                    if (is_singleton) {
                        residual_to_push -= r_max / (1 - c); // singleton nodes do not need to push all residual as 1
                    }

                    marker[np] = false;
                    residual_ref -= residual_to_push;
                    P[np] += residual_to_push;
                    for (auto off_a = g.off_out[a]; off_a < g.off_out[a + 1]; off_a++) {
                        auto out_nei_a = g.neighbors_out[off_a];
                        task_hash_table[out_nei_a].emplace_back(b, residual_to_push, is_singleton);
                    }
                }
            }
            task_vec.clear();
            for (auto &key_val: task_hash_table) { task_vec.emplace_back(std::move(key_val)); }
            task_hash_table.clear();
#pragma omp barrier

#pragma omp single
            {
                counter++;
                cout << "gen finished," << counter << endl;
            }

            // 2nd: task preparation
            for (auto v_a:local_expansion_set) { expansion_pair_lst[v_a].clear(); }
            local_expansion_set.clear();
#pragma omp barrier

            // 3rd: computation
            for (auto &task_vec_g: thread_local_task_vec_lst) {
#pragma omp for schedule(dynamic, 10)
                for (auto i = 0; i < task_vec_g.size(); i++) {
                    auto a_prime = task_vec_g[i].first;
                    bool is_enqueue = false;

                    for (auto &task :task_vec_g[i].second) {
                        auto b = task.b_;
                        if (task.is_singleton_) {
                            for (auto off_b = g.off_out[b]; off_b < g.off_out[b + 1]; off_b++) {
                                auto out_nei_b = g.neighbors_out[off_b];
                                if (a_prime < out_nei_b) { // only push to partial pairs for a < b
                                    NodePair pab(a_prime, out_nei_b); // the node-pair to be pushed to
                                    double inc = c * task.residual_ / (g.in_degree(a_prime) * g.in_degree(out_nei_b));
                                    push(pab, sqrt(2) * inc, is_enqueue);
                                }
                            }
                        } else {
                            for (auto off_b = g.off_out[b]; off_b < g.off_out[b + 1]; off_b++) {
                                auto out_nei_a = a_prime;
                                auto out_nei_b = g.neighbors_out[off_b];

                                double inc = c * task.residual_ / (g.in_degree(a_prime) * g.in_degree(out_nei_b));
                                if (out_nei_a != out_nei_b) {
                                    if (out_nei_a > out_nei_b) {
                                        swap(out_nei_a, out_nei_b);
                                    }
                                    NodePair pab(out_nei_a, out_nei_b);
                                    push(pab, 1 * inc, is_enqueue);
                                }
                            }
                        }
                    }
                    if (is_enqueue) { local_expansion_set.emplace_back(a_prime); }
                }
            }
        }
    }
    cout << "rounds:" << counter << endl;
}

void PRLP::push(NodePair &pab, double inc, bool &is_enqueue) {
    // only probing once
    auto &res_ref = R[pab];
    res_ref += inc;
    if (fabs(res_ref) / sqrt(2) > r_max) { // the criteria for reduced linear system
        auto &is_in_q_flag_ref = marker[pab];
        if (!is_in_q_flag_ref) {
            is_enqueue = true;
            expansion_pair_lst[pab.first].emplace_back(pab.second);
            is_in_q_flag_ref = true;
        }
    }
}

double PRLP::how_much_residual_to_push(GraphYche &g, NodePair &np) {
    // determine the residual value for current pair to push
    double r = R[np];
    if (np.first == np.second) { //singleton node
        return r - r_max / (1 - c); // singleton nodes do not need to push all residual as 1
    }

#ifdef SELF_LOOP_MERGE
    /* check whether np forms a self-loop */
    if (g.exists_edge(np.first, np.second) && g.exists_edge(np.second, np.first)) { // check whether exists reverse edge
        auto in_deg_a = g.in_degree(np.first);
        auto in_deg_b = g.in_degree(np.second);
        double alpha = c / (in_deg_a * in_deg_b);
        int k = ceil(log(r_max / fabs(r)) / log(alpha));
        double residual_to_push = (1 - pow(alpha, k)) * r / (1 - alpha);
        return residual_to_push;
    }
#endif
    return r;
}

double PRLP::query_P(int a, int b) {
    if (a == b) {
        return P.query(a, b);
    } else if (a > b) {
        return P.query(b, a) / sqrt(2);
    } else {
        return P.query(a, b) / sqrt(2);
    }
}