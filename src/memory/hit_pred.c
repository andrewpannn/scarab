#include "memory/hit_pred.h"

Flag predict_l1_hit(Addr pc);
void train_l1_hit_predictor(Addr pc, Flag actual_hit);
void init_hit_predictor(void);

static uns8 l1_hit_predictor[HIT_PRED_TABLE_SIZE];

void init_hit_predictor(void) {
    for (int i = 0; i < HIT_PRED_TABLE_SIZE; i++) {
        l1_hit_predictor[i] = 1; // Default to weakly miss
    }
}

Flag predict_l1_hit(Addr pc) {
    // Shift right by 2 to ignore the byte-alignment bits of the instruction address
    uns index = (pc >> 2) & (HIT_PRED_TABLE_SIZE - 1);
    
    // If the counter is 2 or 3, we predict a hit.
    if (l1_hit_predictor[index] >= 2) {
        return TRUE;
    } else {
        return FALSE;
    }
}

void train_l1_hit_predictor(Addr pc, Flag actual_hit) {
    uns index = (pc >> 2) & (HIT_PRED_TABLE_SIZE - 1);
    
    if (actual_hit) {
        // Saturate at 3 (Strongly Hit)
        if (l1_hit_predictor[index] < 3) {
            l1_hit_predictor[index]++;
        }
    } else {
        // Saturate at 0 (Strongly Miss)
        if (l1_hit_predictor[index] > 0) {
            l1_hit_predictor[index]--;
        }
    }
}