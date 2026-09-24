/**
 * @file espyak_phoneme_program.c
 * @brief Phoneme program interpreter implementation
 * 
 * Port of Python phoneme_program.py to C for ESP32-P4.
 * Executes context-dependent phoneme transformations.
 */

#include "espyak_phoneme_program.h"
#include "espyak_internal.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <esp_log.h>

/* Maximum recursion depth for ChangePhoneme re-interpretation */
#define MAX_PROGRAM_DEPTH 8

/* Maximum phoneme list entries per word (guards against runaway InsertPhoneme) */
#define MAX_PHONEME_LIST_ENTRIES 512

/* Tag for logging */
static const char* TAG = "espyak_program";

/* Program execution context */
typedef struct {
    const espyak_phoneme_table_t* phoneme_table;
    espyak_phlist_entry_t* plist;
    size_t plist_len;
    size_t max_len;
    bool ipa_only;              ///< Pass 2: only re-evaluate IPA overrides
    int depth;                  ///< Recursion depth guard
    uint32_t insert_done_mask;  ///< Bitmap: already inserted before this index
} program_context_t;

/* Context info for condition evaluation */
typedef struct {
    bool word_end;
    bool first_vowel;
    bool second_vowel;
    bool after_stress;
    bool final_vowel;
    bool max_stress;
    int stress_level;  ///< -1 = no level (consonant before consonant)
} eval_context_t;

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

static bool eval_condition(program_context_t* ctx, size_t i, 
                           const char* condition, eval_context_t* ectx);

/* ========================================================================
 * HELPER FUNCTIONS
 * ======================================================================== */

/**
 * @brief Check if phoneme type matches a category
 */
static bool is_vowel_type(uint8_t type) {
    return type == ESPYAK_PHONEME_VOWEL;
}

static bool is_pause_type(uint8_t type) {
    return type == ESPYAK_PHONEME_PAUSE;
}

/**
 * @brief Build evaluation context for position i
 */
static void build_eval_context(program_context_t* ctx, size_t i, eval_context_t* ectx) {
    memset(ectx, 0, sizeof(*ectx));
    
    if (i >= ctx->plist_len) {
        ectx->word_end = true;
        ectx->stress_level = -1;
        return;
    }
    
    /* Find word boundaries */
    size_t word_start = i;
    while (word_start > 0 && !(ctx->plist[word_start].newword & PHLIST_START_OF_WORD)) {
        word_start--;
    }
    
    size_t word_end = i + 1;
    while (word_end < ctx->plist_len && 
           !(ctx->plist[word_end].newword & PHLIST_START_OF_WORD)) {
        word_end++;
    }
    
    /* Check if at word end */
    ectx->word_end = (i + 1 >= ctx->plist_len) || 
                     (ctx->plist[i + 1].newword & PHLIST_START_OF_WORD);
    
    /* Find vowels in word */
    int vowel_indices[8];
    int num_vowels = 0;
    for (size_t j = word_start; j < word_end && num_vowels < 8; j++) {
        if (is_vowel_type(ctx->plist[j].ph->type)) {
            vowel_indices[num_vowels++] = j;
        }
    }
    
    /* Check vowel positions */
    for (int v = 0; v < num_vowels; v++) {
        if (vowel_indices[v] == (int)i) {
            ectx->first_vowel = (v == 0);
            ectx->second_vowel = (v == 1);
            ectx->final_vowel = (v == num_vowels - 1);
            break;
        }
    }
    
    /* Find max stress in word */
    uint8_t max_stress = 0;
    for (size_t j = word_start; j < word_end; j++) {
        if (is_vowel_type(ctx->plist[j].ph->type)) {
            if ((ctx->plist[j].stresslevel & 0x0F) >= 4) {
                max_stress = ctx->plist[j].stresslevel;
                break;
            }
        }
    }
    ectx->max_stress = (max_stress >= 4);
    
    /* Check if after stress */
    ectx->after_stress = false;
    for (size_t j = word_start; j < i; j++) {
        if (is_vowel_type(ctx->plist[j].ph->type)) {
            if ((ctx->plist[j].stresslevel & 0x0F) >= 4) {
                ectx->after_stress = true;
                break;
            }
        }
    }
    
    /* Stress level for this position */
    if (is_vowel_type(ctx->plist[i].ph->type)) {
        ectx->stress_level = ctx->plist[i].stresslevel & 0x0F;
    } else {
        /* For consonant, take stress from following vowel */
        if (i + 1 < ctx->plist_len && 
            is_vowel_type(ctx->plist[i + 1].ph->type)) {
            ectx->stress_level = ctx->plist[i + 1].stresslevel & 0x0F;
        } else {
            ectx->stress_level = -1;  /* No stress level */
        }
    }
}

/**
 * @brief Get phoneme at relative position (handles word boundaries)
 * 
 * @param ctx Program context
 * @param i Current position
 * @param offset Relative offset (-2, -1, 0, +1, +2)
 * @param check_word_boundary If true, return NULL if crosses word boundary
 * @return Phoneme at position, or NULL if out of bounds/crosses boundary
 */
static const espyak_phoneme_t* get_phoneme_at(
    program_context_t* ctx, 
    size_t i, 
    int offset,
    bool check_word_boundary
) {
    int target_idx = (int)i + offset;
    
    if (target_idx < 0 || target_idx >= (int)ctx->plist_len) {
        return NULL;
    }
    
    if (check_word_boundary) {
        /* Check if we cross a word boundary */
        if (offset > 0) {
            for (int j = (int)i + 1; j <= target_idx; j++) {
                if (ctx->plist[j].newword & PHLIST_START_OF_WORD) {
                    return NULL;  /* Crossed word boundary */
                }
            }
        } else if (offset < 0) {
            for (int j = (int)i; j > target_idx; j--) {
                if (ctx->plist[j].newword & PHLIST_START_OF_WORD) {
                    return NULL;  /* Crossed word boundary */
                }
            }
        }
    }
    
    return ctx->plist[target_idx].ph;
}

/* ========================================================================
 * CONDITION PREDICATES
 * ======================================================================== */

/**
 * @brief Evaluate a single predicate like "isVowel" or "isStressed"
 */
static bool eval_predicate(
    const espyak_phoneme_t* ph,
    const char* pred,
    eval_context_t* ectx
) {
    if (!ph) return false;
    
    /* Type predicates */
    if (strcmp(pred, "isVowel") == 0) {
        return is_vowel_type(ph->type);
    }
    if (strcmp(pred, "isNotVowel") == 0) {
        return !is_vowel_type(ph->type);
    }
    if (strcmp(pred, "isPause") == 0) {
        return is_pause_type(ph->type);
    }
    if (strcmp(pred, "isNasal") == 0) {
        return ph->type == ESPYAK_PHONEME_NASAL;
    }
    if (strcmp(pred, "isLiquid") == 0) {
        return ph->type == ESPYAK_PHONEME_LIQUID;
    }
    if (strcmp(pred, "isRhotic") == 0) {
        return (ph->flags & ESPYAK_PHONEME_FLAG_RHOTIC) != 0;
    }
    
    /* Stress predicates (require ectx) */
    if (strcmp(pred, "isStressed") == 0) {
        return ectx->stress_level > 3;
    }
    if (strcmp(pred, "isNotStressed") == 0) {
        return ectx->stress_level >= 0 && ectx->stress_level < 4;
    }
    if (strcmp(pred, "isUnstressed") == 0) {
        return ectx->stress_level >= 0 && ectx->stress_level <= 1;
    }
    if (strcmp(pred, "isDiminished") == 0) {
        return ectx->stress_level == 0;
    }
    
    /* Position predicates */
    if (strcmp(pred, "isWordEnd") == 0) {
        return ectx->word_end;
    }
    if (strcmp(pred, "isFirstVowel") == 0) {
        return ectx->first_vowel;
    }
    if (strcmp(pred, "isSecondVowel") == 0) {
        return ectx->second_vowel;
    }
    if (strcmp(pred, "isFinalVowel") == 0) {
        return ectx->final_vowel;
    }
    if (strcmp(pred, "isAfterStress") == 0) {
        return ectx->after_stress;
    }
    if (strcmp(pred, "isMaxStress") == 0) {
        return ectx->max_stress;
    }
    
    /* Unknown predicate */
    ESP_LOGW(TAG, "Unknown predicate: %s", pred);
    return false;
}

/**
 * @brief Parse and evaluate "nextPh(isVowel)" or "prevPh(isNotVowel)" etc.
 * 
 * Format: "whichPh(predicate)" where which = next|prev|next2|prev2|nextPhW|prevPhW
 */
static bool eval_phoneme_condition(
    program_context_t* ctx,
    size_t i,
    const char* condition,
    eval_context_t* ectx
) {
    char which[16] = {0};
    char predicate[32] = {0};
    
    /* Parse "whichPh(predicate)" */
    const char* paren = strchr(condition, '(');
    if (!paren) return false;
    
    size_t which_len = paren - condition;
    if (which_len >= sizeof(which)) return false;
    strncpy(which, condition, which_len);
    
    const char* pred_start = paren + 1;
    const char* pred_end = strchr(pred_start, ')');
    if (!pred_end) return false;
    
    size_t pred_len = pred_end - pred_start;
    if (pred_len >= sizeof(predicate)) return false;
    strncpy(predicate, pred_start, pred_len);
    
    /* Determine offset and word boundary check */
    int offset = 0;
    bool check_boundary = false;
    
    if (strcmp(which, "thisPh") == 0) {
        offset = 0;
    } else if (strcmp(which, "nextPh") == 0) {
        offset = 1;
    } else if (strcmp(which, "prevPh") == 0) {
        offset = -1;
    } else if (strcmp(which, "next2Ph") == 0) {
        offset = 2;
    } else if (strcmp(which, "prev2Ph") == 0) {
        offset = -2;
    } else if (strcmp(which, "nextPhW") == 0) {
        offset = 1;
        check_boundary = true;
    } else if (strcmp(which, "prevPhW") == 0) {
        offset = -1;
        check_boundary = true;
    } else {
        ESP_LOGW(TAG, "Unknown which-phoneme: %s", which);
        return false;
    }
    
    /* Get target phoneme */
    const espyak_phoneme_t* target = get_phoneme_at(ctx, i, offset, check_boundary);
    if (!target) {
        return false;  /* Out of bounds or crossed boundary */
    }
    
    /* Build context for target position if needed */
    eval_context_t target_ectx;
    if (offset != 0) {
        build_eval_context(ctx, i + offset, &target_ectx);
        ectx = &target_ectx;
    }
    
    return eval_predicate(target, predicate, ectx);
}

/**
 * @brief Evaluate condition string with AND/OR/NOT
 * 
 * Left-to-right evaluation, no precedence.
 * Example: "nextPh(isVowel) AND NOT prevPh(isPause)"
 */
static bool eval_condition(
    program_context_t* ctx,
    size_t i,
    const char* condition,
    eval_context_t* ectx
) {
    if (!condition || !condition[0]) {
        return true;  /* Empty condition = always true */
    }
    
    /* Simple implementation: parse left-to-right */
    /* TODO: Full parser for complex conditions */
    
    /* For now, handle simple cases */
    if (strstr(condition, "(")) {
        /* Phoneme condition like "nextPh(isNotVowel)" */
        return eval_phoneme_condition(ctx, i, condition, ectx);
    }
    
    /* Direct predicate */
    return eval_predicate(ctx->plist[i].ph, condition, ectx);
}

/* To be continued... */

/* ========================================================================
 * PROGRAM EXECUTION - ChangePhoneme, InsertPhoneme
 * ======================================================================== */

/**
 * @brief Execute ChangePhoneme(mnemonic) instruction
 * 
 * Changes current phoneme to specified variant. Only fires once per phoneme
 * per pass (guarded by _changed flag).
 */
static bool execute_change_phoneme(
    program_context_t* ctx,
    size_t i,
    const char* target_mnemonic
) {
    if (ctx->plist[i]._changed) {
        /* Already changed this phoneme in this pass */
        return false;
    }
    
    /* ChangePhoneme(NULL) means delete */
    if (strcmp(target_mnemonic, "NULL") == 0) {
        ctx->plist[i].deleted = true;
        ctx->plist[i]._changed = true;
        return true;
    }
    
    /* Look up target phoneme */
    const espyak_phoneme_t* target_ph = espyak_phoneme_lookup(
        ctx->phoneme_table, 
        target_mnemonic
    );
    
    if (!target_ph) {
        ESP_LOGW(TAG, "ChangePhoneme: phoneme '%s' not found", target_mnemonic);
        return false;
    }
    
    /* Change the phoneme */
    ctx->plist[i].ph = target_ph;
    ctx->plist[i]._changed = true;
    ctx->plist[i].ipa_override[0] = '\0';  /* Clear IPA override */
    
    /* Update SFLAG_SYLLABLE based on new type */
    if (is_vowel_type(target_ph->type)) {
        ctx->plist[i].synthflags |= SFLAG_SYLLABLE;
    } else {
        ctx->plist[i].synthflags &= ~SFLAG_SYLLABLE;
    }
    
    /* TODO: Re-run new phoneme's program (with depth guard) */
    
    ESP_LOGD(TAG, "ChangePhoneme(%s) at pos %d", target_mnemonic, (int)i);
    return true;
}

/**
 * @brief Execute InsertPhoneme(mnemonic) instruction
 * 
 * Inserts phoneme BEFORE current position. Transfers word-start marker
 * and stress from following vowel.
 */
static bool execute_insert_phoneme(
    program_context_t* ctx,
    size_t i,
    const char* target_mnemonic
) {
    /* Check if already inserted at this position */
    if (ctx->insert_done_mask & (1u << (i % 32))) {
        return false;  /* Already inserted here */
    }
    
    /* Check capacity */
    if (ctx->plist_len >= ctx->max_len) {
        ESP_LOGE(TAG, "InsertPhoneme: phoneme list full");
        return false;
    }
    
    /* Look up phoneme to insert */
    const espyak_phoneme_t* target_ph = espyak_phoneme_lookup(
        ctx->phoneme_table,
        target_mnemonic
    );
    
    if (!target_ph) {
        ESP_LOGW(TAG, "InsertPhoneme: phoneme '%s' not found", target_mnemonic);
        return false;
    }
    
    /* Shift everything right */
    for (size_t j = ctx->plist_len; j > i; j--) {
        ctx->plist[j] = ctx->plist[j - 1];
    }
    ctx->plist_len++;
    
    /* Insert new entry */
    memset(&ctx->plist[i], 0, sizeof(espyak_phlist_entry_t));
    ctx->plist[i].ph = target_ph;
    ctx->plist[i].stresslevel = STRESS_UNSTRESSED;
    ctx->plist[i].synthflags = 0;
    ctx->plist[i].newword = 0;
    
    /* Transfer word-start marker from next phoneme */
    if (ctx->plist[i + 1].newword & PHLIST_START_OF_WORD) {
        ctx->plist[i].newword = ctx->plist[i + 1].newword;
        ctx->plist[i + 1].newword = 0;
    }
    
    /* Transfer stress if next is vowel */
    if (is_vowel_type(ctx->plist[i + 1].ph->type)) {
        ctx->plist[i].stresslevel = ctx->plist[i + 1].stresslevel;
        ctx->plist[i + 1].stresslevel = STRESS_UNSTRESSED;
    }
    
    /* Mark as inserted */
    ctx->insert_done_mask |= (1u << (i % 32));
    
    ESP_LOGD(TAG, "InsertPhoneme(%s) at pos %d", target_mnemonic, (int)i);
    return true;
}

/**
 * @brief Parse instruction and execute
 * 
 * Handles: ChangePhoneme(X), InsertPhoneme(X), ipa XXXX
 */
static bool execute_instruction(
    program_context_t* ctx,
    size_t i,
    const char* instruction
) {
    /* Skip empty lines and comments */
    while (*instruction && isspace(*instruction)) instruction++;
    if (!*instruction || *instruction == '#') {
        return false;
    }
    
    /* ChangePhoneme(X) */
    if (strncmp(instruction, "ChangePhoneme(", 14) == 0) {
        if (ctx->ipa_only) return false;  /* Pass 2: skip structural changes */
        
        const char* start = instruction + 14;
        const char* end = strchr(start, ')');
        if (!end) return false;
        
        char mnemonic[16];
        size_t len = end - start;
        if (len >= sizeof(mnemonic)) return false;
        strncpy(mnemonic, start, len);
        mnemonic[len] = '\0';
        
        return execute_change_phoneme(ctx, i, mnemonic);
    }
    
    /* InsertPhoneme(X) */
    if (strncmp(instruction, "InsertPhoneme(", 14) == 0) {
        if (ctx->ipa_only) return false;  /* Pass 2: skip structural changes */
        
        const char* start = instruction + 14;
        const char* end = strchr(start, ')');
        if (!end) return false;
        
        char mnemonic[16];
        size_t len = end - start;
        if (len >= sizeof(mnemonic)) return false;
        strncpy(mnemonic, start, len);
        mnemonic[len] = '\0';
        
        return execute_insert_phoneme(ctx, i, mnemonic);
    }
    
    /* ipa XXXX (conditional IPA override) */
    if (strncmp(instruction, "ipa ", 4) == 0) {
        const char* ipa_str = instruction + 4;
        /* Skip leading whitespace */
        while (*ipa_str && isspace(*ipa_str)) ipa_str++;
        
        /* Handle "ipa NULL" (no IPA output) */
        if (strcmp(ipa_str, "NULL") == 0) {
            ctx->plist[i].ipa_override[0] = '\0';
        } else {
            strncpy(ctx->plist[i].ipa_override, ipa_str, 
                   sizeof(ctx->plist[i].ipa_override) - 1);
            ctx->plist[i].ipa_override[sizeof(ctx->plist[i].ipa_override) - 1] = '\0';
        }
        
        ESP_LOGD(TAG, "ipa override: %s", ctx->plist[i].ipa_override);
        return true;
    }
    
    return false;
}

/**
 * @brief Execute phoneme program for position i
 * 
 * Parses raw program text (ph->program) line-by-line and executes instructions.
 * Uses static buffer to avoid stack overflow (ESP32 constraint: 256 bytes max).
 */
static bool execute_program_for_phoneme(
    program_context_t* ctx,
    size_t i
) {
    const espyak_phoneme_t* ph = ctx->plist[i].ph;
    
    /* Check if phoneme has a program */
    if (!ph->program || !ph->program[0]) {
        return false;
    }
    
    ESP_LOGD(TAG, "Execute program for phoneme '%s' at pos %d", 
             ph->mnemonic, (int)i);
    
    /* Parse program text line-by-line (static buffer for stack safety) */
    static char line_buf[256];
    const char* prog = ph->program;
    size_t prog_len = strlen(prog);
    size_t line_start = 0;
    
    while (line_start < prog_len) {
        /* Find line end */
        size_t line_end = line_start;
        while (line_end < prog_len && prog[line_end] != '\n') {
            line_end++;
        }
        
        /* Extract line */
        size_t line_len = line_end - line_start;
        if (line_len >= sizeof(line_buf)) {
            line_len = sizeof(line_buf) - 1;
        }
        
        memcpy(line_buf, &prog[line_start], line_len);
        line_buf[line_len] = '\0';
        
        /* Trim leading/trailing whitespace */
        char* line = line_buf;
        while (*line && isspace(*line)) line++;
        
        char* end = line + strlen(line) - 1;
        while (end > line && isspace(*end)) *end-- = '\0';
        
        /* Skip empty lines and comments */
        if (*line && *line != '#') {
            /* IF block - needs special handling for multi-line */
            if (strncmp(line, "IF ", 3) == 0) {
                /* For MVP: Simple IF without ELIF/ELSE */
                /* Parse condition and execute inline THEN instructions */
                
                /* Extract condition between "IF" and "THEN" */
                const char* cond_start = line + 3;
                const char* then_pos = strstr(cond_start, "THEN");
                
                if (then_pos) {
                    char condition[128];
                    size_t cond_len = then_pos - cond_start;
                    if (cond_len < sizeof(condition)) {
                        strncpy(condition, cond_start, cond_len);
                        condition[cond_len] = '\0';
                        
                        /* Trim condition */
                        char* p = condition + strlen(condition) - 1;
                        while (p >= condition && isspace(*p)) *p-- = '\0';
                        
                        /* Evaluate condition */
                        eval_context_t ectx;
                        build_eval_context(ctx, i, &ectx);
                        bool cond_result = eval_condition(ctx, i, condition, &ectx);
                        
                        ESP_LOGD(TAG, "IF %s => %s", condition, cond_result ? "true" : "false");
                        
                        /* Find and execute THEN body until ENDIF */
                        if (cond_result) {
                            line_start = line_end + 1;
                            
                            while (line_start < prog_len) {
                                /* Get next line */
                                line_end = line_start;
                                while (line_end < prog_len && prog[line_end] != '\n') {
                                    line_end++;
                                }
                                
                                line_len = line_end - line_start;
                                if (line_len < sizeof(line_buf)) {
                                    memcpy(line_buf, &prog[line_start], line_len);
                                    line_buf[line_len] = '\0';
                                    
                                    line = line_buf;
                                    while (*line && isspace(*line)) line++;
                                    
                                    if (strncmp(line, "ENDIF", 5) == 0) {
                                        break;
                                    }
                                    
                                    if (*line && *line != '#') {
                                        execute_instruction(ctx, i, line);
                                    }
                                }
                                
                                line_start = line_end + 1;
                            }
                        } else {
                            /* Skip to ENDIF */
                            while (line_start < prog_len) {
                                line_end = line_start;
                                while (line_end < prog_len && prog[line_end] != '\n') {
                                    line_end++;
                                }
                                
                                line_len = line_end - line_start;
                                if (line_len < sizeof(line_buf)) {
                                    memcpy(line_buf, &prog[line_start], line_len);
                                    line_buf[line_len] = '\0';
                                    
                                    line = line_buf;
                                    while (*line && isspace(*line)) line++;
                                    
                                    if (strncmp(line, "ENDIF", 5) == 0) {
                                        break;
                                    }
                                }
                                
                                line_start = line_end + 1;
                            }
                        }
                    }
                }
            } else {
                /* Direct instruction */
                execute_instruction(ctx, i, line);
            }
        }
        
        /* Move to next line */
        line_start = line_end + 1;
    }
    
    return false;
}

/* ========================================================================
 * PUBLIC API
 * ======================================================================== */

espyak_err_t espyak_phoneme_program_init(const espyak_phoneme_table_t* phoneme_table) {
    /* Nothing to initialize for now */
    (void)phoneme_table;
    return ESPYAK_OK;
}

espyak_err_t espyak_phoneme_program_run(
    const espyak_phoneme_table_t* phoneme_table,
    espyak_phlist_entry_t* plist,
    size_t* plist_len,
    size_t max_len
) {
    if (!phoneme_table || !plist || !plist_len) {
        return ESPYAK_ERR_INVALID_ARG;
    }
    
    program_context_t ctx = {
        .phoneme_table = phoneme_table,
        .plist = plist,
        .plist_len = *plist_len,
        .max_len = max_len,
        .ipa_only = false,
        .depth = 0,
        .insert_done_mask = 0
    };
    
    ESP_LOGD(TAG, "=== Pass 1: Structural changes ===");
    
    /* Pass 1: Apply ChangePhoneme/InsertPhoneme */
    size_t i = 0;
    while (i < ctx.plist_len) {
        /* Clear changed flag for this pass */
        ctx.plist[i]._changed = false;
        
        execute_program_for_phoneme(&ctx, i);
        
        i++;
    }
    
    ESP_LOGD(TAG, "=== Pass 2: IPA overrides ===");
    
    /* Pass 2: Re-evaluate IPA overrides */
    ctx.ipa_only = true;
    for (i = 0; i < ctx.plist_len; i++) {
        execute_program_for_phoneme(&ctx, i);
    }
    
    /* Update output length */
    *plist_len = ctx.plist_len;
    
    ESP_LOGD(TAG, "Program execution complete: %d phonemes", (int)*plist_len);
    
    return ESPYAK_OK;
}

void espyak_phoneme_program_compact(
    espyak_phlist_entry_t* plist,
    size_t* plist_len
) {
    if (!plist || !plist_len) return;
    
    size_t write_idx = 0;
    for (size_t read_idx = 0; read_idx < *plist_len; read_idx++) {
        if (!plist[read_idx].deleted) {
            if (write_idx != read_idx) {
                plist[write_idx] = plist[read_idx];
            }
            write_idx++;
        }
    }
    
    *plist_len = write_idx;
    ESP_LOGD(TAG, "Compacted to %d phonemes", (int)*plist_len);
}

void espyak_phoneme_program_deinit(const espyak_phoneme_table_t* phoneme_table) {
    /* Nothing to clean up for now */
    (void)phoneme_table;
}
