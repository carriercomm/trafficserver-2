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

/*****************************************************************************
 *
 *  MatcherUtils.h - Various helper routines used in ControlMatcher
 *                    and ReverseProxy
 *
 *
 ****************************************************************************/

#ifndef _MATCHER_UTILS_H_
#define _MATCHER_UTILS_H_

#include "ParseRules.h"
// Look in MatcherUtils.cc for comments on function usage
char *readIntoBuffer(char *file_path, const char *module_name, int *read_size_ptr);

int unescapifyStr(char *buffer);

typedef unsigned long ip_addr_t;
const char *ExtractIpRange(char *match_str, ip_addr_t * addr1, ip_addr_t * addr2);
char *tokLine(char *buf, char **last);

const char *processDurationString(char *str, int *seconds);

// The first class types we support matching on
enum matcher_type
{ MATCH_NONE, MATCH_HOST, MATCH_DOMAIN,
  MATCH_IP, MATCH_REGEX, MATCH_HOST_REGEX
};
extern const char *matcher_type_str[];

// A parsed config file line
const int MATCHER_MAX_TOKENS = 40;
struct matcher_line
{
  matcher_type type;            // dest type
  int dest_entry;               // entry which specifies the destination
  int num_el;                   // Number of elements
  char *line[2][MATCHER_MAX_TOKENS];    // label, value pairs
  int line_num;                 // config file line number
  matcher_line *next;           // use for linked list
};

// Tag set to use to determining primary selector type
struct matcher_tags
{
  const char *match_host;
  const char *match_domain;
  const char *match_ip;
  const char *match_regex;
  const char *match_host_regex;
  bool dest_error_msg;          // wether to use src or destination in any
  //   errog messages
};
extern const matcher_tags http_dest_tags;
extern const matcher_tags ip_allow_tags;
extern const matcher_tags socks_server_tags;

const char *parseConfigLine(char *line, matcher_line * p_line, const matcher_tags * tags);

// inline void LowerCaseStr(char* str)
//
//   Modifies str so all characters are lower
//     case
//
static inline void
LowerCaseStr(char *str)
{
  if (!str)
    return;
  while (*str != '\0') {
    *str = ParseRules::ink_tolower(*str);
    str++;
  }
}

#endif
