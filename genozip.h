// ------------------------------------------------------------------
//   genozip.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef GENOZIP_INCLUDED
#define GENOZIP_INCLUDED

#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#ifndef _MSC_VER // Microsoft compiler
#include <inttypes.h>
#include <unistd.h> 
#else
#include "compatibility/visual_c_stdint.h"
#include "compatibility/visual_c_unistd.h"
#include "compatibility/visual_c_misc_funcs.h"
#endif
#include <string.h> // must be after inttypes

// -----------------
// system parameters
// -----------------
#define GENOZIP_EXT ".genozip"
#define GENOZIP_URL "https://github.com/divonlan/genozip"

#define MAX_POS ((PosType)UINT32_MAX) // maximum allowed value for POS (constraint: fit into uint32 ctx.local). Note: in SAM the limit is 2^31-1

#define MAX_SUBFIELDS 2048  // Maximum number of items in a Container (for example: VCF/FORMAT fields, VCF/INFO fields GVF/ATTR fields, SAM/OPTIONAL fields etc). This value can be increased subject to MAX_DICTS<=253.

#define DEFAULT_MAX_THREADS 8 // used if num_cores is not discoverable and the user didn't specifiy --threads

// ------------------------------------------------------------------------------------------------------------------------
// pointers used in header files - so we don't need to include the whole .h (and avoid cyclicity and save compilation time)
// ------------------------------------------------------------------------------------------------------------------------
typedef struct VBlock *VBlockP;
typedef const struct VBlock *ConstVBlockP;
typedef struct File *FileP;
typedef const struct File *ConstFileP;
typedef struct Buffer *BufferP;
typedef const struct Buffer *ConstBufferP;
typedef struct Container *ContainerP;
typedef const struct Container *ConstContainerP;
typedef struct Context *ContextP;
typedef const struct Context *ConstContextP;
typedef struct CtxNode *MtfNodeP;
typedef const struct CtxNode *ConstMtfNodeP;
typedef struct SectionHeader *SectionHeaderP;
typedef struct SectionListEntry *SectionListEntryP;
typedef const struct SectionListEntry *ConstSectionListEntryP;
typedef struct Range *RangeP;
typedef struct BitArray *BitArrayP;
typedef const struct BitArray *ConstBitArrayP;
typedef struct RAEntry *RAEntryP;
typedef const struct RAEntry *ConstRAEntryP;
typedef union LastValueType *LastValueTypeP;
typedef struct Mutex *MutexP;

typedef void BgEnBufFunc (BufferP buf, uint8_t *lt); // we use uint8_t instead of LocalType (which 1 byte) to avoid #including sections.h
typedef BgEnBufFunc (*BgEnBuf);

typedef enum { EXE_GENOZIP, EXE_GENOUNZIP, EXE_GENOLS, EXE_GENOCAT, NUM_EXE_TYPES } ExeType;

// IMPORTANT: DATATYPES GO INTO THE FILE FORMAT - THEY CANNOT BE CHANGED
typedef enum { DT_NONE=-1, // used in the code logic, never written to the file
               DT_REF=0, DT_VCF=1, DT_SAM=2, DT_FASTQ=3, DT_FASTA=4, DT_GFF3=5, DT_ME23=6, // these values go into SectionHeaderGenozipHeader.data_type
               DT_BAM=7, DT_BCF=8, DT_GENERIC=9, DT_PHYLIP=10, NUM_DATATYPES 
             } DataType; 
        
#pragma pack(1) // structures that are part of the genozip format are packed.
#define DICT_ID_LEN    ((int)sizeof(uint64_t))    // VCF/GFF3 specs don't limit the field name (tag) length, we limit it to 8 chars. zero-padded. (note: if two fields have the same 8-char prefix - they will just share the same dictionary)
typedef union DictId {
    uint64_t num;            // num is just for easy comparisons - it doesn't have a numeric value and endianity should not be changed
    uint8_t id[DICT_ID_LEN]; // \0-padded IDs 
    uint16_t map_key;        // we use the first two bytes as they key into vb/z_file->dict_id_mapper
} DictId;
#pragma pack()

typedef uint16_t DidIType;   // index of a context in vb->contexts or z_file->contexts / a counter of contexts
#define DID_I_NONE ((DidIType)-1)

typedef uint64_t CharIndex;   // index within dictionary
typedef int32_t WordIndex;    // used for word and node indices
typedef int64_t PosType;      // used for position coordinate within a genome

typedef union LastValueType { // 64 bit
    int64_t i;
    double f;
} LastValueType;

// global parameters - set before any thread is created, and never change
extern uint32_t global_max_threads;
extern const char *global_cmd;            // set once in main()
extern ExeType exe_type;

// global files (declared in file.c)
extern FileP z_file, txt_file; 

// IMPORTANT: This is part of the genozip file format. Also update codec.h/codec_args
// If making any changes, update arrays in 1. codec.h 2. txtfile_estimate_txt_data_size
typedef enum __attribute__ ((__packed__)) { // 1 byte
    CODEC_UNKNOWN=0, 
    CODEC_NONE=1, CODEC_GZ=2, CODEC_BZ2=3, CODEC_LZMA=4, CODEC_BSC=5, // internal compressors
    
    CODEC_ACGT    = 10, CODEC_XCGT = 11, // compress sequence data - slightly better compression LZMA, 20X faster (these compress NONREF and NONREF_X respectively)
    CODEC_HAPM    = 12, // compress a VCF haplotype matrix - transpose, then sort lines, then bz2. 
    CODEC_DOMQ    = 13, // compress SAM/FASTQ quality scores, if dominated by a single character
    CODEC_GTSHARK = 14, // compress VCF haplotype matrix with gtshark
    CODEC_PBWT    = 15, // compress VCF haplotype matrix with pbwt
    
    // external compressors (used by executing an external application)
    CODEC_BGZF=20, CODEC_XZ=21, CODEC_BCF=22, 
    V8_CODEC_BAM=23,    // in v8 BAM was a codec which was compressed using samtools as external compressor. since v9 it is a full data type, and no longer a codec.
    CODEC_CRAM=24, CODEC_ZIP=25,  

    NUM_CODECS
} Codec; 

// PIZ / ZIP inspired by "We don't sell Duff. We sell Fudd"
typedef enum { NO_COMMAND=-1, ZIP='z', PIZ='d' /* this is unzip */, LIST='l', LICENSE='L', VERSION='V', HELP='h', TEST_AFTER_ZIP } CommandType;
extern CommandType command, primary_command;

// external vb - used when an operation is needed outside of the context of a specific variant block;
extern VBlockP evb;

// macros
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b) )
#define MAX(a, b) (((a) > (b)) ? (a) : (b) )
#endif

#define SWAP(a,b) do { typeof(a) tmp = a; a = b; b = tmp; } while(0)

// we defined these ourselves (normally defined in stdbool.h), as not always available on all platforms (namely issues with Docker Hub)
#ifndef __bool_true_false_are_defined
typedef _Bool bool;
#define true 1
#define false 0
#endif

#define SAVE_VALUE(flag) typeof(flag) save_##flag = flag 
#define TEMP_VALUE(flag,temp) typeof(flag) save_##flag = flag ; flag = (temp)
#define RESET_VALUE(flag) SAVE_VALUE(flag) ; flag=(typeof(flag))(uint64_t)0
#define RESTORE_VALUE(flag) flag = save_##flag

// returns true if new_value has been set
#define SPECIAL_RECONSTRUCTOR(func) bool func (VBlockP vb, ContextP ctx, const char *snip, unsigned snip_len, LastValueTypeP new_value, bool reconstruct)
#define SPECIAL(dt,num,name,func) \
    extern SPECIAL_RECONSTRUCTOR(func); \
    enum { dt##_SPECIAL_##name = (num + 32) }; // define constant - +32 to make it printable ASCII that can go into a snip 

// translations of Container items - when genounzipping one format translated to another
typedef uint8_t TranslatorId;
// note on return value: self-translators (translator only item[0] need to return the change in prefixes_len, the return value of other translators is ignored
#define TRANSLATOR_FUNC(func) int32_t func(VBlockP vb, ContextP ctx, char *reconstructed, int32_t reconstructed_len)
#define TRANSLATOR(src_dt,dst_dt,num,name,func)\
    extern TRANSLATOR_FUNC(func); \
    enum { src_dt##2##dst_dt##_##name = num }; // define constant

#define CONTAINER_FILTER_FUNC(func) bool func(VBlockP vb, DictId dict_id, ConstContainerP con, unsigned rep, int item)
#define CONTAINER_CALLBACK(func) void func(VBlockP vb, DictId dict_id, unsigned rep, char *reconstructed, int32_t reconstructed_len)

#define TXTHEADER_TRANSLATOR(func) void func (BufferP txtheader_buf)

// IMPORTANT: This is part of the genozip file format. 
typedef enum __attribute__ ((__packed__)) { // 1 byte
    ENC_NONE   = 0,
    ENC_AES256 = 1,
    NUM_ENCRYPTION_TYPES
} EncryptionType;

#define ENC_NAMES { "NO_ENC", "AES256" }

#define COMPRESSOR_CALLBACK(func) \
void func (VBlockP vb, uint32_t vb_line_i, \
           char **line_data, uint32_t *line_data_len,\
           uint32_t maximum_size); // might be less than the size available if we're sampling in zip_assign_best_codec()
#define CALLBACK_NO_SIZE_LIMIT 0xffffffff // for maximum_size

typedef COMPRESSOR_CALLBACK (LocalGetLineCB);

#define SAFE_ASSIGN(addr,char_val) /* we are careful to evaluate addr, char_val only once, lest they contain eg ++ */ \
    char *__addr = (char*)(addr); \
    char __save = *__addr; \
    *__addr = (char_val)

#define SAFE_RESTORE *__addr = __save

// sanity checks
extern void main_exit (bool show_stack, bool is_error);
#define exit_on_error(show_stack) main_exit (show_stack, true)
#define exit_ok main_exit (false, false)

#define iputc(c)                             fputc ((c), info_stream ? info_stream : stderr)
#define iprintf(format, ...)                 do { fprintf (info_stream ? info_stream : stderr, (format), __VA_ARGS__); fflush (info_stream); } while(0)
#define iprint0(str)                         do { fprintf (info_stream ? info_stream : stderr, (str)); fflush (info_stream); } while(0)

// check for a user error
#define ASSINP(condition, format, ...)       do { if (!(condition)) { fprintf (stderr, "\n%s: ", global_cmd); fprintf (stderr, format, __VA_ARGS__); fprintf (stderr, "\n"); exit_on_error(false); }} while(0)
#define ASSINP0(condition, string)           do { if (!(condition)) { fprintf (stderr, "\n%s: %s\n", global_cmd, string); exit_on_error(false); }} while(0)
#define ABORTINP(format, ...)                do { fprintf (stderr, "\n%s: ", global_cmd); fprintf (stderr, format, __VA_ARGS__); fprintf (stderr, "\n"); exit_on_error(false);} while(0)
#define ABORTINP0(string)                    do { fprintf (stderr, "\n%s: %s\n", global_cmd, string); exit_on_error(false);} while(0)

// check for a bug - prints stack
#define ASSERTE(condition, format, ...)      do { if (!(condition)) { fprintf (stderr, "\nError in %s:%u: ", __FUNCTION__, __LINE__); fprintf (stderr, format, __VA_ARGS__); fprintf (stderr, "\n"); exit_on_error(true); }} while(0)
#define ASSERTE0(condition, string)          do { if (!(condition)) { fprintf (stderr, "\nError in %s:%u: %s\n", __FUNCTION__, __LINE__, string); exit_on_error(true); }} while(0)
#define ASSERTW(condition, format, ...)      do { if (!(condition) && !flag.quiet) { fprintf (stderr, "\n"); fprintf (stderr, format, __VA_ARGS__); fprintf (stderr, "\n"); }} while(0)
#define ASSERTW0(condition, string)          do { if (!(condition) && !flag.quiet) { fprintf (stderr, "\n%s\n", string); } } while(0)
#define RETURNW(condition, ret, format, ...) do { if (!(condition)) { if (!flag.quiet) { fprintf (stderr, "\n"); fprintf (stderr, format, __VA_ARGS__); fprintf (stderr, "\n"); } return ret; }} while(0)
#define RETURNW0(condition, ret, string)     do { if (!(condition)) { if (!flag.quiet) { fprintf (stderr, "\n%s\n", string); } return ret; } } while(0)
#define ABORT(format, ...)                   do { fprintf (stderr, "\n"); fprintf (stderr, format, __VA_ARGS__); fprintf (stderr, "\n"); exit_on_error(true);} while(0)
#define ABORT_R(format, ...) /*w/ return 0*/ do { fprintf (stderr, "\n"); fprintf (stderr, format, __VA_ARGS__); fprintf (stderr, "\n"); exit_on_error(true); return 0;} while(0)
#define ABORT0(string)                       do { fprintf (stderr, "\n%s\n", string); exit_on_error(true);} while(0)
#define ABORT0_R(string)                     do { fprintf (stderr, "\n%s\n", string); exit_on_error(true); return 0; } while(0)
#define WARN(format, ...)                    do { if (!flag.quiet) { fprintf (stderr, "\n"); fprintf (stderr, format, __VA_ARGS__); fprintf (stderr, "\n"); } } while(0)
#define WARN0(string)                        do { if (!flag.quiet) fprintf (stderr, "\n%s\n", string); } while(0)
#define ASSERTGOTO(condition, format, ...)   do { if (!(condition)) { fprintf (stderr, "\n"); fprintf (stderr, format, __VA_ARGS__); fprintf (stderr, "\n"); goto error; }} while(0)

#endif