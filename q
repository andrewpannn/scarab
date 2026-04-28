[1mdiff --git a/src/dcache_stage.c b/src/dcache_stage.c[m
[1mindex eaf68a8..5fbfe3c 100644[m
[1m--- a/src/dcache_stage.c[m
[1m+++ b/src/dcache_stage.c[m
[36m@@ -263,8 +263,6 @@[m [mvoid update_dcache_stage(Stage_Data* src_sd) {[m
                 wake_up_ops(op, REG_DATA_DEP, model->wake_hook);[m
             }[m
 [m
[31m-            //dc->sd.ops[oldest_index] = NULL;[m
[31m-            //dc->sd.op_count--;[m
             continue;[m
         }[m
         // ASSERT(dc->proc_id, get_rfp_state(op->unique_num) == RFP_PENDING);[m
[1mdiff --git a/src/memory/memory.c b/src/memory/memory.c[m
[1mindex aac386b..1f06e18 100644[m
[1m--- a/src/memory/memory.c[m
[1m+++ b/src/memory/memory.c[m
[36m@@ -3410,8 +3410,48 @@[m [mFlag new_mem_req(Mem_Req_Type type, uns8 proc_id, Addr addr, uns size, uns delay[m
       priority_offset = 0;[m
   }[m
 [m
[32m+[m[32m  if (type == MRT_RFP) {[m
[32m+[m[32m    priority_offset = 0;[m
[32m+[m[32m  }[m
[32m+[m
   new_priority = Mem_Req_Priority_Offset[type] + priority_offset;[m
[32m+[m[32m  if (op != NULL) {[m
[32m+[m
[32m+[m[32m    /* 1) On-path demand loads: keep default priority[m
[32m+[m[32m    *    We do NOT aggressively boost them here.[m
[32m+[m[32m    */[m
[32m+[m
[32m+[m[32m    /* 2) Stores: slightly lower priority than loads */[m
[32m+[m[32m    if (!op->off_path && type == MRT_DSTORE) {[m
[32m+[m[32m      new_priority += 4;[m
[32m+[m[32m    }[m
[32m+[m
[32m+[m[32m    /* 3) Off-path demand requests: deprioritize */[m
[32m+[m[32m    if (op->off_path &&[m
[32m+[m[32m        (type == MRT_DFETCH || type == MRT_DSTORE || type == MRT_IFETCH)) {[m
[32m+[m[32m      new_priority += 8;[m
[32m+[m[32m    }[m
[32m+[m[32m    if (type == MRT_RFP) {[m
[32m+[m[32m      Addr load_pc = op->inst_info ? op->inst_info->addr : 0;[m
 [m
[32m+[m[32m      if ((load_pc & 1) == 0) {[m
[32m+[m[32m        /* higher-tier RFP: only slightly better than default RFP */[m
[32m+[m[32m        if (new_priority >= 2) {[m
[32m+[m[32m          new_priority -= 2;[m
[32m+[m[32m        } else {[m
[32m+[m[32m          new_priority = 0;[m
[32m+[m[32m        }[m
[32m+[m[32m      } else {[m
[32m+[m[32m        /* lower-tier RFP: slightly worse than default RFP */[m
[32m+[m[32m        new_priority += 6;[m
[32m+[m[32m      }[m
[32m+[m[32m    }[m
[32m+[m
[32m+[m[32m    /* 5) Normal prefetches remain low priority */[m
[32m+[m[32m    if (type == MRT_DPRF || type == MRT_IPRF) {[m
[32m+[m[32m      new_priority += 8;[m
[32m+[m[32m    }[m
[32m+[m[32m  }[m
   /* Step 1: Figure out if this access is already in the request buffer */[m
   // Search ramulator queue[m
   matching_req = mem_search_reqbuf(proc_id, addr, type, size, &demand_hit_prefetch, &demand_hit_writeback,[m
