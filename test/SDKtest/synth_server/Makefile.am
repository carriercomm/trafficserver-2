# 
# Makefile.am for SDKtest server
#
#  Licensed to the Apache Software Foundation (ASF) under one
#  or more contributor license agreements.  See the NOTICE file
#  distributed with this work for additional information
#  regarding copyright ownership.  The ASF licenses this file
#  to you under the Apache License, Version 2.0 (the
#  "License"); you may not use this file except in compliance
#  with the License.  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.

DEFS = @API_DEFS@ 

INCLUDES = -I$(srcdir)/api

noinst_PROGRAMS = SDKtest_server	

SDKtest_server_SOURCES = synth_server.c

SDKtest_server_LDFLAGS = -lm @LIBSOCKET@ @LIBNSL@ @LIBDL@



clean:
	-$(RM) -rf Templates.DB cxx_repository .deps $(noinst_PROGRAMS)
