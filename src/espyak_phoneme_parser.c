#include "espyak_internal.h"
#include <string.h>
#include <esp_log.h>

static const char* TAG = "espyak_parser";

/**
 * @brief Parse concatenated phoneme string (greedy longest-match)
 */
espyak_err_t espyak_parse_phoneme_string(
    const espyak_phoneme_table_t* phoneme_table,
    const char* input,
    const espyak_phoneme_t** output,
    size_t max_output,
    size_t* out_count
) {
    if (!phoneme_table || !input || !output || !out_count) {
        return ESPYAK_ERR_INVALID_ARG;
    }
    
    *out_count = 0;
    size_t pos = 0;
    size_t input_len = strlen(input);
    const size_t MAX_PHONEME_LEN = 4;  // Max: "oU", "tS", "dZ", etc.
    
    while (pos < input_len && *out_count < max_output) {
        // Skip spaces/barriers
        if (input[pos] == ' ' || input[pos] == '\t' || input[pos] == '|') {
            pos++;
            continue;
        }
        
        // Try longest match first (greedy!)
        const espyak_phoneme_t* found = NULL;
        size_t matched_len = 0;
        
        size_t max_try = (input_len - pos < MAX_PHONEME_LEN) 
                         ? (input_len - pos) 
                         : MAX_PHONEME_LEN;
        
        for (size_t len = max_try; len > 0; len--) {
            char candidate[MAX_PHONEME_LEN + 1];
            memcpy(candidate, &input[pos], len);
            candidate[len] = '\0';
            
            // Lookup in phoneme table
            found = espyak_phoneme_lookup(phoneme_table, candidate);
            
            if (found) {
                matched_len = len;
                break;  // Found! Use this match
            }
        }
        
        if (found) {
            output[*out_count] = found;
            (*out_count)++;
            pos += matched_len;
        } else {
            // Handle literal digits or single quotes (stress)
            if (input[pos] == '\'' || (input[pos] >= '0' && input[pos] <= '9')) {
                // Try looking up just this character
                char candidate[2] = {input[pos], '\0'};
                found = espyak_phoneme_lookup(phoneme_table, candidate);
                if (found) {
                    output[*out_count] = found;
                    (*out_count)++;
                }
            } else {
                // Unknown character - skip with warning
                ESP_LOGW(TAG, "Unknown phoneme char at pos %zu: '%c'", pos, input[pos]);
            }
            pos++;
        }
    }
    
    return ESPYAK_OK;
}
