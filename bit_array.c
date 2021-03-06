// ------------------------------------------------------------------
//   bit_array.c
//   Copyright (C) 2020-2021 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt
//   Copyright claimed on additions and modifications vs public domain.
//
// a module for handling arrays of 2-bit elements, partially based on public domain code here: 
// https://github.com/noporpoise/BitArray/. 


// 64 bit words
// Array length can be zero
// Unused top bits must be zero

#include <stdarg.h>
#include "genozip.h"
#include "endianness.h"
#include "bit_array.h"
#include "buffer.h"

//
// Tables of constants
//

#define assert(x) ASSERTE ((x), "%s", #x)

// byte reverse look up table
static const word_t reverse_table[256] =
{
  0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0,
  0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
  0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8,
  0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
  0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4,
  0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
  0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC,
  0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
  0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2,
  0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
  0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA,
  0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
  0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6,
  0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
  0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE,
  0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
  0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1,
  0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
  0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9,
  0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9,
  0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5,
  0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
  0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED,
  0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
  0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3,
  0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3,
  0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB,
  0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
  0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7,
  0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
  0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF,
  0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF,
};

#ifndef __GNUC__

// See http://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetParallel
static word_t __inline windows_popcount(word_t w)
{
  w = w - ((w >> 1) & (word_t)~(word_t)0/3);
  w = (w & (word_t)~(word_t)0/15*3) + ((w >> 2) & (word_t)~(word_t)0/15*3);
  w = (w + (w >> 4)) & (word_t)~(word_t)0/255*15;
  w = (word_t)(w * ((word_t)~(word_t)0/255)) >> (sizeof(word_t) - 1) * 8;
}

#define POPCOUNT(x) windows_popcountl(x)
#else
#define POPCOUNT(x) (unsigned)__builtin_popcountll(x)
#endif

// word of all 1s
#define WORD_MAX  (~(word_t)0)

#define SET_REGION(arr,start,len)    _set_region((arr),(start),(len),FILL_REGION)
#define CLEAR_REGION(arr,start,len)  _set_region((arr),(start),(len),ZERO_REGION)
#define TOGGLE_REGION(arr,start,len) _set_region((arr),(start),(len),SWAP_REGION)

//
// Common internal functions
//

#define bits_in_top_word(nbits) ((nbits) ? bitset64_idx((nbits) - 1) + 1 : 0)

// prints right to left
static inline char* _word_to_str(word_t word, char str[WORD_SIZE+1])
  __attribute__((unused));

static inline char* _word_to_str(word_t word, char str[WORD_SIZE+1])
{
  word_offset_t i;
  for(i = 0; i < WORD_SIZE; i++)
  {
    str[WORD_SIZE-i-1] = ((word >> i) & (word_t)0x1) == 0 ? '0' : '1';
  }
  str[WORD_SIZE] = '\0';
  return str;
}

// Used in debugging
#ifdef DEBUG

// Mostly used for debugging
typedef struct { char s[WORD_SIZE+1]; } WordStr;
static inline WordStr _print_word (word_t word)
{
    WordStr w;
    w.s[WORD_SIZE] = 0;

    for (word_offset_t i=0; i < WORD_SIZE; i++)
        w.s[i] = ((word >> i) & (word_t)0x1) == 0 ? '0' : '1';

    return w;
}

#define DEBUG_VALIDATE(a) validate_bitarr((a), __FUNCTION__, __LINE__)
static inline void validate_bitarr (BitArray *arr, const char *file, int lineno)
{
    // Verify that its allocated
    ASSERTE (arr->type != BITARR_UNALLOCATED, "[%s:%i] bitarray is not allocated", file, lineno);

    // Check top word is masked (only if not overlayed - the unused bits of top word don't belong to this bit array and might be used eg by another bit array in genome.ref/genome.is_set)
    if (arr->type == BITARR_REGULAR) {
      word_addr_t tw = arr->nwords == 0 ? 0 : arr->nwords - 1;
      bit_index_t top_bits = bits_in_top_word(arr->nbits);

      ASSERTE (arr->words[tw] <= bitmask64(top_bits), "[%s:%i] Expected %i bits in top word[%i] but word=%s\n", 
               file, lineno, (int)top_bits, (int)tw, _print_word(arr->words[tw]).s);
    }

    // Check num of words is correct
    word_addr_t num_words = roundup_bits2words64(arr->nbits);
    ASSERTE (num_words == arr->nwords, "[%s:%i] num of words wrong [bits: %i, word: %i, actual words: %i]", 
             file, lineno, (int)arr->nbits, (int)num_words, (int)arr->nwords);
}

#else
    #define DEBUG_VALIDATE(a)
#endif

// Reverse a word
static inline word_t _reverse_word(word_t word)
{
    word_t reverse = (reverse_table[(word)       & 0xff] << 56) |
                    (reverse_table[(word >>  8) & 0xff] << 48) |
                    (reverse_table[(word >> 16) & 0xff] << 40) |
                    (reverse_table[(word >> 24) & 0xff] << 32) |
                    (reverse_table[(word >> 32) & 0xff] << 24) |
                    (reverse_table[(word >> 40) & 0xff] << 16) |
                    (reverse_table[(word >> 48) & 0xff] << 8) |
                    (reverse_table[(word >> 56) & 0xff]);

    return reverse;
}

static inline void _mask_top_word(BitArray* bitarr)
{
    // Mask top word
    word_addr_t nwords = MAX(1, bitarr->nwords);
    word_offset_t bits_active = bits_in_top_word(bitarr->nbits);
    bitarr->words[nwords-1] &= bitmask64(bits_active);
}

// Set 64 bits from a particular start position
// Doesn't extend bit array
static inline void _set_word (BitArray *bitarr, bit_index_t start, word_t word)
{
    word_addr_t word_index = bitset64_wrd(start);
    word_offset_t word_offset = bitset64_idx(start);

    if(word_offset == 0)
        bitarr->words[word_index] = word;
    
    else {
        bitarr->words[word_index] = (word << word_offset) |
                                    (bitarr->words[word_index] & bitmask64(word_offset));

        if(word_index+1 < bitarr->nwords) {

            bitarr->words[word_index+1] = (word >> (WORD_SIZE - word_offset)) |
                                          (bitarr->words[word_index+1] & (WORD_MAX << word_offset));

            // added by divon: Mask top word only if its the last word --divon
            if (word_index+2 == bitarr->nwords)
                _mask_top_word (bitarr);
        }
    }

    // _mask_top_word(bitarr); // divon: removed bc not thread safe when setting different parts of the bit array with multiple threads

    DEBUG_VALIDATE(bitarr);
}

static inline void _set_byte(BitArray *bitarr, bit_index_t start, uint8_t byte)
{
  word_t w = _get_word(bitarr, start);
  _set_word(bitarr, start, (w & ~(word_t)0xff) | byte);
}

// Wrap around
static inline word_t _get_word_cyclic(const BitArray* bitarr, bit_index_t start)
{
  word_t word = _get_word(bitarr, start);

  bit_index_t bits_taken = bitarr->nbits - start;

  if(bits_taken < WORD_SIZE)
  {
    word |= (bitarr->words[0] << bits_taken);

    if(bitarr->nbits < (bit_index_t)WORD_SIZE)
    {
      // Mask word to prevent repetition of the same bits
      word = word & bitmask64(bitarr->nbits);
    }
  }

  return word;
}

// Wrap around
static inline void _set_word_cyclic(BitArray* bitarr,
                                    bit_index_t start, word_t word)
{
  _set_word(bitarr, start, word);

  bit_index_t bits_set = bitarr->nbits - start;

  if(bits_set < WORD_SIZE && start > 0)
  {
    word >>= bits_set;

    // Prevent overwriting the bits we've just set
    // by setting 'start' as the upper bound for the number of bits to write
    word_offset_t bits_remaining = MIN(WORD_SIZE - bits_set, start);
    word_t mask = bitmask64(bits_remaining);

    bitarr->words[0] = bitmask_merge(word, bitarr->words[0], mask);
  }
}

//
// Fill a region (internal use only)
//

// FillAction is fill with 0 or 1 or toggle
typedef enum {ZERO_REGION, FILL_REGION, SWAP_REGION} FillAction;

static inline void _set_region(BitArray* bitarr, bit_index_t start,
                               bit_index_t length, FillAction action)
{
  if(length == 0) return;

  word_addr_t first_word = bitset64_wrd(start);
  word_addr_t last_word = bitset64_wrd(start+length-1);
  word_offset_t foffset = bitset64_idx(start);
  word_offset_t loffset = bitset64_idx(start+length-1);

  if(first_word == last_word)
  {
    word_t mask = bitmask64(length) << foffset;

    switch(action)
    {
      case ZERO_REGION: bitarr->words[first_word] &= ~mask; break;
      case FILL_REGION: bitarr->words[first_word] |=  mask; break;
      case SWAP_REGION: bitarr->words[first_word] ^=  mask; break;
    }
  }
  else
  {
    // Set first word
    switch(action)
    {
      case ZERO_REGION: bitarr->words[first_word] &=  bitmask64(foffset); break;
      case FILL_REGION: bitarr->words[first_word] |= ~bitmask64(foffset); break;
      case SWAP_REGION: bitarr->words[first_word] ^= ~bitmask64(foffset); break;
    }

    word_addr_t i;

    // Set whole words
    switch(action)
    {
      case ZERO_REGION:
        for(i = first_word + 1; i < last_word; i++)
          bitarr->words[i] = (word_t)0;
        break;
      case FILL_REGION:
        for(i = first_word + 1; i < last_word; i++)
          bitarr->words[i] = WORD_MAX;
        break;
      case SWAP_REGION:
        for(i = first_word + 1; i < last_word; i++)
          bitarr->words[i] ^= WORD_MAX;
        break;
    }

    // Set last word
    switch(action)
    {
      case ZERO_REGION: bitarr->words[last_word] &= ~bitmask64(loffset+1); break;
      case FILL_REGION: bitarr->words[last_word] |=  bitmask64(loffset+1); break;
      case SWAP_REGION: bitarr->words[last_word] ^=  bitmask64(loffset+1); break;
    }
  }
}

//
// Constructor
//

BitArray bit_array_alloc_do (bit_index_t nbits, bool clear, const char *func, uint32_t code_line)
{
    BitArray bitarr = { .type   = BITARR_REGULAR,
                        .nbits  = nbits,
                        .nwords = roundup_bits2words64(nbits),
                        .words  = buf_low_level_malloc (roundup_bits2bytes64(nbits), clear, func, code_line) };

    // zero the bits in the top word that are beyond nbits (if not already cleared)
    if (!clear) bit_array_clear_excess_bits_in_top_word (&bitarr);
    
    return bitarr;
}

void bit_array_free (BitArray* bitarr)
{
    FREE (bitarr->words);
    memset (bitarr, 0, sizeof(BitArray));
}

bit_index_t bit_array_length(const BitArray* bit_arr)
{
  return bit_arr->nbits;
}

// If bitarr length < num_bits, resizes to num_bits
char bit_array_ensure_size(BitArray* bitarr, bit_index_t ensure_num_of_bits)
{
    ASSERTE (bitarr->nbits >= ensure_num_of_bits, "bit_array of out range: bitarr->nbits=%"PRIu64" ensure_num_of_bits=%"PRIu64,
             bitarr->nbits, ensure_num_of_bits);

    return 1;
}

void bit_array_ensure_size_critical(BitArray* bitarr, bit_index_t nbits)
{
    ASSERTE (bitarr->nbits >= nbits, "bit_array of out range: bitarr->nbits=%"PRIu64" nbits=%"PRIu64,
             bitarr->nbits, nbits);
}

//
// Get, set, clear, assign and toggle individual bits
//

// Get the value of a bit (returns 0 or 1)
char bit_array_get_bit(const BitArray* bitarr, bit_index_t b)
{
    ASSERTE (b < bitarr->nbits, "Expecting b(%"PRId64") < bitarr->nbits(%"PRId64")", b, bitarr->nbits);
    return bit_array_get(bitarr, b);
}

// set a bit (to 1) at position b
void bit_array_set_bit(BitArray* bitarr, bit_index_t b)
{
    assert(b < bitarr->nbits);
    bit_array_set(bitarr,b);
    DEBUG_VALIDATE(bitarr);
}

// clear a bit (to 0) at position b
void bit_array_clear_bit(BitArray* bitarr, bit_index_t b)
{
    assert(b < bitarr->nbits);
    bit_array_clear(bitarr, b);
    DEBUG_VALIDATE(bitarr);
}

// If bit is 0 -> 1, if bit is 1 -> 0.  AKA 'flip'
void bit_array_toggle_bit(BitArray* bitarr, bit_index_t b)
{
    assert(b < bitarr->nbits);
    bit_array_toggle(bitarr, b);
    DEBUG_VALIDATE(bitarr);
}

// If char c != 0, set bit; otherwise clear bit
void bit_array_assign_bit(BitArray* bitarr, bit_index_t b, char c)
{
    assert(b < bitarr->nbits);
    bit_array_assign(bitarr, b, c ? 1 : 0);
    DEBUG_VALIDATE(bitarr);
}

//
// Get, set, clear and toggle several bits at once
//

// Get the offsets of the set bits (for offsets start<=offset<end)
// Returns the number of bits set
// It is assumed that dst is at least of length (end-start)
bit_index_t bit_array_get_bits(const BitArray* bitarr,
                               bit_index_t start, bit_index_t end,
                               bit_index_t* dst)
{
  bit_index_t i, n = 0;
  assert(end <= bitarr->nbits);
  for(i = start; i < end; i++) {
    if(bit_array_get(bitarr, i)) {
      dst[n++] = i;
    }
  }
  return n;
}

// Set multiple bits at once.
// e.g. set bits 1, 20 & 31: bit_array_set_bits(bitarr, 3, 1,20,31);
void bit_array_set_bits(BitArray* bitarr, size_t n, ...)
{
  size_t i;
  va_list argptr;
  va_start(argptr, n);

  for(i = 0; i < n; i++)
  {
    unsigned int bit_index = va_arg(argptr, unsigned int);
    bit_array_set_bit(bitarr, bit_index);
  }

  va_end(argptr);
  DEBUG_VALIDATE(bitarr);
}

// Clear multiple bits at once.
// e.g. clear bits 1, 20 & 31: bit_array_clear_bits(bitarr, 3, 1,20,31);
void bit_array_clear_bits(BitArray* bitarr, size_t n, ...)
{
  size_t i;
  va_list argptr;
  va_start(argptr, n);

  for(i = 0; i < n; i++)
  {
    unsigned int bit_index = va_arg(argptr, unsigned int);
    bit_array_clear_bit(bitarr, bit_index);
  }

  va_end(argptr);
  DEBUG_VALIDATE(bitarr);
}

//
// Set, clear all bits in a region
//

// Set all the bits in a region
void bit_array_set_region (BitArray *bitarr, bit_index_t start, bit_index_t len)
{
    if (!len) return; // nothing to do 

    ASSERTE (start + len - 1 <= bitarr->nbits, "Expecting: start(%"PRId64") + len(%"PRId64") - 1 <= bitarr->nbits(%"PRId64")",
             start, len, bitarr->nbits); // divon fixed bug

    SET_REGION (bitarr, start, len);
    DEBUG_VALIDATE (bitarr);
}


// Clear all the bits in a region
void bit_array_clear_region_do (BitArray *bitarr, bit_index_t start, bit_index_t len, const char *func, unsigned code_line)
{
    if (!len) return; // nothing to do 

    ASSERTE (start + len - 1 <= bitarr->nbits, "called from %s:%u: Expecting: start(%"PRId64") + len(%"PRId64") - 1 <= bitarr->nbits(%"PRId64")",
             func, code_line, start, len, bitarr->nbits); // divon fixed bug

    CLEAR_REGION (bitarr, start, len);
    DEBUG_VALIDATE (bitarr);
}

//
// Set, clear all bits at once
//

// set all elements of data to one
void bit_array_set_all (BitArray *bitarr)
{
    bit_index_t num_of_bytes = bitarr->nwords * sizeof(word_t);
    memset(bitarr->words, 0xFF, num_of_bytes);
    _mask_top_word(bitarr);
    DEBUG_VALIDATE(bitarr);
}

// set all elements of data to zero
void bit_array_clear_all (BitArray *bitarr)
{
    if (!bitarr->words) return; // nothing to do

    memset(bitarr->words, 0, bitarr->nwords * sizeof(word_t));
    DEBUG_VALIDATE(bitarr);
}

//
// Get a word at a time
//

uint64_t bit_array_get_wordn(const BitArray* bitarr, bit_index_t start, int n /* up to 64 */)
{
  ASSERTE (start + n <= bitarr->nbits, "expecting start=%"PRIu64" + n=%d <= bitarr->nbits=%"PRIu64, 
           start, n, bitarr->nbits);
           
  return (uint64_t)(_get_word(bitarr, start) & bitmask64(n));
}

//
// Set a word at a time
//
// Doesn't extend bit array. However it is safe to TRY to set bits beyond the
// end of the array, as long as: `start` is < `bit_array_length(arr)`
//

void bit_array_set_word64(BitArray* bitarr, bit_index_t start, uint64_t word)
{
  ASSERTE (start < bitarr->nbits, "expecting start(%"PRIu64") < bitarr->nbits(%"PRIu64")", start, bitarr->nbits);
  _set_word(bitarr, start, (word_t)word);
}

void bit_array_set_word32(BitArray* bitarr, bit_index_t start, uint32_t word)
{
  ASSERTE (start < bitarr->nbits, "expecting start(%"PRIu64") < bitarr->nbits(%"PRIu64")", start, bitarr->nbits);
  word_t w = _get_word(bitarr, start);
  _set_word(bitarr, start, bitmask_merge(w, word, 0xffffffff00000000UL));
}

void bit_array_set_word16(BitArray* bitarr, bit_index_t start, uint16_t word)
{
  ASSERTE (start < bitarr->nbits, "expecting start(%"PRIu64") < bitarr->nbits(%"PRIu64")", start, bitarr->nbits);
  word_t w = _get_word(bitarr, start);
  _set_word(bitarr, start, bitmask_merge(w, word, 0xffffffffffff0000UL));
}

void bit_array_set_word8(BitArray* bitarr, bit_index_t start, uint8_t byte)
{
  ASSERTE (start < bitarr->nbits, "expecting start(%"PRIu64") < bitarr->nbits(%"PRIu64")", start, bitarr->nbits);
  _set_byte(bitarr, start, byte);
}

void bit_array_set_wordn(BitArray* bitarr, bit_index_t start, uint64_t word, int n)
{
  ASSERTE (start < bitarr->nbits, "expecting start(%"PRIu64") < bitarr->nbits(%"PRIu64")", start, bitarr->nbits);
  assert(n <= 64);
  word_t w = _get_word(bitarr, start), m = bitmask64(n);
  _set_word(bitarr, start, bitmask_merge(word,w,m));
}

//
// Number of bits set
//

// Get the number of bits set (hamming weight)
bit_index_t bit_array_num_bits_set (const BitArray *bitarr)
{
    if (!bitarr->nbits) return 0;

    bit_index_t num_of_bits_set = 0;

    // all words but last one
    for (word_addr_t i=0; i < bitarr->nwords-1; i++)
        if (bitarr->words[i])
            num_of_bits_set += POPCOUNT (bitarr->words[i]);

    // last word might be partial
    word_offset_t bits_active = bits_in_top_word (bitarr->nbits);    
    num_of_bits_set += POPCOUNT (bitarr->words[bitarr->nwords-1] & bitmask64 (bits_active));
    
    return num_of_bits_set;
}

// added by divon
bit_index_t bit_array_num_bits_set_region(const BitArray* bitarr, bit_index_t start, bit_index_t length)
{
  if(length == 0) return 0;

  word_addr_t first_word = bitset64_wrd(start);
  word_addr_t last_word  = bitset64_wrd(start+length-1);
  word_offset_t foffset  = bitset64_idx(start);
  word_offset_t loffset  = bitset64_idx(start+length-1);

  bit_index_t num_of_bits_set = 0;

  if(first_word == last_word)
  {
    word_t mask = bitmask64(length) << foffset;
    num_of_bits_set += POPCOUNT (bitarr->words[first_word] & mask);
  }
  else
  {
    // first word
    num_of_bits_set += POPCOUNT (bitarr->words[first_word] & ~bitmask64(foffset));

    // whole words
    for(word_addr_t i = first_word + 1; i < last_word; i++)
      num_of_bits_set += POPCOUNT (bitarr->words[i]);

    // last word
    num_of_bits_set += POPCOUNT (bitarr->words[last_word] & bitmask64(loffset+1));
  }

  return num_of_bits_set;
}

// Get the number of bits not set (1 - hamming weight)
bit_index_t bit_array_num_bits_cleared(const BitArray* bitarr)
{
  return bitarr->nbits - bit_array_num_bits_set(bitarr);
}



//
// Find indices of set/clear bits
//

// Find the index of the next bit that is set/clear, at or after `offset`
// Returns 1 if such a bit is found, otherwise 0
// Index is stored in the integer pointed to by `result`
// If no such bit is found, value at `result` is not changed
#define _next_bit_func_def(FUNC,GET) \
char FUNC(const BitArray* bitarr, bit_index_t offset, bit_index_t* result) \
{ \
  ASSERTE (offset < bitarr->nbits, "expecting offset(%"PRId64") < bitarr->nbits(%"PRId64")", offset, bitarr->nbits); \
  if(bitarr->nbits == 0 || offset >= bitarr->nbits) { return 0; } \
 \
  /* Find first word that is greater than zero */ \
  word_addr_t i = bitset64_wrd(offset); \
  word_t w = GET(bitarr->words[i]) & ~bitmask64(bitset64_idx(offset)); \
 \
  while(1) { \
    if(w > 0) { \
      bit_index_t pos = i * WORD_SIZE + trailing_zeros(w); \
      if(pos < bitarr->nbits) { *result = pos; return 1; } \
      else { return 0; } \
    } \
    i++; \
    if(i >= bitarr->nwords) break; \
    w = GET(bitarr->words[i]); \
  } \
 \
  return 0; \
}

// Find the index of the previous bit that is set/clear, before `offset`.
// Returns 1 if such a bit is found, otherwise 0
// Index is stored in the integer pointed to by `result`
// If no such bit is found, value at `result` is not changed
#define _prev_bit_func_def(FUNC,GET) \
char FUNC(const BitArray* bitarr, bit_index_t offset, bit_index_t* result) \
{ \
  assert(offset <= bitarr->nbits); \
  if(bitarr->nbits == 0 || offset == 0) { return 0; } \
 \
  /* Find prev word that is greater than zero */ \
  word_addr_t i = bitset64_wrd(offset-1); \
  word_t w = GET(bitarr->words[i]) & bitmask64(bitset64_idx(offset-1)+1); \
 \
  if(w > 0) { *result = (i+1) * WORD_SIZE - leading_zeros(w) - 1; return 1; } \
 \
  /* i is unsigned so have to use break when i == 0 */ \
  for(--i; i != BIT_INDEX_MAX; i--) { \
    w = GET(bitarr->words[i]); \
    if(w > 0) { \
      *result = (i+1) * WORD_SIZE - leading_zeros(w) - 1; \
      return 1; \
    } \
  } \
 \
  return 0; \
}

#define GET_WORD(x) (x)
#define NEG_WORD(x) (~(x))
_next_bit_func_def(bit_array_find_next_set_bit,  GET_WORD);
_next_bit_func_def(bit_array_find_next_clear_bit,NEG_WORD);
_prev_bit_func_def(bit_array_find_prev_set_bit,  GET_WORD);
_prev_bit_func_def(bit_array_find_prev_clear_bit,NEG_WORD);

// Find the index of the first bit that is set.
// Returns 1 if a bit is set, otherwise 0
// Index of first set bit is stored in the integer pointed to by result
// If no bits are set, value at `result` is not changed
char bit_array_find_first_set_bit(const BitArray* bitarr, bit_index_t* result)
{
  return bit_array_find_next_set_bit(bitarr, 0, result);
}

// same same
char bit_array_find_first_clear_bit(const BitArray* bitarr, bit_index_t* result)
{
  return bit_array_find_next_clear_bit(bitarr, 0, result);
}

// Find the index of the last bit that is set.
// Returns 1 if a bit is set, otherwise 0
// Index of last set bit is stored in the integer pointed to by `result`
// If no bits are set, value at `result` is not changed
char bit_array_find_last_set_bit(const BitArray* bitarr, bit_index_t* result)
{
  return bit_array_find_prev_set_bit(bitarr, bitarr->nbits, result);
}

// same same
char bit_array_find_last_clear_bit(const BitArray* bitarr, bit_index_t* result)
{
  return bit_array_find_prev_clear_bit(bitarr, bitarr->nbits, result);
}


//
// Strings and printing
//

// Construct a BitArray from a substring with given on and off characters.
void bit_array_from_substr(BitArray* bitarr, bit_index_t offset,
                           const char *str, size_t len,
                           const char *on, const char *off,
                           char left_to_right)
{
  bit_array_ensure_size(bitarr, offset + len);
  bit_array_clear_region(bitarr, offset, len);

  // BitArray region is now all 0s -- just set the 1s
  size_t i;
  bit_index_t j;

  for(i = 0; i < len; i++)
  {
    if(strchr(on, str[i]) != NULL)
    {
      j = offset + (left_to_right ? i : len - i - 1);
      bit_array_set(bitarr, j);
    }
    else { assert(strchr(off, str[i]) != NULL); }
  }

  DEBUG_VALIDATE(bitarr);
}

// From string method
void bit_array_from_str(BitArray* bitarr, const char* str)
{
  bit_array_from_substr(bitarr, 0, str, strlen(str), "1", "0", 1);
}

// Takes a char array to write to.  `str` must be bitarr->nbits+1 in length
// Terminates string with '\0'
char* bit_array_to_str(const BitArray* bitarr, char* str)
{
  bit_index_t i;

  for(i = 0; i < bitarr->nbits; i++)
  {
    str[i] = bit_array_get(bitarr, i) ? '1' : '0';
  }

  str[bitarr->nbits] = '\0';

  return str;
}

char* bit_array_to_str_rev(const BitArray* bitarr, char* str)
{
  bit_index_t i;

  for(i = 0; i < bitarr->nbits; i++)
  {
    str[i] = bit_array_get(bitarr, bitarr->nbits-i-1) ? '1' : '0';
  }

  str[bitarr->nbits] = '\0';

  return str;
}


// Get a string representations for a given region, using given on/off characters.
// Note: does not nul-terminate
void bit_array_to_substr(const BitArray* bitarr,
                         bit_index_t start, bit_index_t length,
                         char* str, char on, char off,
                         char left_to_right)
{
  assert(start + length <= bitarr->nbits);

  bit_index_t i, j;
  bit_index_t end = start + length - 1;

  for(i = 0; i < length; i++)
  {
    j = (left_to_right ? start + i : end - i);
    str[i] = bit_array_get(bitarr, j) ? on : off;
  }

//  str[length] = '\0';
}

void bit_array_print_do (const BitArray *bitarr, const char *msg, FILE *file)
{
    fprintf (file, "%s (nbits=%"PRIu64"): ", msg, bitarr->nbits);

    for (bit_index_t i=0; i < bitarr->nbits; i++)
        fprintf (file, "%c", bit_array_get(bitarr, i) ? '1' : '0');

    fprintf (file, "\n");
}

void bit_array_print_binary_word_do (word_t word, const char *msg, FILE *file)
{
    BitArray bitarr = { .nbits=64, .nwords=1, .words = &word };
    bit_array_print_do (&bitarr, msg, file);
}

// Print a string representations for a given region, using given on/off characters.
void bit_array_print_substr (const char *msg, 
                             const BitArray* bitarr,
                             bit_index_t start, bit_index_t length,
                             FILE *file)
{
  length = MIN (length, bitarr->nbits - start);

  bit_index_t i, j;

  if (msg) fprintf (file, "%s: ", msg);
  for(i = 0; i < length; i++)
  {
    j = start + i;
    fprintf (file, "%c", bit_array_get(bitarr, j) ? '1' : '0');
  }
  fputc ('\n', file);
}

//
// Clone and copy
//

// destination and source may be the same bit_array
// and src/dst regions may overlap
static void _array_copy (BitArray* dst, bit_index_t dstindx,
                         const BitArray* src, bit_index_t srcindx,
                         bit_index_t length)
{
    // Num of full words to copy
    word_addr_t num_of_full_words = length / WORD_SIZE;
    word_addr_t i;

    word_offset_t bits_in_last_word = length % WORD_SIZE; //bits_in_top_word(length); // divon: fixed this bug in the original library

    if (dst == src && srcindx > dstindx)
    {
        // Work left to right
        for (i=0; i < num_of_full_words; i++)
        {
            word_t word = _get_word(src, srcindx+i*WORD_SIZE);
            _set_word(dst, dstindx+i*WORD_SIZE, word);
        }

      if (bits_in_last_word > 0)
      {
          word_t src_word = _get_word(src, srcindx+i*WORD_SIZE);
          word_t dst_word = _get_word(dst, dstindx+i*WORD_SIZE);

          word_t mask = bitmask64(bits_in_last_word);
          word_t word = bitmask_merge(src_word, dst_word, mask);

          _set_word(dst, dstindx+num_of_full_words*WORD_SIZE, word);
      }
    }
    else
    {
        // Work right to left
        for(i = 0; i < num_of_full_words; i++)
        {
            word_t word = _get_word(src, srcindx+length-(i+1)*WORD_SIZE);
            _set_word(dst, dstindx+length-(i+1)*WORD_SIZE, word);
        }

        if(bits_in_last_word > 0)
        {
            word_t src_word = _get_word(src, srcindx);
            word_t dst_word = _get_word(dst, dstindx);

            word_t mask = bitmask64(bits_in_last_word);
            word_t word = bitmask_merge(src_word, dst_word, mask);
            _set_word(dst, dstindx, word);
        }
    }

    // divon: removed due to thread safety - not needed, _set_word already masks top word if needed
    // _mask_top_word(dst);
}

// destination and source may be the same bit_array
// and src/dst regions may overlap
void bit_array_copy(BitArray* dst, bit_index_t dstindx,
                    const BitArray* src, bit_index_t srcindx,
                    bit_index_t length)
{
  ASSERTE (dstindx + length <= dst->nbits, "dstindx(%"PRIu64") + length(%"PRIu64") > dst->nbits(%"PRIu64")", dstindx, length, dst->nbits);
  ASSERTE (srcindx + length <= src->nbits, "srcindx(%"PRIu64") + length(%"PRIu64") > src->nbits(%"PRIu64")", srcindx, length, src->nbits);

  _array_copy(dst, dstindx, src, srcindx, length);

  DEBUG_VALIDATE(dst);
}

void bit_array_overlay (BitArray *overlaid_bitarr, BitArray *regular_bitarr, bit_index_t start, bit_index_t nbits)
{
    ASSERTE (start % 64 == 0, "start=%"PRIu64" must be a multiple of 64", start);
    ASSERTE (start + nbits <= regular_bitarr->nbits, "start(%"PRIu64") + nbits(%"PRIu64") <= regular_bitarr->nbits(%"PRIu64")",
            start, nbits, regular_bitarr->nbits);

    bit_index_t word_i = start / 64;
    *overlaid_bitarr = (BitArray){ .type         = BITARR_OVERLAY,
                                   .words        = &regular_bitarr->words[word_i],
                                   .nwords = roundup_bits2words64 (nbits),
                                   .nbits  = nbits };
} 

//
// Logic operators
//

// Destination can be the same as one or both of the sources
void bit_array_and(BitArray* dst, const BitArray* src1, const BitArray* src2)
{
  // Ensure dst array is big enough
  word_addr_t max_bits = MAX(src1->nbits, src2->nbits);
  bit_array_ensure_size_critical(dst, max_bits);

  word_addr_t min_words = MIN(src1->nwords, src2->nwords);

  word_addr_t i;

  for(i = 0; i < min_words; i++)
  {
    dst->words[i] = src1->words[i] & src2->words[i];
  }

  // Set remaining bits to zero
  for(i = min_words; i < dst->nwords; i++)
  {
    dst->words[i] = (word_t)0;
  }

  DEBUG_VALIDATE(dst);
}

// Destination can be the same as one or both of the sources
static void _logical_or_xor(BitArray* dst,
                            const BitArray* src1,
                            const BitArray* src2,
                            char use_xor)
{
  // Ensure dst array is big enough
  bit_array_ensure_size_critical(dst, MAX(src1->nbits, src2->nbits));

  word_addr_t min_words = MIN(src1->nwords, src2->nwords);
  word_addr_t max_words = MAX(src1->nwords, src2->nwords);

  word_addr_t i;

  if(use_xor)
  {
    for(i = 0; i < min_words; i++)
      dst->words[i] = src1->words[i] ^ src2->words[i];
  }
  else
  {
    for(i = 0; i < min_words; i++)
      dst->words[i] = src1->words[i] | src2->words[i];
  }

  // Copy remaining bits from longer src array
  if(min_words != max_words)
  {
    const BitArray* longer = src1->nwords > src2->nwords ? src1 : src2;

    for(i = min_words; i < max_words; i++)
    {
      dst->words[i] = longer->words[i];
    }
  }

  // Set remaining bits to zero
  size_t size = (dst->nwords - max_words) * sizeof(word_t);
  memset(dst->words + max_words, 0, size);

  DEBUG_VALIDATE(dst);
}

void bit_array_or(BitArray* dst, const BitArray* src1, const BitArray* src2)
{
  _logical_or_xor(dst, src1, src2, 0);
}

// Destination can be the same as one or both of the sources
void bit_array_xor(BitArray* dst, const BitArray* src1, const BitArray* src2)
{
  _logical_or_xor(dst, src1, src2, 1);
}

// If dst is longer than src, top bits are set to 1
void bit_array_not(BitArray* dst, const BitArray* src)
{
  bit_array_ensure_size_critical(dst, src->nbits);

  word_addr_t i;

  for(i = 0; i < src->nwords; i++)
  {
    dst->words[i] = ~(src->words[i]);
  }

  // Set remaining words to 1s
  for(i = src->nwords; i < dst->nwords; i++)
  {
    dst->words[i] = WORD_MAX;
  }

  _mask_top_word(dst);

  DEBUG_VALIDATE(dst);
}

/*
void bit_array_or_with (BitArray *dst, bit_index_t dst_start_bit, BitArray *src , bit_index_t src_start_bit, bit_index_t len)
{
    word_t dst_word, src_word;
    bit_index_t i, last_word_bits = len % WORD_SIZE; 
   
    for (i=0; i < len - (last_word_bits != 0); i += WORD_SIZE) {
        dst_word = _get_word (dst, dst_start_bit + i);
        src_word = _get_word (src, src_start_bit + i);
        _set_word (dst, dst_start_bit + i, dst_word | src_word);
    }

    // partial last word
    if (last_word_bits) {
        dst_word = _get_word (dst, dst_start_bit + i);
        src_word = _get_word (src, src_start_bit + i) & bitmask64 (last_word_bits);
        _set_word (dst, dst_start_bit + i, dst_word | src_word);
    }
}
*/
//
// Comparisons
//

// Compare two bit arrays by value stored, with index 0 being the Least
// Significant Bit (LSB). Arrays do not have to be the same length.
// Example: ..0101 (5) > ...0011 (3) [index 0 is LSB at right hand side]
// Sorts on length if all zeros: (0,0) < (0,0,0)
// returns:
//  >0 iff bitarr1 > bitarr2
//   0 iff bitarr1 == bitarr2
//  <0 iff bitarr1 < bitarr2
int bit_array_cmp(const BitArray* bitarr1, const BitArray* bitarr2)
{
  word_addr_t i;
  word_t word1, word2;
  word_addr_t min_words = bitarr1->nwords;

  // i is unsigned so break when i == 0
  if(bitarr1->nwords > bitarr2->nwords) {
    min_words = bitarr2->nwords;
    for(i = bitarr1->nwords-1; ; i--) {
      if(bitarr1->words[i]) return 1;
      if(i == bitarr2->nwords) break;
    }
  }
  else if(bitarr1->nwords < bitarr2->nwords) {
    for(i = bitarr2->nwords-1; ; i--) {
      if(bitarr2->words[i]) return 1;
      if(i == bitarr1->nwords) break;
    }
  }

  if(min_words == 0) return 0;

  for(i = min_words-1; ; i--)
  {
    word1 = bitarr1->words[i];
    word2 = bitarr2->words[i];
    if(word1 != word2) return (word1 > word2 ? 1 : -1);
    if(i == 0) break;
  }

  if(bitarr1->nbits == bitarr2->nbits) return 0;
  return bitarr1->nbits > bitarr2->nbits ? 1 : -1;
}

// Compare two bit arrays by value stored, with index 0 being the Most
// Significant Bit (MSB). Arrays do not have to be the same length.
// Example: 10.. > 01.. [index 0 is MSB at left hand side]
// Sorts on length if all zeros: (0,0) < (0,0,0)
// returns:
//  >0 iff bitarr1 > bitarr2
//   0 iff bitarr1 == bitarr2
//  <0 iff bitarr1 < bitarr2
int bit_array_cmp_big_endian(const BitArray* bitarr1, const BitArray* bitarr2)
{
  word_addr_t min_words = MAX(bitarr1->nwords, bitarr2->nwords);

  word_addr_t i;
  word_t word1, word2;

  for(i = 0; i < min_words; i++) {
    word1 = _reverse_word(bitarr1->words[i]);
    word2 = _reverse_word(bitarr2->words[i]);
    if(word1 != word2) return (word1 > word2 ? 1 : -1);
  }

  // Check remaining words. Only one of these loops will execute
  for(; i < bitarr1->nwords; i++)
    if(bitarr1->words[i]) return 1;
  for(; i < bitarr2->nwords; i++)
    if(bitarr2->words[i]) return -1;

  if(bitarr1->nbits == bitarr2->nbits) return 0;
  return bitarr1->nbits > bitarr2->nbits ? 1 : -1;
}

// compare bitarr with (bitarr2 << pos)
// bit_array_cmp(bitarr1, bitarr2<<pos)
// returns:
//  >0 iff bitarr1 > bitarr2
//   0 iff bitarr1 == bitarr2
//  <0 iff bitarr1 < bitarr2
int bit_array_cmp_words(const BitArray *arr1,
                        bit_index_t pos, const BitArray *arr2)
{
  if(arr1->nbits == 0 && arr2->nbits == 0)
  {
    return 0;
  }

  bit_index_t top_bit1 = 0, top_bit2 = 0;

  char arr1_zero = !bit_array_find_last_set_bit(arr1, &top_bit1);
  char arr2_zero = !bit_array_find_last_set_bit(arr2, &top_bit2);

  if(arr1_zero && arr2_zero) return 0;
  if(arr1_zero) return -1;
  if(arr2_zero) return 1;

  bit_index_t top_bit2_offset = top_bit2 + pos;

  if(top_bit1 != top_bit2_offset) {
    return top_bit1 > top_bit2_offset ? 1 : -1;
  }

  word_addr_t i;
  word_t word1, word2;

  for(i = top_bit2 / WORD_SIZE; i > 0; i--)
  {
    word1 = _get_word(arr1, pos + i * WORD_SIZE);
    word2 = arr2->words[i];

    if(word1 > word2) return 1;
    if(word1 < word2) return -1;
  }

  word1 = _get_word(arr1, pos);
  word2 = arr2->words[0];

  if(word1 > word2) return 1;
  if(word1 < word2) return -1;

  // return 1 if arr1[0..pos] != 0, 0 otherwise

  // Whole words
  word_addr_t num_words = pos / WORD_SIZE;

  for(i = 0; i < num_words; i++)
  {
    if(arr1->words[i] > 0)
    {
      return 1;
    }
  }

  word_offset_t bits_remaining = pos - num_words * WORD_SIZE;

  if(arr1->words[num_words] & bitmask64(bits_remaining))
  {
    return 1;
  }

  return 0;
}


//
// Reverse -- coords may wrap around
//

// No bounds checking
// length cannot be zero
static void _reverse_region(BitArray* bitarr,
                            bit_index_t start,
                            bit_index_t length)
{
  bit_index_t left = start;
  bit_index_t right = (start + length - WORD_SIZE) % bitarr->nbits;

  while(length >= 2 * WORD_SIZE)
  {
    // Swap entire words
    word_t left_word = _get_word_cyclic(bitarr, left);
    word_t right_word = _get_word_cyclic(bitarr, right);

    // reverse words individually
    left_word = _reverse_word(left_word);
    right_word = _reverse_word(right_word);

    // Swap
    _set_word_cyclic(bitarr, left, right_word);
    _set_word_cyclic(bitarr, right, left_word);

    // Update
    left = (left + WORD_SIZE) % bitarr->nbits;
    right = (right < WORD_SIZE ? right + bitarr->nbits : right) - WORD_SIZE;
    length -= 2 * WORD_SIZE;
  }

  word_t word, rev;

  if(length == 0)
  {
    return;
  }
  else if(length > WORD_SIZE)
  {
    // Words overlap
    word_t left_word = _get_word_cyclic(bitarr, left);
    word_t right_word = _get_word_cyclic(bitarr, right);

    rev = _reverse_word(left_word);
    right_word = _reverse_word(right_word);

    // fill left 64 bits with right word rev
    _set_word_cyclic(bitarr, left, right_word);

    // Now do remaining bits (length is between 1 and 64 bits)
    left += WORD_SIZE;
    length -= WORD_SIZE;

    word = _get_word_cyclic(bitarr, left);
  }
  else
  {
    word = _get_word_cyclic(bitarr, left);
    rev = _reverse_word(word);
  }

  rev >>= WORD_SIZE - length;
  word_t mask = bitmask64(length);

  word = bitmask_merge(rev, word, mask);

  _set_word_cyclic(bitarr, left, word);
}

void bit_array_reverse_region(BitArray* bitarr, bit_index_t start, bit_index_t len)
{
  assert(start + len <= bitarr->nbits);
  if(len > 0) _reverse_region(bitarr, start, len);
  DEBUG_VALIDATE(bitarr);
}

void bit_array_reverse(BitArray* bitarr)
{
  if(bitarr->nbits > 0) _reverse_region(bitarr, 0, bitarr->nbits);
  DEBUG_VALIDATE(bitarr);
}

// for each 2 bits in the src array, the dst array will contain those 2 bits in the reverse
// position, as well as transform them 00->11 11->00 01->10 10->01
// works on arrays with full words
void bit_array_reverse_complement_all (BitArray *dst, const BitArray *src,
                                       bit_index_t src_start_base, bit_index_t max_num_bases) // one can call this function piecemiel - eg divide it to threads
{
    if (!max_num_bases) max_num_bases = src->nbits / 2; // entire bitarray

    static const word_t rev_comp_table[256] = { // 00=A 01=C 10=G 11=T
        0b11111111, 0b10111111, 0b01111111, 0b00111111, 0b11101111, 0b10101111, 0b01101111, 0b00101111, 0b11011111, 0b10011111, 0b01011111, 0b00011111, 0b11001111, 0b10001111, 0b01001111, 0b00001111,
        0b11111011, 0b10111011, 0b01111011, 0b00111011, 0b11101011, 0b10101011, 0b01101011, 0b00101011, 0b11011011, 0b10011011, 0b01011011, 0b00011011, 0b11001011, 0b10001011, 0b01001011, 0b00001011,
        0b11110111, 0b10110111, 0b01110111, 0b00110111, 0b11100111, 0b10100111, 0b01100111, 0b00100111, 0b11010111, 0b10010111, 0b01010111, 0b00010111, 0b11000111, 0b10000111, 0b01000111, 0b00000111,
        0b11110011, 0b10110011, 0b01110011, 0b00110011, 0b11100011, 0b10100011, 0b01100011, 0b00100011, 0b11010011, 0b10010011, 0b01010011, 0b00010011, 0b11000011, 0b10000011, 0b01000011, 0b00000011,
        0b11111110, 0b10111110, 0b01111110, 0b00111110, 0b11101110, 0b10101110, 0b01101110, 0b00101110, 0b11011110, 0b10011110, 0b01011110, 0b00011110, 0b11001110, 0b10001110, 0b01001110, 0b00001110,
        0b11111010, 0b10111010, 0b01111010, 0b00111010, 0b11101010, 0b10101010, 0b01101010, 0b00101010, 0b11011010, 0b10011010, 0b01011010, 0b00011010, 0b11001010, 0b10001010, 0b01001010, 0b00001010,
        0b11110110, 0b10110110, 0b01110110, 0b00110110, 0b11100110, 0b10100110, 0b01100110, 0b00100110, 0b11010110, 0b10010110, 0b01010110, 0b00010110, 0b11000110, 0b10000110, 0b01000110, 0b00000110,
        0b11110010, 0b10110010, 0b01110010, 0b00110010, 0b11100010, 0b10100010, 0b01100010, 0b00100010, 0b11010010, 0b10010010, 0b01010010, 0b00010010, 0b11000010, 0b10000010, 0b01000010, 0b00000010,
        0b11111101, 0b10111101, 0b01111101, 0b00111101, 0b11101101, 0b10101101, 0b01101101, 0b00101101, 0b11011101, 0b10011101, 0b01011101, 0b00011101, 0b11001101, 0b10001101, 0b01001101, 0b00001101,
        0b11111001, 0b10111001, 0b01111001, 0b00111001, 0b11101001, 0b10101001, 0b01101001, 0b00101001, 0b11011001, 0b10011001, 0b01011001, 0b00011001, 0b11001001, 0b10001001, 0b01001001, 0b00001001,
        0b11110101, 0b10110101, 0b01110101, 0b00110101, 0b11100101, 0b10100101, 0b01100101, 0b00100101, 0b11010101, 0b10010101, 0b01010101, 0b00010101, 0b11000101, 0b10000101, 0b01000101, 0b00000101,
        0b11110001, 0b10110001, 0b01110001, 0b00110001, 0b11100001, 0b10100001, 0b01100001, 0b00100001, 0b11010001, 0b10010001, 0b01010001, 0b00010001, 0b11000001, 0b10000001, 0b01000001, 0b00000001,
        0b11111100, 0b10111100, 0b01111100, 0b00111100, 0b11101100, 0b10101100, 0b01101100, 0b00101100, 0b11011100, 0b10011100, 0b01011100, 0b00011100, 0b11001100, 0b10001100, 0b01001100, 0b00001100, 
        0b11111000, 0b10111000, 0b01111000, 0b00111000, 0b11101000, 0b10101000, 0b01101000, 0b00101000, 0b11011000, 0b10011000, 0b01011000, 0b00011000, 0b11001000, 0b10001000, 0b01001000, 0b00001000,
        0b11110100, 0b10110100, 0b01110100, 0b00110100, 0b11100100, 0b10100100, 0b01100100, 0b00100100, 0b11010100, 0b10010100, 0b01010100, 0b00010100, 0b11000100, 0b10000100, 0b01000100, 0b00000100,
        0b11110000, 0b10110000, 0b01110000, 0b00110000, 0b11100000, 0b10100000, 0b01100000, 0b00100000, 0b11010000, 0b10010000, 0b01010000, 0b00010000, 0b11000000, 0b10000000, 0b01000000, 0b00000000
    };

    ASSERTE (src->nbits == src->nwords * 64, "expecting full words, bitarr->nwords=%"PRIu64" and bitarr->num_of_bit=%"PRIu64,
            src->nwords, src->nbits);

    ASSERTE0 (src->nbits == dst->nbits && src->nwords == dst->nwords, "expecting src and dst to have the same number of bits and words");

    ASSERTE0 (src_start_base % 32 == 0 && max_num_bases % 32 == 0, "invalid start_base or num_bases");

  # define REV_COMP(w) ((rev_comp_table[(w)       & 0xff] << 56) | \
                        (rev_comp_table[(w >>  8) & 0xff] << 48) | \
                        (rev_comp_table[(w >> 16) & 0xff] << 40) | \
                        (rev_comp_table[(w >> 24) & 0xff] << 32) | \
                        (rev_comp_table[(w >> 32) & 0xff] << 24) | \
                        (rev_comp_table[(w >> 40) & 0xff] << 16) | \
                        (rev_comp_table[(w >> 48) & 0xff] << 8)  | \
                        (rev_comp_table[(w >> 56) & 0xff]        ))

    bit_index_t after_word = MIN (src->nwords, (src_start_base + max_num_bases) / 32); // 32 nucleotides in a word

    for (bit_index_t i=src_start_base / 32; i < after_word; i++)
        dst->words[dst->nwords-1 - i] = REV_COMP (src->words[i]);
}

//
// Shift left / right
//

// Shift towards MSB / higher index
void bit_array_shift_left(BitArray* bitarr, bit_index_t shift_dist, char fill)
{
  if(shift_dist >= bitarr->nbits)
  {
    fill ? bit_array_set_all(bitarr) : bit_array_clear_all(bitarr);
    return;
  }
  else if(shift_dist == 0)
  {
    return;
  }

  FillAction action = fill ? FILL_REGION : ZERO_REGION;

  bit_index_t cpy_length = bitarr->nbits - shift_dist;
  _array_copy(bitarr, shift_dist, bitarr, 0, cpy_length);
  _set_region(bitarr, 0, shift_dist, action);
}

// Shift towards LSB / lower index
void bit_array_shift_right(BitArray* bitarr, bit_index_t shift_dist, char fill)
{
  if(shift_dist >= bitarr->nbits)
  {
    fill ? bit_array_set_all(bitarr) : bit_array_clear_all(bitarr);
    return;
  }
  else if(shift_dist == 0)
  {
    return;
  }

  FillAction action = fill ? FILL_REGION : ZERO_REGION;

  bit_index_t cpy_length = bitarr->nbits - shift_dist;
  bit_array_copy(bitarr, 0, bitarr, shift_dist, cpy_length);

  _set_region(bitarr, cpy_length, shift_dist, action);
}

// Shift towards LSB / lower index
void bit_array_shift_right_shrink(BitArray* bitarr, bit_index_t shift_dist) // added by divon
{
  if(shift_dist >= bitarr->nbits)
  {
    bitarr->nbits = bitarr->nwords = 0;
    return;
  }
  else if(shift_dist == 0)
  {
    return;
  }

  bit_index_t cpy_length = bitarr->nbits - shift_dist;
  bit_array_copy(bitarr, 0, bitarr, shift_dist, cpy_length);

  bitarr->nbits -= shift_dist;
  bitarr->nwords = roundup_bits2words64 (bitarr->nbits);

  bit_array_clear_excess_bits_in_top_word (bitarr);
}

// removes flanking bits on boths sides, shrinking bitarr
void bit_array_remove_flanking (BitArray *bitarr, bit_index_t lsb_flanking, bit_index_t msb_flanking) // added by divon
{
    DEBUG_VALIDATE (bitarr); // catch a bug

    bit_index_t cpy_length = bitarr->nbits - lsb_flanking;
    bit_array_copy (bitarr, 0, bitarr, lsb_flanking, cpy_length);

    bitarr->nbits -= lsb_flanking + msb_flanking;
    bitarr->nwords = roundup_bits2words64 (bitarr->nbits);   
}

// shortens an array to a certain number of bits (divon)
void bit_array_truncate (BitArray* bitarr, bit_index_t new_num_of_bits)
{
  ASSERTE (new_num_of_bits <= bitarr->nbits, "expecting new_num_of_bits=%"PRIu64" to be <= bitarr->nbits=%"PRIu64,
           new_num_of_bits, bitarr->nbits);

  bitarr->nbits  = new_num_of_bits;
  bitarr->nwords = roundup_bits2words64 (new_num_of_bits);   
}

// create words - if the word is not aligned to the bitmap word boundaries, and hence spans 2 bitmap words, 
// we take the MSb's from the left word and the LSb's from the right word, to create shift_1 (divon)
static inline word_t _bit_array_combined_word (word_t word_a, word_t word_b, uint8_t shift)
{
    word_t first_word_msb  = word_a >> shift; // first word's MSb are the LSb of our combined word
    word_t second_word_lsb = (word_b & bitmask64 (shift)) << (64-shift); // second word's LSb are the MSb of our combined word
    return first_word_msb | second_word_lsb; 
}

// calculate the number of bits that are different between two bitarrays at arbitrary positions (divon)
uint32_t bit_array_manhattan_distance (const BitArray *bitarr_1, bit_index_t index_1, 
                                       const BitArray *bitarr_2, bit_index_t index_2, 
                                       bit_index_t len)
{
    const word_t *words_1 = &bitarr_1->words[index_1 >> 6];
    uint8_t shift_1 = index_1 & bitmask64(6); // word 1 contributes (64-shift) most-significant bits, word 2 contribute (shift) least significant bits

    const word_t *words_2 = &bitarr_2->words[index_2 >> 6];
    uint8_t shift_2 = index_2 & bitmask64(6); // word 1 contributes (64-shift) most-significant bits, word 2 contribute (shift) least significant bits

    word_t word=0;
    uint32_t nonmatches=0; 
    uint32_t nwords = roundup_bits2words64 (len);

    for (uint32_t i=0; i < nwords; i++) {

        word_t word_1 = shift_1 ? _bit_array_combined_word (words_1[i], words_1[i+1], shift_1) : words_1[i];
        word_t word_2 = shift_2 ? _bit_array_combined_word (words_2[i], words_2[i+1], shift_2) : words_2[i];
        
        word = word_1 ^ word_2; // xor the words - resulting in 1 in each position they differ and 0 where they're equal

        // we count the number of different bits. note that identical nucleotides results in 2 equal bits while
        // different nucleotides results in 0 or 1 equal bits (a 64 bit word contains 32 nucleotides x 2 bit each)
        nonmatches += __builtin_popcountll (word);
    }
    
    // remove non-matches due to the unused part of the last word
    if (len % 64)
        nonmatches -= __builtin_popcountll (word & ~bitmask64 (len % 64));

    return nonmatches; // this is the "manhattan distance" between the two vectors - number of non-matches
}

//
// Cycle
//

// Cycle towards index 0
void bit_array_cycle_right(BitArray* bitarr, bit_index_t cycle_dist)
{
  if(bitarr->nbits == 0)
  {
    return;
  }

  cycle_dist = cycle_dist % bitarr->nbits;

  if(cycle_dist == 0)
  {
    return;
  }

  bit_index_t len1 = cycle_dist;
  bit_index_t len2 = bitarr->nbits - cycle_dist;

  _reverse_region(bitarr, 0, len1);
  _reverse_region(bitarr, len1, len2);
  bit_array_reverse(bitarr);
}

// Cycle away from index 0
void bit_array_cycle_left(BitArray* bitarr, bit_index_t cycle_dist)
{
  if(bitarr->nbits == 0)
  {
    return;
  }

  cycle_dist = cycle_dist % bitarr->nbits;

  if(cycle_dist == 0)
  {
    return;
  }

  bit_index_t len1 = bitarr->nbits - cycle_dist;
  bit_index_t len2 = cycle_dist;

  _reverse_region(bitarr, 0, len1);
  _reverse_region(bitarr, len1, len2);
  bit_array_reverse(bitarr);
}


//
// Generally useful functions
//

// Generalised 'binary to string' function
// Adds bits to the string in order of lsb to msb
// e.g. 0b11010 (26 in decimal) would come out as "01011"
char* bit_array_word2str(const void *ptr, size_t nbits, char *str)
{
  const uint8_t* d = (const uint8_t*)ptr;

  size_t i;
  for(i = 0; i < nbits; i++)
  {
    uint8_t bit = (d[i/8] >> (i % 8)) & 0x1;
    str[i] = bit ? '1' : '0';
  }
  str[nbits] = '\0';
  return str;
}

char* bit_array_word2str_rev(const void *ptr, size_t nbits, char *str)
{
  const uint8_t* d = (const uint8_t*)ptr;

  size_t i;
  for(i = 0; i < nbits; i++)
  {
    uint8_t bit = (d[i/8] >> (i % 8)) & 0x1;
    str[nbits-1-i] = bit ? '1' : '0';
  }
  str[nbits] = '\0';
  return str;
}

void LTEN_bit_array (BitArray* bitarr)
{
#ifndef __LITTLE_ENDIAN__

  int64_t num_words = (bitarr->nbits >> 6) + ((bitarr->nbits & 0x3f) != 0);
  
  for (uint64_t i=0; i < num_words; i++)
    bitarr->words[i] = LTEN64 (bitarr->words[i]);

#endif
}
