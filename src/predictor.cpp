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

// -------------------- TAGE predictor configuration --------------------
#define TAGE_BIMODAL_BITS 15                // bimodal size = 16K
#define TAGE_BIMODAL_SIZE (1 << TAGE_BIMODAL_BITS)
#define TAGE_NUM_TAGGED 7                   // number of tagged components
#define TAGE_TAGGED_BITS 12                 // each table has 4K entries (4096)
#define TAGE_TAGGED_SIZE (1 << TAGE_TAGGED_BITS)
#define TAGE_TAG_BITS TAGE_TAGGED_BITS - 5
#define TAGE_CTR_MAX 3                      // 3-bit signed-like counter: 0..7 used as saturating
#define TAGE_CTR_INIT 4
#define TAGE_U_MAX 3                        // useful counter 0..3
#define TAGE_U_INIT 0

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
//
// Custom
// history lengths (geometric growth)
//static const int tage_hist_lengths[TAGE_NUM_TAGGED] = {4, 10, 20, 60}; // example lengths
static const int tage_hist_lengths[TAGE_NUM_TAGGED] = {4, 8, 16, 32, 64, 128, 256}; // lengths for 15-bit ghist

// Data structures
typedef struct {
  uint16_t tag;    // truncated tag (TAG_BITS can be up to 16 here)
  uint8_t ctr;     // prediction counter (3-bit using 0..7)
  uint8_t u;       // useful counter (0..TAGE_U_MAX)
} tage_entry_t;

static uint8_t *tage_bimodal;                      // bimodal base table (2-bit counters)
static tage_entry_t **tage_tables;                 // array of pointers to tagged tables
static uint64_t tage_ghist = 0;                    // global history (kept wide)
static int tage_hist_mask = 0;

// helper: get lower N bits of history (we store ghist as bits LSB = most recent)
static inline uint32_t get_hist_bits(uint64_t hist, int len) {
  if (len >= 64) return (uint32_t)(hist & (~0ULL));
  return (uint32_t)(hist & ((1ULL << len) - 1));
}

// index function: combine pc and history
static inline uint32_t tage_index(uint32_t pc, uint64_t hist, int table) {
  uint32_t h = get_hist_bits(hist, tage_hist_lengths[table]);
  // simple mix: XOR pc shifted with history and table id
  uint32_t idx = (pc ^ (h * 0x9e3779b9u) ^ (table * 0xabcdefu)) & (TAGE_TAGGED_SIZE - 1);
  return idx;
}

// tag function: truncated tag
static inline uint16_t tage_tag(uint32_t pc, uint64_t hist, int table) {
  uint32_t h = get_hist_bits(hist, tage_hist_lengths[table]);
  uint32_t tag = (pc ^ (h >> (table + 1))) & ((1u << TAGE_TAG_BITS) - 1);
  return (uint16_t)tag;
}

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
    pred = NOTTAKEN; \
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

void init_tage()
{
  // allocate bimodal (2-bit saturating counters), init to weakly taken (WT)
  tage_bimodal = (uint8_t *)malloc(TAGE_BIMODAL_SIZE * sizeof(uint8_t));
  if (!tage_bimodal) { fprintf(stderr, "TAGE: bimodal malloc failed\n"); exit(1); }
  for (int i = 0; i < TAGE_BIMODAL_SIZE; ++i) tage_bimodal[i] = WT;

  // allocate tagged tables
  tage_tables = (tage_entry_t **)malloc(TAGE_NUM_TAGGED * sizeof(tage_entry_t *));
  if (!tage_tables) { fprintf(stderr, "TAGE: tables malloc failed\n"); exit(1); }
  for (int t = 0; t < TAGE_NUM_TAGGED; ++t) {
    tage_tables[t] = (tage_entry_t *)malloc(TAGE_TAGGED_SIZE * sizeof(tage_entry_t));
    if (!tage_tables[t]) { fprintf(stderr, "TAGE: table[%d] malloc failed\n", t); exit(1); }
    // initialize entries
    for (int i = 0; i < TAGE_TAGGED_SIZE; ++i) {
      tage_tables[t][i].tag = 0xFFFFu;         // invalid tag
      tage_tables[t][i].ctr = TAGE_CTR_INIT;   // weakly taken
      tage_tables[t][i].u = TAGE_U_INIT;
    }
  }

  tage_ghist = 0;
  tage_hist_mask = (1 << ghistoryBits) - 1; // reuse ghistoryBits as global history register width
}

uint8_t tage_predict(uint32_t pc)
{
  // bimodal index
  uint32_t bim_idx = pc & (TAGE_BIMODAL_SIZE - 1);
  uint8_t bim_pred;
  PREDICT2b(tage_bimodal[bim_idx], bim_pred);

  int provider = -1;
  int alt = -1;
  uint8_t provider_pred = bim_pred;
  uint8_t alt_pred = bim_pred;

  // search from longest history (highest table index) to shortest
  for (int t = TAGE_NUM_TAGGED - 1; t >= 0; --t) {
    uint32_t idx = tage_index(pc, tage_ghist, t);
    uint16_t tag = tage_tag(pc, tage_ghist, t);
    tage_entry_t *e = &tage_tables[t][idx];
    if (e->tag == tag) {
      uint8_t pred;
      PREDICT3b(e->ctr, pred);
      if (provider == -1) {
        provider = t;
        provider_pred = pred;
      } else if (alt == -1) {
        alt = t;
        alt_pred = pred;
      }
    }
  }

  // if we have a provider, choose it; else use bimodal
  if (provider != -1) return provider_pred;
  return bim_pred;
}

void train_tage(uint32_t pc, uint8_t outcome)
{
  // bimodal index
  uint32_t bim_idx = pc & (TAGE_BIMODAL_SIZE - 1);
  uint8_t bim_pred;
  PREDICT2b(tage_bimodal[bim_idx], bim_pred);

  int provider = -1;
  int alt = -1;
  uint8_t provider_pred = bim_pred;
  uint8_t alt_pred = bim_pred;

  for (int t = TAGE_NUM_TAGGED - 1; t >= 0; --t) {
    uint32_t idx = tage_index(pc, tage_ghist, t);
    uint16_t tag = tage_tag(pc, tage_ghist, t);
    tage_entry_t *e = &tage_tables[t][idx];
    if (e->tag == tag) {
      uint8_t pred;
      PREDICT3b(e->ctr, pred);
      if (provider == -1) {
        provider = t;
        provider_pred = pred;
      } else if (alt == -1) {
        alt = t;
        alt_pred = pred;
      }
    }
  }

  // if provider exists, update its counter
  if (provider != -1) {
    uint32_t idx = tage_index(pc, tage_ghist, provider);
    tage_entry_t *e = &tage_tables[provider][idx];
    // update 3-bit saturating-like counter: keep in 0..7
    COUNTERUPDATE3b(e->ctr, outcome);
    // update useful bit: if provider predicted correctly and alternate predicted incorrectly, increase u
    if (provider_pred == outcome && alt != -1 && alt_pred != outcome) {
      COUNTERUPDATE2b(e->u, TAKEN);
    }
    // if provider wrong but alt correct, decrement u
    if (provider_pred != outcome && alt != -1 && alt_pred == outcome) {
      COUNTERUPDATE2b(e->u, NOTTAKEN);
    }
  } else {
    // no provider: update bimodal only for now
    COUNTERUPDATE2b(tage_bimodal[bim_idx], outcome);
  }
  // If provider did not exist and prediction was incorrect, allocate in a low-utility entry (simple allocation)
  if (provider == -1) {
    // try to allocate in one of the higher tables where an entry has u==0
    for (int t = 0; t < TAGE_NUM_TAGGED; ++t) {
      uint32_t idx = tage_index(pc, tage_ghist, t);
      tage_entry_t *e = &tage_tables[t][idx];
      if (e->u == 0) {
        // allocate
        e->tag = tage_tag(pc, tage_ghist, t);
        e->ctr = TAGE_CTR_INIT + (outcome ? 1 : -1); // bias toward outcome
        if (e->ctr > 7) e->ctr = 7;
        if (e->ctr < 0) e->ctr = 0;
        e->u = 0;
        break;
      } else {
        // decay usefulness slowly
        if (e->u > 0 && (rand() & 0x3F) == 0) e->u--;
      }
    }
  }

  // If provider existed, also update alternate or bimodal sometimes (helpful fallback)
  if (provider != -1 && alt == -1) {
    // update bimodal as alternate
    if (outcome == TAKEN) {
      if (tage_bimodal[bim_idx] < ST) tage_bimodal[bim_idx]++;
    } else {
      if (tage_bimodal[bim_idx] > SN) tage_bimodal[bim_idx]--;
    }
  }

  // update global history (keep width limited by ghistoryBits)
  tage_ghist = ((tage_ghist << 1) | (outcome & 1)) & ((1ULL << ghistoryBits) - 1);
}

// cleanup
void cleanup_tage()
{
  if (tage_bimodal) { free(tage_bimodal); tage_bimodal = NULL; }
  if (tage_tables) {
    for (int t = 0; t < TAGE_NUM_TAGGED; ++t) {
      if (tage_tables[t]) { free(tage_tables[t]); tage_tables[t] = NULL; }
    }
    free(tage_tables); tage_tables = NULL;
  }
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
  init_tage();
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
    return tage_predict(pc);
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
      return train_tage(pc, outcome);
    default:
      break;
    }
  }
}
