//========================================================//
//  predictor.c                                           //
//  Source file for the Branch Predictor                  //
//                                                        //
//  Implement the various branch predictors below as      //
//  described in the README                               //
//========================================================//
#include <stdio.h>
#include <math.h>
#include "predictor.h"

// -------------------- Tournament predictor configuration --------------------
#define T_LHT_BITS   11               // local history bits 
#define T_LHT_ENTRIES (1UL << T_LHT_BITS) // 1024 entries
#define T_LPT_ENTRIES (1UL << T_LHT_BITS) // index by local history 
#define T_LPT_COUNTER_MAX 7           // 3-bit saturating counter
#define T_LPT_INIT 0

#define T_GHR_BITS   13               // global history bits
#define T_GPT_ENTRIES (1UL << T_GHR_BITS) // 4096 entries
#define T_GPT_COUNTER_MAX 3           // 2-bit saturating counter
#define T_GPT_INIT 0

#define T_CHOOSER_ENTRIES (1UL << T_GHR_BITS) // 4096 entries
#define T_CHOOSER_MAX 3           // 2-bit saturating counter 
#define T_CHOOSER_INIT 2              // bias slightly toward global
#define INC_SAT8(x, max) if ((x) < (max)) (x)++
#define DEC_SAT8(x, min) if ((x) > (min)) (x)--

//
// TODO:Student Information
//
const char *studentName = "Zuo Yang";
const char *studentID = "A16631720";
const char *email = "zuy001@ucsd.edu";

//------------------------------------//
//      Predictor Configuration       //
//------------------------------------//

// Handy Global for use in output routines
const char *bpName[4] = {"Static", "Gshare",
                         "Tournament", "Custom"};

// define number of bits required for indexing the BHT here.
int ghistoryBits = 15; // Number of bits used for Global History
int bpType;            // Branch Prediction Type
int verbose;

//------------------------------------//
//      Predictor Data Structures     //
//------------------------------------//

//
// Tournament
static uint64_t *t_localHistory;   // T_LHT_ENTRIES entries, each holds T_LHT_BITS bits
static uint8_t  *t_localPred;      // T_LPT_ENTRIES of 3-bit counters (stored in uint8_t)
static uint8_t  *t_globalPred;     // T_GPT_ENTRIES of 2-bit counters
static uint8_t  *t_chooser;        // T_CHOOSER_ENTRIES of 2-bit counters
static uint64_t t_ghr = 0;         // global history register
//
// gshare
uint8_t *bht_gshare;
uint64_t ghistory;

//------------------------------------//
//        Predictor Functions         //
//------------------------------------//

// Tournament functions
void init_tournament()
{
  // allocate
  t_localHistory = (uint64_t *)malloc(sizeof(uint64_t) * T_LHT_ENTRIES);
  t_localPred    = (uint8_t  *)malloc(sizeof(uint8_t)  * T_LPT_ENTRIES);
  t_globalPred   = (uint8_t  *)malloc(sizeof(uint8_t)  * T_GPT_ENTRIES);
  t_chooser      = (uint8_t  *)malloc(sizeof(uint8_t)  * T_CHOOSER_ENTRIES);

  if (!t_localHistory || !t_localPred || !t_globalPred || !t_chooser) {
    fprintf(stderr, "Error: tournament predictor malloc failed\n");
    exit(1);
  }

  // initialize
  memset(t_localHistory, 0, sizeof(uint64_t) * T_LHT_ENTRIES);
  for (int i = 0; i < T_LPT_ENTRIES; ++i) t_localPred[i]  = T_LPT_INIT;
  for (int i = 0; i < T_GPT_ENTRIES; ++i) t_globalPred[i] = T_GPT_INIT;
  for (int i = 0; i < T_CHOOSER_ENTRIES; ++i) t_chooser[i]  = T_CHOOSER_INIT;

  t_ghr = 0;
}

uint8_t tournament_predict(uint32_t pc)
{
  // index into local history table using low bits of PC 
  uint32_t lht_index = (pc >> 2) & (T_LHT_ENTRIES - 1);
  uint32_t local_hist = t_localHistory[lht_index];

  // local predictor indexed by local history
  uint32_t local_index = local_hist & (T_LPT_ENTRIES - 1);
  uint8_t local_counter = t_localPred[local_index];
  uint8_t local_taken = (local_counter >= (T_LPT_COUNTER_MAX + 1) / 2); // >=4 taken

  // global predictor indexed by GHR
  uint32_t global_index = t_ghr & (T_GPT_ENTRIES - 1);
  uint8_t global_counter = t_globalPred[global_index];
  uint8_t global_taken = (global_counter >= (T_GPT_COUNTER_MAX + 1) / 2); // >=2 taken

  // chooser selects: smaller values prefer local, larger prefer global
  uint8_t chooser_val = t_chooser[global_index];
  uint8_t prefer_local = (chooser_val < 2); // 0/1 -> local, 2/3 -> global
  static uint32_t branch_count = 0;
  /*if (branch_count < 100) {
    printf("PC=0x%x chooser[%d]=%d prefer_%s local_taken=%d global_taken=%d, local_counter=%d, global_counter=%d\n",
          pc, global_index, chooser_val,
          prefer_local ? "local" : "global",
          local_taken, global_taken, local_counter, global_counter);
  }*/
  branch_count++;
  return prefer_local ? (local_taken ? TAKEN : NOTTAKEN) : (global_taken ? TAKEN : NOTTAKEN);
}

void train_tournament(uint32_t pc, uint8_t outcome)
{
  static uint32_t branch_count = 0;

  // outcome: TAKEN (1) or NOTTAKEN (0)
  uint8_t taken = outcome == TAKEN;

  // local indexes
  uint32_t lht_index = (pc >> 2) & (T_LHT_ENTRIES - 1);
  uint32_t local_hist = t_localHistory[lht_index];
  //printf("PC=0x%x local_hist=0x%x", pc, local_hist);
  uint32_t local_index = local_hist & (T_LPT_ENTRIES - 1);

  // global indexes
  uint32_t global_index = t_ghr & (T_GPT_ENTRIES - 1);

  // current predictions
  uint8_t local_taken = (t_localPred[local_index] >= (T_LPT_COUNTER_MAX + 1) / 2);
  uint8_t global_taken = (t_globalPred[global_index] >= (T_GPT_COUNTER_MAX + 1) / 2);

  // update local predictor (3-bit saturating)
  if (taken)
    INC_SAT8(t_localPred[local_index], T_LPT_COUNTER_MAX);
  else
    DEC_SAT8(t_localPred[local_index], 0);

  // update global predictor (2-bit saturating)
  if (taken)
    INC_SAT8(t_globalPred[global_index], T_GPT_COUNTER_MAX);
  else
    DEC_SAT8(t_globalPred[global_index], 0);

  // update chooser only when local and global disagree
  if (local_taken != global_taken) {
    // if global was correct, move chooser towards global (increment)
    if (global_taken == taken) {
      INC_SAT8(t_chooser[global_index], T_CHOOSER_MAX);
    } else if (local_taken == taken) {
      // if local was correct, move chooser towards local (decrement)
      DEC_SAT8(t_chooser[global_index], 0);
    }
  }
  if (branch_count < 100) {
    /*printf("PC=0x%x outcome=%d localPred[%d]=%d globalPred[%d]=%d chooser[%d]=%d, localHistory[%d]=%d\n",
         pc, outcome, local_index, t_localPred[local_index],
         global_index, t_globalPred[global_index],
         global_index, t_chooser[global_index], lht_index, t_localHistory[lht_index]);
         */
  }
  branch_count++;
  // update local history (per-PC)
  t_localHistory[lht_index] = ((t_localHistory[lht_index] << 1) | taken) & (T_LHT_ENTRIES - 1);

  // update global history
  t_ghr = ((t_ghr << 1) | taken) & (T_GPT_ENTRIES - 1);
}

void cleanup_tournament()
{
  if (t_localHistory) { free(t_localHistory); t_localHistory = NULL; }
  if (t_localPred)    { free(t_localPred);    t_localPred    = NULL; }
  if (t_globalPred)   { free(t_globalPred);   t_globalPred   = NULL; }
  if (t_chooser)      { free(t_chooser);      t_chooser      = NULL; }
}

// gshare functions
void init_gshare()
{
  int bht_entries = 1 << ghistoryBits;
  bht_gshare = (uint8_t *)malloc(bht_entries * sizeof(uint8_t));
  int i = 0;
  for (i = 0; i < bht_entries; i++)
  {
    bht_gshare[i] = WN;
  }
  ghistory = 0;
}

uint8_t gshare_predict(uint32_t pc)
{
  // get lower ghistoryBits of pc
  uint32_t bht_entries = 1 << ghistoryBits;
  uint32_t pc_lower_bits = pc & (bht_entries - 1);
  uint32_t ghistory_lower_bits = ghistory & (bht_entries - 1);
  uint32_t index = pc_lower_bits ^ ghistory_lower_bits;
  switch (bht_gshare[index])
  {
  case WN:
    return NOTTAKEN;
  case SN:
    return NOTTAKEN;
  case WT:
    return TAKEN;
  case ST:
    return TAKEN;
  default:
    printf("Warning: Undefined state of entry in GSHARE BHT!\n");
    return NOTTAKEN;
  }
}

void train_gshare(uint32_t pc, uint8_t outcome)
{
  // get lower ghistoryBits of pc
  uint32_t bht_entries = 1 << ghistoryBits;
  uint32_t pc_lower_bits = pc & (bht_entries - 1);
  uint32_t ghistory_lower_bits = ghistory & (bht_entries - 1);
  uint32_t index = pc_lower_bits ^ ghistory_lower_bits;

  // Update state of entry in bht based on outcome
  switch (bht_gshare[index])
  {
  case WN:
    bht_gshare[index] = (outcome == TAKEN) ? WT : SN;
    break;
  case SN:
    bht_gshare[index] = (outcome == TAKEN) ? WN : SN;
    break;
  case WT:
    bht_gshare[index] = (outcome == TAKEN) ? ST : WN;
    break;
  case ST:
    bht_gshare[index] = (outcome == TAKEN) ? ST : WT;
    break;
  default:
    printf("Warning: Undefined state of entry in GSHARE BHT!\n");
    break;
  }

  // Update history register
  ghistory = ((ghistory << 1) | outcome);
}

void cleanup_gshare()
{
  free(bht_gshare);
}

void init_predictor()
{
  switch (bpType)
  {
  case STATIC:
    break;
  case GSHARE:
    init_gshare();
    break;
  case TOURNAMENT:
    init_tournament();
    break;
  case CUSTOM:
    break;
  default:
    break;
  }
}

// Make a prediction for conditional branch instruction at PC 'pc'
// Returning TAKEN indicates a prediction of taken; returning NOTTAKEN
// indicates a prediction of not taken
//
uint32_t make_prediction(uint32_t pc, uint32_t target, uint32_t direct)
{

  // Make a prediction based on the bpType
  switch (bpType)
  {
  case STATIC:
    return TAKEN;
  case GSHARE:
    return gshare_predict(pc);
  case TOURNAMENT:
    return tournament_predict(pc);
  case CUSTOM:
    return NOTTAKEN;
  default:
    break;
  }

  // If there is not a compatable bpType then return NOTTAKEN
  return NOTTAKEN;
}

// Train the predictor the last executed branch at PC 'pc' and with
// outcome 'outcome' (true indicates that the branch was taken, false
// indicates that the branch was not taken)
//

void train_predictor(uint32_t pc, uint32_t target, uint32_t outcome, uint32_t condition, uint32_t call, uint32_t ret, uint32_t direct)
{
  if (condition)
  {
    switch (bpType)
    {
    case STATIC:
      return;
    case GSHARE:
      return train_gshare(pc, outcome);
    case TOURNAMENT:
      return train_tournament(pc, outcome);
    case CUSTOM:
      return;
    default:
      break;
    }
  }
}
