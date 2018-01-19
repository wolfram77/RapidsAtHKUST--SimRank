//
// Created by yche on 1/8/18.
//

#include <chrono>

#include "../ground_truth/graph_yche.h"
#include "../ground_truth/simrank.h"

#include "linearD.h"
#include "../ground_truth/random_pair_generator.h"

using namespace std;
using namespace std::chrono;

int main(int argc, char *argv[]) {
    double c = 0.6;
    int L = 3;
    int R = 100;

    int T = 10;
    int R_prime = 10000;

    auto data_name = string(argv[1]);
    string file_name = string(argv[1]);
    int pair_num = atoi(argv[2]);
    int round_i = atoi(argv[3]);

    srand(static_cast<unsigned int>(time(nullptr))); // random number generator
    std::chrono::duration<double> elapsed{};

    // 1st: indexing
    auto start = std::chrono::high_resolution_clock::now();
    LinearD lin(data_name, c, T, L, R);
    auto pre_time = std::chrono::high_resolution_clock::now();
    cout << "constructing time:" << float(duration_cast<microseconds>(pre_time - start).count()) / (pow(10, 6))
         << " s\n";

    // 2nd: query
#ifdef GROUND_TRUTH
    string path = get_edge_list_path(data_name);
    GraphYche g_gt(path);
    TruthSim ts(string(argv[1]), g_gt, c, 0.01);
    auto max_err = 0.0;
    auto failure_count = 0;
#endif

    auto sample_pairs = read_sample_pairs(file_name, pair_num, round_i);
    start = std::chrono::high_resolution_clock::now();
    auto clock_start = clock();

#pragma omp parallel
    {
        // allocate memory in advance
        VectorXd lhs_vec(lin.n), rhs_vec(lin.n);
#ifdef GROUND_TRUTH
#pragma omp for reduction(max:max_err) schedule(dynamic, 100)
#else
#pragma omp for schedule(dynamic, 1)
#endif
        for (auto pair_i = 0; pair_i < pair_num; pair_i++) {
            auto u = sample_pairs[pair_i].first;
            auto v = sample_pairs[pair_i].second;
#ifdef GROUND_TRUTH
            auto res = lin.single_pair(u, v, lhs_vec, rhs_vec);

            max_err = max(max_err, abs(ts.sim(u, v) - res));
            if (abs(ts.sim(u, v) - res) > 0.01) {
#pragma omp critical
                {
                    cout << u << ", " << v << "," << ts.sim(u, v) << "," << res << endl;
                    failure_count++;
                }
            }
#else
            lin.single_pair(u, v, lhs_vec, rhs_vec);
#endif
        }
    };

    auto end = std::chrono::high_resolution_clock::now();
    auto clock_end = clock();
    cout << "total query cpu time:" << static_cast<double>(clock_end - clock_start) / CLOCKS_PER_SEC << "s" << endl;
    elapsed = end - start;

#ifdef GROUND_TRUTH
    cout << "failure count:" << failure_count << endl;
    cout << "max err:" << max_err << endl;
#endif
    cout << format("total query cost: %s s") % elapsed.count() << endl; // record the pre-processing time
}