// ------------------------------------------------------------------
//   sections.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef SECTIONS_INCLUDED
#define SECTIONS_INCLUDED

#include "genozip.h"
#include "digest.h"

// note: the numbering of the sections cannot be modified, for backward compatibility
typedef enum __attribute__ ((__packed__)) { // 1 byte
    SEC_NONE            = -1, // doesn't appear in the file 

    SEC_RANDOM_ACCESS   = 0,
    SEC_REFERENCE       = 1,
    SEC_REF_IS_SET      = 2,
    SEC_REF_HASH        = 3,
    SEC_REF_RAND_ACC    = 4,
    SEC_REF_CONTIGS     = 5,
    SEC_GENOZIP_HEADER  = 6, // SEC_GENOZIP_HEADER remains 6 as in v2-v5, to be able to read old versions' genozip header
    SEC_DICT_ID_ALIASES = 7,
    SEC_TXT_HEADER      = 8, 
    SEC_VB_HEADER       = 9,
    SEC_DICT            = 10, 
    SEC_B250            = 11, 
    SEC_LOCAL           = 12, 
    SEC_REF_ALT_CHROMS  = 13,
    SEC_STATS           = 14,
    SEC_BGZF            = 15, // optionally appears per component (txt header) and contains the uncompressed sizes of the source file bgzf block

    NUM_SEC_TYPES // fake section for counting
} SectionType;

// this data must be perfectly aligned with SectionType.
#define SECTIONTYPE_ABOUT {    \
    {"SEC_RANDOM_ACCESS",   sizeof (SectionHeader)              }, \
    {"SEC_REFERENCE",       sizeof (SectionHeaderReference)     }, \
    {"SEC_REF_IS_SET",      sizeof (SectionHeaderReference)     }, \
    {"SEC_REF_HASH",        sizeof (SectionHeaderRefHash)       }, \
    {"SEC_REF_RAND_ACC",    sizeof (SectionHeader)              }, \
    {"SEC_REF_CONTIGS",     sizeof (SectionHeader)              }, \
    {"SEC_GENOZIP_HEADER",  sizeof (SectionHeaderGenozipHeader) }, \
    {"SEC_DICT_ID_ALIASES", sizeof (SectionHeader)              }, \
    {"SEC_TXT_HEADER",      sizeof (SectionHeaderTxtHeader)     }, \
    {"SEC_VB_HEADER",       sizeof (SectionHeaderVbHeader)      }, \
    {"SEC_DICT",            sizeof (SectionHeaderDictionary)    }, \
    {"SEC_B250",            sizeof (SectionHeaderCtx)           }, \
    {"SEC_LOCAL",           sizeof (SectionHeaderCtx)           }, \
    {"SEC_REF_ALT_CHROMS",  sizeof (SectionHeader)              }, \
    {"SEC_STATS",           sizeof (SectionHeader)              }, \
    {"SEC_BGZF",            sizeof (SectionHeader)              }, \
}

// Section headers - big endian

#define GENOZIP_MAGIC 0x27052012

#pragma pack(1) // structures that are part of the genozip format are packed.

// section headers are encoded in Big Endian (see https://en.wikipedia.org/wiki/Endianness)
// the reason for selecting big endian is that I am developing on little endian CPU (Intel) so
// endianity bugs will be discovered more readily this way

// note: #pragma pack doesn't affect enums
typedef enum __attribute__ ((__packed__)) { BGZF_LIBDEFLATE, BGZF_ZLIB, NUM_BGZF_LIBRARIES } BgzfLibraryType; // constants for BGZF FlagsBgzf.library
typedef enum __attribute__ ((__packed__)) { STORE_NONE, STORE_INT, STORE_FLOAT, STORE_INDEX } StoreType; // values for SectionFlags.ctx.store

// goes into SectionHeader.flags
typedef union SectionFlags {  
    uint8_t flags;

    struct FlagsGenozipHeader {
        // note: if updating dts_* flags, update in zfile_compress_genozip_header, zfile_show_header too
        #define dts_ref_internal dt_specific // SAM, BAM: REF_INTERNAL was used for compressing (i.e. SAM file without reference)
        #define dts_paired       dt_specific // FASTQ: This z_file contains one or more pairs of FASTQs compressed with --pair (introduced v9.0.13)
        uint8_t dt_specific      : 1;        // this flag has a different meaning depending on the data_type, may be one of the above ^ 
        uint8_t aligner          : 1;        // our aligner was used to align sequences to the reference (always with FASTQ, sometimes with SAM)
        uint8_t txt_is_bin       : 1;        // Source file is binary (BAM)
        uint8_t bgzf             : 1;        // Reconstruct as BGZF (user may override) (determined by the last component)
        uint8_t adler            : 1;        // true if Adler32 is used, false if MD5 is used (>= v9) or (either MD5 or nothing) (v8)
    } genozip_header;

    struct FlagsBgzf {
        uint8_t has_eof_block    : 1;
        uint8_t level            : 4; // 0-12 for libdeflate or 0-9 for zlib level: 15 means unknown
        BgzfLibraryType library  : 3; // ignored if level=15 (introduced 9.0.16)
    } bgzf;

    struct FlagsCtx {
        StoreType store          : 2; // after reconstruction of a snip, store it in ctx.last_value
        uint8_t paired           : 1; // reconstruction of this context requires access to the same section from the same vb of the previous (paired) file
        uint8_t v8_container     : 1; // (canceled in 9 - files compressed with 8.0 will have this flag set for any context that contains 1 or more containers)
        uint8_t copy_param       : 1; // copy ctx.b250/local.param from SectionHeaderCtx.param
        uint8_t all_the_same     : 1; // the b250 data contains only one element, and should be used to reconstruct any number of snips from this context
        #define ctxs_dot_is_0    ctx_specific // used in dict_id_FORMAT_GT_SHARK_GT between 10.0.3 and 10.0.8
        uint8_t ctx_specific     : 1; // flag specific a context (introduced 10.0.3)
    } ctx;
    
} SectionFlags;

#define SECTION_FLAGS_NONE ((SectionFlags){ .flags = 0 })

typedef struct SectionHeader {
    uint32_t     magic; 
    uint32_t     compressed_offset;    // number of bytes from the start of the header that is the start of compressed data (sizeof header + header encryption padding)
    uint32_t     data_encrypted_len;   // = data_compressed_len + padding if encrypted, 0 if not
    uint32_t     data_compressed_len;
    uint32_t     data_uncompressed_len;
    uint32_t     vblock_i;             // VB with in file starting from 1 ; 0 for non-VB sections
    SectionType  section_type;         // 1 byte
    Codec        codec;                // 1 byte - primary codec in which this section is compressed
    Codec        sub_codec;            // 1 byte - sub codec, in case primary codec invokes another codec
    SectionFlags flags;                
} SectionHeader; 

typedef struct {
    SectionHeader h;
    uint8_t  genozip_version;
    EncryptionType encryption_type;   // one of ENC_TYPE_*
    uint16_t data_type;               // one of DATA_TYPE_*
    uint64_t uncompressed_data_size;  // data size of uncompressed` file, if uncompressed as a single file
    uint64_t num_items_bound;         // number of items in a bound file. "item" is data_type-dependent. For VCF, it is lines.
    uint32_t num_sections;            // number sections in this file (including this one)
    uint32_t num_components;          // number of txt bound components in this file (1 if no binding)
    Digest   digest_bound;
    uint8_t  password_test[16];       // short encrypted block - used to test the validy of a password
#define FILE_METADATA_LEN 72
    char     created[FILE_METADATA_LEN];  // nul-terminated metadata
    Digest   license_hash;            // MD5(license_num)
#define REF_FILENAME_LEN 256
    char     ref_filename[REF_FILENAME_LEN]; // external reference filename, null-terimated. ref_filename[0]=0 if there is no external reference.
    Digest   ref_file_md5;            // SectionHeaderGenozipHeader.digest_bound.md5 of the reference FASTA genozip file
} SectionHeaderGenozipHeader;

// this footer appears AFTER the genozip header data, facilitating reading the genozip header in reverse from the end of the file
typedef struct {
    uint64_t genozip_header_offset;
    uint32_t magic;
} SectionFooterGenozipHeader;

// The text file header section appears once in the file (or multiple times in case of bound file), and includes the txt file header 
typedef struct {
    SectionHeader h;
    uint64_t txt_data_size;    // number of bytes in the original txt file. 
    uint64_t num_lines;        // number of data (non-header) lines in the original txt file. Concat mode: entire file for first SectionHeaderTxtHeader, and only for that txt if not first
    uint32_t max_lines_per_vb; // upper bound on how many data lines a VB can have in this file
    Codec    codec;            // codec of original txt file (none, bgzf, gz, bz2...)
    uint8_t  codec_info[3];    // codec specific info: for CODEC_BGZF, these are the LSB, 2nd-LSB, 3rd-LSB of the source BGZF-compressed file size
    Digest   digest_single;    // digest of original single txt file. non-0 only if this genozip file is a result of binding. MD5 if --md5 or Adler32 otherwise. 0 if compressed in v8 without --md5. 
    Digest   digest_header;    // MD5 or Adler32 of header
#define TXT_FILENAME_LEN 256
    char     txt_filename[TXT_FILENAME_LEN]; // filename of this single component. without path, 0-terminated. always in base form like .vcf or .sam, even if the original is compressed .vcf.gz or .bam
} SectionHeaderTxtHeader; 

typedef struct {
    SectionHeader h;            
    uint32_t first_line;       // line (starting from 1) of this vblock in the single txt file 
                               // if this value is 0, then this is the terminating section of the file. after it is either the global area or a TXT Header section of the next bound file 
    uint32_t num_lines;        // number of records in this vblock 
    uint32_t vb_data_size;     // size of vblock as it appears in the source file 
    uint32_t z_data_bytes;     // total bytes of this vblock in the genozip file including all sections and their headers 
    uint32_t longest_line_len; // length of the longest line in this vblock 
    Digest   digest_so_far;    // partial calculation of MD5 or Adler32 up to and including this VB 
} SectionHeaderVbHeader; 

typedef struct {
    SectionHeader h;           
    uint32_t num_snips;        // number of items in dictionary
    DictId   dict_id;           
} SectionHeaderDictionary; 

// LT_* values are consistent with BAM optional 'B' types (and extend them)
typedef enum __attribute__ ((__packed__)) { // 1 byte
    LT_TEXT      = 0,
    LT_INT8      = 1,    
    LT_UINT8     = 2,
    LT_INT16     = 3,
    LT_UINT16    = 4,
    LT_INT32     = 5,
    LT_UINT32    = 6,
    LT_INT64     = 7,   // ffu
    LT_UINT64    = 8,   // ffu
    LT_FLOAT32   = 9,   // ffu
    LT_FLOAT64   = 10,  // ffu
    LT_SEQUENCE  = 11,  // length of data extracted is determined by vb->seq_len
    LT_BITMAP    = 12,  // a bitmap
    LT_CODEC     = 13,  // codec specific type with its codec specific reconstructor
    LT_UINT8_TR  = 14,  // transposed array - number of columns in original array is in param (up to 255 columns)
    LT_UINT16_TR = 15,  // "
    LT_UINT32_TR = 16,  // "
    LT_UINT64_TR = 17,  // "
    NUM_LOCAL_TYPES
} LocalType;

typedef struct LocalTypeDesc {
    const char *name;
    const char sam_char;
    unsigned width;
    bool is_signed;
    BgEnBuf file_to_native;
} LocalTypeDesc;

extern const LocalTypeDesc lt_desc[NUM_LOCAL_TYPES];
#define LOCALTYPE_DESC { \
/*   name   sam  wid signed file_to_native */ \
   { "TXT", 0,   1,  0,     0                        }, \
   { "I8 ", 'c', 1,  1,     BGEN_deinterlace_d8_buf  }, \
   { "U8 ", 'C', 1,  0,     BGEN_u8_buf              }, \
   { "I16", 's', 2,  1,     BGEN_deinterlace_d16_buf }, \
   { "U16", 'S', 2,  0,     BGEN_u16_buf             }, \
   { "I32", 'i', 4,  1,     BGEN_deinterlace_d32_buf }, \
   { "U32", 'I', 4,  0,     BGEN_u32_buf             }, \
   { "I64", 0,   8,  1,     BGEN_deinterlace_d64_buf }, \
   { "U64", 0,   8,  0,     BGEN_u64_buf             }, \
   { "F32", 'f', 4,  0,     0                        }, \
   { "F64", 0,   8,  0,     0                        }, \
   { "SEQ", 0,   1,  0,     0                        }, \
   { "BMP", 0,   8,  0,     0                        }, \
   { "COD", 0,   1,  0,     0                        }, \
   { "T8 ", 0,   1,  0,     BGEN_transpose_u8_buf    }, \
   { "T16", 0,   2,  0,     BGEN_transpose_u16_buf   }, \
   { "T32", 0,   4,  0,     BGEN_transpose_u32_buf   }, \
   { "T64", 0,   8,  0,     BGEN_transpose_u64_buf   }, \
}

typedef struct {
    SectionHeader h;
    LocalType ltype; // used by SEC_LOCAL: goes into ctx.ltype - type of data for the ctx.local buffer
    uint8_t param;   // Two options: 1. goes into ctx.b250/local.param if flags.copy_param. 
                     //              2. starting 9.0.11 for ltype=LT_BITMAP: number of unused bits in top bitarray word
    uint8_t ffu[2];
    DictId dict_id;           
} SectionHeaderCtx;         

// two ways of storing a range:
// uncompacted - we will have one section, SEC_REFERENCE, containing the data, and first/last_pos containing the coordinates of this range
// compacting we will have 2 sections:
// - SEC_REF_IS_SET - containing a bitmap (1 bit per base), and chrom,first/last_pos containing the coordinates of this range
// - SEC_REFERENCE - containing the compacted range (i.e. excluding bases that have a "0" in the bitmap), 
//   with chrom,first_pos same as SEC_REF_IS_SET and last_pos set so that (last_pos-first_pos+1) = number of '1' bits in SEC_REF_IS_SET
// SEC_REFERENCE (in both cases) contains 2 bits per base, and SEC_REF_IS_SET contains 1 bit per location.
typedef struct {
    SectionHeader h;
    PosType pos;               // first pos within chrom (1-based) of this range         
    PosType gpos;              // first pos within genome (0-based) of this range
    uint32_t num_bases;        // number of bases (nucleotides) in this range
    uint32_t chrom_word_index; // index in context->word_list of the chrom of this reference range    
} SectionHeaderReference;

typedef struct {
    SectionHeader h;
    uint8_t num_layers;        // total number of layers
    uint8_t layer_i;           // layer number of this section (0=base layer, with the most bits)
    uint8_t layer_bits;        // number of bits in layer
    uint8_t ffu;
    uint32_t start_in_layer;   // start index within layer
} SectionHeaderRefHash;

// the data of SEC_SECTION_LIST is an array of the following type, as is the z_file->section_list_buf
typedef struct SectionListEntry {
    uint64_t offset;           // offset of this section in the file
    DictId dict_id;            // used if this section is a DICT, LOCAL or a B250 section
    uint32_t vblock_i;
    SectionType section_type;  // 1 byte
    uint8_t unused[3];         
} SectionListEntry;

// the data of SEC_RANDOM_ACCESS is an array of the following type, as is the z_file->ra_buf and vb->ra_buf
// we maintain one RA entry per vb per every chrom in the the VB
typedef struct RAEntry {
    uint32_t vblock_i;           // the vb_i in which this range appears
    WordIndex chrom_index;       // before merge: node index into chrom context nodes, after merge - word index in CHROM dictionary
    PosType min_pos, max_pos;    // POS field value of smallest and largest POS value of this chrom in this VB (regardless of whether the VB is sorted)
} RAEntry; 

// the data of SEC_REF_RAND_ACC is an array of the following type
typedef struct RefContig {
    CharIndex char_index;        // char index in CHROM dictionary of this contig
    uint32_t snip_len;
    WordIndex chrom_index;
    PosType min_pos, max_pos;    // POS field value of smallest and largest POS value of this contig
    PosType gpos;                // The GPOS in genome matching min_pos in contig.

    // Properties, as they appear in the DESC line of the reference FASTA (character arrays are 0-padded)
    char AC[16];
    char AS[16];
    char rl[32];
    uint64_t gi;
    uint64_t LN;
    Digest M5;
} RefContig; 

// the data of SEC_REF_ALT_CHROMS
typedef struct { WordIndex txt_chrom, ref_chrom; } AltChrom; 

#pragma pack()

// zip stuff
extern void sections_add_to_list (VBlockP vb, const SectionHeader *header);
extern void sections_list_concat (VBlockP vb);

// piz stuff
extern const SectionListEntry *sections_get_first_section_of_type (SectionType st, bool soft_fail);
extern bool sections_get_next_section_of_type2 (const SectionListEntry **sl_ent, SectionType st1, SectionType st2, bool must_be_next_section, bool seek);
#define sections_get_next_section_of_type(sl_ent,st,must_be_next_section,seek) sections_get_next_section_of_type2(sl_ent,st,SEC_NONE,must_be_next_section,seek)

extern uint32_t sections_count_sections (SectionType st);
extern const SectionListEntry *sections_vb_first (uint32_t vb_i, bool soft_fail);
extern void sections_get_prev_component_vb_i (const SectionListEntry *sl, uint32_t *prev_file_first_vb_i, uint32_t *prev_file_last_vb_i);

extern void BGEN_sections_list(void);
extern const char *st_name (SectionType sec_type);
extern SectionType sections_st_by_name (char *name);
extern uint32_t st_header_size (SectionType sec_type);

extern void sections_show_gheader (const SectionHeaderGenozipHeader *header);

extern void sections_get_refhash_details (uint32_t *num_layers, uint32_t *base_layer_bits);

extern const char *lt_name (LocalType lt);

#endif
