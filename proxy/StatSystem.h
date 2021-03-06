/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

/****************************************************************************

  StatSystem.h --
  Created On          : Fri Apr 3 19:41:39 1998
 ****************************************************************************/
#if !defined (_StatSystem_h_)
#define _StatSystem_h_

#include "ink_platform.h"
#include "ink_hrtime.h"
#include "ink_atomic.h"
#ifdef USE_LOCKS_FOR_DYN_STATS
#include "Lock.h"
#endif

#include "ink_apidefs.h"

#define STATS_MAJOR_VERSION    6        // increment when changing the stats!
#define DEFAULT_SNAP_FILENAME             "stats.snap"

/////////////////////////////////////////////////////////////
//
// class TransactionMilestones
//
/////////////////////////////////////////////////////////////
class TransactionMilestones
{
public:
  ////////////////////////////////////////////////////////
  // user agent:                                        //
  // user_agent_begin represents the time this          //
  // transaction started. If this is the first          //
  // transaction in a connection, then user_agent_begin //
  // is set to accept time. otherwise it is set to      //
  // first read time.                                   //
  ////////////////////////////////////////////////////////
  ink_hrtime ua_begin;
  ink_hrtime ua_read_header_done;
  ink_hrtime ua_begin_write;
  ink_hrtime ua_close;

  ////////////////////////////////////////////////////////
  // server (origin_server , parent, blind tunnnel //
  ////////////////////////////////////////////////////////
  ink_hrtime server_first_connect;
  ink_hrtime server_connect;
  // ink_hrtime  server_connect_end;
  // ink_hrtime  server_begin_write;            //  http only
  ink_hrtime server_first_read; //  http only
  ink_hrtime server_read_header_done;   //  http only
  ink_hrtime server_close;

  /////////////////////////////////////////////
  // cache:                                  //
  // all or some variables may not be set in //
  // certain conditions                      //
  /////////////////////////////////////////////
  ink_hrtime cache_open_read_begin;
  ink_hrtime cache_open_read_end;
  // ink_hrtime  cache_read_begin;
  // ink_hrtime  cache_read_end;
  // ink_hrtime  cache_open_write_begin;
  // ink_hrtime  cache_open_write_end;
  // ink_hrtime  cache_write_begin;
  // ink_hrtime  cache_write_end;

  ink_hrtime dns_lookup_begin;
  ink_hrtime dns_lookup_end;

  ///////////////////
  // state machine //
  ///////////////////
  ink_hrtime sm_start;
  ink_hrtime sm_finish;

    TransactionMilestones();
#ifdef DEBUG
  bool invariant();
#endif
};

typedef enum
{
  NO_HTTP_TRANSACTION_MILESTONES = 0,
  client_accept_time,           // u_a_accept - u_a_begin
  client_header_read_time,      // u_a_read_header_done - u_a_begin_read
  client_response_write_time,   // u_a_close - u_a_begin_write
  //
  cache_lookup_time,            // cache_lookup_end - cache_lookup_begin
  cache_update_time,            // cache_update_end - cache_update_begin
  cache_open_read_time,         // cache_open_read_end - cache_open_read_begin
  cache_read_time,              // cache_read_end - cache_read_begin
  cache_open_write_time,        // cache_open_write_end - cache_open_write_begin
  cache_write_time,             // cache_write_end - cache_write_begin
  //
  server_open_time,             // o_s_open_end - o_s_open_begin
  server_response_time,         // o_s_begin_read - o_s_begin_write
  server_read_time,             // o_s_read_header_done - o_s_begin_read
  //
  TOTAL_HTTP_TRANSACTION_MILESTONES
} HttpTransactionMilestone_t;

extern ink_hrtime GlobalHttpMilestones[TOTAL_HTTP_TRANSACTION_MILESTONES];
extern ink_hrtime TenSecondHttpMilestones[TOTAL_HTTP_TRANSACTION_MILESTONES];


// Modularization Project: Build w/o thread-local-dyn-stats
// temporarily until we switch over to librecords.  Revert to old
// non-thread-local system so that TS will still build and run.

//---------------------------------------------------------------------//
//                       Welcome to enum land!                         //
//---------------------------------------------------------------------//

// Before adding a stat variable, decide whether it is of
// a "transaction" type or if it is of a "dynamic" type.
// Then add the stat variable to the appropriate enumeration
// type. Make sure that DYN_STAT_START is large enough
// (read comment below).

//
// Http Transaction Stats
//
#define _HEADER \
typedef enum { \
    NO_HTTP_TRANS_STATS = 0,

#define _FOOTER \
    MAX_HTTP_TRANS_STATS \
} HttpTransactionStat_t;

#if defined(freebsd)
#undef _D
#endif
#define _D(_x) _x,

#include "HttpTransStats.h"
#undef _HEADER
#undef _FOOTER
#undef _D

struct HttpTransactionStatsString_t
{
  HttpTransactionStat_t i;
  char *name;
};

//
// Note: DYN_STAT_START needs to be at least the next
// power of 2 bigger than the value of MAX_HTTP_TRANS_STATS
//
#define DYN_STAT_START 2048
#define DYN_STAT_MASK (~(2047UL))
//
// Dynamic Stats
//
#define _HEADER \
typedef enum { \
    NO_DYN_STATS = DYN_STAT_START,

#define _FOOTER \
    MAX_DYN_STATS \
} DynamicStat_t;

#define _D(_x) _x,

#include "DynamicStats.h"

#undef _HEADER
#undef _FOOTER
#undef _D

struct DynamicStatsString_t
{
  DynamicStat_t i;
  const char *name;
};

extern HttpTransactionStatsString_t HttpTransactionStatsStrings[];
extern DynamicStatsString_t DynamicStatsStrings[];

//---------------------------------------------------------------------//
//                          Typedefs, etc.                             //
//---------------------------------------------------------------------//

// For now, use mutexes. May later change to spin_locks, try_locks.
#define ink_stat_lock_t ink_mutex

typedef int64_t ink_statval_t;

struct ink_local_stat_t
{
  ink_statval_t count;
  ink_statval_t value;
};

struct ink_prot_global_stat_t
{
  ink_stat_lock_t access_lock;
  ink_statval_t count;
  ink_statval_t sum;

  ink_prot_global_stat_t()
    : count(0), sum(0)
  {
    ink_mutex_init(&access_lock, "Stats Access Lock");
  }
};

struct ink_unprot_global_stat_t
{
  ink_statval_t count;
  ink_statval_t sum;
    ink_unprot_global_stat_t():count(0), sum(0)
  {
  }
};


//---------------------------------------------------------------------//
//                     External interface macros                       //
//---------------------------------------------------------------------//

// Set count and sum to 0.
#define CLEAR_DYN_STAT(X) \
{ \
    ink_assert (X & DYN_STAT_MASK); \
    CLEAR_GLOBAL_DYN_STAT(X-DYN_STAT_START); \
}

#define DECREMENT_DYN_STAT(X) SUM_DYN_STAT(X, (ink_statval_t)-1)

#define COUNT_DYN_STAT(X,C) \
{ \
    ink_assert (X & DYN_STAT_MASK); \
    ADD_TO_GLOBAL_DYN_COUNT((X-DYN_STAT_START), C); \
}

#define FSUM_DYN_STAT(X, S) \
{ \
    ink_assert (X & DYN_STAT_MASK); \
    ADD_TO_GLOBAL_DYN_FSUM((X-DYN_STAT_START), S); \
}

// Increment the count, sum.
#define INCREMENT_DYN_STAT(X) SUM_DYN_STAT(X, (ink_statval_t)1)

// Get the count and sum in a single lock acquire operation.
// Would it make sense to have three functions - a combined
// read of the count and sum, and two more functions - one
// to read just the count and the other to read just the sum?
#define READ_DYN_STAT(X,C,S) \
{ \
    ink_assert (X & DYN_STAT_MASK); \
    READ_GLOBAL_DYN_STAT((X-DYN_STAT_START),C,S); \
}

#define READ_DYN_COUNT(X,C) \
{ \
    ink_assert (X & DYN_STAT_MASK); \
    READ_GLOBAL_DYN_COUNT((X-DYN_STAT_START),C); \
}

#define READ_DYN_SUM(X,S) \
{ \
    ink_assert (X & DYN_STAT_MASK); \
    READ_GLOBAL_DYN_SUM((X-DYN_STAT_START),S); \
}

// set the stat.count to a specific value
#define SET_DYN_COUNT(X, V) \
{ \
    ink_assert (X & DYN_STAT_MASK); \
    SET_GLOBAL_DYN_COUNT((X-DYN_STAT_START), V); \
}

// set the stat.count stat.sum to specific values
#define SET_DYN_STAT(X, C, S) \
{ \
    ink_assert (X & DYN_STAT_MASK); \
    SET_GLOBAL_DYN_STAT((X-DYN_STAT_START), C, S); \
}

// Add a specific value to the sum.
#define SUM_DYN_STAT(X,S) \
{ \
    ink_assert (X & DYN_STAT_MASK); \
    ADD_TO_GLOBAL_DYN_SUM((X-DYN_STAT_START), S); \
}

// Add a specific value to the sum.
#define SUM_GLOBAL_DYN_STAT(X,S) \
{ \
    ink_assert (X & DYN_STAT_MASK); \
    ADD_TO_GLOBAL_GLOBAL_DYN_SUM((X-DYN_STAT_START), S); \
}

#define __CLEAR_TRANS_STAT(local_stat_struct_, X) \
{ \
    ink_assert (!(X & DYN_STAT_MASK)); \
    local_stat_struct_[X].count = (ink_statval_t)0; \
    local_stat_struct_[X].value = (ink_statval_t)0; \
}

#define __DECREMENT_TRANS_STAT(local_stat_struct_, X) __SUM_TRANS_STAT(local_stat_struct_, X, (ink_statval_t)-1)

#define __FSUM_TRANS_STAT(local_stat_struct_, X, S) \
{ \
    ink_assert (!(X & DYN_STAT_MASK)); \
    local_stat_struct_[X].count++; \
    (*(double *)&local_stat_struct_[X].value) += S; \
}

// Increment the count, sum.
#define __INCREMENT_TRANS_STAT(local_stat_struct_, X) __SUM_TRANS_STAT(local_stat_struct_, X, (ink_statval_t)1);

#define __INITIALIZE_LOCAL_STAT_STRUCT(local_stat_struct_, X) __CLEAR_TRANS_STAT(local_stat_struct_, X)

#define	INITIALIZE_GLOBAL_TRANS_STATS(X) \
{ \
    X.count = (ink_statval_t)0; \
    X.sum = (ink_statval_t)0; \
}

// Get the count and sum in a single lock acquire operation.
// Would it make sense to have three functions - a combined
// read of the count and sum, and two more functions - one
// to read just the count and the other to read just the sum?
#define READ_HTTP_TRANS_STAT(X,C,S) \
{ \
    ink_assert (!(X & DYN_STAT_MASK)); \
    READ_GLOBAL_HTTP_TRANS_STAT(X,C,S); \
}

// set the stat.count to a specific value
#define __SET_TRANS_COUNT(local_stat_struct_, X, V) \
{ \
    ink_assert (!(X & DYN_STAT_MASK)); \
    local_stat_struct_[X].value = (ink_statval_t)V; \
}

// set the stat.count and the stat.sum to specific values
#define __SET_TRANS_STAT(local_stat_struct_, X, C, S) \
{ \
    ink_assert (!(X & DYN_STAT_MASK)); \
    local_stat_struct_[X].value = (ink_statval_t)S; \
}

// Add a specific value to local stat.
// Both ADD_TO_SUM_STAT and ADD_TO_COUNT_STAT do the same thing
// to the local copy of the transaction stat.
#define __SUM_TRANS_STAT(local_stat_struct_, X,S) \
{ \
    ink_assert (!(X & DYN_STAT_MASK)); \
    local_stat_struct_[X].count += 1; \
    local_stat_struct_[X].value += S; \
}

#define UPDATE_HTTP_TRANS_STATS(local_stat_struct_) \
{ \
    int i; \
    STAT_LOCK_ACQUIRE(&(global_http_trans_stat_lock)); \
    for (i=NO_HTTP_TRANS_STATS; i<MAX_HTTP_TRANS_STATS; i++) { \
	global_http_trans_stats[i].count += local_stat_struct_[i].count; \
	global_http_trans_stats[i].sum += local_stat_struct_[i].value; \
    } \
    STAT_LOCK_RELEASE(&(global_http_trans_stat_lock)); \
}

#define STAT_LOCK_ACQUIRE(X) (ink_mutex_acquire(X))
#define STAT_LOCK_RELEASE(X) (ink_mutex_release(X))
#define STAT_LOCK_INIT(X,S) (ink_mutex_init(X,S))

//---------------------------------------------------------------------//
// Internal macros to support adding, setting, reading, clearing, etc. //
//---------------------------------------------------------------------//

#ifndef USE_LOCKS_FOR_DYN_STATS

#ifdef USE_THREAD_LOCAL_DYN_STATS
// Modularization Project: See note above
#error "Should not build with USE_THREAD_LOCAL_DYN_STATS"


#define ADD_TO_GLOBAL_DYN_COUNT(X,C) \
mutex->thread_holding->global_dyn_stats[X].count += (C)

#define ADD_TO_GLOBAL_DYN_SUM(X,S) \
mutex->thread_holding->global_dyn_stats[X].count ++; \
mutex->thread_holding->global_dyn_stats[X].sum += (S)

#define ADD_TO_GLOBAL_GLOBAL_DYN_SUM(X,S) \
ink_atomic_increment64(&global_dyn_stats[X].count,(ink_statval_t)1); \
ink_atomic_increment64(&global_dyn_stats[X].sum,S)
/*
 * global_dyn_stats[X].count ++; \
 * global_dyn_stats[X].sum += (S)
 */

#define ADD_TO_GLOBAL_DYN_FSUM(X,S) \
mutex->thread_holding->global_dyn_stats[X].count++; \
mutex->thread_holding->global_dyn_stats[X].sum += (S)

#define CLEAR_GLOBAL_DYN_STAT(X) \
global_dyn_stats[X].count = 0; \
global_dyn_stats[X].sum = 0

#ifdef _WIN32

#define READ_GLOBAL_DYN_STAT(X,C,S) do { \
  ink_unprot_global_stat_t _s = global_dyn_stats[X]; \
  for (int _e = 0; _e < eventProcessor.get_thread_count() ; _e++) { \
    _s.count += eventProcessor._all_ethreads[_e]->global_dyn_stats[X].count; \
    _s.sum += eventProcessor._all_ethreads[_e]->global_dyn_stats[X].sum; \
  } \
  C = _s.count; \
  S = _s.sum; \
} while (0)

#define READ_GLOBAL_DYN_COUNT(X,C) do { \
  ink_statval_t _s = global_dyn_stats[X].count; \
  for (int _e = 0; _e < eventProcessor.get_thread_count() ; _e++) \
    _s += eventProcessor._all_ethreads[_e]->global_dyn_stats[X].count; \
  C = _s; \
} while (0)

#define READ_GLOBAL_DYN_SUM(X,S) do { \
  ink_statval_t _s = global_dyn_stats[X].sum; \
  for (int _e = 0; _e < eventProcessor.get_thread_count() ; _e++) \
    _s += eventProcessor._all_ethreads[_e]->global_dyn_stats[X].sum; \
  S = _s; \
} while (0)

#else

#define READ_GLOBAL_DYN_STAT(X,C,S) do { \
  ink_unprot_global_stat_t _s = global_dyn_stats[X]; \
  for (int _e = 0; _e < eventProcessor.n_ethreads ; _e++) { \
    _s.count += eventProcessor.all_ethreads[_e]->global_dyn_stats[X].count; \
    _s.sum += eventProcessor.all_ethreads[_e]->global_dyn_stats[X].sum; \
  } \
  C = _s.count; \
  S = _s.sum; \
} while (0)

#define READ_GLOBAL_DYN_COUNT(X,C) do { \
  ink_statval_t _s = global_dyn_stats[X].count; \
  for (int _e = 0; _e < eventProcessor.n_ethreads ; _e++) \
    _s += eventProcessor.all_ethreads[_e]->global_dyn_stats[X].count; \
  C = _s; \
} while (0)

#define READ_GLOBAL_DYN_SUM(X,S) do { \
  ink_statval_t _s = global_dyn_stats[X].sum; \
  for (int _e = 0; _e < eventProcessor.n_ethreads ; _e++) \
    _s += eventProcessor.all_ethreads[_e]->global_dyn_stats[X].sum; \
  S = _s; \
} while (0)

#endif

#define READ_GLOBAL_HTTP_TRANS_STAT(X,C,S) \
{ \
    C = global_http_trans_stats[X].count; \
    S = global_http_trans_stats[X].sum; \
}

#define SET_GLOBAL_DYN_COUNT(X,V) \
global_dyn_stats[X].count = V

#define SET_GLOBAL_DYN_STAT(X,C,S) \
global_dyn_stats[X].count = C; \
global_dyn_stats[X].sum = S

#define	INITIALIZE_GLOBAL_DYN_STATS(X, T) \
{ \
    X.count = (ink_statval_t)0; \
    X.sum = (ink_statval_t)0; \
}

#else

#define ADD_TO_GLOBAL_DYN_COUNT(X,C) \
ink_atomic_increment64(&global_dyn_stats[X].count,C)

#define ADD_TO_GLOBAL_DYN_SUM(X,S) \
ink_atomic_increment64(&global_dyn_stats[X].count,(ink_statval_t)1); \
ink_atomic_increment64(&global_dyn_stats[X].sum,S)

#define ADD_TO_GLOBAL_GLOBAL_DYN_SUM(X,S) \
ink_atomic_increment64(&global_dyn_stats[X].count,(ink_statval_t)1); \
ink_atomic_increment64(&global_dyn_stats[X].sum,S)

#define ADD_TO_GLOBAL_DYN_FSUM(X,S) \
ink_atomic_increment64(&global_dyn_stats[X].count,(ink_statval_t)1); \
(*(double *)&global_dyn_stats[X].sum) += S

#define CLEAR_GLOBAL_DYN_STAT(X) \
global_dyn_stats[X].count = 0; \
global_dyn_stats[X].sum = 0

#define READ_GLOBAL_DYN_STAT(X,C,S) \
C = global_dyn_stats[X].count; \
S = global_dyn_stats[X].sum

#define READ_GLOBAL_DYN_COUNT(X,C) \
C = global_dyn_stats[X].count;

#define READ_GLOBAL_DYN_SUM(X,S) \
S = global_dyn_stats[X].sum;

#define READ_GLOBAL_HTTP_TRANS_STAT(X,C,S) \
{ \
    C = global_http_trans_stats[X].count; \
    S = global_http_trans_stats[X].sum; \
}

#define SET_GLOBAL_DYN_COUNT(X,V) \
global_dyn_stats[X].count = V

#define SET_GLOBAL_DYN_STAT(X,C,S) \
global_dyn_stats[X].count = C; \
global_dyn_stats[X].sum = S

#define	INITIALIZE_GLOBAL_DYN_STATS(X, T) \
{ \
    X.count = (ink_statval_t)0; \
    X.sum = (ink_statval_t)0; \
}

#endif /* USE_THREAD_LOCAL_DYN_STATS */

#else /* USE_LOCKS_FOR_DYN_STATS */

#define ADD_TO_GLOBAL_DYN_COUNT(X,C) \
{ \
    STAT_LOCK_ACQUIRE(&(global_dyn_stats[X].access_lock)); \
    global_dyn_stats[X].count += C; \
    STAT_LOCK_RELEASE(&(global_dyn_stats[X].access_lock)); \
}
#define ADD_TO_GLOBAL_DYN_SUM(X,S) \
{ \
    STAT_LOCK_ACQUIRE(&(global_dyn_stats[X].access_lock)); \
    global_dyn_stats[X].count += 1; \
    global_dyn_stats[X].sum += S; \
    STAT_LOCK_RELEASE(&(global_dyn_stats[X].access_lock)); \
}
#define ADD_TO_GLOBAL_GLOBAL_DYN_SUM(X,S) \
{ \
    STAT_LOCK_ACQUIRE(&(global_dyn_stats[X].access_lock)); \
    global_dyn_stats[X].count += 1; \
    global_dyn_stats[X].sum += S; \
    STAT_LOCK_RELEASE(&(global_dyn_stats[X].access_lock)); \
}
#define ADD_TO_GLOBAL_DYN_FSUM(X,S) \
{ \
    STAT_LOCK_ACQUIRE(&(global_dyn_stats[X].access_lock)); \
    global_dyn_stats[X].count += (ink_statval_t)1; \
    (*(double *)&global_dyn_stats[X].sum) += S; \
    STAT_LOCK_RELEASE(&(global_dyn_stats[X].access_lock)); \
}
#define CLEAR_GLOBAL_DYN_STAT(X) \
{ \
    STAT_LOCK_ACQUIRE(&(global_dyn_stats[X].access_lock)); \
    global_dyn_stats[X].count = (ink_statval_t)0; \
    global_dyn_stats[X].sum = (ink_statval_t)0; \
    STAT_LOCK_RELEASE(&(global_dyn_stats[X].access_lock)); \
}
#define READ_GLOBAL_DYN_STAT(X,C,S) \
{ \
    STAT_LOCK_ACQUIRE(&(global_dyn_stats[X].access_lock)); \
    C = global_dyn_stats[X].count; \
    S = global_dyn_stats[X].sum; \
    STAT_LOCK_RELEASE(&(global_dyn_stats[X].access_lock)); \
}
#define READ_GLOBAL_HTTP_TRANS_STAT(X,C,S) \
{ \
    C = global_http_trans_stats[X].count; \
    S = global_http_trans_stats[X].sum; \
}
#define SET_GLOBAL_DYN_COUNT(X,V) \
{ \
    STAT_LOCK_ACQUIRE(&(global_dyn_stats[X].access_lock)); \
    global_dyn_stats[X].count = V; \
    STAT_LOCK_RELEASE(&(global_dyn_stats[X].access_lock)); \
}

#define SET_GLOBAL_DYN_STAT(X,C,S) \
{ \
    STAT_LOCK_ACQUIRE(&(global_dyn_stats[X].access_lock)); \
    global_dyn_stats[X].count = C; \
    global_dyn_stats[X].sum = S; \
    STAT_LOCK_RELEASE(&(global_dyn_stats[X].access_lock)); \
}

#define	INITIALIZE_GLOBAL_DYN_STATS(X, T) \
{ \
    STAT_LOCK_INIT(&(X.access_lock), T); \
    X.count = (ink_statval_t)0; \
    X.sum = (ink_statval_t)0; \
}

#endif /* USE_LOCKS_FOR_DYN_STATS */

//---------------------------------------------------------------------//
//                        Function prototypes                          //
//---------------------------------------------------------------------//
extern void start_stats_snap(void);
void initialize_all_global_stats();

// TODO: I don't think these are necessary any more, but double check.
//void *tmp_stats_lock_function(UpdateLockAction action);
//void *stats_lock_function(void *data, UpdateLockAction action);

void *http_trans_stats_count_cb(void *data, void *res);
void *http_trans_stats_sum_cb(void *data, void *res);
void *http_trans_stats_avg_cb(void *data, void *res);
void *http_trans_stats_fsum_cb(void *data, void *res);
void *http_trans_stats_favg_cb(void *data, void *res);
void *http_trans_stats_time_seconds_cb(void *data, void *res);
void *http_trans_stats_time_mseconds_cb(void *data, void *res);
void *http_trans_stats_time_useconds_cb(void *data, void *res);

void *dyn_stats_count_cb(void *data, void *res);
inkcoreapi void *dyn_stats_sum_cb(void *data, void *res);
void *dyn_stats_avg_cb(void *data, void *res);
void *dyn_stats_fsum_cb(void *data, void *res);
void *dyn_stats_favg_cb(void *data, void *res);
void *dyn_stats_time_seconds_cb(void *data, void *res);
void *dyn_stats_time_mseconds_cb(void *data, void *res);
void *dyn_stats_time_useconds_cb(void *data, void *res);
void *dyn_stats_int_msecs_to_float_seconds_cb(void *data, void *res);
//---------------------------------------------------------------------//
//                 Global variables declaration.                       //
//---------------------------------------------------------------------//
extern ink_stat_lock_t global_http_trans_stat_lock;
extern ink_unprot_global_stat_t global_http_trans_stats[MAX_HTTP_TRANS_STATS];
#ifndef USE_LOCKS_FOR_DYN_STATS
extern inkcoreapi ink_unprot_global_stat_t global_dyn_stats[MAX_DYN_STATS - DYN_STAT_START];
#else
extern inkcoreapi ink_prot_global_stat_t global_dyn_stats[MAX_DYN_STATS - DYN_STAT_START];
#endif

#ifdef DEBUG
extern ink_mutex http_time_lock;
extern time_t last_http_local_time;
#endif

#define MAX_HTTP_HANDLER_EVENTS 25
extern void clear_http_handler_times();
extern void print_http_handler_time(int event);
extern void print_all_http_handler_times();
#ifdef DEBUG
extern ink_hrtime http_handler_times[MAX_HTTP_HANDLER_EVENTS];
extern int http_handler_counts[MAX_HTTP_HANDLER_EVENTS];
#endif


#endif /* _StatSystem_h_ */
