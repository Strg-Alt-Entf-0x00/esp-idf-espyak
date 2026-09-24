/**
 * @file espyak_phsource_parser.c
 * @brief Parser for phsource/phonemes files
 * 
 * Parses espeak-ng phoneme definition format:
 * 
 * phoneme X
 *   keyword value
 *   ...
 * endphoneme
 * 
 * Reference: .reference_codes/espyak/espyak/data/phsource/phonemes
 * 
 * NOTE: Uses static buffers to minimize stack usage (ESP32 stack safety)
 */

#include "espyak_internal.h"
#include <string.h>
#include <ctype.h>
#include <esp_log.h>
#include <esp_task_wdt.h>

static const char* TAG = "espyak_parser";

// Static buffers to reduce stack usage (thread-safe for single-threaded parsing)
static char g_keyword_buf[32];
static char g_value_buf[64];
static char g_line_buf[128];

// Parser state
typedef struct {
    const char* input;
    size_t pos;
    size_t line;
} parser_state_t;

/**
 * Skip whitespace and comments
 */
static void skip_whitespace(parser_state_t* state) {
    while (state->input[state->pos]) {
        char c = state->input[state->pos];
        
        // Skip whitespace
        if (isspace(c)) {
            if (c == '\n') state->line++;
            state->pos++;
            continue;
        }
        
        // Skip // comments
        if (c == '/' && state->input[state->pos + 1] == '/') {
            while (state->input[state->pos] && state->input[state->pos] != '\n') {
                state->pos++;
            }
            continue;
        }
        
        break;
    }
}

/**
 * Read a token (word or identifier)
 */
static bool read_token(parser_state_t* state, char* buffer, size_t buf_size) {
    skip_whitespace(state);
    
    size_t i = 0;
    while (state->input[state->pos] && !isspace(state->input[state->pos]) && 
           state->input[state->pos] != '/' && i < buf_size - 1) {
        buffer[i++] = state->input[state->pos++];
    }
    buffer[i] = '\0';
    
    return i > 0;
}

/**
 * Read rest of line
 */
static void read_line(parser_state_t* state, char* buffer, size_t buf_size) {
    skip_whitespace(state);
    
    size_t i = 0;
    while (state->input[state->pos] && state->input[state->pos] != '\n' && i < buf_size - 1) {
        buffer[i++] = state->input[state->pos++];
    }
    buffer[i] = '\0';
    
    // Trim trailing whitespace
    while (i > 0 && isspace(buffer[i - 1])) {
        buffer[--i] = '\0';
    }
}

/**
 * Parse phoneme type keyword
 */
static uint8_t parse_type_keyword(const char* keyword) {
    if (strcmp(keyword, "stress") == 0) return 2;  // PH_STRESS
    if (strcmp(keyword, "pause") == 0) return 1;   // PH_PAUSE
    if (strcmp(keyword, "vwl") == 0 || strcmp(keyword, "vowel") == 0) return 3;  // PH_VOWEL
    if (strcmp(keyword, "liquid") == 0) return 4;  // PH_LIQUID
    if (strcmp(keyword, "stop") == 0 || strcmp(keyword, "stp") == 0) return 5;  // PH_STOP
    if (strcmp(keyword, "frc") == 0) return 7;     // PH_FRICATIVE
    if (strcmp(keyword, "nas") == 0 || strcmp(keyword, "nasal") == 0) return 9;  // PH_NASAL
    if (strcmp(keyword, "virtual") == 0) return 10; // PH_VIRTUAL
    
    return 0;  // INVALID
}

/**
 * Parse a single phoneme attribute
 */
static void parse_attribute(parser_state_t* state, espyak_phoneme_t* phoneme) {
    if (!read_token(state, g_keyword_buf, sizeof(g_keyword_buf))) {
        return;
    }
    
    // Check for program: block
    if (strcmp(g_keyword_buf, "program:") == 0) {
        // Parse program block: multiple lines until "endprogram"
        // Store as raw text (will be interpreted at runtime)
        
        // Estimate size needed
        size_t program_start = state->pos;
        size_t program_end = program_start;
        
        // Find endprogram
        while (state->input[program_end]) {
            if (strncmp(&state->input[program_end], "endprogram", 10) == 0) {
                break;
            }
            program_end++;
        }
        
        if (state->input[program_end]) {
            // Allocate and copy program text
            size_t program_len = program_end - program_start;
            if (program_len > 0) {
                // Note: Using malloc directly (not espyak_malloc) since we don't have config here
                // TODO: Pass config to parser for proper PSRAM allocation
                phoneme->program = (char*)malloc(program_len + 1);
                if (phoneme->program) {
                    memcpy(phoneme->program, &state->input[program_start], program_len);
                    phoneme->program[program_len] = '\0';
                    
                    // Remove leading/trailing whitespace
                    char* p = phoneme->program;
                    while (*p && isspace(*p)) p++;
                    size_t len = strlen(p);
                    while (len > 0 && isspace(p[len - 1])) len--;
                    p[len] = '\0';
                    
                    if (p != phoneme->program) {
                        memmove(phoneme->program, p, len + 1);
                    }
                    
                    ESP_LOGD(TAG, "  program: %zu bytes", strlen(phoneme->program));
                }
            }
            
            // Skip to after "endprogram"
            state->pos = program_end + 10;
            
            // Skip rest of line
            while (state->input[state->pos] && state->input[state->pos] != '\n') {
                state->pos++;
            }
        }
        
        return;
    }
    
    // Check if it's a type keyword
    uint8_t type = parse_type_keyword(g_keyword_buf);
    if (type != 0) {
        phoneme->type = type;
        return;
    }
    
    // Other attributes
    if (strcmp(g_keyword_buf, "stress_type") == 0) {
        if (read_token(state, g_value_buf, sizeof(g_value_buf))) {
            phoneme->stress_type = atoi(g_value_buf);
        }
    }
    else if (strcmp(g_keyword_buf, "ipa") == 0) {
        read_line(state, g_line_buf, sizeof(g_line_buf));
        // IPA allocation skipped for now (Phase 3 completion)
    }
    else if (strcmp(g_keyword_buf, "unstressed") == 0) {
        phoneme->flags |= 0x0001;  // FLAG_UNSTRESSED
    }
    else if (strcmp(g_keyword_buf, "nolink") == 0) {
        phoneme->flags |= 0x0002;  // FLAG_NOLINK
    }
    else if (strcmp(g_keyword_buf, "starttype") == 0 ||
             strcmp(g_keyword_buf, "endtype") == 0 ||
             strcmp(g_keyword_buf, "length") == 0 ||
             strcmp(g_keyword_buf, "lengthmod") == 0) {
        // Read and ignore for now
        read_token(state, g_value_buf, sizeof(g_value_buf));
    }
    else {
        // Unknown keyword - skip rest of line
        read_line(state, g_line_buf, sizeof(g_line_buf));
    }
}

/**
 * Parse a phoneme block:
 * phoneme X
 *   ...
 * endphoneme
 */
static bool parse_phoneme(parser_state_t* state, espyak_phoneme_t* phoneme) {
    // Read "phoneme"
    if (!read_token(state, g_keyword_buf, sizeof(g_keyword_buf)) || 
        strcmp(g_keyword_buf, "phoneme") != 0) {
        return false;
    }
    
    // Read mnemonic
    if (!read_token(state, g_value_buf, sizeof(g_value_buf))) {
        ESP_LOGE(TAG, "Line %zu: Expected phoneme mnemonic", state->line);
        return false;
    }
    
    // Unescape backslashes: \, -> ,
    size_t j = 0;
    for (size_t i = 0; g_value_buf[i] && j < 7; i++) {
        if (g_value_buf[i] == '\\' && g_value_buf[i + 1]) {
            phoneme->mnemonic[j++] = g_value_buf[++i];
        } else {
            phoneme->mnemonic[j++] = g_value_buf[i];
        }
    }
    phoneme->mnemonic[j] = '\0';
    
    ESP_LOGD(TAG, "Parsing phoneme '%s'", phoneme->mnemonic);
    
    // Parse attributes until "endphoneme"
    while (true) {
        skip_whitespace(state);
        
        // Check for endphoneme
        size_t saved_pos = state->pos;
        if (read_token(state, g_keyword_buf, sizeof(g_keyword_buf))) {
            if (strcmp(g_keyword_buf, "endphoneme") == 0) {
                break;
            }
            // Not endphoneme, restore position
            state->pos = saved_pos;
        }
        
        parse_attribute(state, phoneme);
    }
    
    return true;
}

/**
 * Parse entire phsource file
 */
espyak_err_t espyak_parse_phsource(const char* content, 
                                    espyak_phoneme_t* phonemes,
                                    size_t max_phonemes,
                                    size_t* num_parsed) {
    if (!content || !phonemes || !num_parsed) {
        return ESPYAK_ERR_INVALID_ARG;
    }
    
    parser_state_t state = {
        .input = content,
        .pos = 0,
        .line = 1
    };
    
    *num_parsed = 0;
    
    ESP_LOGI(TAG, "Parsing phsource file...");
    
    while (state.input[state.pos]) {
        skip_whitespace(&state);
        
        if (!state.input[state.pos]) break;
        
        // Feed watchdog to prevent timeout during long parsing
        // Removed esp_task_wdt_reset(); to avoid "task not found" spam if task is not subscribed
        
        // Check for "phoneme" keyword
        size_t saved_pos = state.pos;
        if (!read_token(&state, g_keyword_buf, sizeof(g_keyword_buf))) {
            break;
        }
        
        if (strcmp(g_keyword_buf, "phoneme") == 0) {
            // Restore position and parse phoneme
            state.pos = saved_pos;
            
            if (*num_parsed >= max_phonemes) {
                ESP_LOGW(TAG, "Max phonemes reached (%zu)", max_phonemes);
                break;
            }
            
            espyak_phoneme_t* ph = &phonemes[*num_parsed];
            memset(ph, 0, sizeof(espyak_phoneme_t));
            
            if (parse_phoneme(&state, ph)) {
                (*num_parsed)++;
            }
        }
        else if (strcmp(g_keyword_buf, "phonemetable") == 0) {
            // Skip phonemetable declaration
            read_line(&state, g_line_buf, sizeof(g_line_buf));
        }
        else {
            // Unknown keyword - skip line
            read_line(&state, g_line_buf, sizeof(g_line_buf));
        }
    }
    
    ESP_LOGI(TAG, "Parsed %zu phonemes", *num_parsed);
    return ESPYAK_OK;
}
