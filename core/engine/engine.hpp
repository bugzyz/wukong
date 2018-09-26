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

#include <tbb/concurrent_queue.h>
#include <algorithm> //sort
#include <regex>

#include "global.hpp"
#include "type.hpp"
#include "coder.hpp"
#include "dgraph.hpp"
#include "query.hpp"
#include "timer.hpp"

#include "sparql.hpp"
#include "rdf.hpp"
#include "msgr.hpp"

#include "comm/adaptor.hpp"

#include "assertion.hpp"


using namespace std;


#define BUSY_POLLING_THRESHOLD 10000000 // busy polling task queue 10s
#define MIN_SNOOZE_TIME 10 // MIX snooze time
#define MAX_SNOOZE_TIME 80 // MAX snooze time

// a vector of pointers of all local engines
class Engine;
std::vector<Engine *> engines;


class Engine {
private:
    void execute(Bundle &bundle) {
        if (bundle.type == SPARQL_QUERY) {
            SPARQLQuery r = bundle.get_sparql_query();
            sparql->execute_sparql_query(r);
        } else if (bundle.type == GSTORE_CHECK) {
            GStoreCheck r = bundle.get_gstore_check();
            rdf->execute_gstore_check(r);
        }
#ifdef DYNAMIC_GSTORE
        else if (bundle.type == DYNAMIC_LOAD) {
            RDFLoad r = bundle.get_rdf_load();
            rdf->execute_load_data(r);
        }
#endif
        else
            // print error msg and just skip the request
            logstream(LOG_ERROR) << "Unsupported type of request." << LOG_endl;
    }

    int next_to_oblige(int own_id, int offset) {
        // pair stealing
        if (global_stealing_pattern == 0) { 
            if (offset == 1)
                return ((global_num_engines - 1) - own_id);
            else
                return -1;
        // ring stealing
        } else if (global_stealing_pattern == 1) {
            return ((own_id + offset) % global_num_engines);
        }
        return -1;
    }

public:
    const static uint64_t TIMEOUT_THRESHOLD = 10000; // 10 msec

    int sid;    // server id
    int tid;    // thread id

    String_Server *str_server;
    DGraph *graph;
    Adaptor *adaptor;

    Coder *coder;
    Messenger *msgr;
    SPARQLEngine *sparql;
    RDFEngine *rdf;

    bool at_work; // whether engine is at work or not
    uint64_t last_time; // busy or not (work-oblige)

    tbb::concurrent_queue<SPARQLQuery> runqueue; // task queue for sparql queries

    Engine(int sid, int tid, String_Server *str_server, DGraph *graph, Adaptor *adaptor)
        : sid(sid), tid(tid), last_time(timer::get_usec()),
          str_server(str_server), graph(graph), adaptor(adaptor) {

        coder = new Coder(sid, tid);
        msgr = new Messenger(sid, tid, adaptor);
        sparql = new SPARQLEngine(sid, tid, str_server, graph, coder, msgr);
        rdf = new RDFEngine(sid, tid, graph, coder, msgr);
    }

    void run() {
        // NOTE: the 'tid' of engine is not start from 0,
        // which can not be used by engines[] directly
        int own_id = tid - global_num_proxies;

        uint64_t snooze_interval = MIN_SNOOZE_TIME;

        // reset snooze
        auto reset_snooze = [&snooze_interval](bool & at_work, uint64_t &last_time) {
            at_work = true; // keep calm (no snooze)
            last_time = timer::get_usec();
            snooze_interval = MIN_SNOOZE_TIME;
        };


        while (true) {
            at_work = false;

            // check and send pending messages first
            msgr->sweep_msgs();

            // priority path: sparql stage (FIXME: only for SPARQL queries)
            SPARQLQuery req;
            at_work = sparql->prior_stage.try_pop(req);
            if (at_work) {
                reset_snooze(at_work, last_time);
                sparql->execute_sparql_query(req);
                continue; // exhaust all queries
            }

            // normal path: own runqueue
            Bundle bundle;
            while (adaptor->tryrecv(bundle)) {
                if (bundle.type == SPARQL_QUERY) {
                    // to be fair, engine will handle sub-queries priority,
                    // instead of processing a new task.
                    SPARQLQuery req = bundle.get_sparql_query();
                    if (req.priority != 0) {
                        reset_snooze(at_work, last_time);
                        sparql->execute_sparql_query(req);
                        break;
                    }

                    runqueue.push(req);
                } else {
                    // FIXME: Jump a queue!
                    reset_snooze(at_work, last_time);
                    execute(bundle);
                    break;
                }
            }

            if (!at_work) {
                SPARQLQuery req;
                if (runqueue.try_pop(req)) {
                    // process a new SPARQL query
                    reset_snooze(at_work, last_time);
                    sparql->execute_sparql_query(req);
                }
            }

            // if work stealing is enabled
            // FIXME: only steal SPARQL queries from runqueue,
            //        steal adaptor as well will significantly improve stealing effect
            //        the problem is, what if a work stealed from adaptor is not SPARQL query ?
            if (global_enable_workstealing) {
                bool success;
                int offset = 1;
                int next_engine;
                SPARQLQuery req;
                
                do {
                    success = false;
                    next_engine = next_to_oblige(own_id, offset);
                    if (next_engine == -1)
                        break;
                    
                    if (engines[next_engine]->at_work
                            && ((timer::get_usec() - engines[next_engine]->last_time) >= TIMEOUT_THRESHOLD)
                            && (engines[next_engine]->runqueue.try_pop(req))) {
                        reset_snooze(at_work, last_time);
                        sparql->execute_sparql_query(req);
                        success = true;
                        offset++;
                    }
                } while (success);
            }

            if (at_work) continue; // keep calm (no snooze)

            // busy polling a little while (BUSY_POLLING_THRESHOLD) before snooze
            if ((timer::get_usec() - last_time) >= BUSY_POLLING_THRESHOLD) {
                timer::cpu_relax(snooze_interval); // relax CPU (snooze)

                // double snooze time till MAX_SNOOZE_TIME
                snooze_interval *= snooze_interval < MAX_SNOOZE_TIME ? 2 : 1;
            }
        }
    }
};
