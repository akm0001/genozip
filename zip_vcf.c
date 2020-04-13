// ------------------------------------------------------------------
//   zip_vcf.c
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include <math.h>
#include "genozip.h"
#include "profiler.h"
#include "vblock.h"
#include "buffer.h"
#include "file.h"
#include "zfile.h"
#include "txtfile.h"
#include "header.h"
#include "seg.h"
#include "vblock.h"
#include "dispatcher.h"
#include "move_to_front.h"
#include "zip.h"
#include "base250.h"
#include "endianness.h"
#include "random_access_vcf.h"

#define DATA_LINE(vb,i) (&((ZipDataLineVCF *)((vb)->data_lines))[(i)])

static uint32_t global_samples_per_block = 0; 

static pthread_mutex_t best_gt_data_compressor_mutex;

void zip_vcf_initialize (void)
{
    pthread_mutex_init (&best_gt_data_compressor_mutex, NULL);
}

void zip_vcf_set_global_samples_per_block (const char *num_samples_str)
{
    unsigned len = strlen (num_samples_str);
    for (unsigned i=0; i < len; i++) 
        ASSERT (num_samples_str[i] >= '0' && num_samples_str[i] <= '9', "Error: invalid argument of --sblock: %s. Expecting an integer number between 1 and 65535", num_samples_str);

    global_samples_per_block = atoi (num_samples_str);

    ASSERT (global_samples_per_block >= 1 && global_samples_per_block <= 65535, "Error: invalid argument of --sblock: %s. Expecting a number between 1 and 65535", num_samples_str);
}

#define SBL(line_i,sb_i) ((line_i) * vb->num_sample_blocks + (sb_i))

static unsigned zip_vcf_get_genotype_vb_start_len (VBlockVCF *vb)
{
    buf_alloc (vb, &vb->genotype_section_lens_buf, sizeof(unsigned) * vb->num_sample_blocks, 1, "section_lens_buf", 0);
    unsigned section_0_len = 0; // all sections are the same length except the last that might be shorter

    // offsets into genotype data of individual lines
    buf_alloc (vb, &vb->gt_sb_line_starts_buf, 
               vb->num_lines * vb->num_sample_blocks * sizeof(uint32_t*), 
               0, "gt_sb_line_starts_buf", vb->vblock_i);
    uint32_t **gt_sb_line_starts = (uint32_t**)vb->gt_sb_line_starts_buf.data; 
    
    // each entry is the length of a single line in a sample block
    buf_alloc (vb, &vb->gt_sb_line_lengths_buf, 
               vb->num_lines * vb->num_sample_blocks * sizeof(unsigned), 
               0, "gt_sb_line_lengths_buf", vb->vblock_i);
    unsigned *gt_sb_line_lengths = (unsigned *)vb->gt_sb_line_lengths_buf.data; 
    
    // calculate offsets and lengths of genotype data of each sample block
    for (uint32_t line_i=0; line_i < vb->num_lines; line_i++) {

        uint32_t *gt_data  = (uint32_t*)GENOTYPE_DATA(vb, DATA_LINE (vb, line_i));        
        unsigned format_mtf_i = DATA_LINE (vb, line_i)->format_mtf_i;
        SubfieldMapper *format_mapper = &((SubfieldMapper *)vb->format_mapper_buf.data)[format_mtf_i];
        
        for (unsigned sb_i=0; sb_i < vb->num_sample_blocks; sb_i++) {

            unsigned num_samples_in_sb = vb_vcf_num_samples_in_sb (vb, sb_i);

            gt_sb_line_starts[SBL(line_i, sb_i)] = 
                &gt_data[global_samples_per_block * sb_i * format_mapper->num_subfields];

            unsigned num_subfields_in_sample_line = format_mapper->num_subfields * num_samples_in_sb; // number of uint32_t
            gt_sb_line_lengths[SBL(line_i, sb_i)] = num_subfields_in_sample_line;

            if (!sb_i) section_0_len += num_subfields_in_sample_line;
        }
    }

    return section_0_len; // in subfields (uint32_t)
}

// split genotype data to sample groups, within a sample group genotypes are separated by a tab
static void zip_vcf_generate_genotype_one_section (VBlockVCF *vb, unsigned sb_i)
{
    START_TIMER;

    // build sample block genetype data
    uint8_t *dst_next = (uint8_t *)vb->genotype_one_section_data.data;
    
    // move the GT items from the line data to the permuted data - with each 
    // sample block of gt data containing the data in TRANSPOSED order - i.e. first
    // the gt data for all the variants for sample 1, then all of samples 2 etc.
    unsigned num_samples_in_sb = vb_vcf_num_samples_in_sb (vb, sb_i);
    for (unsigned sample_i=0; sample_i < num_samples_in_sb; sample_i++) {

        if (flag_show_gt_nodes) fprintf (stderr, "sample=%u (vb_i=%u sb_i=%u):\n", sb_i * global_samples_per_block + sample_i + 1, vb->vblock_i, sb_i);

        for (unsigned line_i=0; line_i < vb->num_lines; line_i++) {

            if (flag_show_gt_nodes) fprintf (stderr, "  L%u: ", line_i);

            ZipDataLineVCF *dl = DATA_LINE (vb, line_i);

            SubfieldMapper *format_mapper = &((SubfieldMapper *)vb->format_mapper_buf.data)[dl->format_mtf_i];

            unsigned **sb_lines = (uint32_t**)vb->gt_sb_line_starts_buf.data;
            unsigned *this_line = sb_lines[SBL(line_i, sb_i)];
            
            // lookup word indices in the global dictionary for all the subfields
            const uint8_t *dst_start = dst_next;

            int num_subfields = format_mapper->num_subfields;
            ASSERT (num_subfields >= 0, "Error: format_mapper->num_subfields=%d", format_mapper->num_subfields);

            // if this VB has subfields in some line, but not in this line, then we have filled it in seg_vcf_complete_missing_lines(), 
            // therefore we have 1 fake subfield
            if (vb->num_format_subfields > 0 && num_subfields==0) num_subfields = 1;

            for (unsigned sf=0; sf < format_mapper->num_subfields; sf++) { // iterate on the order as in the line
            
                uint32_t node_index = this_line[format_mapper->num_subfields * sample_i + sf];

                if (node_index <= WORD_INDEX_MAX_INDEX) { // normal index

                    MtfContext *ctx = MAPPER_CTX (format_mapper, sf);
                    MtfNode *node = mtf_node (ctx, node_index, NULL, NULL);
                    Base250 index = node->word_index;

                    if (flag_show_gt_nodes) fprintf (stderr, "%.*s:%u ", DICT_ID_LEN, dict_id_printable (ctx->dict_id).id, index.n);

                    base250_copy (dst_next, index);
                    dst_next += base250_len (index.encoded.numerals);
                }
                else if (node_index == WORD_INDEX_MISSING_SF) {
                    *(dst_next++) = BASE250_MISSING_SF;
                }
                else {  // node_index == WORD_INDEX_EMPTY_SF
                    *(dst_next++) = BASE250_EMPTY_SF;
                }
            }

            vb->genotype_one_section_data.len += dst_next - dst_start;
            
            if (flag_show_gt_nodes) fprintf (stderr, "\n");
        }
    }

    COPY_TIMER (vb->profile.zip_generate_genotype_sections)
}

// split phase data to sample groups, in each group is a string of / | or -
static void zip_vcf_generate_phase_sections (VBlockVCF *vb)
{   
    START_TIMER;

    // we allocate memory for the Buffer array only once the first time this VBlockVCF
    // is used. Subsequent blocks reusing the memory will have the same number of samples (by VCF spec)
    if (!vb->phase_sections_data) 
        vb->phase_sections_data = (Buffer *)calloc (vb->num_sample_blocks, sizeof(Buffer)); // allocate once, never free

    for (unsigned sb_i=0; sb_i < vb->num_sample_blocks; sb_i++) {

        unsigned num_samples_in_sb = vb_vcf_num_samples_in_sb (vb, sb_i); 
    
        // allocate memory for phase data for each sample block - one character per sample
        buf_alloc (vb, &vb->phase_sections_data[sb_i], vb->num_lines * num_samples_in_sb, 
                   0, "phase_sections_data", vb->vblock_i);

        // build sample block genetype data
        char *next = vb->phase_sections_data[sb_i].data;
        
        for (unsigned line_i=0; line_i < vb->num_lines; line_i++) {
            
            ZipDataLineVCF *dl = DATA_LINE (vb, line_i);
            if (dl->phase_type == PHASE_MIXED_PHASED) 
                memcpy (next, &PHASE_DATA(vb,dl)[sb_i * num_samples_in_sb], num_samples_in_sb);
            else
                memset (next, (char)dl->phase_type, num_samples_in_sb);

            next += num_samples_in_sb;
        }
        vb->phase_sections_data[sb_i].len = num_samples_in_sb * vb->num_lines;
    }
          
    // add back the phase data bytes that weren't actually "saved"
    COPY_TIMER (vb->profile.zip_vcf_generate_phase_sections)
}

typedef struct {
    int num_alt_alleles;
    unsigned index_in_original_line;
    unsigned index_in_sorted_line;
} HaploTypeSortHelperIndex;

static int sort_by_alt_allele_comparator(const void *p, const void *q)  
{ 
    int l = ((HaploTypeSortHelperIndex *)p)->num_alt_alleles; 
    int r = ((HaploTypeSortHelperIndex *)q)->num_alt_alleles;  
    return (l - r); 
}

static int sort_by_original_index_comparator(const void *p, const void *q)  
{ 
    int l = ((HaploTypeSortHelperIndex *)p)->index_in_original_line; 
    int r = ((HaploTypeSortHelperIndex *)q)->index_in_original_line;  
    return (l - r); 
}

static HaploTypeSortHelperIndex *zip_vcf_construct_ht_permutation_helper_index (VBlockVCF *vb)
{
    START_TIMER; 

    buf_alloc (vb, &vb->helper_index_buf, vb->num_haplotypes_per_line * sizeof(HaploTypeSortHelperIndex), 0,
               "helper_index_buf", vb->vblock_i);
    buf_zero (&vb->helper_index_buf);
    HaploTypeSortHelperIndex *helper_index = (HaploTypeSortHelperIndex *)vb->helper_index_buf.data;

    // build index array 
    for (unsigned ht_i=0; ht_i < vb->num_haplotypes_per_line; ht_i++) 
        helper_index[ht_i].index_in_original_line = ht_i;

    for (unsigned line_i=0; line_i < vb->num_lines; line_i++) {
        for (unsigned ht_i=0; ht_i < vb->num_haplotypes_per_line; ht_i++) {

            // we count as alt alleles : 1 - 99 (ascii 49 to 147)
            //             ref alleles : 0 . (unknown) - (missing) * (ploidy padding)
            //char one_ht = vb->data_lines.zip[line_i].haplotype_data.data[ht_i];
            
            char *haplotype_data = HAPLOTYPE_DATA (vb, DATA_LINE (vb, line_i));
            char one_ht = haplotype_data[ht_i];
            if (one_ht >= '1')
                helper_index[ht_i].num_alt_alleles++;
        }
    }
    COPY_TIMER (vb->profile.count_alt_alleles);

    return helper_index;
}

// sort haplogroups by alt allele count within the variant group, create an index for it, and split
// it to sample groups. for each sample a haplotype is just a string of 1 and 0 etc (could be other alleles too)
static void zip_vcf_generate_haplotype_sections (VBlockVCF *vb)
{
    START_TIMER;

    // we allocate memory for the Buffer array only once the first time this VBlockVCF
    // is used. Subsequent blocks reusing the memory will have the same number of samples (by VCF spec)
    if (!vb->haplotype_sections_data) 
        vb->haplotype_sections_data = (Buffer *)calloc (vb->num_sample_blocks, sizeof(Buffer)); // allocate once, never free
    
    // create a permutation index for the whole variant block, and permuted haplotypes for each sample block        
    buf_alloc (vb, &vb->haplotype_permutation_index, vb->num_haplotypes_per_line * sizeof(uint32_t), 
               0, "haplotype_permutation_index", vb->vblock_i);

    HaploTypeSortHelperIndex *helper_index = zip_vcf_construct_ht_permutation_helper_index (vb);

    // set dl->haplotype_ptr for all lines (for effeciency in the time loop below)
    for (unsigned line_i=0; line_i < vb->num_lines; line_i++) 
        DATA_LINE (vb, line_i)->haplotype_ptr = HAPLOTYPE_DATA (vb, DATA_LINE (vb, line_i));

    // now build per-sample-block haplotype array, picking haplotypes by the order of the helper index array
    for (unsigned sb_i=0; sb_i < vb->num_sample_blocks; sb_i++) {

        unsigned num_haplotypes_in_sample_block = 
            vb->ploidy * vb_vcf_num_samples_in_sb (vb, sb_i); 

        unsigned helper_index_sb_i = sb_i * vb->num_samples_per_block * vb->ploidy;

        // sort the portion of the helper index related to this sample block. We sort by number of alt alleles.
        if (!flag_gtshark) // gtshark does better without sorting
            qsort (&helper_index[helper_index_sb_i], num_haplotypes_in_sample_block, sizeof (HaploTypeSortHelperIndex), sort_by_alt_allele_comparator);

        // allocate memory for haplotype data for each sample block - one character per haplotype
        buf_alloc (vb, &vb->haplotype_sections_data[sb_i], vb->num_lines * num_haplotypes_in_sample_block, 
                   0, "haplotype_sections_data", vb->vblock_i);

        // build sample block haplptype data - 
        // -- using the helper index to access the haplotypes in sorted order
        // -- transposing the array
        char *next = vb->haplotype_sections_data[sb_i].data;
        
        {   // this loop, tested with 1KGP data, takes up to 1/5 of total compute time, so its highly optimized
            START_TIMER;

            for (unsigned ht_i=0; ht_i < num_haplotypes_in_sample_block; ht_i++) {
                
                unsigned haplotype_data_char_i = helper_index[helper_index_sb_i + ht_i].index_in_original_line;
                const char **ht_data_ptr = &DATA_LINE (vb, 0)->haplotype_ptr; // this pointer moves sizeof(ZipDataLineVCF) bytes each iteration - i.e. to the exact same field in the next line

                for (unsigned line_i=0; line_i < vb->num_lines; line_i++, ht_data_ptr += sizeof(ZipDataLineVCF)/sizeof(char*)) 
                    *(next++) = (*ht_data_ptr)[haplotype_data_char_i];
            }
            COPY_TIMER (vb->profile.sample_haplotype_data);
        }
        vb->haplotype_sections_data[sb_i].len = num_haplotypes_in_sample_block * vb->num_lines;
    }

    // final step - build the reverse index that will allow access by the original index to the sorted array
    // this will be included in the genozip file
    for (unsigned ht_i=0; ht_i < vb->num_haplotypes_per_line ; ht_i++)
        helper_index[ht_i].index_in_sorted_line = ht_i;

    // sort array back to its original order
    qsort (helper_index, vb->num_haplotypes_per_line, sizeof (HaploTypeSortHelperIndex), sort_by_original_index_comparator);

    // construct file index
    unsigned *hp_index = (unsigned *)vb->haplotype_permutation_index.data;
    for (unsigned ht_i=0; ht_i < vb->num_haplotypes_per_line ; ht_i++)
        hp_index[ht_i] = helper_index[ht_i].index_in_sorted_line;

    buf_free (&vb->helper_index_buf);

    COPY_TIMER (vb->profile.zip_vcf_generate_haplotype_sections);
}

static CompressorAlg zip_vcf_get_best_gt_compressor (VBlock *vb, Buffer *test_data)
{
    static CompressorAlg best_gt_data_compressor = COMPRESS_UNKNOWN;
    static Buffer compressed = EMPTY_BUFFER; // no thread issues as protected my mutex

    // get best compression algorithm for gt data - lzma or bzlib - their performance varies considerably with
    // the type of data - with either winning by a big margin
    pthread_mutex_lock (&best_gt_data_compressor_mutex);    

    if (best_gt_data_compressor != COMPRESS_UNKNOWN) goto finish; // answer already known

    #define TEST_BLOCK_SIZE 100000
    buf_alloc (vb, &compressed, TEST_BLOCK_SIZE+1000, 1, "compressed_data_test", 0);

    uint32_t uncompressed_len = MIN (test_data->len, TEST_BLOCK_SIZE);

    compressed.len = compressed.size;
    comp_compress_bzlib (vb, test_data->data, uncompressed_len, NULL, compressed.data, &compressed.len, false);
    uint32_t bzlib_comp_len = compressed.len;
    
    compressed.len = compressed.size;
    comp_compress_lzma (vb, test_data->data, uncompressed_len, NULL, compressed.data, &compressed.len, false);
    uint32_t lzma_comp_len = compressed.len;
    
    if      (bzlib_comp_len < uncompressed_len && bzlib_comp_len < lzma_comp_len) best_gt_data_compressor = COMPRESS_BZLIB;
    else if (lzma_comp_len  < uncompressed_len && lzma_comp_len < bzlib_comp_len) best_gt_data_compressor = COMPRESS_LZMA;
    else                                                                          best_gt_data_compressor = COMPRESS_NONE;

    buf_free (&compressed);

finish:
    pthread_mutex_unlock (&best_gt_data_compressor_mutex);
    return best_gt_data_compressor;
}

// this function receives all lines of a variant block and processes them
// in memory to the compressed format. This thread then terminates the I/O thread writes the output.
void zip_vcf_compress_one_vb (VBlockP vb_)
{ 
    START_TIMER;

    VBlockVCF *vb = (VBlockVCF *)vb_;

    // if we're vb_i=1 lock, and unlock only when we're done merging. all other vbs need
    // to wait for our merge. that is because our dictionaries are sorted
    if (vb->vblock_i == 1) mtf_vb_1_lock(vb_);

    // allocate memory for the final compressed data of this vb. allocate 20% of the 
    // vb size on the original file - this is normally enough. if not, we will realloc downstream
    buf_alloc (vb, &vb->z_data, vb->vb_data_size / 5, 1.2, "z_data", 0);

    // initalize variant block data (everything else is initialzed to 0 via calloc)
    vb->phase_type = PHASE_UNKNOWN;  // phase type of this block
    vb->num_samples_per_block = global_samples_per_block;
    vb->num_sample_blocks = ceil((float)global_vcf_num_samples / (float)vb->num_samples_per_block);

    unsigned max_genotype_section_len=0; // length in subfields

    // clone global dictionaries while granted exclusive access to the global dictionaries
    mtf_clone_ctx (vb_);

    // split each line in this variant block to its components

    seg_all_data_lines (vb_, seg_vcf_data_line, sizeof (ZipDataLineVCF), 
                        VCF_CHROM, VCF_FORMAT, vcf_field_names, SEC_VCF_CHROM_DICT);
    
    if (vb->has_haplotype_data)
        seg_vcf_complete_missing_lines (vb);

    // for the first vb only - sort dictionaries so that the most frequent entries get single digit
    // base-250 indices. This can be done only before any dictionary is written to disk, but likely
    // beneficial to all vbs as they are likely to more-or-less have the same frequent entries
    if (vb->vblock_i == 1) {
        mtf_sort_dictionaries_vb_1(vb_);

        // for a VCF file that's compressed (and hence we don't know the size of VCF content a-priori) AND we know the
        // compressed file size (eg a local gz/bz2 file or a compressed file on a ftp server) - we now estimate the 
        // txt_data_size_single that will be used for the global_hash and the progress indicator
        txtfile_estimate_txt_data_size (vb_);
    }

    // if block has haplotypes - handle them now
    if (vb->has_haplotype_data)
        zip_vcf_generate_haplotype_sections (vb); 

    // if block has genetype data 
    if (vb->has_genotype_data) 
        // calculate starts, lengths and allocate memory
        max_genotype_section_len = zip_vcf_get_genotype_vb_start_len (vb); // length in subfields

    // if block has phase data - handle it
    if (vb->phase_type == PHASE_MIXED_PHASED) 
        zip_vcf_generate_phase_sections (vb);

    //unsigned variant_data_header_pos = vb->z_data.len;
    zfile_vcf_compress_vb_header (vb); // variant data header + ht index

    // merge new words added in this vb into the z_file.mtf_ctx, ahead of zip_generate_b250_section() and
    // zip_vcf_generate_genotype_one_section(). writing indices based on the merged dictionaries. dictionaries are compressed. 
    // all this is done while holding exclusive access to the z_file dictionaries.
    mtf_merge_in_vb_ctx(vb_);

    // now, we merge vb->ra_buf into z_file->ra_buf
    random_access_merge_in_vb (vb);

    // generate & write b250 data for all fields (CHROM to FORMAT)
    for (VcfFields f=VCF_CHROM ; f <= VCF_FORMAT ; f++) {
        MtfContext *ctx = &vb->mtf_ctx[f];
        zip_generate_b250_section (vb_, ctx);
        zfile_compress_b250_data (vb_, ctx);
    }

    // generate & write b250 data for all INFO subfields
    uint8_t num_info_subfields=0;
    for (unsigned did_i=0; did_i < MAX_DICTS; did_i++) {
                
        MtfContext *ctx = &vb->mtf_ctx[did_i];
        
        if (ctx->dict_section_type == SEC_VCF_INFO_SF_DICT && ctx->mtf_i.len) {
            zip_generate_b250_section (vb_, ctx);
            zfile_compress_b250_data (vb_, ctx);
            num_info_subfields++;
        }
    }
    ASSERT (num_info_subfields <= MAX_SUBFIELDS, "Error: vb_i=%u has %u INFO subfields, which exceeds the maximum of %u",
            vb->vblock_i, num_info_subfields, MAX_SUBFIELDS);

    // compress the sample data - genotype, haplotype and phase sections. genotype data is generated here too.
    CompressorAlg gt_data_alg;
    for (unsigned sb_i=0; sb_i < vb->num_sample_blocks; sb_i++) {
        
        if (vb->has_genotype_data) {

            // in the worst case scenario, each subfield is represnted by 4 bytes in Base250
            buf_alloc (vb, &vb->genotype_one_section_data, max_genotype_section_len * MAX_BASE250_NUMERALS, 1, "genotype_one_section_data", sb_i);

            // we compress each section at a time to save memory
            zip_vcf_generate_genotype_one_section (vb, sb_i); 

            gt_data_alg = zip_vcf_get_best_gt_compressor (vb_, &vb->genotype_one_section_data);

            zfile_compress_section_data_alg (vb_, SEC_VCF_GT_DATA, &vb->genotype_one_section_data, NULL, 0, gt_data_alg);

            buf_free (&vb->genotype_one_section_data);
        }

        if (vb->phase_type == PHASE_MIXED_PHASED)
            zfile_compress_section_data (vb_, SEC_VCF_PHASE_DATA, &vb->phase_sections_data[sb_i]);

        if (vb->has_haplotype_data) {
            if (!flag_gtshark)
                zfile_compress_section_data (vb_, SEC_VCF_HT_DATA , &vb->haplotype_sections_data[sb_i]);
            else 
                zfile_vcf_compress_haplotype_data_gtshark (vb, &vb->haplotype_sections_data[sb_i], sb_i);
        }
    }

    // tell dispatcher this thread is done and can be joined. 
    // thread safety: this isn't protected by a mutex as it will just be false until it at some point turns to true
    // this this operation needn't be atomic, but it likely is anyway
    vb->is_processed = true; 

    COPY_TIMER (vb->profile.compute);
}
