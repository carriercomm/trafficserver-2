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

/* orderstartplugin.c: one of the set of 5 plugins that help determine if
 *                 the order in which the plugins are invoked at any
 *                 hook is the same as the order in which they appear
 *                 in plugin.config file. The plugin logs an error
 *                 message in logs/error.log file if the sequence in
 *                 which the plugin is invoked is incorrect.
 *
 *
 *   Usage:
 *   (NT):orderstartplugin.dll
 *   (Solaris):orderstartplugin.so
 *
 *
 */

#include<stdio.h>
#include<string.h>
#include<assert.h>
#include "ts.h"

#define FIELD_NAME "RANK"
int value;

static int
startplugin(TSCont contp, TSEvent event, void *edata)
{

  TSMBuffer bufp;
  TSMLoc hdr_loc;
  TSMLoc field_loc;
  TSHttpTxn txnp = (TSHttpTxn) edata;

  if (!TSHttpTxnClientReqGet(txnp, &bufp, &hdr_loc)) {
    printf("Couldn't retrieve Client Request Header\n");
    TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
    return 0;
  }

  if ((field_loc = TSMimeHdrFieldFind(bufp, hdr_loc, FIELD_NAME, -1)) != 0) {
    int count;

    TSMimeHdrFieldValueIntGet(bufp, hdr_loc, field_loc, 0, &count);
    TSMimehdrFieldValueIntSet(bufp, hdr_loc, field_loc, 0, value);
  } else {
    field_loc = TSMimeHdrFieldCreate(bufp, hdr_loc);
    TSMimeHdrFieldNameSet(bufp, hdr_loc, field_loc, FIELD_NAME, -1);
    TSMimeHdrFieldValueIntInsert(bufp, hdr_loc, field_loc, -1, value);
    TSMimeHdrFieldAppend(bufp, hdr_loc, field_loc);
  }

  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
  return 0;
}

void
TSPluginInit(int argc, const char *argv[])
{


  TSMutex lock1 = TSMutexCreate();

  TSCont contp = TSContCreate(startplugin, lock1);

  value = 0;

  TSHttpHookAdd(TS_HTTP_READ_REQUEST_HDR_HOOK, contp);
  TSHttpHookAdd(TS_HTTP_OS_DNS_HOOK, contp);
  TSHttpHookAdd(TS_HTTP_SEND_REQUEST_HDR_HOOK, contp);
  TSHttpHookAdd(TS_HTTP_READ_CACHE_HDR_HOOK, contp);
  TSHttpHookAdd(TS_HTTP_READ_RESPONSE_HDR_HOOK, contp);
  TSHttpHookAdd(TS_HTTP_SEND_RESPONSE_HDR_HOOK, contp);
}
