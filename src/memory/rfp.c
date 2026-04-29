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
RFP_Queue_Entry rfp_queue[RFP_QUEUE_SIZE];

void rfp_try_schedule(Op* op) {
    if (!op->rfp_eligible) return;

    for (int i = 0; i < RFP_QUEUE_SIZE; i++) {
        int prio = !op->oracle_info.l1_miss ? 0 : op->unique_num;
        if (!rfp_queue[i].valid) {
            rfp_queue[i].addr = op->oracle_info.va;
            rfp_queue[i].unique_num = op->unique_num;
            rfp_queue[i].proc_id = op->proc_id;
            rfp_queue[i].phys_reg = op->dst_reg_id[0][REG_TABLE_TYPE_PHYSICAL];
            rfp_queue[i].op_unique_num = prio; // Priority key
            rfp_queue[i].valid = TRUE;
            
            STAT_EVENT(op->proc_id, RFP_QUEUED);
            return;
        }
    }
    STAT_EVENT(op->proc_id, RFP_QUEUE_FULL);

}

void send_rfp(Op* op) {
    uns proc_id = op->proc_id;
    Addr oracle_addr = op->oracle_info.va;

    // Prepare Prefetch Info
    Pref_Req_Info pref_info = {0};
    pref_info.dest = DEST_L1;
    pref_info.is_l1_to_rf_pref = TRUE;
    pref_info.dest_phys_reg = op->dst_reg_id[0][REG_TABLE_TYPE_PHYSICAL];

    Flag success = new_mem_req(MRT_RFP, 
                               proc_id, 
                               oracle_addr, 
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
 
// --- 2. ISSUE FROM QUEUE (The new logic) ---
void rfp_advance_queue() {
    // We only try to issue if the system isn't too busy
    // You can also limit bandwidth here (e.g., only 1 issue per cycle)
    // uns proc_id = 0; // Assume single core or loop through cores

    while (rfp_available_send()) {
        int best_idx = -1; 
        Counter oldest_unq = 0xFFFFFFFFFFFFFFFFULL;

        // Search for the highest priority (oldest) valid request
        for (int i = 0; i < RFP_QUEUE_SIZE; i++) {
            if (rfp_queue[i].valid && rfp_queue[i].op_unique_num < oldest_unq) {
                oldest_unq = rfp_queue[i].op_unique_num;
                best_idx = i;
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