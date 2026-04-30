#include "globals/global_types.h"
#include "globals/global_defs.h"
#include "op.h"

#define HIT_PRED_TABLE_SIZE 4096

Flag predict_l1_hit(Addr pc);
void train_l1_hit_predictor(Addr pc, Flag actual_hit);
void init_hit_predictor(void);