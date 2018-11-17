/*
 * Copyright (c) 2016 Shanghai Jiao Tong University.
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://ipads.se.sjtu.edu.cn/projects/wukong
 *
 */


#pragma once

#include <boost/unordered_set.hpp>
#include <boost/unordered_map.hpp>
#include <tbb/concurrent_queue.h>
#include <algorithm> // sort
#include <regex>

#include "global.hpp"
#include "type.hpp"
#include "bind.hpp"
#include "coder.hpp"
#include "dgraph.hpp"
#include "query.hpp"

// engine
#include "rmap.hpp"
#include "msgr.hpp"

// utils
#include "assertion.hpp"
#include "math.hpp"
#include "timer.hpp"

using namespace std;


#define QUERY_FROM_PROXY(r) (coder->tid_of((r).pqid) < global_num_proxies)

typedef pair<int64_t, int64_t> int64_pair;

int64_t hash_pair(const int64_pair &x) {
    int64_t r = x.first;
    r = r << 32;
    r += x.second;
    return std::hash<int64_t>()(r);
}


class SPARQLEngine {
private:
    int sid;    // server id
    int tid;    // thread id

    String_Server *str_server;
    DGraph *graph;
    Coder *coder;
    Messenger *msgr;

    RMap rmap; // a map of replies for pending (fork-join) queries
    pthread_spinlock_t rmap_lock;

    /// A query whose parent's PGType is UNION may call this pattern
    void index_to_known(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t tpid = pattern.subject;
        ssid_t id01 = pattern.predicate;
        dir_t d     = pattern.direction;
        ssid_t var  = pattern.object;
        SPARQLQuery::Result &res = req.result;
        int col = res.var2col(var);

        ASSERT(col != NO_RESULT);
        ASSERT(id01 == PREDICATE_ID || id01 == TYPE_ID); // predicate or type index

        vector<sid_t> updated_result_table;

        uint64_t sz = 0;
        edge_t *edges = graph->get_index(tid, tpid, d, sz);
        int start = req.tid % req.mt_factor;
        int length = sz / req.mt_factor;

        boost::unordered_set<sid_t> unique_set;
        // every thread takes a part of consecutive edges
        for (uint64_t k = start * length; k < (start + 1) * length; k++)
            unique_set.insert(edges[k].val);
        // fixup the last participant
        if (start == req.mt_factor - 1)
            for (uint64_t k = (start + 1) * length; k < sz; k++)
                unique_set.insert(edges[k].val);

        for (uint64_t i = 0; i < res.get_row_num(); i++) {
            if (req.pg_type == SPARQLQuery::PGType::OPTIONAL) {
                // matched
                if (unique_set.find(res.get_row_col(i, col)) != unique_set.end())
                    res.optional_matched_rows[i] = (true && res.optional_matched_rows[i]);
                else {
                    if (res.optional_matched_rows[i])
                        req.correct_optional_result(i);
                    res.optional_matched_rows[i] = false;
                }
            } else {
                // matched
                if (unique_set.find(res.get_row_col(i, col)) != unique_set.end())
                    res.append_row_to(i, updated_result_table);
            }
        }
        if (req.pg_type != SPARQLQuery::PGType::OPTIONAL)
            res.result_table.swap(updated_result_table);
        req.pattern_step++;
    }

    /// A query whose parent's PGType is UNION may call this pattern
    void const_to_known(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        std::vector<sid_t> updated_result_table;
        SPARQLQuery::Result &res = req.result;
        int col = res.var2col(end);

        ASSERT(col != NO_RESULT);

        uint64_t sz = 0;
        edge_t *edges = graph->get_triples(tid, start, pid, d, sz);

        boost::unordered_set<sid_t> unique_set;
        for (uint64_t k = 0; k < sz; k++)
            unique_set.insert(edges[k].val);

        if (req.pg_type == SPARQLQuery::PGType::OPTIONAL) {
            int row_num = res.get_row_num();
            ASSERT(row_num == res.optional_matched_rows.size());
            for (uint64_t i = 0; i < row_num; i++) {
                // matched
                if (unique_set.find(res.get_row_col(i, col)) != unique_set.end()) {
                    res.optional_matched_rows[i] = (true && res.optional_matched_rows[i]);
                } else {
                    if (res.optional_matched_rows[i]) req.correct_optional_result(i);
                    res.optional_matched_rows[i] = false;
                }
            }
        } else {
            for (uint64_t i = 0; i < res.get_row_num(); i++) {
                // matched
                if (unique_set.find(res.get_row_col(i, col)) != unique_set.end())
                    res.append_row_to(i, updated_result_table);
            }
            res.result_table.swap(updated_result_table);
        }
        req.pattern_step++;
    }


    /// IDX P ?X . (IDX and P are GIVEN, ?X is UNKNOWN)
    /// e.g., "?X __PREDICATE__ ub:subOrganizationOf" (predicate index)
    /// e.g., "?X  rdf:type  ub:GraduateStudent"      (type index)
    ///
    /// 1) Use [IDX]+[P] to retrieve all of neighbors (?X) on every node
    void index_to_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t tpid = pattern.subject;
        ssid_t id01 = pattern.predicate;
        dir_t d     = pattern.direction;
        ssid_t var  = pattern.object;
        SPARQLQuery::Result &res = req.result;

        ASSERT(id01 == PREDICATE_ID || id01 == TYPE_ID); // predicate or type index
        ASSERT(res.get_col_num() == 0);

        vector<sid_t> updated_result_table;

        uint64_t sz = 0;
        edge_t *edges = graph->get_index(tid, tpid, d, sz);
        int start = req.tid % req.mt_factor;
        int length = sz / req.mt_factor;

        // every thread takes a part of consecutive edges
        for (uint64_t k = start * length; k < (start + 1) * length; k++)
            updated_result_table.push_back(edges[k].val);

        // fixup the last participant
        if (start == req.mt_factor - 1)
            for (uint64_t k = (start + 1) * length; k < sz; k++)
                updated_result_table.push_back(edges[k].val);

        res.result_table.swap(updated_result_table);
        res.set_col_num(1);
        res.add_var2col(var, 0);
        req.pattern_step++;
        req.local_var = var;
    }

    /// C P ?X . (C/P: GIVEN, ?X: UNKNOWN)
    /// e.g., "?X  ub:subOrganizationOf  <http://www.University0.edu>"
    ///
    /// 1) Use [C]+[P] to retrieve all of neighbors (?X) on a certain (HASH(C)) node
    void const_to_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        std::vector<sid_t> updated_result_table;
        SPARQLQuery::Result &res = req.result;

        ASSERT(res.get_col_num() == 0);
        uint64_t sz = 0;
        edge_t *edges = graph->get_triples(tid, start, pid, d, sz);
        for (uint64_t k = 0; k < sz; k++)
            updated_result_table.push_back(edges[k].val);

        res.result_table.swap(updated_result_table);
        res.add_var2col(end, res.get_col_num());
        res.set_col_num(res.get_col_num() + 1);
        req.pattern_step++;
    }

    /// C P ?X . (C/P: GIVEN, ?X: UNKNOWN)
    /// e.g., "<http://www.Department7.University0.edu/Course44>  ub:id  ?X"
    ///
    void const_to_unknown_attr(SPARQLQuery &req) {
        // prepare for query
        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t start = pattern.subject;
        ssid_t aid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        SPARQLQuery::Result &res = req.result;

        ASSERT(d == OUT); // attribute always uses OUT

        std::vector<attr_t> updated_attr_table;

        int type = INT_t;
        // get the reusult
        bool has_value;
        attr_t v = graph->get_attr(tid, start, aid, d, has_value);
        if (has_value) {
            updated_attr_table.push_back(v);
            type = boost::apply_visitor(variant_type(), v);
        }

        // update the result table and metadata
        res.attr_res_table.swap(updated_attr_table);
        res.add_var2col(end, 0, type);   //update the unknown_attr to known
        res.set_attr_col_num(1);
        req.pattern_step++;
    }

    /// ?X P ?Y . (?X: KNOWN, P: GIVEN, ?X: UNKNOWN)
    /// e.g., "?X rdf:type ub:GraduateStudent"
    ///       "?X ub:memberOf ?Y"
    ///
    /// 1) Use [?X]+[P] to retrieve all of neighbors (?Y)
    void known_to_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        SPARQLQuery::Result &res = req.result;

        std::vector<sid_t> updated_result_table;
        updated_result_table.reserve(res.result_table.size());
        vector<bool> updated_optional_matched_rows;
        if (req.pg_type == SPARQLQuery::PGType::OPTIONAL) {
            updated_optional_matched_rows.reserve(res.optional_matched_rows.size());
        }
        std::vector<attr_t> updated_attr_table;
        updated_attr_table.reserve(res.result_table.size());

        // simple dedup for consecutive same vertices
        sid_t cached = BLANK_ID;
        edge_t *edges = NULL;
        uint64_t sz = 0;
        for (int i = 0; i < res.get_row_num(); i++) {
            sid_t cur = res.get_row_col(i, res.var2col(start));
            if (req.pg_type == SPARQLQuery::PGType::OPTIONAL &&
                    (!res.optional_matched_rows[i] || cur == BLANK_ID)) {
                res.append_row_to(i, updated_result_table);
                updated_result_table.push_back(BLANK_ID);
                updated_optional_matched_rows.push_back(res.optional_matched_rows[i]);
                continue;
            }
            if (cur != cached) {  // a new vertex
                cached = cur;
                edges = graph->get_triples(tid, cur, pid, d, sz);
            }

            // append a new intermediate result (row)
            if (req.pg_type == SPARQLQuery::PGType::OPTIONAL) {
                if (sz > 0) {
                    for (uint64_t k = 0; k < sz; k++) {
                        res.append_row_to(i, updated_result_table);
                        updated_result_table.push_back(edges[k].val);
                        updated_optional_matched_rows.push_back(true);
                    }
                } else {
                    res.append_row_to(i, updated_result_table);
                    updated_result_table.push_back(BLANK_ID);
                    updated_optional_matched_rows.push_back(true);
                }
            } else {
                for (uint64_t k = 0; k < sz; k++) {
                    res.append_row_to(i, updated_result_table);
                    // update attr table to map the result table
                    if (global_enable_vattr)
                        res.append_attr_row_to(i, updated_attr_table);
                    updated_result_table.push_back(edges[k].val);
                }
            }
        }
        res.result_table.swap(updated_result_table);
        if (req.pg_type == SPARQLQuery::PGType::OPTIONAL)
            res.optional_matched_rows.swap(updated_optional_matched_rows);
        if (global_enable_vattr)
            res.attr_res_table.swap(updated_attr_table);
        res.add_var2col(end, res.get_col_num());
        res.set_col_num(res.get_col_num() + 1);
        req.pattern_step++;
    }

    /// ?X P ?Y . (?X: KNOWN, P: GIVEN, ?X: UNKNOWN)
    /// e.g., "?X rdf:type ub:Course"
    ///       "?X ub:id ?Y"
    void known_to_unknown_attr(SPARQLQuery &req) {
        // prepare for query
        // the attr_res_table and result_table should be update
        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        SPARQLQuery::Result &res = req.result;

        ASSERT(d == OUT); // attribute always uses OUT

        std::vector<sid_t> updated_result_table;
        std::vector<attr_t> updated_attr_table;

        // In most time, the size of attr_res_table table is equal to the size of result_table
        // reserve size of updated_result_table to the size of result_table
        updated_attr_table.reserve(res.result_table.size());
        int type = req.get_pattern(req.pattern_step).pred_type ;
        variant_type get_type;
        for (int i = 0; i < res.get_row_num(); i++) {
            sid_t prev_id = res.get_row_col(i, res.var2col(start));
            bool has_value;
            attr_t v = graph->get_attr(tid, prev_id, pid, d, has_value);
            if (has_value) {
                res.append_row_to(i, updated_result_table);
                res.append_attr_row_to(i, updated_attr_table);
                updated_attr_table.push_back(v);
                type = boost::apply_visitor(get_type, v);
            }
        }

        // update the result table, attr_res_table and metadata
        res.result_table.swap(updated_result_table);
        res.attr_res_table.swap(updated_attr_table);
        res.add_var2col(end, res.get_attr_col_num(), type); // update the unknown_attr to known
        res.set_attr_col_num(res.get_attr_col_num() + 1);
        req.pattern_step++;
    }

    /// ?Y P ?X . (?Y:KNOWN, ?X: KNOWN)
    /// e.g., "?Z ub:undergraduateDegreeFrom ?X"
    ///       "?Z ub:memberOf ?Y"
    ///       "?Y ub:subOrganizationOf ?X"
    ///
    /// 1) Use [?Y]+[P] to retrieve all of neighbors (X')
    /// 2) Match KNOWN X with X'
    void known_to_known(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        SPARQLQuery::Result &res = req.result;

        vector<sid_t> updated_result_table;
        vector<attr_t> updated_attr_table;

        // simple dedup for consecutive same vertices
        sid_t cached = BLANK_ID;
        edge_t *edges = NULL;
        uint64_t sz = 0;
        for (int i = 0; i < res.get_row_num(); i++) {
            sid_t cur = res.get_row_col(i, res.var2col(start));
            if (cur != cached) {  // a new vertex
                cached = cur;
                edges = graph->get_triples(tid, cur, pid, d, sz);
            }

            sid_t known = res.get_row_col(i, res.var2col(end));
            if (req.pg_type == SPARQLQuery::PGType::OPTIONAL) {
                bool matched = false;
                for (uint64_t k = 0; k < sz; k++) {
                    if (edges[k].val == known) {
                        matched = true;
                        break;
                    }
                }
                if (res.optional_matched_rows[i] && (!matched)) req.correct_optional_result(i);
                res.optional_matched_rows[i] = (matched && res.optional_matched_rows[i]);
            } else {
                for (uint64_t k = 0; k < sz; k++) {
                    if (edges[k].val == known) {
                        // append a matched intermediate result
                        res.append_row_to(i, updated_result_table);
                        if (global_enable_vattr)
                            res.append_attr_row_to(i, updated_attr_table);
                        break;
                    }
                }
            }
        }
        if (req.pg_type != SPARQLQuery::PGType::OPTIONAL) {
            res.result_table.swap(updated_result_table);
            if (global_enable_vattr)
                res.attr_res_table.swap(updated_attr_table);
        }
        req.pattern_step++;
    }

    /// ?X P C . (?X is KNOWN)
    /// e.g., "?X rdf:type ub:FullProfessor"
    ///       "?X ub:worksFor <http://www.Department0.University0.edu>"
    ///
    /// 1) Use [?X]+[P] to retrieve all of neighbors (C')
    /// 2) Match const C with C'
    void known_to_const(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        SPARQLQuery::Result &res = req.result;

        vector<sid_t> updated_result_table;
        vector<attr_t> updated_attr_table;

        // simple dedup for consecutive same vertices
        sid_t cached = BLANK_ID;
        edge_t *edges = NULL;
        uint64_t sz = 0;
        bool exist = false;
        for (int i = 0; i < res.get_row_num(); i++) {
            sid_t cur = res.get_row_col(i, res.var2col(start));
            if (cur != cached) {  // a new vertex
                exist = false;
                cached = cur;
                edges = graph->get_triples(tid, cur, pid, d, sz);

                for (uint64_t k = 0; k < sz; k++) {
                    if (edges[k].val == end) {
                        // append a matched intermediate result
                        exist = true;
                        if (req.pg_type != SPARQLQuery::PGType::OPTIONAL) {
                            res.append_row_to(i, updated_result_table);
                            if (global_enable_vattr)
                                res.append_attr_row_to(i, updated_attr_table);
                        }
                        break;
                    }
                }
                if (req.pg_type == SPARQLQuery::PGType::OPTIONAL) {
                    if (res.optional_matched_rows[i] && (!exist)) req.correct_optional_result(i);
                    res.optional_matched_rows[i] = (exist && res.optional_matched_rows[i]);
                }
            } else {
                // the matching result can also be reused
                if (exist && req.pg_type != SPARQLQuery::PGType::OPTIONAL) {
                    res.append_row_to(i, updated_result_table);
                    if (global_enable_vattr)
                        res.append_attr_row_to(i, updated_attr_table);
                } else if (req.pg_type == SPARQLQuery::PGType::OPTIONAL) {
                    if (res.optional_matched_rows[i] && (!exist)) req.correct_optional_result(i);
                    res.optional_matched_rows[i] = (exist && res.optional_matched_rows[i]);
                }
            }

        }
        if (req.pg_type != SPARQLQuery::PGType::OPTIONAL) {
            res.result_table.swap(updated_result_table);
            if (global_enable_vattr)
                res.attr_res_table.swap(updated_attr_table);
        }
        req.pattern_step++;
    }

    /// C ?P ?X . (?P and ?X are UNKNOWN)
    /// e.g., "?X ?P <http://www.Department0.University0.edu>"
    ///
    /// 1) Use [C]+[__PREDICATE__] to retrieve all of predicates (?P)
    /// 2) Use [C]+[?P] to retrieve all of neighbors (?X)
    void const_unknown_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        SPARQLQuery::Result &res = req.result;

        vector<sid_t> updated_result_table;

        uint64_t npids = 0;
        edge_t *pids = graph->get_triples(tid, start, PREDICATE_ID, d, npids);

        // use a local buffer to store "known" predicates
        edge_t *tpids = (edge_t *)malloc(npids * sizeof(edge_t));
        memcpy((char *)tpids, (char *)pids, npids * sizeof(edge_t));

        for (uint64_t p = 0; p < npids; p++) {
            uint64_t sz = 0;
            edge_t *res = graph->get_triples(tid, start, tpids[p].val, d, sz);
            for (uint64_t k = 0; k < sz; k++) {
                updated_result_table.push_back(tpids[p].val);
                updated_result_table.push_back(res[k].val);
            }
        }

        free(tpids);

        res.result_table.swap(updated_result_table);
        res.set_col_num(2);
        res.add_var2col(pid, 0);
        res.add_var2col(end, 1);
        req.pattern_step++;
    }

    /// ?X ?P ?Y . (?X is KNOWN; ?P and ?X are UNKNOWN)
    /// e.g., "?X ub:subOrganizationOf <http://www.University0.edu>"
    ///       "?X ?P ?Y"
    ///
    /// 1) Use [?X]+[__PREDICATE__] to retrieve all of predicates (?P)
    /// 2) Use [?X]+[?P] to retrieve all of neighbors (?Y)
    void known_unknown_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        SPARQLQuery::Result &res = req.result;

        vector<sid_t> updated_result_table;

        for (int i = 0; i < res.get_row_num(); i++) {
            sid_t cur = res.get_row_col(i, res.var2col(start));
            uint64_t npids = 0;
            edge_t *pids = graph->get_triples(tid, cur, PREDICATE_ID, d, npids);

            // use a local buffer to store "known" predicates
            edge_t *tpids = (edge_t *)malloc(npids * sizeof(edge_t));
            memcpy((char *)tpids, (char *)pids, npids * sizeof(edge_t));

            for (uint64_t p = 0; p < npids; p++) {
                uint64_t sz = 0;
                edge_t *edges = graph->get_triples(tid, cur, tpids[p].val, d, sz);
                for (uint64_t k = 0; k < sz; k++) {
                    res.append_row_to(i, updated_result_table);
                    updated_result_table.push_back(tpids[p].val);
                    updated_result_table.push_back(edges[k].val);
                }
            }

            free(tpids);
        }

        res.result_table.swap(updated_result_table);
        res.add_var2col(pid, res.get_col_num());
        res.add_var2col(end, res.get_col_num() + 1);
        res.set_col_num(res.get_col_num() + 2);
        req.pattern_step++;
    }

    /// ?X ?P C . (?X is KNOWN; ?P is UNKNOWN)
    /// e.g., "<http://www.University0.edu> ub:subOrganizationOf ?X"
    ///       "?X ?P <http://www.Department4.University0.edu>"
    ///
    /// 1) Use [?X]+[__PREDICATE__] to retrieve all of predicates (?P)
    /// 2) Use [?X]+[?P] to retrieve all of neighbors (C')
    /// 3) Match const C with C'
    void known_unknown_const(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        SPARQLQuery::Result &result = req.result;

        vector<sid_t> updated_result_table;

        for (int i = 0; i < result.get_row_num(); i++) {
            sid_t prev_id = result.get_row_col(i, result.var2col(start));
            uint64_t npids = 0;
            edge_t *pids = graph->get_triples(tid, prev_id, PREDICATE_ID, d, npids);

            // use a local buffer to store "known" predicates
            edge_t *tpids = (edge_t *)malloc(npids * sizeof(edge_t));
            memcpy((char *)tpids, (char *)pids, npids * sizeof(edge_t));

            for (uint64_t p = 0; p < npids; p++) {
                uint64_t sz = 0;
                edge_t *res = graph->get_triples(tid, prev_id, tpids[p].val, d, sz);
                for (uint64_t k = 0; k < sz; k++) {
                    if (res[k].val == end) {
                        result.append_row_to(i, updated_result_table);
                        updated_result_table.push_back(tpids[p].val);
                        break;
                    }
                }
            }

            free(tpids);
        }

        result.result_table.swap(updated_result_table);
        result.add_var2col(pid, result.get_col_num());
        result.set_col_num(result.get_col_num() + 1);
        req.pattern_step++;
    }

    /// C1 ?P C2 . (?P is UNKNOWN)
    /// e.g., "<http://www.University0.edu> ?P <http://www.Department4.University0.edu>"
    ///
    /// 1) Use C+[__PREDICATE__] to retrieve all of predicates (?P)
    /// 2) Use [?X]+[?P] to retrieve all of neighbors (C')
    /// 3) Match const C with C'
    void const_unknown_const(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;

        // the query plan is wrong
        ASSERT(result.get_col_num() == 0);

        uint64_t npids = 0;
        edge_t *pids = graph->get_triples(tid, start, PREDICATE_ID, d, npids);

        // use a local buffer to store "known" predicates
        edge_t *tpids = (edge_t *)malloc(npids * sizeof(edge_t));
        memcpy((char *)tpids, (char *)pids, npids * sizeof(edge_t));

        for (uint64_t p = 0; p < npids; p++) {
            uint64_t sz = 0;
            edge_t *res = graph->get_triples(tid, start, tpids[p].val, d, sz);
            for (uint64_t k = 0; k < sz; k++) {
                if (res[k].val == end) {
                    updated_result_table.push_back(tpids[p].val);
                    break;
                }
            }
        }

        free(tpids);

        result.result_table.swap(updated_result_table);
        result.set_col_num(1);
        result.add_var2col(pid, 0);
        req.pattern_step++;
    }


    vector<SPARQLQuery> generate_sub_query(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t start = pattern.subject;

        // generate sub requests for all servers
        vector<SPARQLQuery> sub_reqs(global_num_servers);
        for (int i = 0; i < global_num_servers; i++) {
            sub_reqs[i].pqid = req.qid;
            sub_reqs[i].pg_type = req.pg_type == SPARQLQuery::PGType::UNION ?
                                  SPARQLQuery::PGType::BASIC : req.pg_type;
            sub_reqs[i].pattern_group = req.pattern_group;
            sub_reqs[i].pattern_step = req.pattern_step;
            sub_reqs[i].corun_step = req.corun_step;
            sub_reqs[i].fetch_step = req.fetch_step;
            sub_reqs[i].local_var = start;
            sub_reqs[i].priority = req.priority + 1;

            sub_reqs[i].result.col_num = req.result.col_num;
            sub_reqs[i].result.attr_col_num = req.result.attr_col_num;
            sub_reqs[i].result.blind = req.result.blind;
            sub_reqs[i].result.v2c_map  = req.result.v2c_map;
            sub_reqs[i].result.nvars  = req.result.nvars;
        }

        // group intermediate results to servers
        for (int i = 0; i < req.result.get_row_num(); i++) {
            int dst_sid = wukong::math::hash_mod(req.result.get_row_col(i, req.result.var2col(start)),
                                                 global_num_servers);
            req.result.append_row_to(i, sub_reqs[dst_sid].result.result_table);
            if (req.pg_type == SPARQLQuery::PGType::OPTIONAL)
                sub_reqs[dst_sid].result.optional_matched_rows.push_back(req.result.optional_matched_rows[i]);
            req.result.append_attr_row_to(i, sub_reqs[dst_sid].result.attr_res_table);
        }

        return sub_reqs;
    }

    // fork-join or in-place execution
    bool need_fork_join(SPARQLQuery &req) {
        // always need NOT fork-join when executing on single machine
        if (global_num_servers == 1) return false;

        // always need fork-join w/o RDMA
        if (!global_use_rdma) return true;

        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ASSERT(req.result.var_stat(pattern.subject) == KNOWN_VAR);
        ssid_t start = pattern.subject;
        return ((req.local_var != start) // next hop is not local
                && (req.result.get_row_num() >= global_rdma_threshold)); // FIXME: not consider dedup
    }

    void do_corun(SPARQLQuery &req) {
        SPARQLQuery::Result &req_result = req.result;
        int corun_step = req.corun_step;
        int fetch_step = req.fetch_step;

        // step.1 remove dup;
        uint64_t t0 = timer::get_usec();

        boost::unordered_set<sid_t> unique_set;
        ssid_t vid = req.get_pattern(corun_step).subject;
        ASSERT(vid < 0);
        int col_idx = req_result.var2col(vid);
        for (int i = 0; i < req_result.get_row_num(); i++)
            unique_set.insert(req_result.get_row_col(i, col_idx));

        // step.2 generate cmd_chain for sub-reqs
        SPARQLQuery::PatternGroup subgroup;
        vector<int> pvars_map; // from new_id to col_idx of id

        boost::unordered_map<sid_t, sid_t> sub_pvars;

        auto lambda = [&](ssid_t id) -> ssid_t {
            if (id < 0) { // remap pattern variable
                if (sub_pvars.find(id) == sub_pvars.end()) {
                    sid_t new_id = - (sub_pvars.size() + 1); // starts from -1
                    sub_pvars[id] = new_id;
                    pvars_map.push_back(req_result.var2col(id));
                }
                return sub_pvars[id];
            } else {
                return id;
            }
        };

        for (int i = corun_step; i < fetch_step; i++) {
            SPARQLQuery::Pattern &pattern = req.get_pattern(i);
            ssid_t subject = lambda(pattern.subject);
            ssid_t predicate = lambda(pattern.predicate);
            dir_t direction = pattern.direction;
            ssid_t object = lambda(pattern.object);
            SPARQLQuery::Pattern newPattern(subject, predicate, direction, object);
            newPattern.pred_type = 0;
            subgroup.patterns.push_back(newPattern);
        }

        // step.3 make sub-req
        SPARQLQuery sub_req;
        SPARQLQuery::Result &sub_result = sub_req.result;

        // query
        sub_req.pattern_group = subgroup;
        sub_result.nvars = pvars_map.size();

        // result
        boost::unordered_set<sid_t>::iterator iter;
        for (iter = unique_set.begin(); iter != unique_set.end(); iter++)
            sub_result.result_table.push_back(*iter);
        sub_result.col_num = 1;

        // init var_map
        sub_result.add_var2col(sub_pvars[vid], 0);

        sub_result.blind = false; // must take back results
        uint64_t t1 = timer::get_usec(); // time to generate the sub-request

        // step.4 execute sub-req
        while (true) {
            execute_one_pattern(sub_req);
            if (sub_req.done(SPARQLQuery::SQState::SQ_PATTERN))
                break;
        }
        uint64_t t2 = timer::get_usec(); // time to run the sub-request

        uint64_t t3, t4;
        vector<sid_t> updated_result_table;

        if (sub_result.get_col_num() > 2) { // qsort
            wukong::tuple::qsort_tuple(sub_result.get_col_num(), sub_result.result_table);

            t3 = timer::get_usec();
            vector<sid_t> tmp_vec;
            tmp_vec.resize(sub_result.get_col_num());
            for (int i = 0; i < req_result.get_row_num(); i++) {
                for (int c = 0; c < pvars_map.size(); c++)
                    tmp_vec[c] = req_result.get_row_col(i, pvars_map[c]);

                if (wukong::tuple::binary_search_tuple(sub_result.get_col_num(),
                                                       sub_result.result_table, tmp_vec))
                    req_result.append_row_to(i, updated_result_table);
            }
            t4 = timer::get_usec();
        } else { // hash join
            boost::unordered_set<int64_pair> remote_set;
            for (int i = 0; i < sub_result.get_row_num(); i++)
                remote_set.insert(int64_pair(sub_result.get_row_col(i, 0),
                                             sub_result.get_row_col(i, 1)));

            t3 = timer::get_usec();
            vector<sid_t> tmp_vec;
            tmp_vec.resize(sub_result.get_col_num());
            for (int i = 0; i < req_result.get_row_num(); i++) {
                for (int c = 0; c < pvars_map.size(); c++)
                    tmp_vec[c] = req_result.get_row_col(i, pvars_map[c]);

                int64_pair target = int64_pair(tmp_vec[0], tmp_vec[1]);
                if (remote_set.find(target) != remote_set.end())
                    req_result.append_row_to(i, updated_result_table);
            }
            t4 = timer::get_usec();
        }

        req_result.result_table.swap(updated_result_table);
        req.pattern_step = fetch_step;
    }

    bool execute_one_pattern(SPARQLQuery &req) {
        ASSERT(!req.done(SPARQLQuery::SQState::SQ_PATTERN));

        logstream(LOG_DEBUG) << "[" << sid << "-" << tid << "]"
                             << " step=" << req.pattern_step << LOG_endl;

        SPARQLQuery::Pattern &pattern = req.get_pattern();
        ssid_t start     = pattern.subject;
        ssid_t predicate = pattern.predicate;
        dir_t direction  = pattern.direction;
        ssid_t end       = pattern.object;

        if (req.pattern_step == 0 && req.start_from_index()) {
            if (req.result.var2col(end) != NO_RESULT)
                index_to_known(req);
            else
                index_to_unknown(req);
            return true;
        }

        // triple pattern with UNKNOWN predicate/attribute
        if (req.result.var_stat(predicate) != CONST_VAR) {
#ifdef VERSATILE
            /// Now unsupported UNKNOWN predicate with vertex attribute enabling.
            /// When doing the query, we judge request of vertex attribute by its predicate.
            /// Therefore we should known the predicate.
            if (global_enable_vattr) {
                logstream(LOG_ERROR) << "Unsupported UNKNOWN predicate with vertex attribute enabling." << LOG_endl;
                logstream(LOG_ERROR) << "Please turn off the vertex attribute enabling." << LOG_endl;
                ASSERT(false);
            }
            switch (const_pair(req.result.var_stat(start),
                               req.result.var_stat(end))) {

            // start from CONST
            case const_pair(CONST_VAR, UNKNOWN_VAR):
                const_unknown_unknown(req);
                break;
            case const_pair(CONST_VAR, CONST_VAR):
                const_unknown_const(req);
                break;
            case const_pair(CONST_VAR, KNOWN_VAR):
                // FIXME: possible or not?
                logstream(LOG_ERROR) << "Unsupported triple pattern [CONST|UNKNOWN|KNOWN]." << LOG_endl;
                ASSERT(false);

            // start from KNOWN
            case const_pair(KNOWN_VAR, UNKNOWN_VAR):
                known_unknown_unknown(req);
                break;
            case const_pair(KNOWN_VAR, CONST_VAR):
                known_unknown_const(req);
                break;
            case const_pair(KNOWN_VAR, KNOWN_VAR):
                // FIXME: possible or not?
                logstream(LOG_ERROR) << "Unsupported triple pattern [KNOWN|UNKNOWN|KNOWN]." << LOG_endl;
                ASSERT(false);

            // start from UNKNOWN (incorrect query plan)
            case const_pair(UNKNOWN_VAR, CONST_VAR):
            case const_pair(UNKNOWN_VAR, KNOWN_VAR):
            case const_pair(UNKNOWN_VAR, UNKNOWN_VAR):
                logstream(LOG_ERROR) << "Unsupported triple pattern [UNKNOWN|UNKNOWN|??]" << LOG_endl;
                ASSERT(false);

            default:
                logstream(LOG_ERROR) << "Unsupported triple pattern (UNKNOWN predicate) "
                                     << "(" << req.result.var_stat(start)
                                     << "|" << req.result.var_stat(end)
                                     << ")." << LOG_endl;
                ASSERT(false);
            }

            return true;
#else
            logstream(LOG_ERROR) << "Unsupported variable at predicate." << LOG_endl;
            logstream(LOG_ERROR) << "Please add definition VERSATILE in CMakeLists.txt." << LOG_endl;
            ASSERT(false);
#endif
        }

        // triple pattern with attribute
        if (global_enable_vattr && req.get_pattern(req.pattern_step).pred_type > 0) {
            switch (const_pair(req.result.var_stat(start),
                               req.result.var_stat(end))) {
            // now support const_to_unknown_attr and known_to_unknown_attr
            case const_pair(CONST_VAR, UNKNOWN_VAR):
                const_to_unknown_attr(req);
                break;
            case const_pair(KNOWN_VAR, UNKNOWN_VAR):
                known_to_unknown_attr(req);
                break;
            default:
                logstream(LOG_ERROR) << "Unsupported triple pattern with attribute "
                                     << "(" << req.result.var_stat(start)
                                     << "|" << req.result.var_stat(end)
                                     << ")" << LOG_endl;
                ASSERT(false);
            }
            return true;
        }

        // triple pattern with KNOWN predicate
        switch (const_pair(req.result.var_stat(start),
                           req.result.var_stat(end))) {

        // start from CONST
        case const_pair(CONST_VAR, CONST_VAR):
            logstream(LOG_ERROR) << "Unsupported triple pattern [CONST|KNOWN|CONST]" << LOG_endl;
            ASSERT(false);
        case const_pair(CONST_VAR, KNOWN_VAR):
            const_to_known(req);
            break;
        case const_pair(CONST_VAR, UNKNOWN_VAR):
            const_to_unknown(req);
            break;

        // start from KNOWN
        case const_pair(KNOWN_VAR, CONST_VAR):
            known_to_const(req);
            break;
        case const_pair(KNOWN_VAR, KNOWN_VAR):
            known_to_known(req);
            break;
        case const_pair(KNOWN_VAR, UNKNOWN_VAR):
            known_to_unknown(req);
            break;

        // start from UNKNOWN (incorrect query plan)
        case const_pair(UNKNOWN_VAR, CONST_VAR):
        case const_pair(UNKNOWN_VAR, KNOWN_VAR):
        case const_pair(UNKNOWN_VAR, UNKNOWN_VAR):
            logstream(LOG_ERROR) << "Unsupported triple pattern [UNKNOWN|KNOWN|??]" << LOG_endl;
            ASSERT(false);

        default:
            logstream(LOG_ERROR) << "Unsupported triple pattern with known predicate "
                                 << "(" << req.result.var_stat(start)
                                 << "|" << req.result.var_stat(end)
                                 << ")" << LOG_endl;
            ASSERT(false);
        }

        return true;
    }

    bool dispatch(SPARQLQuery &r) {
        if (QUERY_FROM_PROXY(r)
                && r.pattern_step == 0  // not started
                && r.start_from_index()  // heavy query (heuristics)
                && (global_num_servers * r.mt_factor > 1)) {  // multi-resource
            logstream(LOG_DEBUG) << "[" << sid << "-" << tid << "] dispatch "
                                 << "Q(qid=" << r.qid << ", pqid=" << r.pqid
                                 << ", step=" << r.pattern_step << ")" << LOG_endl;
            // NOTE: the mt_factor can be set on proxy side before sending to engine,
            // but must smaller than global_mt_threshold (default: mt_factor == 1)
            // Normally, we will NOT let global_mt_threshold == #engines, which will cause HANG

            // register rmap for waiting reply
            rmap.put_parent_request(r, global_num_servers * r.mt_factor);

            // dispatch (sub)requests
            SPARQLQuery sub_query = r;
            for (int i = 0; i < global_num_servers; i++) {
                for (int j = 0; j < r.mt_factor; j++) {
                    sub_query.pqid = r.qid;
                    sub_query.qid = -1;
                    sub_query.tid = j;

                    // start from the next engine thread
                    int dst_tid = global_num_proxies
                                  + (tid + j + 1 - global_num_proxies) % global_num_engines;

                    Bundle bundle(sub_query);
                    msgr->send_msg(bundle, i, dst_tid);
                }
            }
            return true;
        }
        return false;
    }

    bool execute_patterns(SPARQLQuery &r) {
        logstream(LOG_DEBUG) << "[" << sid << "-" << tid << "] execute patterns of "
                             << "Q(pqid=" << r.pqid << ", qid=" << r.qid
                             << ", step=" << r.pattern_step << ")" << LOG_endl;
        do {
            execute_one_pattern(r);
            logstream(LOG_DEBUG) << "step: " << r.pattern_step
                                 << " #rows = " << r.result.get_row_num() << LOG_endl;;

            // co-run optimization
            if (r.corun_enabled && (r.pattern_step == r.corun_step))
                do_corun(r);

            if (r.done(SPARQLQuery::SQState::SQ_PATTERN)) {
                // only send back row_num in blind mode
                r.result.row_num = r.result.get_row_num();
                return true;
            }

            if (need_fork_join(r)) {
                vector<SPARQLQuery> sub_reqs = generate_sub_query(r);
                rmap.put_parent_request(r, sub_reqs.size());
                for (int i = 0; i < sub_reqs.size(); i++) {
                    if (i != sid) {
                        Bundle bundle(sub_reqs[i]);
                        msgr->send_msg(bundle, i, tid);
                    } else {
                        prior_stage.push(sub_reqs[i]);
                    }
                }
                return false;
            }
        } while (true);
    }


// relational operator: < <= > >= == !=
    void relational_filter(SPARQLQuery::Filter &filter,
                           SPARQLQuery::Result &result,
                           vector<bool> &is_satisfy) {
        int col1 = (filter.arg1->type == SPARQLQuery::Filter::Type::Variable)
                   ? result.var2col(filter.arg1->valueArg) : -1;
        int col2 = (filter.arg2->type == SPARQLQuery::Filter::Type::Variable)
                   ? result.var2col(filter.arg2->valueArg) : -1;

        auto get_str = [&](SPARQLQuery::Filter & filter, int row, int col) -> string {
            int id = 0;
            switch (filter.type) {
            case SPARQLQuery::Filter::Type::Variable:
                id = result.get_row_col(row, col);
                return str_server->exist(id) ? str_server->id2str[id] : "";
            case SPARQLQuery::Filter::Type::Literal:
                return "\"" + filter.value + "\"";
            default:
                logstream(LOG_ERROR) << "Unsupported FILTER type" << LOG_endl;
                ASSERT(false);
            }
            return "";
        };

        switch (filter.type) {
        case SPARQLQuery::Filter::Type::Equal:
            for (int row = 0; row < result.get_row_num(); row ++)
                if (is_satisfy[row]
                        && (get_str(*filter.arg1, row, col1)
                            != get_str(*filter.arg2, row, col2)))
                    is_satisfy[row] = false;
            break;
        case SPARQLQuery::Filter::Type::NotEqual:
            for (int row = 0; row < result.get_row_num(); row ++)
                if (is_satisfy[row]
                        && (get_str(*filter.arg1, row, col1)
                            == get_str(*filter.arg2, row, col2)))
                    is_satisfy[row] = false;
            break;
        case SPARQLQuery::Filter::Type::Less:
            for (int row = 0; row < result.get_row_num(); row ++)
                if (is_satisfy[row]
                        && (get_str(*filter.arg1, row, col1)
                            >= get_str(*filter.arg2, row, col2)))
                    is_satisfy[row] = false;
            break;
        case SPARQLQuery::Filter::Type::LessOrEqual:
            for (int row = 0; row < result.get_row_num(); row ++)
                if (is_satisfy[row]
                        && (get_str(*filter.arg1, row, col1)
                            > get_str(*filter.arg2, row, col2)))
                    is_satisfy[row] = false;
            break;
        case SPARQLQuery::Filter::Type::Greater:
            for (int row = 0; row < result.get_row_num(); row ++)
                if (is_satisfy[row]
                        && (get_str(*filter.arg1, row, col1)
                            <= get_str(*filter.arg2, row, col2)))
                    is_satisfy[row] = false;
            break;
        case SPARQLQuery::Filter::Type::GreaterOrEqual:
            for (int row = 0; row < result.get_row_num(); row ++)
                if (is_satisfy[row]
                        && get_str(*filter.arg1, row, col1)
                        < get_str(*filter.arg2, row, col2))
                    is_satisfy[row] = false;
            break;
        }
    }

    void bound_filter(SPARQLQuery::Filter &filter,
                      SPARQLQuery::Result &result,
                      vector<bool> &is_satisfy) {
        int col = result.var2col(filter.arg1 -> valueArg);

        for (int row = 0; row < is_satisfy.size(); row ++) {
            if (!is_satisfy[row])
                continue;

            if (result.get_row_col(row, col) == BLANK_ID)
                is_satisfy[row] = false;
        }
    }

// IRI and URI are the same in SPARQL
    void isIRI_filter(SPARQLQuery::Filter &filter,
                      SPARQLQuery::Result &result,
                      vector<bool> &is_satisfy) {
        int col = result.var2col(filter.arg1->valueArg);

        string IRI_REF = R"(<([^<>\\"{}|^`\\])*>)";
        string prefixed_name = ".*:.*";
        string IRIref_str = "(" + IRI_REF + "|" + prefixed_name + ")";

        regex IRI_pattern(IRIref_str);
        for (int row = 0; row < is_satisfy.size(); row ++) {
            if (!is_satisfy[row])
                continue;

            int id = result.get_row_col(row, col);
            string str = str_server->exist(id) ? str_server->id2str[id] : "";
            if (!regex_match(str, IRI_pattern))
                is_satisfy[row] = false;
        }
    }

    void isliteral_filter(SPARQLQuery::Filter &filter,
                          SPARQLQuery::Result &result,
                          vector<bool> &is_satisfy) {
        int col = result.var2col(filter.arg1->valueArg);

        string langtag_pattern_str("@[a-zA-Z]+(-[a-zA-Z0-9]+)*");

        string literal1_str = R"('([^\x27\x5C\x0A\x0D]|\\[tbnrf\"'])*')";
        string literal2_str = R"("([^\x22\x5C\x0A\x0D]|\\[tbnrf\"'])*")";
        string literal_long1_str = R"('''(('|'')?([^'\\]|\\[tbnrf\"']))*''')";
        string literal_long2_str = R"("""(("|"")?([^"\\]|\\[tbnrf\"']))*""")";
        string literal = "(" + literal1_str + "|" + literal2_str + "|"
                         + literal_long1_str + "|" + literal_long2_str + ")";

        string IRI_REF = R"(<([^<>\\"{}|^`\\])*>)";
        string prefixed_name = ".*:.*";
        string IRIref_str = "(" + IRI_REF + "|" + prefixed_name + ")";

        regex RDFLiteral_pattern(literal + "(" + langtag_pattern_str + "|(\\^\\^" + IRIref_str +  "))?");

        for (int row = 0; row < is_satisfy.size(); row ++) {
            if (!is_satisfy[row])
                continue;

            int id = result.get_row_col(row, col);
            string str = str_server->exist(id) ? str_server->id2str[id] : "";
            if (!regex_match(str, RDFLiteral_pattern))
                is_satisfy[row] = false;
        }
    }

// regex flag only support "i" option now
    void regex_filter(SPARQLQuery::Filter &filter,
                      SPARQLQuery::Result &result,
                      vector<bool> &is_satisfy) {
        regex pattern;
        if (filter.arg3 != nullptr && filter.arg3->value == "i")
            pattern = regex(filter.arg2->value, std::regex::icase);
        else
            pattern = regex(filter.arg2->value);

        int col = result.var2col(filter.arg1->valueArg);
        for (int row = 0; row < is_satisfy.size(); row ++) {
            if (!is_satisfy[row])
                continue;

            int id = result.get_row_col(row, col);
            string str = str_server->exist(id) ? str_server->id2str[id] : "";
            if (str.front() != '\"' || str.back() != '\"')
                logstream(LOG_ERROR) << "The first parameter of function regex must be string"
                                     << LOG_endl;
            else
                str = str.substr(1, str.length() - 2);

            if (!regex_match(str, pattern))
                is_satisfy[row] = false;
        }
    }

    void general_filter(SPARQLQuery::Filter &filter,
                        SPARQLQuery::Result &result,
                        vector<bool> &is_satisfy) {
        if (filter.type <= SPARQLQuery::Filter::Type::And) {
            // conditional operator: Or(0), And(1)
            vector<bool> is_satisfy1(result.get_row_num(), true);
            vector<bool> is_satisfy2(result.get_row_num(), true);
            if (filter.type == SPARQLQuery::Filter::Type::And) {
                general_filter(*filter.arg1, result, is_satisfy);
                general_filter(*filter.arg2, result, is_satisfy);
            } else if (filter.type == SPARQLQuery::Filter::Type::Or) {
                general_filter(*filter.arg1, result, is_satisfy1);
                general_filter(*filter.arg2, result, is_satisfy2);
                for (int i = 0; i < is_satisfy.size(); i ++)
                    is_satisfy[i] = is_satisfy[i] && (is_satisfy1[i] || is_satisfy2[i]);
            }
        } else if (filter.type <= SPARQLQuery::Filter::Type::GreaterOrEqual) {
            // relational operator: Equal(2), NotEqual(3), Less(4), LessOrEqual(5),
            //                      Greater(6), GreaterOrEqual(7)
            relational_filter(filter, result, is_satisfy);
        } else if (filter.type == SPARQLQuery::Filter::Type::Builtin_bound) {
            bound_filter(filter, result, is_satisfy);
        } else if (filter.type == SPARQLQuery::Filter::Type::Builtin_isiri) {
            isIRI_filter(filter, result, is_satisfy);
        } else if (filter.type == SPARQLQuery::Filter::Type::Builtin_isliteral) {
            isliteral_filter(filter, result, is_satisfy);
        } else if (filter.type == SPARQLQuery::Filter::Type::Builtin_regex) {
            regex_filter(filter, result, is_satisfy);
        } else {
            ASSERT(false); // unsupport filter type
        }
    }

    void execute_filter(SPARQLQuery &r) {
        ASSERT(r.has_filter());

        // during filtering, flag of unsatified row will be set to false one by one
        vector<bool> is_satisfy(r.result.get_row_num(), true);

        for (int i = 0; i < r.pattern_group.filters.size(); i ++) {
            SPARQLQuery::Filter filter = r.pattern_group.filters[i];
            general_filter(filter, r.result, is_satisfy);
        }

        vector<sid_t> new_table;
        for (int row = 0; row < r.result.get_row_num(); row ++)
            if (is_satisfy[row])
                r.result.append_row_to(row, new_table);

        r.result.result_table.swap(new_table);
        r.result.row_num = r.result.get_row_num();
    }

    class Compare {
    private:
        SPARQLQuery &query;
        String_Server *str_server;
    public:
        Compare(SPARQLQuery &query, String_Server *str_server)
            : query(query), str_server(str_server) { }

        bool operator()(const int* a, const int* b) {
            int cmp = 0;
            for (int i = 0; i < query.orders.size(); i ++) {
                int col = query.result.var2col(query.orders[i].id);
                string str_a = str_server->exist(a[col]) ? str_server->id2str[a[col]] : "";
                string str_b = str_server->exist(a[col]) ? str_server->id2str[b[col]] : "";
                cmp = str_a.compare(str_b);
                if (cmp != 0) {
                    cmp = query.orders[i].descending ? -cmp : cmp;
                    break;
                }
            }
            return cmp < 0;
        }
    };

    class ReduceCmp {
    private:
        int col_num;
    public:
        ReduceCmp(int col_num): col_num(col_num) { }

        bool operator()(const int* a, const int* b) {
            for (int i = 0; i < col_num; i ++) {
                if (a[i] == b[i])
                    continue;
                return a[i] < b[i];
            }
            return 0;
        }
    };

    void final_process(SPARQLQuery &r) {
        if (r.result.blind || r.result.result_table.size() == 0)
            return;

        // DISTINCT and ORDER BY
        if (r.distinct || r.orders.size() > 0) {
            // initialize table
            int **table;
            int size = r.result.get_row_num();
            int new_size = size;

            table = new int*[size];
            for (int i = 0; i < size; i ++)
                table[i] = new int[r.result.col_num];

            for (int i = 0; i < size; i ++)
                for (int j = 0; j < r.result.col_num; j ++)
                    table[i][j] = r.result.get_row_col(i, j);

            // DISTINCT
            if (r.distinct) {
                // sort and then compare
                sort(table, table + size, ReduceCmp(r.result.col_num));
                int p = 0, q = 1;
                auto equal = [&r](int *a, int *b) -> bool{
                    for (int i = 0; i < r.result.required_vars.size(); i ++) {
                        int col = r.result.var2col(r.result.required_vars[i]);
                        if (a[col] != b[col]) return false;
                    }
                    return true;
                };

                auto swap = [](int *&a, int *&b) {
                    int *temp = a;
                    a = b;
                    b = temp;
                };

                while (q < size) {
                    while (equal(table[p], table[q])) {
                        q++;
                        if (q >= size) goto out;
                    }
                    p ++;
                    swap(table[p], table[q]);
                    q ++;
                }
out:
                new_size = p + 1;
            }

            // ORDER BY
            if (r.orders.size() > 0)
                sort(table, table + new_size, Compare(r, str_server));

            // write back data and delete **table
            for (int i = 0; i < new_size; i ++)
                for (int j = 0; j < r.result.col_num; j ++)
                    r.result.result_table[r.result.col_num * i + j] = table[i][j];

            if (new_size < size)
                r.result.result_table.erase(r.result.result_table.begin() + new_size * r.result.col_num,
                                            r.result.result_table.begin() + size * r.result.col_num);

            for (int i = 0; i < size; i ++)
                delete[] table[i];
            delete[] table;
        }

        // OFFSET
        if (r.offset > 0)
            r.result.result_table.erase(r.result.result_table.begin(),
                                        min(r.result.result_table.begin()
                                            + r.offset * r.result.col_num,
                                            r.result.result_table.end()));

        // LIMIT
        if (r.limit >= 0)
            r.result.result_table.erase(min(r.result.result_table.begin()
                                            + r.limit * r.result.col_num,
                                            r.result.result_table.end()),
                                        r.result.result_table.end());

        // remove unrequested variables
        // separate var to normal and attribute
        // need to think about attribute result table
        vector<ssid_t> normal_var;
        vector<ssid_t> attr_var;
        for (int i = 0; i < r.result.required_vars.size(); i++) {
            ssid_t vid = r.result.required_vars[i];
            if (r.result.var_type(vid) == ENTITY)
                normal_var.push_back(vid); // entity
            else
                attr_var.push_back(vid); // attributed
        }

        int new_row_num = r.result.get_row_num();
        int new_col_num = normal_var.size();
        int new_attr_col_num = attr_var.size();

        //update result table
        vector<sid_t> new_result_table(new_row_num * new_col_num);
        for (int i = 0; i < new_row_num; i ++) {
            for (int j = 0; j < new_col_num; j++) {
                int col = r.result.var2col(normal_var[j]);
                new_result_table[i * new_col_num + j] = r.result.get_row_col(i, col);
            }
        }

        r.result.result_table.swap(new_result_table);
        r.result.col_num = new_col_num;
        r.result.row_num = r.result.get_row_num();

        // update attribute result table
        vector<attr_t> new_attr_result_table(new_row_num * new_attr_col_num);
        for (int i = 0; i < new_row_num; i ++) {
            for (int j = 0; j < new_attr_col_num; j++) {
                int col = r.result.var2col(attr_var[j]);
                new_attr_result_table[i * new_attr_col_num + j] = r.result.get_attr_row_col(i, col);
            }
        }
        r.result.attr_res_table.swap(new_attr_result_table);
        r.result.attr_col_num = new_attr_col_num;
    }

public:
    tbb::concurrent_queue<SPARQLQuery> prior_stage;

    SPARQLEngine(int sid, int tid, String_Server *str_server,
                 DGraph *graph, Coder *coder, Messenger *msgr)
        : sid(sid), tid(tid), str_server(str_server),
          graph(graph), coder(coder), msgr(msgr) {

        pthread_spin_init(&rmap_lock, 0);
    }

    void execute_sparql_query(SPARQLQuery &r) {
        // encode the lineage of the query (server & thread)
        if (r.qid == -1) r.qid = coder->get_and_inc_qid();

        // 0. query has done
        if (r.state == SPARQLQuery::SQState::SQ_REPLY) {
            pthread_spin_lock(&rmap_lock);
            rmap.put_reply(r);

            if (!rmap.is_ready(r.pqid)) {
                pthread_spin_unlock(&rmap_lock);
                return; // not ready (waiting for the rest)
            }

            // all sub-queries have done, continue to execute
            r = rmap.get_merged_reply(r.pqid);
            pthread_spin_unlock(&rmap_lock);
        }

        // 1. Pattern
        if (r.has_pattern() && !r.done(SPARQLQuery::SQState::SQ_PATTERN)) {
            r.state = SPARQLQuery::SQState::SQ_PATTERN;

            // exploit parallelism (multi-server and multi-threading)
            if (dispatch(r))
                return; // async waiting reply by rmap

            if (!execute_patterns(r))
                return; // outstanding
        }

        // 2. Union
        if (r.has_union() && !r.done(SPARQLQuery::SQState::SQ_UNION)) {
            r.state = SPARQLQuery::SQState::SQ_UNION;
            int size = r.pattern_group.unions.size();
            r.union_done = true;
            rmap.put_parent_request(r, size);
            for (int i = 0; i < size; i++) {
                SPARQLQuery union_req;
                union_req.inherit_union(r, i);
                int dst_sid = wukong::math::hash_mod(union_req.pattern_group.get_start(),
                                                     global_num_servers);
                if (dst_sid != sid) {
                    Bundle bundle(union_req);
                    msgr->send_msg(bundle, dst_sid, tid);
                } else {
                    prior_stage.push(union_req);
                }
            }
            return;
        }

        // 3. Optional
        if (r.has_optional() && !r.done(SPARQLQuery::SQState::SQ_OPTIONAL)) {
            r.state = SPARQLQuery::SQState::SQ_OPTIONAL;
            SPARQLQuery optional_req;
            optional_req.inherit_optional(r);
            r.optional_step++;
            if (need_fork_join(optional_req)) {
                optional_req.qid = r.qid;
                vector<SPARQLQuery> sub_reqs = generate_sub_query(optional_req);
                rmap.put_parent_request(r, sub_reqs.size());
                for (int i = 0; i < sub_reqs.size(); i++) {
                    if (i != sid) {
                        Bundle bundle(sub_reqs[i]);
                        msgr->send_msg(bundle, i, tid);
                    } else {
                        prior_stage.push(sub_reqs[i]);
                    }
                }
            } else {
                rmap.put_parent_request(r, 1);
                int dst_sid = wukong::math::hash_mod(optional_req.pattern_group.get_start(),
                                                     global_num_servers);
                if (dst_sid != sid) {
                    Bundle bundle(optional_req);
                    msgr->send_msg(bundle, dst_sid, tid);
                } else {
                    prior_stage.push(optional_req);
                }
            }
            return;
        }

        // 4. Filter
        if (r.has_filter()) {
            r.state = SPARQLQuery::SQState::SQ_FILTER;
            execute_filter(r);
        }

        // 5. Final
        if (QUERY_FROM_PROXY(r)) {
            r.state = SPARQLQuery::SQState::SQ_FINAL;
            final_process(r);
        }

        // 6. Reply
        r.shrink_query();
        r.state = SPARQLQuery::SQState::SQ_REPLY;
        Bundle bundle(r);
        msgr->send_msg(bundle, coder->sid_of(r.pqid), coder->tid_of(r.pqid));
    }

};

