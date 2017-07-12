/*
 * Copyright (c) 2016, 2017 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * A raft client which takes a list of commands to send to a raft server
 * driver (the kind in tests/mc/raft-driver.c) and sends them using library 
 * calls interposed on by the model checker
 */

#include <config.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "command-line.h"
#include "jsonrpc.h"
#include "mc.h"
#include "mc_wrap.h"
#include "openvswitch/json.h"
#include "ovsdb-error.h"
#include "poll-loop.h"
#include "unixctl.h"
#include "util.h"

#define MAX_LINE_SIZE 50

/* First arg is unix socket path for communicating with a raft server
 *    -- In future make this a list and allow client to switch to other servers
 * Second arg is unix socket path for communicating with model checker
 * Third arg is the file containing a list of commands to send to the servers
 */

int
main(int argc, char *argv[])
{
    if (argc < 4) {
	ovs_fatal(0, "Not enough arguments provided to raft-client");
    }

    /*XXX Possibly add usage help and more sophisticated option processing */

    struct jsonrpc_session *mc_conn = jsonrpc_session_open(argv[2], true);
    union mc_rpc rpc;
    rpc.common.type = MC_RPC_HELLO;
    rpc.common.pid = getpid();

    while(!jsonrpc_session_is_connected(mc_conn)) {
	jsonrpc_session_run(mc_conn);
	jsonrpc_session_send(mc_conn, mc_rpc_to_jsonrpc(&rpc));
    }
    struct jsonrpc *raft_conn;
    mc_wrap_unixctl_client_create(argv[1], &raft_conn);

    FILE *fp = fopen(argv[3], "r");

    if (fp == NULL) {
	ovs_fatal(0, "Client cannot open the command file");
    }

    char* linep = xmalloc(MAX_LINE_SIZE);
    while (fgets(linep, MAX_LINE_SIZE, fp) != NULL) {
	char *result, *err, *cmd, *arg;
	struct json *cmd_json = json_object_create();

	/* Assuming that all the commands will have exactly one argument */
	char *copy_linep = linep;
	cmd = strsep(&copy_linep, " ");
	arg = strsep(&copy_linep, " \n");
	json_object_put_string(cmd_json, cmd, arg);

	char* cmd_str[] = {json_to_string(cmd_json, 0)};
	int error_num = mc_wrap_unixctl_client_transact(raft_conn, "execute", 1,
							cmd_str, &result, &err);

	if (error_num != 0) {
	    /* This could be because the server crashed (including deliberately
	     * by the model checker). Contact some other server ?
	     */
	    fprintf(stderr, "Error: %s\n", ovs_strerror(error_num));
	} else {
	    /* Again here the server being contacted might not be the leader
	     * in which case, maybe contact another server */
	    if (err == NULL) {
		fprintf(stderr, "Command %s %s resulted in %s\n", cmd, arg, result);
	    } else {
		fprintf(stderr, "Command %s %s. Server error %s\n", cmd, arg, err);
	    }
	}
    }

    return 0;
}

