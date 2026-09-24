/**
 * @file espyak_rules.c
 * @brief Simplified rule matching engine for ESP32
 * 
 * This is a minimal implementation focusing on basic letter-to-phoneme rules.
 * Full espeak-ng rule engine (with conditions, letter groups, scoring) is deferred
 * to Phase 6 optimization.
 * 
 * Design: Simple longest-match algorithm with hardcoded English rules.
 */

#include "espyak_internal.h"
#include <string.h>
#include <ctype.h>
#include <esp_log.h>

static const char* TAG = "espyak_rules";

/**
 * Simple rule: match → phoneme
 */
typedef struct {
    const char* match;      // What to match (lowercase)
    const char* phoneme;    // Output phoneme mnemonic
    uint8_t priority;       // Higher = more specific (for tie-breaking)
} simple_rule_t;

/**
 * Basic English pronunciation rules (most common patterns)
 * 
 * Priority guide:
 *   1 = single letter (fallback)
 *   2 = digraph (2 letters)
 *   3 = trigraph (3 letters)
 *   4 = special context
 */
static const simple_rule_t ENGLISH_RULES[] = {
    // Common digraphs (priority 2)
    {"th", "T", 2},      // thin
    {"sh", "S", 2},      // ship
    {"ch", "tS", 2},     // chip
    {"ph", "f", 2},      // phone
    {"wh", "w", 2},      // what
    {"ck", "k", 2},      // back
    {"ng", "N", 2},      // sing
    
    // Common trigraphs (priority 3)
    {"tch", "tS", 3},    // watch
    {"sch", "sk", 3},    // school
    
    // Vowels (priority 1-2)
    {"ee", "i:", 2},     // see
    {"oo", "u:", 2},     // food
    {"ea", "i:", 2},     // eat
    {"ou", "aU", 2},     // out
    {"ow", "aU", 2},     // how
    {"ay", "eI", 2},     // say
    {"ai", "eI", 2},     // rain
    {"oy", "OI", 2},     // boy
    {"oi", "OI", 2},     // coin
    {"aw", "O:", 2},     // saw
    {"au", "O:", 2},     // caught
    
    // Single consonants (priority 1)
    {"b", "b", 1},
    {"c", "k", 1},       // cat (default, 's' context handled elsewhere)
    {"d", "d", 1},
    {"f", "f", 1},
    {"g", "g", 1},       // go (default, 'dZ' context handled elsewhere)
    {"h", "h", 1},
    {"j", "dZ", 1},      // jump
    {"k", "k", 1},
    {"l", "l", 1},
    {"m", "m", 1},
    {"n", "n", 1},
    {"p", "p", 1},
    {"q", "kw", 1},      // queen
    {"r", "r", 1},
    {"s", "s", 1},
    {"t", "t", 1},
    {"v", "v", 1},
    {"w", "w", 1},
    {"x", "ks", 1},      // box
    {"y", "j", 1},       // yes
    {"z", "z", 1},
    
    // Single vowels (priority 1)
    {"a", "@", 1},       // about (schwa, most common)
    {"e", "e", 1},       // bed
    {"i", "I", 1},       // bit
    {"o", "o", 1},       // on
    {"u", "V", 1},       // but
};

#define NUM_RULES (sizeof(ENGLISH_RULES) / sizeof(ENGLISH_RULES[0]))

/**
 * Match rules against input text at given position
 * 
 * Uses longest-match algorithm:
 * 1. Try all rules at current position
 * 2. Keep track of longest match
 * 3. Return best match (highest priority if same length)
 * 
 * @param text Input text (lowercase)
 * @param pos Current position
 * @param out_phoneme Output phoneme buffer
 * @param out_len Output buffer size
 * @return Number of characters consumed (0 if no match)
 */
size_t espyak_match_rule(const char* text, size_t pos, 
                         char* out_phoneme, size_t out_len) {
    if (!text || !out_phoneme || out_len == 0) {
        return 0;
    }
    
    const char* input = &text[pos];
    size_t input_len = strlen(input);
    
    const simple_rule_t* best_rule = NULL;
    size_t best_len = 0;
    
    // Try each rule
    for (size_t i = 0; i < NUM_RULES; i++) {
        const simple_rule_t* rule = &ENGLISH_RULES[i];
        size_t match_len = strlen(rule->match);
        
        // Check if rule matches at current position
        if (match_len <= input_len && 
            strncmp(input, rule->match, match_len) == 0) {
            
            // Longest match wins, or higher priority if same length
            if (match_len > best_len || 
                (match_len == best_len && rule->priority > best_rule->priority)) {
                best_rule = rule;
                best_len = match_len;
            }
        }
    }
    
    // Apply best match
    if (best_rule) {
        strncpy(out_phoneme, best_rule->phoneme, out_len - 1);
        out_phoneme[out_len - 1] = '\0';
        
        ESP_LOGD(TAG, "Matched '%.*s' -> '%s'", 
                 (int)best_len, input, best_rule->phoneme);
        
        return best_len;
    }
    
    // No match - return the character as-is
    out_phoneme[0] = input[0];
    out_phoneme[1] = '\0';
    
    ESP_LOGD(TAG, "No match for '%c', pass-through", input[0]);
    return 1;
}

/**
 * Convert text to phonemes using rule matching
 * 
 * Simple algorithm:
 * 1. Convert to lowercase
 * 2. Match rules left-to-right
 * 3. Concatenate phonemes with spaces
 * 
 * @param text Input text
 * @param output Output phoneme buffer
 * @param out_size Output buffer size
 * @return ESPYAK_OK on success
 */
espyak_err_t espyak_text_to_phonemes(const char* text,
                                      char* output,
                                      size_t out_size) {
    if (!text || !output || out_size == 0) {
        return ESPYAK_ERR_INVALID_ARG;
    }
    
    // Lowercase copy for matching
    char lower[256];
    size_t text_len = strlen(text);
    if (text_len >= sizeof(lower)) {
        text_len = sizeof(lower) - 1;
    }
    
    for (size_t i = 0; i < text_len; i++) {
        lower[i] = tolower((unsigned char)text[i]);
    }
    lower[text_len] = '\0';
    
    ESP_LOGD(TAG, "Converting: '%s'", text);
    
    // Match rules
    output[0] = '\0';
    size_t out_pos = 0;
    size_t pos = 0;
    
    while (pos < text_len && out_pos < out_size - 16) {
        char c = lower[pos];
        
        // Skip non-alphabetic characters
        if (!isalpha((unsigned char)c)) {
            if (c == ' ' && out_pos > 0 && output[out_pos - 1] != ' ') {
                // Add space between words
                output[out_pos++] = ' ';
            }
            pos++;
            continue;
        }
        
        // Match rule
        char phoneme[32];
        size_t consumed = espyak_match_rule(lower, pos, phoneme, sizeof(phoneme));
        
        // Append phoneme
        if (out_pos > 0 && output[out_pos - 1] != ' ') {
            output[out_pos++] = ' ';  // Space between phonemes
        }
        
        size_t ph_len = strlen(phoneme);
        if (out_pos + ph_len < out_size) {
            strcpy(&output[out_pos], phoneme);
            out_pos += ph_len;
        }
        
        pos += consumed;
    }
    
    output[out_pos] = '\0';
    
    ESP_LOGD(TAG, "Result: '%s'", output);
    return ESPYAK_OK;
}

/**
 * Get number of rules loaded
 */
size_t espyak_rules_count(void) {
    return NUM_RULES;
}
