/* -*- c -*-
 *
 * Copyright (c) 2016 Nicira, Inc.
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

for (ls in Logical_Switch) {
    /* Ingress and Egress pre-stateful Table (Priority 0): Packets are
     * allowed by default. */
    flow(ls, LS_IN_PRE_STATEFUL, 0, 1) { next; };
    flow(ls, LS_OUT_PRE_STATEFUL, 0, 1) { next; };

    /* If REGBIT_CONNTRACK_DEFRAG is set as 1, then the packets should be
     * sent to conntrack for tracking and defragmentation. */
    flow(ls, LS_IN_PRE_STATEFUL, 100, reg0[0] /* conntrack.defrag */) { ct_next; };
    flow(ls, LS_OUT_PRE_STATEFUL, 100, reg0[0] /* conntrack.defrag */) { ct_next; };
}
