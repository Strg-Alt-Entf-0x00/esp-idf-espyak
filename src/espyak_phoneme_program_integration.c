/**
 * @file espyak_phoneme_program_integration.c
 * @brief Integration layer: Phoneme string → list conversion
 * 
 * Converts phoneme strings (e.g. "kA:r") to phoneme list structures
 * for phoneme program execution.
 * 
 * Strategy (per Jev AI):
 * - MVP: Hardcode "car" test case first (57% confidence)
 * - Full: State machine parser (97% confidence)
 * - Time: 1-2 hours for full implementation
 */

#include "espyak_phoneme_program.h"
#include "espyak_internal.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <esp_log.h>

static const char* TAG = "espyak_integration";

/**
 * @brief Convert phoneme string to phoneme list
 * 
 * Parses phoneme string like "kA:r" into array of espyak_phlist_entry_t.
 * 
 * Phoneme format:
 * - Single chars: @, h, l, k, r
 * - Multi-char: A: (ɑː), oU (oʊ), tS (tʃ), dZ (dʒ)
 * - Stress: ' = primary, '' = secondary
 * - Example: "k'A:r" = [k, A: with primary stress, r]
 * 
 * @param phoneme_table Phoneme table for lookups
 * @param phoneme_str Input phoneme string
 * @param plist Output phoneme list (pre-allocated)
 * @param max_len Maximum entries in plist
 * @param out_len Number of entries written (output)
 * @return ESPYAK_OK on success
 */
espyak_err_t espyak_phoneme_string_to_list(
    const espyak_phoneme_table_t* phoneme_table,
    const char* phoneme_str,
    espyak_phlist_entry_t* plist,
    size_t max_len,
    size_t* out_len
) {
    if (!phoneme_table || !phoneme_str || !plist || !out_len) {
        return ESPYAK_ERR_INVALID_ARG;
    }
    
    *out_len = 0;
    size_t pos = 0;
    size_t str_len = strlen(phoneme_str);
    bool is_first_phoneme = true;
    
    ESP_LOGD(TAG, "Converting phoneme string: '%s'", phoneme_str);
    
    while (pos < str_len && *out_len < max_len) {
        char mnemonic[8] = {0};
        size_t mnem_len = 0;
        uint8_t stress_level = STRESS_UNSTRESSED;
        
        // Skip leading spaces
        while (pos < str_len && phoneme_str[pos] == ' ') {
            pos++;
        }
        
        if (pos >= str_len) {
            break;
        }
        
        // Check for stress marker first
        if (phoneme_str[pos] == '\'') {
            if (phoneme_str[pos + 1] == '\'') {
                stress_level = STRESS_SECONDARY;  // ''
                pos += 2;
            } else {
                stress_level = STRESS_PRIMARY;    // '
                pos++;
            }
        }
        
        // Parse phoneme mnemonic - read until space or end
        // Phonemes can be:
        // - Single char: @, h, l, k, r
        // - Multi-char: A:, oU, tS, dZ, r/, i:, etc.
        
        while (pos < str_len && phoneme_str[pos] != ' ' && mnem_len < 7) {
            mnemonic[mnem_len++] = phoneme_str[pos++];
        }
        
        // Skip if empty
        if (mnem_len == 0) {
            break;
        }
        
        mnemonic[mnem_len] = '\0';
        
        // Look up phoneme in table
        const espyak_phoneme_t* ph = espyak_phoneme_lookup(phoneme_table, mnemonic);
        
        if (!ph) {
            ESP_LOGW(TAG, "Phoneme not found: '%s' (skipping)", mnemonic);
            continue;
        }
        
        // Create phoneme list entry
        espyak_phlist_entry_t* entry = &plist[*out_len];
        memset(entry, 0, sizeof(espyak_phlist_entry_t));
        
        entry->ph = ph;
        entry->stresslevel = stress_level;
        entry->synthflags = 0;
        entry->newword = is_first_phoneme ? PHLIST_START_OF_WORD : 0;
        entry->deleted = false;
        entry->_changed = false;
        entry->ipa_override[0] = '\0';
        
        // Mark vowels as syllable nuclei
        if (ph->type == ESPYAK_PHONEME_VOWEL) {
            entry->synthflags |= SFLAG_SYLLABLE;
        }
        
        ESP_LOGD(TAG, "  [%zu] '%s' stress=%d%s", 
                 *out_len, mnemonic, stress_level,
                 is_first_phoneme ? " WORD_START" : "");
        
        (*out_len)++;
        is_first_phoneme = false;
    }
    
    ESP_LOGD(TAG, "Converted %zu phonemes", *out_len);
    return ESPYAK_OK;
}

/**
 * @brief Convert phoneme list back to string
 * 
 * @param plist Phoneme list
 * @param plist_len Number of entries
 * @param output Output string buffer
 * @param out_size Buffer size
 * @return ESPYAK_OK on success
 */
espyak_err_t espyak_phoneme_list_to_string(
    const espyak_phlist_entry_t* plist,
    size_t plist_len,
    char* output,
    size_t out_size
) {
    if (!plist || !output || out_size == 0) {
        return ESPYAK_ERR_INVALID_ARG;
    }
    
    size_t pos = 0;
    
    for (size_t i = 0; i < plist_len; i++) {
        if (plist[i].deleted) {
            continue;  // Skip deleted phonemes
        }
        
        const espyak_phoneme_t* ph = plist[i].ph;
        
        // Add stress marker if present
        if (plist[i].stresslevel >= STRESS_PRIMARY) {
            if (pos < out_size - 1) {
                output[pos++] = '\'';
            }
        } else if (plist[i].stresslevel == STRESS_SECONDARY) {
            if (pos < out_size - 2) {
                output[pos++] = '\'';
                output[pos++] = '\'';
            }
        }
        
        // Add phoneme mnemonic
        size_t mnem_len = strlen(ph->mnemonic);
        if (pos + mnem_len < out_size) {
            memcpy(output + pos, ph->mnemonic, mnem_len);
            pos += mnem_len;
        }
        
        // Add space separator between phonemes
        if (pos < out_size - 1 && i < plist_len - 1) {
            // Only add space if the next phoneme isn't deleted
            bool has_next = false;
            for (size_t next_i = i + 1; next_i < plist_len; next_i++) {
                if (!plist[next_i].deleted) {
                    has_next = true;
                    break;
                }
            }
            if (has_next) {
                output[pos++] = ' ';
            }
        }
    }
    
    output[pos] = '\0';
    return ESPYAK_OK;
}

/**
 * @brief Execute phoneme programs on a phoneme string
 * 
 * High-level integration function:
 * 1. Convert string → list
 * 2. Run phoneme programs
 * 3. Compact (remove deleted)
 * 4. Convert list → string
 * 
 * @param phoneme_table Phoneme table for lookups
 * @param phoneme_str Input phoneme string (modified in place if space allows)
 * @param buf_size Size of phoneme_str buffer
 * @return ESPYAK_OK on success
 */
espyak_err_t espyak_execute_phoneme_programs_on_string(
    const espyak_phoneme_table_t* phoneme_table,
    char* phoneme_str,
    size_t buf_size
) {
    if (!phoneme_table || !phoneme_str) {
        return ESPYAK_ERR_INVALID_ARG;
    }
    
    // Allocate phoneme list on HEAP (ESP32-P4 stack limit: 256 bytes)
    // 64 entries * ~32 bytes = ~2KB - too large for stack!
    espyak_phlist_entry_t* plist = (espyak_phlist_entry_t*)malloc(
        64 * sizeof(espyak_phlist_entry_t)
    );
    
    if (!plist) {
        ESP_LOGE(TAG, "Failed to allocate phoneme list (heap)");
        return ESPYAK_ERR_NO_MEM;
    }
    
    size_t plist_len = 0;
    
    // Convert string → list
    espyak_err_t err = espyak_phoneme_string_to_list(
        phoneme_table,
        phoneme_str,
        plist,
        64,
        &plist_len
    );
    
    if (err != ESPYAK_OK) {
        ESP_LOGE(TAG, "Failed to convert string to list: %d", err);
        free(plist);
        return err;
    }
    
    if (plist_len == 0) {
        free(plist);
        return ESPYAK_OK;  // Empty string, nothing to do
    }
    
    // Execute phoneme programs
    err = espyak_phoneme_program_run(phoneme_table, plist, &plist_len, 64);
    
    if (err != ESPYAK_OK) {
        ESP_LOGW(TAG, "Phoneme programs failed: %d", err);
        // Continue anyway - programs are optional
    }
    
    // Compact (remove deleted phonemes)
    espyak_phoneme_program_compact(plist, &plist_len);
    
    // Convert list → string
    err = espyak_phoneme_list_to_string(plist, plist_len, phoneme_str, buf_size);
    
    if (err != ESPYAK_OK) {
        ESP_LOGE(TAG, "Failed to convert list to string: %d", err);
        free(plist);
        return err;
    }
    
    ESP_LOGD(TAG, "Phoneme programs executed successfully");
    free(plist);
    return ESPYAK_OK;
}
