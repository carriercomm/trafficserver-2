/** @file

  Climate plugin

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

  @section details Details

  Log information regarding transactions (headers, bytes,
  timing) in a specific log file (climate.log).

  Used by the Climate Lab log analysis that needs precise
  timing information not available in custom logs.

  Basic design:

  1. TS_HTTP_TXN_START_HOOK is added to the main plug-in
  continuation as a global hook in TSPluginInit().

  2. global_http_handler() is called for each transaction.

  3. new_transaction() creates a new continuation for
  each transaction, register transaction hooks and
  allocates data.

  4. TS_HTTP_(READ|SEND)_(REQUEST|RESPONSE)_HDR_HOOKs log
  timing information.

  5. TS_HTTP_TXN_CLOSE retrieves and logs transaction
  information,  client request/server response headers
  and timing information.

*/

#include <stdio.h>
#include <limits.h>
#include <sys/time.h>

#include "ts.h"
#include "experimental.h"

#include "events.h"

#define HIGH "climateh"
#define MED  "climatehm"
#define LOW  "climatehml"

typedef enum
{
  SUCCESS = 0,
  FAILURE = -1
} ReturnCode;

#define IP_a(_x) ((ntohl(_x) & 0xFF000000) >> 24)
#define IP_b(_x) ((ntohl(_x) & 0x00FF0000) >> 16)
#define IP_c(_x) ((ntohl(_x) & 0x0000FF00) >> 8)
#define IP_d(_x)  (ntohl(_x) & 0x000000FF)


typedef struct
{
  unsigned int txn_id;

  TSAction pending_action;

  unsigned int client_ip;
  char *method;
  int client_version;
  char *full_url;

  int cache_lookup_status;
  int client_abort;

  unsigned int server_ip;
  int server_version;
  int resp_status_code;

  char *content_type;

  int client_req_hdr_bytes;
  int client_req_body_bytes;
  int client_resp_hdr_bytes;
  int client_resp_body_bytes;
  int server_req_hdr_bytes;
  int server_req_body_bytes;
  int server_resp_hdr_bytes;
  int server_resp_body_bytes;

  char *server_response_header;
  char *client_request_header;

  double txn_start_time;
  double read_request_hdr_time;
  double send_request_hdr_time;
  double read_response_hdr_time;
  double send_response_hdr_time;
  double txn_close_time;

  double txn_time_start;
  double txn_time_end;

} TransactionData;



/* Event handler management */
typedef int (*TransactionStateHandler) (TSCont contp, TSEvent event, void *edata, TransactionData * data);



/* Unique txn ID management */
static unsigned int id = 0;
static TSMutex id_mutex;
#define INIT_TXN_ID {id=0; id_mutex=TSMutexCreate(); }
#define INC_AND_GET_TXN_ID(_x) {TSMutexLock(id_mutex); _x = id++; TSMutexUnlock(id_mutex); }


/* Log stuff */

/* The plugin can roll manually its log if this feature is not enabled in TS */
#ifdef LOG_ROLL
#define DEFAULT_LOG_NBMAX_ENTRIES 1000000       /* default number max of entries in one transaction log */
static TSMutex log_mutex;
static int log_nb_rollover;
static int log_nb_entries;
static int log_nbmax_entries;
#endif /* LOG_ROLL */

static TSTextLogObject log;


/*------------------------------------------------------
FUNCTIONS
--------------------------------------------------------*/

TransactionData *transaction_data_alloc();
void transaction_data_destroy(TransactionData * data);
int delete_transaction(TSCont contp, TransactionData * data);
void TSPluginInit(int argc, const char *argv[]);
void create_new_log();
int global_http_handler(TSCont contp, TSEvent event, void *edata);
void new_transaction(TSHttpTxn txnp);
int transaction_handler(TSCont contp, TSEvent event, void *edata);
int read_request_hdr_handler(TSCont contp, TSEvent event, void *edata, TransactionData * data);
int send_request_hdr_handler(TSCont contp, TSEvent event, void *edata, TransactionData * data);
int read_response_hdr_handler(TSCont contp, TSEvent event, void *edata, TransactionData * data);
int send_response_hdr_handler(TSCont contp, TSEvent event, void *edata, TransactionData * data);
int txn_close_handler(TSCont contp, TSEvent event, void *edata, TransactionData * data);
int retrieve_transaction_info(TSHttpTxn txnp, TransactionData * data);
int log_transaction_info(TSCont contp, TransactionData * data);
void print_mime_headers(TSMBuffer bufp, TSMLoc hdr_loc, char **output_string);





static TransactionData *
transaction_data_alloc()
{
  TransactionData *data;

  data = (TransactionData *) TSmalloc(sizeof(TransactionData));

  data->txn_id = -1;

  data->pending_action = NULL;

  data->client_ip = 0;
  data->client_version = -1;
  data->method = NULL;
  data->full_url = NULL;

  data->cache_lookup_status = -1;
  data->client_abort = -1;

  data->server_ip = 0;
  data->server_version = -1;
  data->resp_status_code = -1;
  data->content_type = NULL;

  data->client_req_hdr_bytes = -1;
  data->client_req_body_bytes = -1;
  data->client_resp_hdr_bytes = -1;
  data->client_resp_body_bytes = -1;
  data->server_req_hdr_bytes = -1;
  data->server_req_body_bytes = -1;
  data->server_resp_hdr_bytes = -1;
  data->server_resp_body_bytes = -1;

  data->txn_start_time = -1;
  data->read_request_hdr_time = -1;
  data->send_request_hdr_time = -1;
  data->read_response_hdr_time = -1;
  data->send_response_hdr_time = -1;
  data->txn_close_time = -1;

  data->txn_time_start = 0;
  data->txn_time_end = 0;

  data->client_request_header = NULL;
  data->server_response_header = NULL;

  return data;
}


static void
transaction_data_destroy(TransactionData * data)
{
  if (data) {

    if ((data->pending_action) && (!TSActionDone(data->pending_action))) {
      TSActionCancel(data->pending_action);
      data->pending_action = NULL;
    }

    if (data->method) {
      TSfree(data->method);
      data->method = NULL;
    }

    if (data->full_url) {
      TSfree(data->full_url);
      data->full_url = NULL;
    }

    if (data->content_type) {
      TSfree(data->content_type);
      data->content_type = NULL;
    }

    if (data->client_request_header) {
      TSfree(data->client_request_header);
    }

    if (data->server_response_header) {
      TSfree(data->server_response_header);
    }

    TSfree(data);
  }
}


static int
delete_transaction(TSCont contp, TransactionData * data)
{
  TSDebug(HIGH, "[%u] Transaction shutdown", data->txn_id);

  transaction_data_destroy(data);
  TSContDestroy(contp);

  return SUCCESS;
}


/*
 * One argument: the max number of lines in a log file before we roll over
 *
 */
void
TSPluginInit(int argc, const char *argv[])
{

  INIT_TXN_ID;                  /* init stuff related to unique txn id */

#ifdef LOG_ROLL
  log_nbmax_entries = DEFAULT_LOG_NBMAX_ENTRIES;
  if (argc == 2) {
    int ival = atoi(argv[1]);
    if (ival > 0) {
      log_nbmax_entries = ival;
    }
  }
  TSDebug(HIGH, "Nb max entries in log set to %d", log_nbmax_entries);

  /* create mutex, used when we roll over logs */
  log_mutex = TSMutexCreate();
  log_nb_rollover = 0;
  log_nb_entries = 0;
#endif /* LOG_ROLL */

  log = NULL;
  create_new_log();

  TSHttpHookAdd(TS_HTTP_TXN_START_HOOK, TSContCreate(global_http_handler, NULL));
}


/*
 Create a new log file.
 Caution: call this at init or when log_mutex is grabbed !
 */
void
create_new_log()
{
  int error = 0;
  char logname[256];
  struct timeval t;

#ifdef LOG_ROLL
  TSDebug(HIGH, "Rolling over transaction logs");

  /* If there is an already existing log object, close it. */
  if (log) {
    TSTextLogObjectDestroy(log);
  }

  /* Then create a new one with a different name */
  gettimeofday(&t, NULL);
  sprintf(logname, "transaction%d_%ld.log", log_nb_rollover, t.tv_sec);

  /* update log stats */
  log_nb_entries = 0;
  log_nb_rollover++;
#else
  sprintf(logname, "climate.log");
#endif /* LOG_ROLL */

  error = TSTextLogObjectCreate(logname, TS_LOG_MODE_ADD_TIMESTAMP, &log);
}


static int
global_http_handler(TSCont contp, TSEvent event, void *edata)
{
  TSHttpTxn txnp = (TSHttpTxn) edata;

  switch (event) {

  case TS_EVENT_HTTP_TXN_START:
    TSDebug(LOW, "Event TS_EVENT_HTTP_TXN_START");
    new_transaction(txnp);
    break;

  default:
    TSAssert(!"Unexpected Event");
    break;
  }

  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
  return 0;
}


static void
new_transaction(TSHttpTxn txnp)
{
  TSCont p_contp;
  TransactionData *p_data;

  /* Create transaction structure */
  p_contp = TSContCreate(transaction_handler, TSMutexCreate());
  p_data = transaction_data_alloc();
  TSContDataSet(p_contp, p_data);

  INC_AND_GET_TXN_ID(p_data->txn_id);

  /* Register transaction to HTTP hooks */
  TSHttpTxnHookAdd(txnp, TS_HTTP_READ_REQUEST_HDR_HOOK, p_contp);
  TSHttpTxnHookAdd(txnp, TS_HTTP_SEND_REQUEST_HDR_HOOK, p_contp);
  TSHttpTxnHookAdd(txnp, TS_HTTP_READ_RESPONSE_HDR_HOOK, p_contp);
  TSHttpTxnHookAdd(txnp, TS_HTTP_SEND_RESPONSE_HDR_HOOK, p_contp);
  TSHttpTxnHookAdd(txnp, TS_HTTP_TXN_CLOSE_HOOK, p_contp);

  p_data->txn_start_time = TSBasedTimeGetD();

  TSDebug(HIGH, "[%u] Added transaction !", p_data->txn_id);
}


/*-------------------------------------------------------------------------
  transaction_handler

  Receives all events for the transaction
  Returns SUCCESS/FAILURE
  -------------------------------------------------------------------------*/
int
transaction_handler(TSCont contp, TSEvent event, void *edata)
{
  TransactionData *data;
  TransactionStateHandler handler;

  data = (TransactionData *) TSContDataGet(contp);

  switch (event) {
  case TS_EVENT_HTTP_READ_REQUEST_HDR:
    handler = (TransactionStateHandler) & read_request_hdr_handler;
    break;
  case TS_EVENT_HTTP_SEND_REQUEST_HDR:
    handler = (TransactionStateHandler) & send_request_hdr_handler;
    break;
  case TS_EVENT_HTTP_READ_RESPONSE_HDR:
    handler = (TransactionStateHandler) & read_response_hdr_handler;
    break;
  case TS_EVENT_HTTP_SEND_RESPONSE_HDR:
    handler = (TransactionStateHandler) & send_response_hdr_handler;
    break;
  case TS_EVENT_HTTP_TXN_CLOSE:
    handler = (TransactionStateHandler) & txn_close_handler;
    break;
  }

  return (*handler) (contp, event, edata, data);
}


/*-------------------------------------------------------------------------
  read_request_hdr_handler

  Return SUCCESS/FAILURE
  -------------------------------------------------------------------------*/
static int
read_request_hdr_handler(TSCont contp, TSEvent event, void *edata, TransactionData * data)
{
  TSHttpTxn txnp = (TSHttpTxn) edata;
  data->read_request_hdr_time = TSBasedTimeGetD();
  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
  return SUCCESS;
}

/*-------------------------------------------------------------------------
  send_request_hdr_handler

  Return SUCCESS/FAILURE
  -------------------------------------------------------------------------*/
static int
send_request_hdr_handler(TSCont contp, TSEvent event, void *edata, TransactionData * data)
{
  TSHttpTxn txnp = (TSHttpTxn) edata;
  data->send_request_hdr_time = TSBasedTimeGetD();
  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
  return SUCCESS;
}


/*-------------------------------------------------------------------------
  read_response_hdr_handler

  Return SUCCESS/FAILURE
  -------------------------------------------------------------------------*/
static int
read_response_hdr_handler(TSCont contp, TSEvent event, void *edata, TransactionData * data)
{
  TSHttpTxn txnp = (TSHttpTxn) edata;
  data->read_response_hdr_time = TSBasedTimeGetD();
  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
  return SUCCESS;
}


/*-------------------------------------------------------------------------
  send_response_hdr_handler

  Return SUCCESS/FAILURE
  -------------------------------------------------------------------------*/
static int
send_response_hdr_handler(TSCont contp, TSEvent event, void *edata, TransactionData * data)
{
  TSHttpTxn txnp = (TSHttpTxn) edata;
  data->send_response_hdr_time = TSBasedTimeGetD();
  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
  return SUCCESS;
}


/*-------------------------------------------------------------------------
  txn_close_handler

  Return SUCCESS/FAILURE
  -------------------------------------------------------------------------*/
static int
txn_close_handler(TSCont contp, TSEvent event, void *edata, TransactionData * data)
{
  TSHttpTxn txnp = (TSHttpTxn) edata;

  data->txn_close_time = TSBasedTimeGetD();

  /* Retrieve transaction information and headers */

  retrieve_transaction_info(txnp, data);

  /* Log everything */

  log_transaction_info(contp, data);

  delete_transaction(contp, data);

  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);

  return SUCCESS;
}


/*-------------------------------------------------------------------------
  retrieve_transaction_info

  Retrieve all request and response info that will be used for logging
  Returns SUCCESS/FAILURE
  -------------------------------------------------------------------------*/
static int
retrieve_transaction_info(TSHttpTxn txnp, TransactionData * data)
{
  char *bp_url;
  TSMBuffer buf_resp, buf_req;
  TSMLoc hdr_resp_loc, hdr_req_loc, str_loc;
  const char *path;
  const char *host;
  int l_host = 0;
  int l_path = 0;
  TSMLoc cookie_loc;
  const char *cookie_value = NULL;
  TSMLoc ctype_loc;
  const char *ctype_value = NULL;
  int str_len, cookie_len, ctype_len;
  int req_by_TS, resp_by_TS, req_by_FP, resp_by_FP;
  char *str;
  int unused;

  /* Retrieve information about the transaction */

  TSHttpTxnStartTimeGetD(txnp, &(data->txn_time_start));
  TSHttpTxnEndTimeGetD(txnp, &(data->txn_time_end));

  /* Retrieve headers */

  if (!TSHttpTxnClientReqGet(txnp, &buf_req, &hdr_req_loc)) {
    TSError("Could not access to client's request http header");
  } else {
    print_mime_headers(buf_req, hdr_req_loc, &(data->client_request_header));

    /* Compute HTTP method */
    str = (char *) TSHttpHdrMethodGet(buf_req, hdr_req_loc, &str_len);
    data->method = TSmalloc(str_len + 1);
    memcpy(data->method, str, str_len);
    data->method[str_len] = '\0';

    /* Get client HTTP version */
    data->client_version = TSHttpHdrVersionGet(buf_req, hdr_req_loc);

    /* Compute Full URL */
    str_loc = TSHttpHdrUrlGet(buf_req, hdr_req_loc);
    str = TSUrlStringGet(buf_req, str_loc, &str_len);
    data->full_url = TSmalloc(str_len + 1);
    memcpy(data->full_url, str, str_len);
    data->full_url[str_len] = '\0';
    TSfree(str);

    TSHandleMLocRelease(buf_req, TS_NULL_MLOC, hdr_req_loc);
  }

  if (!TSHttpTxnServerRespGet(txnp, &buf_resp, &hdr_resp_loc)) {
    TSError("Could not access to server response header");
  } else {
    print_mime_headers(buf_resp, hdr_resp_loc, &(data->server_response_header));

    /* Get response status code */
    data->resp_status_code = TSHttpHdrStatusGet(buf_resp, hdr_resp_loc);

    /* Get server HTTP version */
    data->server_version = TSHttpHdrVersionGet(buf_resp, hdr_resp_loc);

    TSHandleMLocRelease(buf_resp, TS_NULL_MLOC, hdr_resp_loc);
  }

  /* Compute client ip */
  data->client_ip = TSHttpTxnClientIPGet(txnp);

  /* Compute server ip */
  data->server_ip = TSHttpTxnServerIPGet(txnp);

  /* Get cache lookup status */
  TSHttpTxnCacheLookupStatusGet(txnp, &(data->cache_lookup_status));

  /* Get client abort */
  data->client_abort = TSHttpTxnClientAborted(txnp);

  /* Get number of header and body bytes for all transfers */

  TSHttpTxnClientReqHdrBytesGet(txnp, &(data->client_req_hdr_bytes));
  TSHttpTxnClientReqBodyBytesGet(txnp, &(data->client_req_body_bytes));
  TSHttpTxnClientRespHdrBytesGet(txnp, &(data->client_resp_hdr_bytes));
  TSHttpTxnClientRespBodyBytesGet(txnp, &(data->client_resp_body_bytes));
  TSHttpTxnServerReqHdrBytesGet(txnp, &(data->server_req_hdr_bytes));
  TSHttpTxnServerReqBodyBytesGet(txnp, &(data->server_req_body_bytes));
  TSHttpTxnServerRespHdrBytesGet(txnp, &(data->server_resp_hdr_bytes));
  TSHttpTxnServerRespBodyBytesGet(txnp, &(data->server_resp_body_bytes));

}


/*-------------------------------------------------------------------------
  log_transaction_info

  Log all request and response info
  Returns SUCCESS/FAILURE
  -------------------------------------------------------------------------*/
static int
log_transaction_info(TSCont contp, TransactionData * data)
{
  double txn_time = 0;

  TSDebug(MED, "[%u] Logging stats", data->txn_id);

  if (!log) {
    return FAILURE;
  }

  /* Compute some stats */
  txn_time = data->txn_time_end - data->txn_time_start;
  txn_time = txn_time / 1000000.0;      /* convert nanosec to msec */

#ifdef LOG_ROLL
  TSMutexLock(log_mutex);

  log_nb_entries++;
  if (log_nb_entries > log_nbmax_entries) {
    create_new_log();
  }
#endif /* LOG_ROLL */

  TSTextLogObjectWrite(log,
                        "|%d.%d.%d.%d|%s|%d|%s|%d|%d|%d.%d.%d.%d|%d|%d|%s|%d|%d|%d|%d|%d|%d|%d|%d|%ld|%0.f|%0.f|%0.f|%0.f|%0.f|%0.f|%s|%s",
/*    client                     server               bytes                       time                    */
                        IP_a(data->client_ip), IP_b(data->client_ip),
                        IP_c(data->client_ip), IP_d(data->client_ip),
                        data->method,
                        data->client_version,
                        data->full_url,
                        data->cache_lookup_status,
                        data->client_abort,
                        IP_a(data->server_ip), IP_b(data->server_ip),
                        IP_c(data->server_ip), IP_d(data->server_ip),
                        data->server_version,
                        data->resp_status_code,
                        data->content_type,
                        data->client_req_hdr_bytes,
                        data->client_req_body_bytes,
                        data->client_resp_hdr_bytes,
                        data->client_resp_body_bytes,
                        data->server_req_hdr_bytes,
                        data->server_req_body_bytes,
                        data->server_resp_hdr_bytes, data->server_resp_body_bytes, (long) txn_time,
/*  Microseconds are enough. Log actual times instead of differences in case different differences
    become of interest */
                        data->txn_start_time / 1000,
                        data->read_request_hdr_time / 1000,
                        data->send_request_hdr_time / 1000,
                        data->read_response_hdr_time / 1000,
                        data->send_response_hdr_time / 1000,
                        data->txn_close_time / 1000, data->client_request_header, data->server_response_header);



#ifdef LOG_ROLL
  TSMutexUnlock(log_mutex);
#endif /* LOG_ROLL */

  return SUCCESS;
}


/*-------------------------------------------------------------------------
  print_mime_headers

  Outputs the full header to a string
  -------------------------------------------------------------------------*/
static void
print_mime_headers(TSMBuffer bufp, TSMLoc hdr_loc, char **output_string)
{

  TSIOBuffer output_buffer;
  TSIOBufferReader reader;
  int total_avail;

  TSIOBufferBlock block;
  const char *block_start;
  int block_avail;

  int output_len;
  char *line_feed;

  output_buffer = TSIOBufferCreate();

  if (!output_buffer) {
    TSError("couldn't allocate IOBuffer\n");
  }

  reader = TSIOBufferReaderAlloc(output_buffer);

  /* This will print  just MIMEFields and not
     the http request line */
  TSMimeHdrPrint(bufp, hdr_loc, output_buffer);

  TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);

  /* Find out how the big the complete header is by
     seeing the total bytes in the buffer.  We need to
     look at the buffer rather than the first block to
     see the size of the entire header */
  total_avail = TSIOBufferReaderAvail(reader);

  /* Allocate the string with an extra byte for the string
     terminator */
  *output_string = (char *) TSmalloc(total_avail + 1);
  output_len = 0;

  /* We need to loop over all the buffer blocks to make
     sure we get the complete header since the header can
     be in multiple blocks */
  block = TSIOBufferReaderStart(reader);
  while (block) {

    block_start = TSIOBufferBlockReadStart(block, reader, &block_avail);

    /* We'll get a block pointer back even if there is no data
       left to read so check for this condition and break out of
       the loop. A block with no data to read means we've exhausted
       buffer of data since if there was more data on a later
       block in the chain, this block would have been skipped over */
    if (block_avail == 0) {
      break;
    }

    memcpy(*output_string + output_len, block_start, block_avail);
    output_len += block_avail;

    /* Consume the data so that we get to the next block */
    TSIOBufferReaderConsume(reader, block_avail);

    /* Get the next block now that we've consumed the
       data off the last block */
    block = TSIOBufferReaderStart(reader);
  }

  /* Terminate the string */
  (*output_string)[output_len] = '\0';
  output_len++;

  /* Replace line feeds (/r/n) by a separator ('`) */

  line_feed = (char *) strstr(*output_string, "\r\n");

  while (line_feed != NULL) {
    line_feed[0] = '\'';
    line_feed[1] = '`';
    line_feed = (char *) strstr(line_feed + 2, "\r\n");
  }

  /* Some servers use just /n (http://slashdot.dk) */

  line_feed = (char *) strchr(*output_string, '\n');

  while (line_feed != NULL) {
    line_feed[0] = '`';
    line_feed = (char *) strchr(line_feed + 1, '\n');
  }


  /* Free up the TSIOBuffer that we used to print out the header */
  TSIOBufferReaderFree(reader);
  TSIOBufferDestroy(output_buffer);
}
