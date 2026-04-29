#include "rfp.h"
#include "memory/memory.h"
#include "memory/memory.param.h"
#include "libs/cache_lib.h"    // For cache_access
#include "dcache_stage.h"      // For Dcache_Stage and ports
#include "cmp_model.h"         // For cmp_model.dcache_stage
#include "statistics.h"

// Parameters - These could be moved to your param file later
#define RFP_DEMAND_RESERVE 8  // Always keep 8 slots free for demand loads
#define RFP_L1Q_THRESHOLD  16 // Don't inject if L1 queue is half full
#define MEM_REQ_BUFFER_ENTRIES 32

RFP_Tracker_Entry rfp_tracker[RFP_TRACKER_SIZE];

// --- TO INSERT / UPDATE ---
void set_rfp_state(Counter unique_num, RFP_State state) {
    uns index = unique_num % RFP_TRACKER_SIZE;
    rfp_tracker[index].unique_num = unique_num;
    rfp_tracker[index].state = state;
}

// --- TO CHECK ---
RFP_State get_rfp_state(Counter unique_num) {
    uns index = unique_num % RFP_TRACKER_SIZE;
    // Verify this entry actually belongs to THIS instruction, not an old one
    if (rfp_tracker[index].unique_num == unique_num) {
        return rfp_tracker[index].state;
    }
    return RFP_NONE; 
}

// Parameters (usually defined in memory.param.h, but used here for bank calc)
extern L1_Data* l1_pref_cache_access(Mem_Req* req); 

#define RFP_PRED_TABLE_SIZE 1024
#define RFP_CONF_THRESHOLD 2
#define RFP_MAX_CONF 3

typedef struct RFP_Pred_Entry {
  Flag valid;
  Addr pc;
  Addr prev_addr;
  Addr last_addr;
  int64 stride;
  uns confidence;
} RFP_Pred_Entry;

static RFP_Pred_Entry rfp_pred_table[RFP_PRED_TABLE_SIZE];

static Flag rfp_predict_addr(Op *op, Addr *pred_addr) {
  if (op == NULL || op->inst_info == NULL) {
    return FALSE;
  }

  Addr pc = op->inst_info->addr;
  uns index = pc % RFP_PRED_TABLE_SIZE;
  RFP_Pred_Entry *entry = &rfp_pred_table[index];

  if (entry->stride == 0) {
    return FALSE;
  }
  if (!entry->valid || entry->pc != pc) {
    return FALSE;
  }

  if (entry->confidence < RFP_CONF_THRESHOLD) {
    return FALSE;
  }

  *pred_addr = (Addr)((int64)entry->last_addr + entry->stride);
  return TRUE;
}

static void rfp_update_predictor(Op *op, Addr actual_addr) {
  if (op == NULL || op->inst_info == NULL) {
    return;
  }

  Addr pc = op->inst_info->addr;
  uns index = pc % RFP_PRED_TABLE_SIZE;
  RFP_Pred_Entry *entry = &rfp_pred_table[index];

  if (!entry->valid || entry->pc != pc) {
    entry->valid = TRUE;
    entry->pc = pc;
    entry->prev_addr = actual_addr;
    entry->last_addr = actual_addr;
    entry->stride = 0;
    entry->confidence = 0;
    return;
  }

  int64 new_stride = (int64)actual_addr - (int64)entry->last_addr;

  if (new_stride == entry->stride) {
    if (entry->confidence < RFP_MAX_CONF) {
      entry->confidence++;
    }
  } else {
    if (entry->confidence > 0) {
      entry->confidence--;
    }
    entry->stride = new_stride;
  }

  entry->prev_addr = entry->last_addr;
  entry->last_addr = actual_addr;
}


void rfp_try_schedule(Op* op) {
  if (!op) return;
  if (!op->rfp_eligible) return;
  if (!op->inst_info) return;
  if (op->inst_info->table_info.mem_type != MEM_LD) return;

  uns proc_id = op->proc_id;
  Addr actual_addr = op->oracle_info.va;
  Addr predicted_addr = 0;
  STAT_EVENT(proc_id, RFP_PREDICT_ATTEMPT);

  if (!rfp_predict_addr(op, &predicted_addr)) {
    rfp_update_predictor(op, actual_addr);
    return;
  }
    STAT_EVENT(proc_id, RFP_PREDICT_HIT);

  if (get_proc_id_from_cmp_addr(predicted_addr) != proc_id) {
    rfp_update_predictor(op, actual_addr);
    return;
  }

  set_dcache_stage(&cmp_model.dcache_stage[proc_id]);

  uns bank = (predicted_addr >> dc->dcache.shift_bits) &
             N_BIT_MASK(LOG2(DCACHE_BANKS));

  if (!get_read_port(&dc->ports[bank])) {
    STAT_EVENT(proc_id, RFP_PROBE_PORT_BUSY);
    rfp_update_predictor(op, actual_addr);
    return;
  }

  Addr dummy_line_addr;
  Dcache_Data* dc_hit =
      (Dcache_Data*)cache_access(&dc->dcache,
                                 predicted_addr,
                                 &dummy_line_addr,
                                 FALSE);

  if (dc_hit) {
    STAT_EVENT(proc_id, RFP_PROBE_HIT);
    rfp_mark_completed(op->unique_num);
    rfp_update_predictor(op, actual_addr);
    return;
  }
  STAT_EVENT(proc_id, RFP_PROBE_MISS);

  if (rfp_is_system_too_busy(proc_id)) {
    STAT_EVENT(proc_id, RFP_THROTTLED_SYS_BUSY);
    rfp_update_predictor(op, actual_addr);
    return;
  }

  Pref_Req_Info pref_info = {0};
  pref_info.dest = DEST_L1;
  pref_info.loadPC = op->inst_info ? op->inst_info->addr : 0;

  Flag success = new_mem_req(MRT_RFP,
                             proc_id,
                             predicted_addr,
                             64,
                             0,
                             NULL,
                             NULL,
                             op->unique_num,
                             &pref_info);

  if (success) {
    set_rfp_state(op->unique_num, RFP_PENDING);
    STAT_EVENT(proc_id, RFP_INJECTED);
  }else{
    STAT_EVENT(proc_id, RFP_INJECTION_FAILED);
  }

  rfp_update_predictor(op, actual_addr);
}

Flag rfp_is_system_too_busy(uns proc_id) {
    // Check global request buffer occupancy
    uns total_used = mem->req_count;
    if (total_used > (MEM_REQ_BUFFER_ENTRIES - RFP_DEMAND_RESERVE)) {
        return TRUE;
    }

    // Check local L1 queue occupancy
    if (mem->l1_queue.entry_count > RFP_L1Q_THRESHOLD) {
        return TRUE;
    }

    return FALSE;
}

void rfp_mark_completed(Counter unique_num) {
    // Centralized place to update state
    set_rfp_state(unique_num, RFP_COMPLETED);
}
