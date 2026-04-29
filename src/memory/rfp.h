#ifndef __RFP_H__
#define __RFP_H__

#include "globals/global_types.h"
#include "globals/global_defs.h"
#include "op.h"

#define RFP_TRACKER_SIZE 4096 // Must be larger than max in-flight ops

typedef enum { RFP_NONE = 0, RFP_PENDING, RFP_COMPLETED } RFP_State;

typedef struct {
    Counter unique_num; // Store this to verify we don't have a stale entry
    RFP_State state;
} RFP_Tracker_Entry;

typedef struct RFP_Queue_Entry_struct {
    Addr    addr;
    Counter unique_num;
    uns     proc_id;
    int     phys_reg;
    Counter op_unique_num; // Used for sorting (older = higher priority)
    Flag    valid;
} RFP_Queue_Entry;

#define RFP_QUEUE_SIZE 1024

// Called once per cycle from the simulator's main loop (e.g., in update_memory)
void rfp_advance_queue(void);

extern RFP_Tracker_Entry rfp_tracker[RFP_TRACKER_SIZE];

/* --- Interface Functions --- */

void set_rfp_state(Counter unique_num, RFP_State state);
RFP_State get_rfp_state(Counter unique_num);

// Called from reg_file_rename in map_rename.c
void rfp_try_schedule(Op* op);

// Called when a request is satisfied (L1 hit or L1 fill)
void rfp_mark_completed(Counter unique_num);

// Throttling helper
Flag rfp_is_system_too_busy(uns proc_id);

Flag rfp_available_send(void);
void send_rfp(Op* op);

#endif // __RFP_H__