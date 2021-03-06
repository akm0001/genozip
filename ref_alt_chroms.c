// ------------------------------------------------------------------
//   ref_alt_chroms.c
//   Copyright (C) 2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "genozip.h"
#include "ref_private.h"
#include "sections.h"
#include "data_types.h"
#include "file.h"
#include "zfile.h"
#include "endianness.h"
#include "strings.h"
#include "vblock.h"
#include "mutex.h"
#include "seg.h"

// ZIP of a file with REF_EXTERNAL/REF_EXT_STORE: When a chrom index in the txt file matches an a different chrom index in the reference file, 
// we create a mapping here pass it to Piz as a SEC_REF_ALT_CHROMS section. It contains at most as many entries 
// as the number of contigs in the reference file.

// this is needed in two cases:
// 1. In a file without a header: in case the chrom_name in the txt is an alternate name to the one in the header (eg "22"->"chr22")
// 2. In a file with a header: the reference index will be different from the txt if the header and reference chroms are not
//    in the same order

void ref_alt_chroms_compress (void)
{
    // case: when compressing SAM or BAM with a header (including BAM header with no SQs - unaligned BAM), 
    // alt chroms were already prepared in ref_contigs_ref_chrom_from_header_chrom
    if (has_header_contigs) goto just_compress;

    Context *ctx = &z_file->contexts[CHROM];
    uint32_t num_chroms = ctx->nodes.len;
    uint32_t num_contigs = ref_contigs_num_contigs();   // chroms that are in the reference file
    uint32_t num_alt_chroms = num_chroms - num_contigs; // chroms that are only in the txt file, not in the reference

    if (!num_alt_chroms) return; // no need for an alt chroms sections as we have none

    buf_alloc (evb, &z_file->alt_chrom_map, sizeof (AltChrom) * num_alt_chroms, 1, "z_file->alt_chrom_map");

    if (flag.show_ref_alts) 
        iprint0 ("\nAlternative chrom indices (output of --show-ref-alts): chroms that are in the file and are mapped to a different name in the reference\n");

    for (uint32_t i=0; i < num_alt_chroms; i++) {
        WordIndex chrom_index = num_contigs + i;
        const CtxNode *chrom_node = ENT (CtxNode, ctx->nodes, chrom_index);
        const char *chrom_name = ENT (const char, ctx->dict, chrom_node->char_index);
        
        WordIndex alt_index = ref_alt_chroms_zip_get_alt_index (chrom_name, chrom_node->snip_len, WI_REF_CONTIG, WORD_INDEX_NONE);

        // an alt_index might be missing for chrom snips like '=' or '*' or sequence-less chroms that don't appear in the reference
        if (alt_index != WORD_INDEX_NONE)           
            NEXTENT (AltChrom, z_file->alt_chrom_map) = (AltChrom){ .txt_chrom = BGEN32 (chrom_index), 
                                                                    .ref_chrom = BGEN32 (alt_index) };
    
        if (flag.show_ref_alts) {
            const CtxNode *alt_node = ENT (CtxNode, ctx->nodes, alt_index);
            const char *alt_name = ENT (const char, ctx->dict, alt_node->char_index);

            iprintf ("In file: '%s' (%d) In reference: '%s' (%d)\n", chrom_name, chrom_index, alt_name, alt_index);
        }
    }

just_compress:
    if (z_file->alt_chrom_map.len) {
        z_file->alt_chrom_map.len *= sizeof (AltChrom);
        zfile_compress_section_data_ex (evb, SEC_REF_ALT_CHROMS, &z_file->alt_chrom_map, 0,0, CODEC_LZMA, SECTION_FLAGS_NONE); // compresses better with LZMA than BZLIB
    }
    
    buf_free (&z_file->alt_chrom_map);
}

void ref_alt_chroms_load (void)
{
    const SectionListEntry *sl = sections_get_first_section_of_type (SEC_REF_ALT_CHROMS, true);
    if (!sl) return; // we don't have alternate chroms

    zfile_read_section (z_file, evb, 0, &evb->z_data, "z_data", SEC_REF_ALT_CHROMS, sl);

    zfile_uncompress_section (evb, evb->z_data.data, &evb->compressed, "compressed", 0, SEC_REF_ALT_CHROMS);

    if (flag.show_ref_alts) 
        iprint0 ("\nAlternative chrom indices (output of --show-ref-alts): chroms that are in the txt file and are mapped to a different index in the reference\n");

    evb->compressed.len /= sizeof (AltChrom);
    Context *ctx = &z_file->contexts[CHROM];

    // create mapping user index -> reference index
    buf_alloc (evb, &z_file->alt_chrom_map, sizeof (WordIndex) * ctx->word_list.len, 1, "z_file->alt_chrom_map");
    z_file->alt_chrom_map.len = ctx->word_list.len;

    // initialize with unity mapping
    ARRAY (WordIndex, map, z_file->alt_chrom_map);
    for (uint32_t i=0; i < ctx->word_list.len; i++)
        map[i] = i;

    // the indices of chroms that are NOT in the reference (they are only in the user file), will be mapped to chroms in
    // the reference
    for (uint32_t i=0; i < evb->compressed.len; i++) {
        AltChrom *ent = ENT (AltChrom, evb->compressed, i);
        WordIndex txt_chrom_index = BGEN32 (ent->txt_chrom);
        WordIndex ref_chrom_index = BGEN32 (ent->ref_chrom);

        ASSERTE (txt_chrom_index >= 0 && txt_chrom_index < ctx->word_list.len, "txt_chrom_index=%d out of range [0,%d]", txt_chrom_index, (int32_t)ctx->word_list.len);
        ASSERTE (ref_chrom_index >= 0 && ref_chrom_index < ref_contigs_num_contigs(), "ref_chrom_index=%d out of range [0,%u]", ref_chrom_index, ref_contigs_num_contigs());

        map[txt_chrom_index] = ref_chrom_index;

        if (flag.show_ref_alts) {
            const char *chrom_name = ctx_get_snip_by_word_index (&ctx->word_list, &ctx->dict, txt_chrom_index, 0, 0);
            const char *alt_name   = ctx_get_snip_by_word_index (&ctx->word_list, &ctx->dict, ref_chrom_index, 0, 0);
            iprintf ("In file: '%s' (%d) In reference: '%s' (%d)\n", chrom_name, txt_chrom_index, alt_name, ref_chrom_index);
        }
    }

    if (flag.show_ref_alts && exe_type == EXE_GENOCAT) exit(0); // in genocat this, not the data

    buf_free (&evb->z_data);
    buf_free (&evb->compressed);
}

// ZIP only: in PIZ, we get the maping from SEC_REF_ALT_CHROMS
WordIndex ref_alt_chroms_zip_get_alt_index (const char *chrom, unsigned chrom_len, GetWordIndexType where_is_alt, WordIndex fallback_index)
{
    WordIndex alternative_chrom_word_index = WORD_INDEX_NONE;
    
    // 22 -> chr22 (1->22, X, Y, M, MT chromosomes)
    if ((chrom_len == 1 && (IS_DIGIT (chrom[0]) || chrom[0]=='X' || chrom[0]=='Y' || chrom[0]=='M')) ||
        (chrom_len == 2 && ((IS_DIGIT (chrom[0]) && IS_DIGIT (chrom[1])) || (chrom[0]=='M' && chrom[1]=='T')))) {

        char chr_chrom[5] = "chr";
        chr_chrom[3] = chrom[0];
        chr_chrom[4] = (chrom_len == 2 ? chrom[1] : 0);

        alternative_chrom_word_index = ref_contigs_get_word_index (chr_chrom, chrom_len+3, where_is_alt, true); 
    }

    // M, chrM -> chrMT
    else if ((chrom_len==4 && !memcmp (chrom, "chrM", 4)) || (chrom_len==1 && chrom[0]=='M')) 
        alternative_chrom_word_index = ref_contigs_get_word_index ("chrMT", 5, where_is_alt, true); 

    // Chr? or Chr?? -> ? or ??
    else if ((chrom_len == 4 || chrom_len == 5) && !memcmp (chrom, "chr", 3))
        alternative_chrom_word_index = ref_contigs_get_word_index (&chrom[3], chrom_len-3, where_is_alt, true); 

    // AC subfield in DESC in reference FASTAs, eg GRCh37/38, eg "GL000207.1" -> "chr18_gl000207_random"
    // https://www.ncbi.nlm.nih.gov/Sequin/acc.html
    else if (where_is_alt == WI_REF_CONTIG && chrom_len >= 6 && IS_CLETTER (chrom[0]) && chrom[chrom_len-2]=='.' && IS_DIGIT(chrom[chrom_len-1])) 
        alternative_chrom_word_index = ref_contigs_get_by_accession_number (chrom, chrom_len);

    return (alternative_chrom_word_index != WORD_INDEX_NONE) ? alternative_chrom_word_index : fallback_index;
}
