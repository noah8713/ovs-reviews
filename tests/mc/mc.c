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

#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "jsonrpc.h"
#include "mc.h"
#include "mc_internal.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/json.h"
#include "openvswitch/list.h"
#include "openvswitch/vlog.h"
#include "openvswitch/util.h"
#include "process.h"
#include "signals.h"
#include "stream.h"
#include "util.h"

VLOG_DEFINE_THIS_MODULE(mc);

/*
 * Set the pointers to 
 * struct process and
 * struct jsonrpc_session
 * to NULL after closing/freeing
 * them, when deliberately crashing 
 * processes */
struct mc_process {
    char* name;
    char** run_cmd;

    struct jsonrpc *js;
    struct ovs_list list_node;
    struct process *p;

    /* Config Options */
    bool failure_inject;

    /* Status data */
    bool running;
    int recv_err;
};

struct mc_conn {
    struct ovs_list list_node;
    struct jsonrpc *js;
};

static struct ovs_list mc_processes = OVS_LIST_INITIALIZER(&mc_processes);
static struct ovs_list mc_conns = OVS_LIST_INITIALIZER(&mc_conns);
static const char* listen_addr = NULL;
static struct pstream *listener = NULL;
static bool all_processes_running = false;

static bool mc_receive_rpc(struct jsonrpc *js, struct mc_process *p,
			   union mc_rpc *rpc);
static void mc_start_process(struct mc_process *new_proc);
static void mc_start_all_processes(void);
static void mc_load_config_run(struct json *config);
static void mc_load_config_processes(struct json *config);
static void mc_load_config(const char *filename);
static void mc_handle_hello(struct jsonrpc *js,
			    const struct mc_rpc_hello *rq);
static void mc_handle_rpc(struct jsonrpc *js, struct mc_process *proc,
			  const union mc_rpc *rpc);
static void mc_run_conn(struct jsonrpc *js, struct mc_process *proc);
static void mc_run(void);
    
static bool
mc_receive_rpc(struct jsonrpc *js, struct mc_process *p, union mc_rpc *rpc)
{
    struct jsonrpc_msg *msg;
    int error = jsonrpc_recv(js, &msg);
    
    if (error != 0) {
	if (error != EAGAIN) {
	    jsonrpc_close(js);

	    if (p) {
		p->recv_err = error;
	    }
	}
	return false;
    }
    
    mc_rpc_from_jsonrpc(msg, rpc);
    return true;
}

static void
mc_start_process(struct mc_process *new_proc) {
    /* Prepare to redirect stderr and stdout of the process to a file
     * and then start the process */

    int stdout_copy = dup(fileno(stdout));
    int stderr_copy = dup(fileno(stderr));
    
    char path[strlen(new_proc->name) + 4];
    strcpy(path, new_proc->name);
    strcpy(path + strlen(new_proc->name), ".out");
    int fdout = open(path, O_CREAT|O_RDWR|O_TRUNC, S_IRWXU);

    strcpy(path + strlen(new_proc->name), ".err");
    int fderr = open(path, O_CREAT|O_RDWR|O_TRUNC, S_IRWXU);

    if (fdout < 0 || fderr < 0) {
	ovs_fatal(errno, "Cannot open outfile for process %s",
		  new_proc->name);
    }
    
    dup2(fdout, fileno(stdout));
    dup2(fderr, fileno(stderr));
    
    int err = process_start(new_proc->run_cmd, &(new_proc->p));

    /* Restore our stdout and stderr */
    dup2(stdout_copy, fileno(stdout));
    dup2(stderr_copy, fileno(stderr));
	
    if (err != 0) {
	ovs_fatal(err, "Cannot start process %s", new_proc->name);
    }

    new_proc->running = true;

    close(stdout_copy);
    close(stderr_copy);
    close(fdout);
    close(fderr);
}

static void
mc_start_all_processes(void)
{
    struct mc_process *new_proc;
    LIST_FOR_EACH (new_proc, list_node, &mc_processes) {
	if (!new_proc->running) {
	    mc_start_process(new_proc);
	}
    }
    all_processes_running = true;
}

static void
mc_load_config_run(struct json *config) {
    
    const struct json *run_conf = get_member_or_die(config, "run_config", 0,
						    "Cannot find run_config");
    listen_addr = get_str_member_copy_or_die(run_conf, "listen_address",
					     0, "Cannot find listen_address");

}

static void
mc_load_config_processes(struct json *config) {

    const struct json_array *mc_conf =
	get_member_or_die(config, "model_check_execute", 0,
			  "Cannot find the execute config");
    
    struct mc_process *new_proc;
    for (int i = 0; i < mc_conf->n; i++) {
	new_proc = xzalloc(sizeof(struct mc_process));

	const struct json *exe = get_first_member(mc_conf->elems[i],
						  &(new_proc->name),
						  true);

	const struct json_array *cmd =
	    get_member_or_die(exe, "command",
			      0, "Did not find command for %s\n", new_proc->name);
	
	char **run_cmd = xmalloc(sizeof(char*) * (cmd->n + 1));
	int j = 0;
	for (; j < cmd->n; j++) {
	    run_cmd[j] = xmalloc(strlen(json_string(cmd->elems[j]) + 1));
	    strcpy(run_cmd[j], json_string(cmd->elems[j]));
	}
	run_cmd[j] = NULL;
	new_proc->run_cmd = run_cmd;
	
	/* Should we failure inject this process ? */

	new_proc->failure_inject =
	    *(bool*) get_member_or_die(exe, "failure_inject",
				      0,
				      "Did not find failure_inject for %s\n",
				      new_proc->name);
	
	new_proc->running = false;
	ovs_list_push_back(&mc_processes, &new_proc->list_node);
    }
}

static void
mc_load_config(const char *filename)
{
    struct json *config = json_from_file(filename);
   
    if (config->type == JSON_STRING) {
	ovs_fatal(0, "Cannot read the json config in %s\n%s", filename,
		  config->u.string);
    }
    
    mc_load_config_run(config);
    mc_load_config_processes(config);
    json_destroy(config);
}

static void
mc_handle_hello(struct jsonrpc *js, const struct mc_rpc_hello *rq)
{
    struct mc_process *proc;
    LIST_FOR_EACH (proc, list_node, &mc_processes) {
	if (rq->common.pid == process_pid(proc->p)) {
	    proc->js = js;
	    
	    struct mc_conn *conn;
	    LIST_FOR_EACH (conn, list_node, &mc_conns) {
		if (conn->js == js) {
		    ovs_list_remove(&conn->list_node);
		    free(conn);
		    break;
		}
	    }
	    break;
	}
    }
}

static void
mc_handle_choose_req(const struct mc_process *proc,
		     const struct mc_rpc_choose_req *rq)
{
    union mc_rpc rpc;
    rpc.common.type = MC_RPC_CHOOSE_REPLY;
    rpc.common.pid = 0;
    rpc.choose_reply.reply = MC_RPC_CHOOSE_REPLY_NORMAL;

    int error = jsonrpc_send_block(proc->js,
    				   mc_rpc_to_jsonrpc(&rpc));
    
    if (error != 0) {
    	ovs_fatal(error, "Cannot send choose reply to %s\n", proc->name);
    }
}

static void
mc_handle_rpc(struct jsonrpc *js, struct mc_process *proc,
	      const union mc_rpc *rpc)
{
    switch (rpc->common.type) {
    case MC_RPC_HELLO:
	mc_handle_hello(js, &rpc->hello);
	break;
	
    case MC_RPC_CHOOSE_REQ:
	mc_handle_choose_req(proc, &rpc->choose_req);
	break;

    case MC_RPC_CHOOSE_REPLY:
	ovs_assert(0);
	break;
	
    case MC_RPC_ASSERT:
	/** Handle Me !! **/
	break;
    }
}

static void
mc_run_conn(struct jsonrpc *js, struct mc_process *proc)
{
    if (js) {
	jsonrpc_run(js);
	
	union mc_rpc rpc;
	if (mc_receive_rpc(js, proc, &rpc)) {
	    mc_handle_rpc(js, proc, &rpc);
	}
    } else if (proc && proc->running) {
	/* This has been called from a process context
	 * which the model checker believes to be running
	 * but the jsonrpc connection either is null or
	 * it is no longer alive */
    }
}

static void
mc_run(void)
{
    if (!listener) {
	int error = pstream_open(listen_addr, &listener, DSCP_DEFAULT);
	
	if (error) {
	    ovs_fatal(error, "Cannot open the listening conn due to %s\n",
		      ovs_strerror(error));
	}
    }
    
    if (!all_processes_running) {
	mc_start_all_processes();
    }
    
    if (listener) {
	struct stream *stream;
	int error = pstream_accept(listener, &stream);
	if (!error) {
	    struct mc_conn *conn = xzalloc(sizeof *conn);
	    ovs_list_push_back(&mc_conns, &conn->list_node);
	    conn->js = jsonrpc_open(stream);
	} 
    }

    struct mc_conn *conn, *next;
    LIST_FOR_EACH_SAFE (conn, next, list_node, &mc_conns) {
	ovs_assert(conn->js != NULL);
	mc_run_conn(conn->js, NULL);
    }

    process_run();
    struct mc_process *proc;
    LIST_FOR_EACH (proc, list_node, &mc_processes) {
	if (proc->running && !process_exited(proc->p)) {
	    mc_run_conn(proc->js, proc);
	} else if (proc->running && process_exited(proc->p)) {
	    /* XXX. Model checker thinks the process is 
	     * running but it is not running anymore ? **/

	    /* Use this check to judge if the process was killed
	       by some signal (e.g. SIGSEGV) */
	    if (WIFSIGNALED(process_status(proc->p))) {
		fprintf(stderr, "%s %s\n", proc->name,
			process_status_msg(process_status(proc->p)));
	    }
	} else if (!proc->running) {
	    /* XXX. This should only be the case when we
	     * crash the process deliberately at some stage 

	     * This should instead be handled in a get_process_actions() 
	     * function called from a larger get_enabled_actions()
	     * function. One of the "actions" that can be applied
	     * to a state is to restart a deliberately crashed 
	     * process */
	} /* XXX another else branch here should check for
	   * timeouts of processes that are believed to be
	   * running but have not contacted the model checker
	   * for a decision on a syscall or libcall 
	   * i.e. they might be stuck in an infinite loop */
    }
}

int
main(int argc, char *argv[])
{
    if (argc < 2) {
	ovs_fatal(0, "Usage is ./mc <configfile>. Not enough arguments provided");
    }

    mc_load_config(argv[1]);
    
    for(;;) {
    	mc_run();
    }
    
    return 0;
}
