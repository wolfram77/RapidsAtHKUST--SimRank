#ifndef __LOCAL_PUSH_H__
#define __LOCAL_PUSH_H__

#include <cstdio>
#include <cmath>

#include <iostream>
#include <utility>
#include <stack>
#include <queue>

#include <boost/graph/adjacency_list.hpp>
#include <boost/format.hpp>

#include <sparsepp/spp.h>

#include "../util/graph_yche.h"
#include "../util/sparse_matrix_utils.h"

using boost::format;
using PairMarker= sparse_hash_map<NodePair, bool>;

extern double cal_rmax(double c, double epsilon); // the r_max for general case

struct FLPTask {
    int b_;
    float residual_;

    FLPTask(int b, float residual) {
        b_ = b;
        residual_ = residual;
    }
};

struct RLPTask {
    int b_;
    float residual_;
    bool is_singleton_;

    RLPTask(int b, float residual, bool is_singleton) {
        b_ = b;
        residual_ = residual;
        is_singleton_ = is_singleton;
    }
};

struct LP {
public:
    string g_name;                  // the name of graph data

    DensePairMap<float> P;          // the estimates
    DensePairMap<float> R;          // the residuals
    DensePairMap<bool> marker;

    double r_max;
    double c;
    size_t n;               // we need to define total number of nodes in advance
    double epsilon;         // the error bound
public:
    LP(GraphYche &, string, double c, double epsilon, size_t);

    virtual double query_P(int a, int b) {};
};

/*local push using reduced system*/
struct PRLP : LP {
    vector<vector<int>> thread_local_expansion_set_lst;

    vector<vector<int>> expansion_pair_lst;

    vector<int> expansion_set_g;

    vector<vector<RLPTask>> task_hash_table;
#ifdef HAS_OPENMP
    vector<omp_lock_t> hash_table_lock;
#endif
    size_t num_threads;
public:
    PRLP(GraphYche &g, string name, double c_, double epsilon, size_t n_);

    double query_P(int a, int b) override;

public:
    double how_much_residual_to_push(GraphYche &g, NodePair &np);

    void push(NodePair &pab, double, bool &);

    void local_push(GraphYche &g);
};

/* local push using full system*/
struct PFLP : LP {
    vector<vector<int>> thread_local_expansion_set_lst;

    vector<vector<int>> expansion_pair_lst;

    size_t num_threads;
public:
    PFLP(GraphYche &g, string name, double c_, double epsilon, size_t n_);

    double query_P(int a, int b) override;

public:
    void local_push(GraphYche &g); // empty function for local push
};

#endif