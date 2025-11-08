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
#define T_LHT_ENTRIES (1UL << T_LHT_BITS) 
#define T_LPT_ENTRIES (1UL << T_LHT_BITS) // index by local history 
#define T_LPT_COUNTER_MAX 7           // 3-bit saturating counter
#define T_LPT_INIT 1

#define T_GHR_BITS   13               // global history bits
#define T_GPT_ENTRIES (1UL << T_GHR_BITS) 
#define T_GPT_COUNTER_MAX 3           // 2-bit saturating counter
#define T_GPT_INIT 1

#define T_CHOOSER_ENTRIES (1UL << T_GHR_BITS) 
#define T_CHOOSER_MAX 3           // 2-bit saturating counter 
#define T_CHOOSER_INIT 1

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

// Helpfer macros
// Prediction
#define PREDICT2b(counter, pred)   switch (counter) \
  { \
  case WN: \
    pred = NOTTAKEN; \
    break; \
  case SN: \
    pred = NOTTAKEN; \
    break; \
  case WT: \
    pred = TAKEN; \
  case ST: \
    pred = TAKEN; \
    break; \
  default: \
    printf("Warning: Undefined state of entry in 2 bit BHT!\n"); \
    pred = NOTTAKEN; \
    break; \
  }

#define PREDICT3b(counter, pred)   switch (counter) \
  { \
  case SSSN: \
    pred = NOTTAKEN; \
    break; \
  case SSN: \
    pred = NOTTAKEN; \
    break; \
  case NS: \
    pred = NOTTAKEN; \
    break; \
  case NW: \
    pred = TAKEN; \
    break; \
  case TW: \
    pred = TAKEN; \
    break; \
  case TS: \
    pred = TAKEN; \
    break; \
  case SST: \
    pred = TAKEN; \
    break; \
  case SSST: \
    pred = TAKEN; \
    break; \
  default: \
    printf("Warning: Undefined state of entry in 3 bit BHT!\n"); \
    pred = NOTTAKEN; \
    break; \
  }

#define COUNTERUPDATE2b(counter, outcome)  switch (counter) \
  { \
  case WN: \
    counter = (outcome == TAKEN) ? WT : SN; \
    break; \
  case SN: \
    counter = (outcome == TAKEN) ? WN : SN; \
    break; \
  case WT: \
    counter = (outcome == TAKEN) ? ST : WN; \
    break; \
  case ST: \
    counter = (outcome == TAKEN) ? ST : WT; \
    break; \
  default: \
    printf("Warning: Undefined state of entry in 2 bit BHT!\n"); \
    break; \
  }

#define COUNTERUPDATE3b(counter, outcome)  switch (counter) \
  { \
  case SSSN: \
    counter = (outcome == TAKEN) ? SSN : SSSN; \
    break; \
  case SSN: \
    counter = (outcome == TAKEN) ? NS : SSSN; \
    break; \
  case NS: \
    counter = (outcome == TAKEN) ? NW : SSN; \
    break; \
  case NW: \
    counter = (outcome == TAKEN) ? TW : NS; \
    break; \
  case TW: \
    counter = (outcome == TAKEN) ? TS : NW; \
    break; \
  case TS: \
    counter = (outcome == TAKEN) ? SST : TW; \
    break; \
  case SST: \
    counter = (outcome == TAKEN) ? SSST : TS; \
    break; \
  case SSST: \
    counter = (outcome == TAKEN) ? SSST : SST; \
    break; \
  default: \
    printf("Warning: Undefined state of entry in 3 bit BHT!\n"); \
    break; \
  }
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
  uint32_t lht_index = pc & (T_LHT_ENTRIES - 1);
  uint32_t local_hist = t_localHistory[lht_index];

  // local predictor indexed by local history
  uint32_t local_index = local_hist & (T_LPT_ENTRIES - 1);
  uint8_t local_counter = t_localPred[local_index];
  uint8_t local_taken;
  PREDICT3b(local_counter, local_taken);

  // global predictor indexed by GHR
  uint32_t global_index = t_ghr & (T_GPT_ENTRIES - 1);
  uint8_t global_counter = t_globalPred[global_index];
  uint8_t global_taken;
  PREDICT2b(global_counter, global_taken);

  // chooser selects: smaller values prefer local, larger prefer global
  uint8_t chooser_val = t_chooser[global_index];
  uint8_t prefer_global;
  PREDICT2b(chooser_val, prefer_global);
  return prefer_global ? global_taken : local_taken;
}

void train_tournament(uint32_t pc, uint8_t outcome)
{
  // local indexes
  uint32_t lht_index = pc & (T_LHT_ENTRIES - 1);
  uint32_t local_hist = t_localHistory[lht_index];
  uint32_t local_index = local_hist & (T_LPT_ENTRIES - 1);

  // global indexes
  uint32_t global_index = t_ghr & (T_GPT_ENTRIES - 1);

  // current predictions
  uint8_t local_taken;
  PREDICT3b(t_localPred[local_index], local_taken);
  uint8_t global_taken;
  PREDICT2b(t_globalPred[global_index], global_taken);
  // update local predictor (3-bit saturating)
  COUNTERUPDATE3b(t_localPred[local_index], outcome);
  // update global predictor (2-bit saturating)
  COUNTERUPDATE2b(t_globalPred[global_index], outcome);
  // update chooser only when local and global disagree
  if (local_taken != global_taken) {
    // if global was correct, move chooser towards global (increment)
    if (global_taken == outcome) {
      COUNTERUPDATE2b(t_chooser[global_index], TAKEN);
    } else if (local_taken == outcome) {
      // if local was correct, move chooser towards local (decrement)
      COUNTERUPDATE2b(t_chooser[global_index], NOTTAKEN);
    }
  }
  // update local history (per-PC)
  t_localHistory[lht_index] = ((t_localHistory[lht_index] << 1) | outcome) & (T_LHT_ENTRIES - 1);

  // update global history
  t_ghr = ((t_ghr << 1) | outcome) & (T_GPT_ENTRIES - 1);
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
