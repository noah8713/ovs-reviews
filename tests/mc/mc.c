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
#include <unistd.h>
#include <fcntl.h>
#include "jsonrpc.h"
#include "mc.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/json.h"
#include "openvswitch/list.h"
#include "openvswitch/vlog.h"
#include "openvswitch/util.h"
#include "process.h"
#include "util.h"

VLOG_DEFINE_THIS_MODULE(mc);

struct mc_process {
    char* name;
    char** run_cmd;
    struct jsonrpc_session *js;
    struct ovs_list list_node;
    struct process *proc_ptr;
    struct uuid sid;
    bool failure_inject;
};

static struct ovs_list mc_processes = OVS_LIST_INITIALIZER(&mc_processes);

static void
start_processes(void)
{
    struct mc_process *new_proc;
    LIST_FOR_EACH (new_proc, list_node, &mc_processes) {  
	/* Prepare to redirect stderr and stdout of the process to a file
	 * and then start the process */

	int stdout_copy = dup(fileno(stdout));
	int stderr_copy = dup(fileno(stderr));
    
	char path[strlen(new_proc->name) + 4];
	strcpy(path, new_proc->name);
	strcpy(path + strlen(new_proc->name), ".out");
	int fd = open(path, O_CREAT|O_RDWR|O_TRUNC, S_IRWXU);
    
	dup2(fd, fileno(stdout));
	dup2(fd, fileno(stderr));
    
	int errno = process_start(new_proc->run_cmd, &(new_proc->proc_ptr));

	/* Restore our stdout and stderr */
	dup2(stdout_copy, fileno(stdout));
	dup2(stderr_copy, fileno(stderr));
	
	if (errno != 0) {
	    ovs_fatal(errno, "Cannot start process %s", new_proc->name);
	}

	close(stdout_copy);
	close(stderr_copy);
	close(fd);
    }
}

static void
read_config_processes(struct json *config) {

    ovs_assert(config->type == JSON_OBJECT);
    
    struct json *exec_conf = shash_find_data(config->u.object,
					     "model_check_execute");
    
    if (exec_conf == NULL) {
	ovs_fatal(0, "Cannot find the execute config");
    }

    ovs_assert(exec_conf->type == JSON_ARRAY);

    struct mc_process *new_proc;
    for (int i = 0; i < exec_conf->u.array.n; i++) {
	struct shash_node *exe =
	    shash_first(exec_conf->u.array.elems[i]->u.object);
	new_proc = xmalloc(sizeof(struct mc_process));
	new_proc->name = xmalloc(strlen(exe->name));
	strcpy(new_proc->name, exe->name);

	struct json *exe_data = exe->data;
	exe_data = shash_find_data(exe_data->u.object, "command");

	if (exe_data == NULL) {
	    ovs_fatal(0, "Did not find command for %s\n", exe->name);
	}
	
	char **run_cmd = xmalloc(sizeof(char*) * (exe_data->u.array.n + 1));
	int j = 0;
	for (; j < exe_data->u.array.n; j++) {
	    run_cmd[j] = xmalloc(strlen(exe_data->u.array.elems[j]->u.string));
	    strcpy(run_cmd[j], exe_data->u.array.elems[j]->u.string);
	}
	run_cmd[j] = NULL;
	new_proc->run_cmd = run_cmd;
	
	/* Should we failure inject this process ? */
	
	exe_data = exe->data;
	exe_data = shash_find_data(exe_data->u.object, "failure_inject");
	if (exe_data == NULL ||
	    !(exe_data->type == JSON_TRUE || exe_data->type == JSON_FALSE)) {

	    ovs_fatal(0,
		      "Did not find failure_inject boolean for %s\n",
		      exe->name);
	} else if (exe_data->type == JSON_TRUE) {
	    new_proc->failure_inject = true;
	} else {
	    new_proc->failure_inject = false;
	}

	ovs_list_push_back(&mc_processes, &new_proc->list_node);
    }
}
    
int
main(int argc, char *argv[])
{
    if (argc < 2) {
	ovs_fatal(0, "Usage is ./mc <configfile>. Not enough arguments provided");
    }

    struct json *config = json_from_file(argv[1]);

    if (config->type == JSON_STRING) {
	ovs_fatal(0, "Cannot read the json config in %s\n%s", argv[1], config->u.string);
    }

    read_config_processes(config);
    start_processes();
    
    return 0;
}
