#include "rfp.h"
#include "memory/memory.h"
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

void rfp_try_schedule(Op* op) {
    if (!op->rfp_eligible) return;

    uns proc_id = op->proc_id;

    // 1. Admission Control (Throttling)
    if (rfp_is_system_too_busy(proc_id)) {
        STAT_EVENT(proc_id, RFP_THROTTLED_SYS_BUSY);
        return; 
    }

    // 2. Prepare Prefetch Info
    // Note: Ensure your Pref_Req_Info struct in memory.h has these fields
    Pref_Req_Info pref_info = {0};
    pref_info.dest = DEST_L1;
    pref_info.is_l1_to_rf_pref = TRUE;
    // pref_info.rfp_op = op; 
    pref_info.dest_phys_reg = op->dst_reg_id[0][REG_TABLE_TYPE_PHYSICAL];

    // 3. Blind Injection
    Addr oracle_addr = op->oracle_info.va;
    
    // We use MRT_RFP so the memory system knows this is high priority
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