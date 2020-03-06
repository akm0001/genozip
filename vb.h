// ------------------------------------------------------------------
//   vb.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef VB_INCLUDED
#define VB_INCLUDED

#include "genozip.h"
#include "buffer.h"
#include "profiler.h"
#include "aes.h"
#include "move_to_front.h"
#include "vcf_header.h"

typedef enum { PHASE_UNKNOWN      = '-',
               PHASE_HAPLO        = '1',
               PHASE_PHASED       = '|',
               PHASE_NOT_PHASED   = '/',
               PHASE_MIXED_PHASED = '+'    } PhaseType;

typedef struct {
    uint8_t num_subfields; // (uint8_t)NIL if this mapper is not defined
    uint8_t did_i[MAX_SUBFIELDS]; // array in the order the subfields appears in FORMAT or INFO - each an index into vb->mtf_ctx[]
} SubfieldMapperZip;

#define MAPPER_CTX(mapper,sf) (((mapper)->did_i[(sf)] != (uint8_t)NIL) ? &vb->mtf_ctx[(mapper)->did_i[(sf)]] : NULL)

// IMPORTANT: if changing fields in DataLine, also update vb_release_vb
typedef struct {

    uint32_t line_i;         // line in VCF file (starting from 1)

    // initially, data from vcf file line, later segregated to components "stored" in the overlay buffers below
    Buffer line;             

    // the following 3 buffers are overlay buffers onto line, so that they don't consume memory
    Buffer genotype_data;    // \t separated genotype data for the line. last one has \t too. no \0. exists if any variant in the variant blck has FORMAT other that "GT"
    Buffer haplotype_data;   // length=ploidy*num_samples. exists if the GT subfield exists in any variant in the variant block

    Buffer phase_data;       // used only if phase is mixed. length=num_samples. exists if haplotype data exists and ploidy>=2
    PhaseType phase_type;    // phase type of this line
    
    bool has_haplotype_data; // FORMAT field contains GT
    bool has_genotype_data;  // FORMAT field contains subfields other than GT

    uint32_t format_mtf_i;   // the mtf_i into mtf_ctx[FORMAT].mtf and also format_mapper_buf that applies to this line. Data on the fields is in vb->format_mapper_buf[dl.format_mtf_i]
    uint32_t info_mtf_i;     // the mtf_i into mtx_ctx[INFO].mtf and also iname_mapper_buf that applies to this line. Data on the infos is in  vb->iname_mapper_buf[dl.info_mtf_i]. either SubfieldInfoMapperPiz or SubfieldInfoZip

    // backward compatability with genozip v1
    Buffer v1_variant_data;

} DataLine;

// IMPORTANT: if changing fields in VariantBlock, also update vb_release_vb
typedef struct variant_block_ {

    int id;                    // id of vb within the vb pool (-1 is the external vb)

    FileP vcf_file, z_file;    // pointers to objects that span multiple VBs

    // memory management
    Buffer buffer_list;        // a buffer containing an array of pointers to all buffers allocated for this VB (either by the I/O thread or its compute thread)

    bool ready_to_dispatch;    // line data is read, and dispatcher can dispatch this VB to a compute thread
    bool is_processed;         // thread completed processing this VB - it is ready for outputting
    bool in_use;               // this vb is in use
        
    DataLine *data_lines;      // if allocated, this array is of length global_max_lines_per_vb. for ZIP this is determined by the number of samples, and for PIZ by the SectionHeaderVCFHeader.max_lines_per_vb
    uint32_t num_lines;        // number of lines in this variant block
    uint32_t first_line;       // line number in VCF file (counting from 1), of this variant block
    uint32_t variant_block_i;  // number of variant block within VCF file

    // tracking execution
    int32_t vb_data_size;      // size of variant block as it appears in the source file
    uint32_t max_gt_line_len;  // length of longest gt line in this vb after segregation

    ProfilerRec profile;

    // charactaristics of the data
    uint16_t ploidy;
    
    uint32_t num_sample_blocks;
    uint32_t num_samples_per_block; // except last sample block that may have less
    uint32_t num_haplotypes_per_line;
    bool has_genotype_data;    // if any variant has genotype data, then the block is considered to have it
    bool has_haplotype_data;   // ditto for haplotype data
    PhaseType phase_type;      // phase type of this variant block

    // random access, chrom and pos
    Buffer ra_buf;             // ZIP only: array of RAEntry - copied to z_file at the end of each vb compression, then written as a SEC_RANDOM_ACCESS section at the end of the genozip file        
    int32_t last_pos;          // value of POS field of the previous line, to do delta encoding - we do delta encoding even across chromosome changes
    RAEntry *curr_ra_ent;      // these two are used by random_access_update_chrom/random_access_update_pos to sync between them
    bool curr_ra_ent_is_initialized; 

    // working memory for segregate - we segregate a line components into these buffers, and when done
    // we copy it back to DataLine - the buffers overlaying the line field
    Buffer line_variant_data;  // string terminated by a newline. len includes the newline. (used for decompressing)
    Buffer line_gt_data;       // \t separated genotype data for the line. last one has \t too. no \0. exists if any variant in the variant blck has FORMAT other that "GT"
    Buffer line_ht_data;       // length=ploidy*num_samples. exists if the GT subfield exists in any variant in the variant block
    Buffer line_phase_data;    // used only if phase is mixed. length=num_samples. exists if haplotype data exists and ploidy>=2

    // crypto stuff
    Buffer spiced_pw;  // used by crypt_generate_aes_key()
    uint8_t aes_round_key[240];// for 256 bit aes
    uint8_t aes_iv[AES_BLOCKLEN];
    int bi;

    // section data - ready to compress
    Buffer haplotype_permutation_index;
    Buffer haplotype_permutation_index_squeezed; // used by piz to unsqueeze the index and zfile to compress it
    Buffer optimized_gl_dict;  // GL dictionary data after extra optimization

    // these are Buffer arrays of size vb->num_sample_blocks allocated once when used for the first time and never freed,
    // unless a subsequent VCF file has more sample blocks, in which case they are first destroyed by vb_cleanup_memory().
    // Each buffer is a string of the data as written to the GENOZIP file

    Buffer *haplotype_sections_data;  // ZIP & PIZ: this is the haplotype character for each haplotype in the transposed sample group
    Buffer *phase_sections_data;      // ZIP & PIZ: this is the phase character for each genotype in the sample group
    Buffer *genotype_sections_data;   // PIZ only:  each entry is a sample block, scanned columns first, each cell containing num_subfields indices (in base250 - 1 to 5 bytes each) into the subfield dictionaries
    Buffer genotype_one_section_data; // for zip we need only one section data
    
    // compresssed file data 
    Buffer z_data;                    // all headers and section data as read from disk

    int16_t z_next_header_i;          // next header of this VB to be encrypted or decrypted

    Buffer z_section_headers;         // PIZ only: an array of unsigned offsets of section headers within z_data

    Buffer gt_sb_line_starts_buf,     // used by zip_get_genotype_vb_start_len 
           gt_sb_line_lengths_buf,
           genotype_section_lens_buf; 

    Buffer helper_index_buf;          // used by zip_do_haplotypes

    Buffer compressed;                // used by various zfile functions
 
    Buffer ht_columns_data;           // used by piz_get_ht_permutation_lookups

    Buffer sample_iterator;           // an array of SnipIterator - one for each sample. used for iterate on gt samples to get one snip at a time 
     
    Buffer format_info_buf;           // used by piz, contains a FormatInfo entry for each unique FORMAT snip in a vb 

    Buffer column_of_zeros;           // used by piz_get_ht_columns_data

    // dictionaries stuff - we use them for 1. subfields with genotype data, 2. fields 1-9 of the VCF file 3. infos within the info field
    uint32_t num_dict_ids;            // total number of dictionaries of all types
    uint32_t num_format_subfields;    // number of format subfields in this VB. num_subfields <= num_dict_ids-9.

    uint32_t num_info_subfields;      // e.g. if one inames is I1=I2=I3 and another one is I2=I3=I4= then we have two inames
                                      // entries in the mapper, which have we have num_info_subfields=4 (I1,I2,I3,I4) between them    
    MtfContext mtf_ctx[MAX_DICTS];    

    Buffer iname_mapper_buf;           // an array of type SubfieldMapperZip - one entry per entry in vb->mtf_ctx[INFO].mtf
    Buffer format_mapper_buf;          // an array of type SubfieldMapperZip - one entry per entry in vb->mtf_ctx[FORMAT].mtf
    
    // regions & filters
    Buffer region_ra_intersection_matrix;      // a byte matrix - each row represents an ra in this vb, and each column is a region specieid in the command. the cell contains 1 if this ra intersects with this region
    
    // Information content stats - how many bytes does this section have more than the corresponding part of the vcf file    
    int32_t vcf_section_bytes[NUM_SEC_TYPES];  // how many bytes did each section have in the original vcf file - should add up to the file size
    int32_t z_section_bytes[NUM_SEC_TYPES];    // how many bytes does each section type have (including headers) in the genozip file - should add up to the file size
    int32_t z_num_sections[NUM_SEC_TYPES];     // how many sections where written to .genozip of this type
    int32_t z_section_entries[NUM_SEC_TYPES];  // how many entries (dictionary or base250) where written to .genozip of this type
    Buffer show_headers_buf;                   // ZIP only: we collect header info, if --show-headers is requested, during compress, but show it only when the vb is written so that it appears in the same order as written to disk
    Buffer show_b250_buf;                      // ZIP only: for collecting b250 during generate - so we can print at onces without threads interspersing
    Buffer section_list_buf;                   // ZIP only: all the sections non-dictionary created in this vb. we collect them as the vb is processed, and add them to the zfile list in correct order of VBs.

#   define NUM_COMPRESS_BUFS 4                 // bzlib2 compress requires 4 and decompress requires 2
    Buffer compress_bufs[NUM_COMPRESS_BUFS];   // memory allocation for compressor so it doesn't do its own malloc/free

    // backward compatability with genozip v1 
    Buffer v1_variant_data_section_data;  // all fields until FORMAT, newline-separated, \0-termianted. .len includes the terminating \0 (used for decompressed V1 files)
    Buffer v1_subfields_start_buf;        // v1 only: these 3 are used by piz_reconstruct_line_components
    Buffer v1_subfields_len_buf;
    Buffer v1_num_subfields_buf;
} VariantBlock;

extern void vb_cleanup_memory(void);
extern VariantBlock *vb_get_vb (FileP vcf_file, FileP z_file, unsigned variant_block_i);
extern void vb_external_vb_initialize(void);
extern unsigned vb_num_samples_in_sb (const VariantBlock *vb, unsigned sb_i);
extern unsigned vb_num_sections(VariantBlock *vb);
extern void vb_release_vb (VariantBlock **vb_p);

typedef struct {
    unsigned num_vbs;
    VariantBlock vb[]; // variable length
} VariantBlockPool;
extern void vb_create_pool (unsigned num_vbs);
extern VariantBlockPool *vb_get_pool(void);

#endif