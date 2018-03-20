#include <omp.h>

#include <fstream>

#include <boost/format.hpp>

#include "parallel_local_push_yche.h"

using boost::format;

double cal_rmax(double c, double epsilon) {
    return (1 - c) * epsilon;
}

LP::LP(GraphYche &g, string gName_, double c_, double epsilon_, size_t n_) :
        g_name(gName_), c(c_), epsilon(epsilon_), n(n_) {
    r_max = cal_rmax(c, epsilon);

    max_q_size = 0;
    n_push = 0;
    cpu_time = -1; // set the init value
}

void LP::init_PR() {
    string data_path = get_file_path_base() + ".P";
    P.add(n);
    R.add(n);
    marker.add(n);

    for (int i = 0; i < n; i++) {
        NodePair np(i, i);
        R[np] = 1;
        Q.push(np);
        marker[np] = true;
    }
}

PFLP::PFLP(GraphYche &g, string name, double c_, double epsilon, size_t n_) : LP(g, name, c_, epsilon, n_) {
    init_PR();
}

PRLP::PRLP(GraphYche &g, string name, double c_, double epsilon, size_t n_) : LP(g, name, c_, epsilon, n_) {
    init_PR();
}

void LP::local_push(GraphYche &g) { // local push given current P and R
    auto start = std::chrono::high_resolution_clock::now();

    while (!Q.empty()) {
        max_q_size = max(max_q_size, Q.size());
        NodePair np = Q.front();
        Q.pop();
        marker[np] = false;
        double residual_to_push = how_much_residual_to_push(g, np);

        R[np] -= residual_to_push;
        P[np] += residual_to_push;
        push_to_neighbors(g, np, residual_to_push); // push residuals to neighbors of np
    }

    auto finish = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = finish - start;
    if (cpu_time == -1) {
        cpu_time = elapsed.count();
        mem_size = getValue();
    }
}

void PRLP::local_push(GraphYche &g) {

}

void PFLP::local_push(GraphYche &g) {
    vector<pair<int, vector<FLPTask>>> task_vec;

    while (!Q.empty()) {
        // 1st: generate tasks
        max_q_size = max(max_q_size, Q.size());
        NodePair np = Q.front();
        Q.pop();
        marker[np] = false;
        double residual_to_push = how_much_residual_to_push(g, np);

        R[np] -= residual_to_push;
        P[np] += residual_to_push;
        push_to_neighbors(g, np, residual_to_push); // push residuals to neighbors of np

        // 2nd: task preparation
        task_vec.clear();
        for (auto &key_val: tmp_task_hash_table) { task_vec.emplace_back(std::move(key_val)); }
        tmp_task_hash_table.clear();

        // 3rd: computation
        for (auto i = 0; i < task_vec.size(); i++) {
            auto out_nei_a = task_vec[i].first;
            auto task_info = task_vec[i].second;
            for (auto &task :task_info) {
                auto b = task.b_;
                for (auto off_b = g.off_out[b]; off_b < g.off_out[b + 1]; off_b++) {
                    auto out_nei_b = g.neighbors_out[off_b];
                    if (out_nei_a == out_nei_b) {
                        continue;
                    }
                    auto in_degree_a = g.in_degree(out_nei_a);
                    auto in_degree_b = g.in_degree(out_nei_b);
                    auto total_in = in_degree_a * in_degree_b;
                    NodePair pab(out_nei_a, out_nei_b);
                    double inc = c * task.residual_ / total_in;

                    // push
                    n_push++;
                    auto &res_ref = R[pab];
                    res_ref += inc;
                    if (fabs(res_ref) > r_max) {
                        auto &is_in_q_flag_ref = marker[pab];
                        if (!is_in_q_flag_ref) {
                            Q.push(pab);
                            is_in_q_flag_ref = true;
                        }
                    }
                }
            }
        }
    }
}

void PFLP::push(NodePair &pab, double inc) {

}

void PRLP::push(NodePair &pab, double inc) {
    // only probing once
    n_push++;
    auto &res_ref = R[pab];
    res_ref += inc;
    if (fabs(res_ref) / sqrt(2) > r_max) { // the criteria for reduced linear system
        auto &is_in_q_flag_ref = marker[pab];
        if (!is_in_q_flag_ref) {
            Q.push(pab);
            is_in_q_flag_ref = true;
        }
    }
}

double PFLP::how_much_residual_to_push(GraphYche &g, NodePair &np) {
    return R[np];
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

void PRLP::push_to_neighbors(GraphYche &g, NodePair &np, double current_residual) {
    // the push method using reduced linear system
    // out-neighbors of a,b
    auto a = np.first;
    auto b = np.second;
    if (a == b) {
        /* starting push for singleton nodes*/
        for (auto off_a = g.off_out[a]; off_a < g.off_out[a + 1]; off_a++) {
            auto out_nei_a = g.neighbors_out[off_a];
            for (auto off_b = g.off_out[b]; off_b < g.off_out[b + 1]; off_b++) {
                auto out_nei_b = g.neighbors_out[off_b];
                auto in_degree_a = g.in_degree(out_nei_a);
                auto in_degree_b = g.in_degree(out_nei_b);
                auto total_in = in_degree_a * in_degree_b;
                if (out_nei_a < out_nei_b) { // only push to partial pairs for a < b
                    NodePair pab(out_nei_a, out_nei_b); // the node-pair to be pushed to
                    double inc = c * current_residual / total_in;
                    push(pab, sqrt(2) * inc);
                }
            }
        }
    } else {
        /* starting push for non-singleton nodes*/
        for (auto off_a = g.off_out[a]; off_a < g.off_out[a + 1]; off_a++) {
            for (auto off_b = g.off_out[b]; off_b < g.off_out[b + 1]; off_b++) {
                // put out_nei_a here, since swap happens later
                auto out_nei_a = g.neighbors_out[off_a];
                auto out_nei_b = g.neighbors_out[off_b];

                auto in_degree_a = g.in_degree(out_nei_a);
                auto in_degree_b = g.in_degree(out_nei_b);
                auto total_in = in_degree_a * in_degree_b;
                double inc = c * current_residual / total_in;
                if (out_nei_a == out_nei_b) { //don't push to singleton nodes
                    continue;
                } else { //  a, b is normal in-neighbors of out_a and out_b
                    if (out_nei_a > out_nei_b) {
                        swap(out_nei_a, out_nei_b);
                    }
                    NodePair pab(out_nei_a, out_nei_b);
                    push(pab, 1 * inc);
                }
            }
        }
    }
}

void PFLP::push_to_neighbors(GraphYche &g, NodePair &np, double current_residual) {
    auto a = np.first;
    auto b = np.second;
    for (auto off_a = g.off_out[a]; off_a < g.off_out[a + 1]; off_a++) {
        auto out_nei_a = g.neighbors_out[off_a];
        if (tmp_task_hash_table.find(out_nei_a) == tmp_task_hash_table.end()) {
            tmp_task_hash_table[out_nei_a] = vector<FLPTask>();
        }
        tmp_task_hash_table[out_nei_a].emplace_back(b, static_cast<float>(current_residual));
    }
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

double PFLP::query_P(int a, int b) {
    return P.query(a, b);
}

double LP::query_R(int a, int b) {
    if (a > b) {
        return R.query(b, a);
    } else {
        return R.query(a, b);
    }
}
