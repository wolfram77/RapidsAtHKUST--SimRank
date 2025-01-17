#ifdef HAS_OPENMP

#include <omp.h>

#endif

#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <boost/format.hpp>

#include "parallel_local_push_yche.h"
//#include "../util/pretty_print.h"

using boost::format;
using namespace std::chrono;

PRLP::PRLP(GraphYche &g, string name, double c_, double epsilon, size_t n_) : LP(g, name, c_, epsilon, n_) {
    P.add(n);
    R.add(n);
    marker.add(n);

    num_threads_ = 64u;

    thread_local_expansion_set_lst = vector<vector<int>>(num_threads_);
    thread_local_hash_slot_indicator_lst = vector<vector<int>>(num_threads_);
    thread_local_expansion_set_lst[0].reserve(n);
    expansion_pair_lst.resize(n);
    expansion_set_g.resize(0);
    g_task_hash_table.resize(n);

    for (int i = 0; i < n; i++) {
        NodePair np(i, i);
        R[np] = 1;
        marker[np] = true;
        thread_local_expansion_set_lst[0].emplace_back(i);
        expansion_pair_lst[i].emplace_back(i);
    }
    is_in_expansion_set = new bool[n];
    for (auto i = 0; i < n; i++) {
        is_in_expansion_set[i] = false;
    }

    thread_local_task_hash_table_lst = vector<vector<vector<RLPTask>>>(num_threads_);
    for (auto &local_hash_table: thread_local_task_hash_table_lst) {
        local_hash_table.resize(n);
    }
}

void PRLP::local_push(GraphYche &g) {
    int counter = 0;
    bool is_go_on;
    auto g_start_time = std::chrono::high_resolution_clock::now();
    auto g_end_time = std::chrono::high_resolution_clock::now();
    auto total_ms = 0;
    vector<long> prefix_sum(num_threads_ + 1, 0);
    long prev_expansion_set_g_size;
    long min_idx;

#ifdef PUSH_NUM_STAT
    long push_num = 0;
#endif

#pragma omp parallel
    {
#ifdef HAS_OPENMP
        auto thread_id = omp_get_thread_num();
#else
        auto thread_id = 0;
#endif
        auto &local_expansion_set = thread_local_expansion_set_lst[thread_id];
        auto &local_task_hash_table = thread_local_task_hash_table_lst[thread_id];
        auto &local_hash_slot_set = thread_local_hash_slot_indicator_lst[thread_id];
        while (true) {
            // 0th: initialize is_go_on flag
#pragma omp single
            {
                is_go_on = false;
#ifdef DEBUG
                cout << "gen beg..." << counter << endl;
#endif
            }
            if (!expansion_set_g.empty() || !local_expansion_set.empty()) { is_go_on = true; }
#pragma omp barrier
            if (!is_go_on) { break; }

#pragma omp single
            {
                // aggregation of v_a for expansion, value in thread_local_expansion_set_lst are distinct
                g_start_time = std::chrono::high_resolution_clock::now();
                for (auto i = 0; i < thread_local_expansion_set_lst.size(); i++) {
                    prefix_sum[i + 1] = prefix_sum[i] + thread_local_expansion_set_lst[i].size();
                }
                prev_expansion_set_g_size = expansion_set_g.size();
                expansion_set_g.resize(prev_expansion_set_g_size + static_cast<unsigned long>(prefix_sum.back()));
            }
            std::copy(std::begin(local_expansion_set), std::end(local_expansion_set),
                      std::begin(expansion_set_g) + prev_expansion_set_g_size + prefix_sum[thread_id]);
            for (auto expansion_v: local_expansion_set) { is_in_expansion_set[expansion_v] = true; }

#pragma omp barrier

            // 1st: generate tasks
#pragma omp single
            {
#ifdef DEBUG
                cout << "total (a, *) to expand:" << expansion_set_g.size() << "\n";
#endif
                auto tmp_time = std::chrono::high_resolution_clock::now();

//                long max_expansion_num = max<long>(n, 100000);
                long max_expansion_num = 10000000;
//                long max_expansion_num = 1000;
                long accumulation = 0;
                for (min_idx = expansion_set_g.size() - 1;
                     min_idx != 0 && accumulation < max_expansion_num; min_idx--) {
                    accumulation += expansion_pair_lst[expansion_set_g[min_idx]].size();
                }
                auto tmp_time_end = std::chrono::high_resolution_clock::now();
#ifdef DEBUG
                cout << "accumulation pair#: " << accumulation << "\n";
                cout << "accumulation (a, *): " << expansion_set_g.size() - min_idx << "\n";
                cout << "min_idx: " << min_idx << "\n";

                cout << "find min_idx cost:"
                     << std::chrono::duration_cast<std::chrono::microseconds>((tmp_time_end - tmp_time)).count()
                     << "us" << endl;
#endif
            }

#pragma omp for schedule(dynamic, 50)
            for (long i = expansion_set_g.size() - 1; i >= min_idx; i--) {
                auto a = expansion_set_g[i];
                for (auto b:expansion_pair_lst[a]) {

                    NodePair np(a, b);
                    auto &residual_ref = R[np];
                    double residual_to_push = residual_ref;
                    if (a == b) {
                        residual_to_push -= r_max / (1 - c); // singleton nodes do not need to push all residual as 1
                    }

                    marker[np] = false;
                    residual_ref -= residual_to_push;
                    P[np] += residual_to_push;

                    for (auto off_a = g.off_out[a]; off_a < g.off_out[a + 1]; off_a++) {
                        auto out_nei_a = g.neighbors_out[off_a];

                        local_task_hash_table[out_nei_a].emplace_back(b, static_cast<float>(residual_to_push), a == b);
                    }
                    // important for the later local push-to-neighbors
                    if (a != b) {
                        for (auto off_b = g.off_out[b]; off_b < g.off_out[b + 1]; off_b++) {
                            auto out_nei_b = g.neighbors_out[off_b];
                            local_task_hash_table[out_nei_b].emplace_back(a, static_cast<float>(residual_to_push),
                                                                          false);
                        }
                    }
                }
                is_in_expansion_set[a] = false;
            }

            // 2nd: clear expansion Q related structures for the next computation stage
#pragma omp for nowait
            for (long i = expansion_set_g.size() - 1; i >= min_idx; i--) {
                expansion_pair_lst[expansion_set_g[i]].clear();
            }
            local_expansion_set.clear();
            local_hash_slot_set.clear();
#pragma omp single
            {
                expansion_set_g.resize(static_cast<unsigned long>(min_idx));
            }
#pragma omp barrier


#pragma omp for
            for (auto i = 0; i < g_task_hash_table.size(); i++) {
                auto is_slot_occupied = false;
                g_task_hash_table[i].clear();
                for (auto &thread_local_hash_table:thread_local_task_hash_table_lst) {
                    for (auto &task: thread_local_hash_table[i]) {
                        g_task_hash_table[i].emplace_back(task);
                        is_slot_occupied = true;
                    }
                    thread_local_hash_table[i].clear();
                }
                if (is_slot_occupied) {
                    local_hash_slot_set.emplace_back(i);
                }
            }


#pragma omp single
            {
                counter++;
                g_end_time = std::chrono::high_resolution_clock::now();
                auto tmp_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        (g_end_time - g_start_time)).count();
                total_ms += tmp_elapsed;
#ifdef DEBUG
                cout << "gen using " << tmp_elapsed << " ms\n" << endl;
#endif
            }

            // 3rd: computation
            for (auto &local_slot_set: thread_local_hash_slot_indicator_lst) {
#ifdef PUSH_NUM_STAT
#pragma omp for schedule(dynamic, 1) nowait reduction(+:push_num)
#else
#pragma omp for schedule(dynamic, 1) nowait
#endif
                for (auto i = 0; i < local_slot_set.size(); i++) {
                    auto out_nei_a = local_slot_set[i];
                    bool is_enqueue = false;

                    for (auto &task :g_task_hash_table[out_nei_a]) {
                        // push to neighbors
                        auto local_b = task.b_;
                        if (task.is_singleton_) {
                            // assume neighbors are sorted
                            // only push to partial pairs for a < local_b
                            auto it = std::upper_bound(std::begin(g.neighbors_out) + g.off_out[local_b],
                                                       std::begin(g.neighbors_out) + g.off_out[local_b + 1], out_nei_a);
                            for (auto off_b = it - std::begin(g.neighbors_out);
                                 off_b < g.off_out[local_b + 1]; off_b++) {
                                auto out_nei_b = g.neighbors_out[off_b];
                                NodePair pab(out_nei_a, out_nei_b);

                                // push
#ifdef PUSH_NUM_STAT
                                push_num++;
#endif
                                auto &res_ref = R[pab];
                                res_ref += sqrt(2) * c * task.residual_ /
                                           (g.in_degree(out_nei_a) * g.in_degree(out_nei_b));

                                if (fabs(res_ref) / sqrt(2) > r_max) { // the criteria for reduced linear system
                                    auto &is_in_q_ref = marker[pab];
                                    if (!is_in_q_ref) {
                                        expansion_pair_lst[out_nei_a].emplace_back(out_nei_b);
                                        is_enqueue = true;
                                        is_in_q_ref = true;
                                    }
                                }
                            }
                        } else {
                            // only push to partial pairs for a < local_b
                            auto it = std::upper_bound(std::begin(g.neighbors_out) + g.off_out[local_b],
                                                       std::begin(g.neighbors_out) + g.off_out[local_b + 1], out_nei_a);
                            for (auto off_b = it - std::begin(g.neighbors_out);
                                 off_b < g.off_out[local_b + 1]; off_b++) {
                                auto out_nei_b = g.neighbors_out[off_b];
                                NodePair pab(out_nei_a, out_nei_b);

                                // push
#ifdef PUSH_NUM_STAT
                                push_num++;
#endif
                                auto &res_ref = R[pab];
                                res_ref += c * task.residual_ / (g.in_degree(out_nei_a) * g.in_degree(out_nei_b));

                                if (fabs(res_ref) / sqrt(2) > r_max) { // the criteria for reduced linear system
                                    auto &is_in_q_ref = marker[pab];
                                    if (!is_in_q_ref) {
                                        expansion_pair_lst[out_nei_a].emplace_back(out_nei_b);
                                        is_enqueue = true;
                                        is_in_q_ref = true;
                                    }
                                }
                            }
                        }
                    }
                    if (is_enqueue && !is_in_expansion_set[out_nei_a]) { local_expansion_set.emplace_back(out_nei_a); }
                }
            }
        }
    } // end of thread pool
    cout << "mem usage:" << getValue() << " KB" << endl;
    cout << "rounds:" << counter << "\n gen accumulation:" << total_ms << " ms" << endl;
#ifdef PUSH_NUM_STAT
    cout << "push number:" << push_num << endl;
#endif
}

void PRLP::push(NodePair &pab, double inc, bool &is_enqueue) {
    // only probing once
    auto &res_ref = R[pab];
    res_ref += inc;
    if (fabs(res_ref) / sqrt(2) > r_max) { // the criteria for reduced linear system
        auto &is_in_q_ref = marker[pab];
        if (!is_in_q_ref) {
            expansion_pair_lst[pab.first].emplace_back(pab.second);
            is_enqueue = true;
            is_in_q_ref = true;
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

string PRLP::get_file_path_base() {
    return LOCAL_PUSH_DIR + str(format("RLP_%s-%.3f-%.6f") % g_name % c % epsilon);
}
