/**
 * @file espyak_phoneme_program.h
 * @brief Phoneme program interpreter for context-dependent transformations
 * 
 * Implements espeak-ng's phoneme programs: IF/THEN conditionals with
 * ChangePhoneme and InsertPhoneme instructions that modify the phoneme
 * list based on phonetic context.
 * 
 * Example: English "r" only outputs IPA when before a vowel:
 *   IF nextPh(isNotVowel) THEN
 *       ChangePhoneme(r/)
 *   ENDIF
 * Result: "car" → [kɑː] not [kɑːɹ]
 */

#ifndef ESPYAK_PHONEME_PROGRAM_H
#define ESPYAK_PHONEME_PROGRAM_H

#include "espyak_internal.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Note: espyak_phoneme_t and espyak_handle_t are defined in espyak_internal.h */

/**
 * @brief Phoneme list entry with context flags
 * 
 * Represents one phoneme in the output stream with its stress,
 * word boundary markers, and runtime state.
 */
typedef struct {
    const espyak_phoneme_t* ph;  ///< Current phoneme
    uint8_t stresslevel;         ///< 0=diminished, 1=unstressed, 2-3=secondary, 4-5=primary
    uint8_t synthflags;          ///< SFLAG_SYLLABLE, etc.
    uint8_t newword;             ///< PHLIST_START_OF_WORD, PHLIST_START_OF_CLAUSE
    char ipa_override[16];       ///< Conditional IPA from program (or empty)
    bool deleted;                ///< ChangePhoneme(NULL) marks for deletion
    bool _changed;               ///< Internal: already ChangePhoneme'd this pass
} espyak_phlist_entry_t;

/* Synthesis flags (synthflags field) */
#define SFLAG_SYLLABLE      0x04  ///< This phoneme is a syllable nucleus
#define SFLAG_LENGTHEN      0x08  ///< Length mark follows

/* New-word flags (newword field) */
#define PHLIST_START_OF_WORD     0x01  ///< Word boundary
#define PHLIST_START_OF_CLAUSE   0x02  ///< Clause boundary  
#define PHLIST_START_OF_SENTENCE 0x04  ///< Sentence boundary

/* Stress levels */
#define STRESS_DIMINISHED    0  ///< Unstressed, reduced
#define STRESS_UNSTRESSED    1  ///< Default unstressed
#define STRESS_SECONDARY     3  ///< Secondary stress
#define STRESS_PRIMARY       4  ///< Primary stress
#define STRESS_PRIORITY      5  ///< Emphatic stress

/**
 * @brief Initialize phoneme program system
 * 
 * @param phoneme_table Phoneme table
 * @return ESPYAK_OK on success
 */
espyak_err_t espyak_phoneme_program_init(const espyak_phoneme_table_t* phoneme_table);

/**
 * @brief Execute phoneme programs on a phoneme list
 * 
 * Two-pass execution:
 * - Pass 1: Apply ChangePhoneme/InsertPhoneme (structural changes)
 * - Pass 2: Re-evaluate conditional IPA overrides
 * 
 * @param phoneme_table Phoneme table for lookups
 * @param plist Phoneme list to modify in-place
 * @param plist_len Length of plist array (will be modified if insertions occur)
 * @param max_len Maximum capacity of plist array
 * @return ESPYAK_OK on success, ESPYAK_ERR_NO_MEM if insertions exceed max_len
 */
espyak_err_t espyak_phoneme_program_run(
    const espyak_phoneme_table_t* phoneme_table,
    espyak_phlist_entry_t* plist,
    size_t* plist_len,
    size_t max_len
);

/**
 * @brief Compact a phoneme list by removing deleted entries
 * 
 * Call after espyak_phoneme_program_run() to remove entries marked deleted=true
 * 
 * @param plist Phoneme list to compact in-place
 * @param plist_len Input/output length
 */
void espyak_phoneme_program_compact(
    espyak_phlist_entry_t* plist,
    size_t* plist_len
);

/**
 * @brief Clean up phoneme program system
 * 
 * @param phoneme_table Phoneme table
 */
void espyak_phoneme_program_deinit(const espyak_phoneme_table_t* phoneme_table);

#ifdef __cplusplus
}
#endif

#endif /* ESPYAK_PHONEME_PROGRAM_H */
