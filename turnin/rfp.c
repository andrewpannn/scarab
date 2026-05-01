#include "memory/rfp.h"
#include "memory/hit_pred.h"
#include "memory/memory.h"
#include "memory/memory.param.h"
#include "libs/cache_lib.h"    // For cache_access
#include "dcache_stage.h"      // For Dcache_Stage and ports
#include "cmp_model.h"         // For cmp_model.dcache_stage
#include "statistics.h"

#define RFP_DEMAND_RESERVE 8  
#define RFP_L1Q_THRESHOLD 16 
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


// Parameters (usually defined in memory.param.h, but used here for bank calc)
extern L1_Data* l1_pref_cache_access(Mem_Req* req); 
RFP_Queue_Entry rfp_queue[RFP_QUEUE_SIZE];

void default_rfp(Op* op) {
    if (!op->rfp_eligible) return;

    uns proc_id = op->proc_id;
    //Addr pred_addr = 0;
    Addr oracle_addr = op->oracle_info.va;
    Addr rfp_addr = 0;

    if (!RFP_USE_STRIDE) {
        rfp_addr = op->oracle_info.va;
    } else {
        Addr actual = op->oracle_info.va;
        Addr pred_addr = 0;

        if (!rfp_predict_addr(op, &pred_addr)) {
            rfp_update_predictor(op, actual);
            return;
        }

       if (get_proc_id_from_cmp_addr(pred_addr) != op->proc_id) {
        rfp_update_predictor(op, actual);
        STAT_EVENT(op->proc_id, RFP_PRED_MISS);
        return;
    }


       rfp_addr = pred_addr;
       rfp_update_predictor(op, actual);
    }
    
    
    // This allows us to use dc->dcache and dc->ports
    set_dcache_stage(&cmp_model.dcache_stage[proc_id]);

    // Uses dc->dcache.shift_bits instead of L1_LINE_SIZE
    // and N_BIT_MASK instead of the % operator for speed
    uns bank = (oracle_addr >> dc->dcache.shift_bits) & N_BIT_MASK(LOG2(DCACHE_BANKS));

    // Check for Port Availability
    if (!get_read_port(&dc->ports[bank])) {
        STAT_EVENT(proc_id, RFP_PROBE_PORT_BUSY);
        return; 
    }

    // Admission Control
    if (rfp_is_system_too_busy(proc_id)) {
        STAT_EVENT(proc_id, RFP_THROTTLED_SYS_BUSY);
        return; 
    }

    // Prepare Prefetch Info
    Pref_Req_Info pref_info = {0};
    pref_info.dest = DEST_L1;
    pref_info.is_l1_to_rf_pref = TRUE;
    pref_info.dest_phys_reg = op->dst_reg_id[0][REG_TABLE_TYPE_PHYSICAL];

    Flag success = new_mem_req(MRT_RFP, 
                               proc_id, 
                               rfp_addr, 
                               64,      // Size (64-byte line)
                               0,       // Delay
                               NULL,    // Op (passed NULL to prevent normal completion logic)
                               NULL,    // Done function
                               op->unique_num, 
                               &pref_info);

    if (success) {
        set_rfp_state(op->unique_num, RFP_PENDING);
        STAT_EVENT(proc_id, RFP_INJECTED);
    } else {
        STAT_EVENT(proc_id, RFP_INJECTION_FAILED);
    }
}



void rfp_try_schedule(Op* op) {
    if (!op->rfp_eligible) return;

    if (RFP_DISABLE_QUEUE) {
        default_rfp(op);
        return;
    }

    Addr rfp_addr = 0;

    if (!RFP_USE_STRIDE) {
        rfp_addr = op->oracle_info.va;
    } else {
        Addr actual = op->oracle_info.va;
        Addr pred_addr = 0;

        if (!rfp_predict_addr(op, &pred_addr)) {
            rfp_update_predictor(op, actual);
            return;
        }

      if (get_proc_id_from_cmp_addr(pred_addr) != op->proc_id) {
        rfp_update_predictor(op, actual);
        STAT_EVENT(op->proc_id, RFP_PRED_MISS);
        return;
    }

       rfp_addr = pred_addr;
       rfp_update_predictor(op, actual);
    }

    // dual priority buffer, RR style
    // FIFO for non priority requeusts
    for (int i = 0; i < RFP_QUEUE_SIZE; i++) {
    if (!rfp_queue[i].valid) {
        int prio;
        Addr addr = op->inst_info->addr;
        Flag actual_hit = !op->oracle_info.l1_miss;

        if (RFP_HIT_PREDICTOR) {
            prio = predict_l1_hit(addr);
            if (prio == actual_hit) {
                STAT_EVENT(op->proc_id, HIT_PRED_ACCURATE);
            }
            train_l1_hit_predictor(addr, actual_hit);
        } else {
            prio = actual_hit;
        }

        rfp_queue[i].addr = rfp_addr;
        rfp_queue[i].unique_num = op->unique_num;
        rfp_queue[i].proc_id = op->proc_id;
        rfp_queue[i].phys_reg = op->dst_reg_id[0][REG_TABLE_TYPE_PHYSICAL];
        rfp_queue[i].op_unique_num = op->unique_num;
        rfp_queue[i].valid = TRUE;
        rfp_queue[i].priority = prio;

        if (!RFP_FIFO) {
            rfp_queue[i].op_unique_num = prio ? 0 : op->unique_num;
        }

        STAT_EVENT(op->proc_id, RFP_QUEUED);
        return;
    }
}

STAT_EVENT(op->proc_id, RFP_QUEUE_FULL);

}

Flag rfp_is_system_too_busy(uns proc_id) {
    // Check global request buffer occupancy
    uns total_used = mem_get_req_count(proc_id);
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

Flag rfp_available_send(void) {
    uns bank = 0;
    uns proc_id = 0;
    set_dcache_stage(&cmp_model.dcache_stage[proc_id]);

    // Check for Port Availability
    if (!get_read_port(&dc->ports[bank])) {
        STAT_EVENT(proc_id, RFP_PROBE_PORT_BUSY);
        return false; 
    }

    // Admission Control (Throttling)
    if (rfp_is_system_too_busy(proc_id)) {
        STAT_EVENT(proc_id, RFP_THROTTLED_SYS_BUSY);
        return false; 
    }
    return true;
}
 
// RFP issue logic
void rfp_advance_queue() {
    while (rfp_available_send()) {
        int best_idx = -1; 
        Counter oldest_unq = 0xFFFFFFFFFFFFFFFFULL;
        Flag found_hit = FALSE;

        // Search for the highest priority (oldest) valid request
        if (RFP_FIFO) {
            for (int i = 0; i < RFP_QUEUE_SIZE; i++) {
                // Search for the highest priority (oldest) valid request
                if (rfp_queue[i].priority && !found_hit && rfp_queue[i].valid) {
                    found_hit = TRUE;
                    best_idx = i;
                    oldest_unq = rfp_queue[i].op_unique_num;
                }
                
                // (Both Hits OR Both Misses)
                else if (rfp_queue[i].priority == found_hit) {
                    // Break ties using strict FIFO (oldest unique_num wins)
                    if (rfp_queue[i].valid && rfp_queue[i].op_unique_num < oldest_unq) {
                        best_idx = i;
                        oldest_unq = rfp_queue[i].op_unique_num;
                    }
                }
            }
        } else {
            for (int i = 0; i < RFP_QUEUE_SIZE; i++) {
                if ((rfp_queue[i].valid && rfp_queue[i].op_unique_num < oldest_unq) ) {
                    oldest_unq = rfp_queue[i].op_unique_num;
                    best_idx = i;
                }
        }
        }

        if (best_idx != -1) {
            RFP_Queue_Entry* req = &rfp_queue[best_idx];
            
            Pref_Req_Info pref_info = {0};
            pref_info.dest = DEST_L1;
            pref_info.dest_phys_reg = req->phys_reg;

            Flag success = new_mem_req(MRT_RFP, req->proc_id, req->addr, 64, 0, 
                                    NULL, NULL, req->unique_num, &pref_info);

            if (success) {
                set_rfp_state(req->unique_num, RFP_PENDING);
                req->valid = FALSE; // Remove from queue
                STAT_EVENT(req->proc_id, RFP_ISSUED_FROM_QUEUE);
            }
        }
    }
}