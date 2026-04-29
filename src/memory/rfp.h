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



#endif // __RFP_H__