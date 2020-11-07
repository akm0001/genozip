// ------------------------------------------------------------------
//   me23.c
//   Copyright (C) 2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "genozip.h"
#include "seg.h"
#include "vblock.h"
#include "context.h"
#include "file.h"
#include "random_access.h"
#include "endianness.h"
#include "strings.h"
#include "piz.h"
#include "dict_id.h"
#include "stats.h"
#include "reference.h"
#include "codec.h"
#include "version.h"

//-----------------------------------------
// Segmentation functions for 23andMe files
//-----------------------------------------

void me23_seg_initialize (VBlock *vb)
{
    vb->contexts[ME23_CHROM].inst = CTX_INST_NO_STONS | CTX_INST_NO_VB1_SORT; // needs b250 node_index for random access ; needs to be in original order to reconstruct VCF header when translating
    vb->contexts[ME23_GENOTYPE].ltype = LT_SEQUENCE;
}

void me23_seg_finalize (VBlockP vb)
{
    // top level snip
    Container top_level = { 
        .repeats   = vb->lines.len,
        .flags     = CONTAINER_TOPLEVEL,
        .num_items = 5,
        .items     = { { (DictId)dict_id_fields[ME23_ID],       DID_I_NONE, "\t" },
                       { (DictId)dict_id_fields[ME23_CHROM],    DID_I_NONE, "\t" },
                       { (DictId)dict_id_fields[ME23_POS],      DID_I_NONE, "\t" },
                       { (DictId)dict_id_fields[ME23_GENOTYPE], DID_I_NONE, ""   },
                       { (DictId)dict_id_fields[ME23_EOL],      DID_I_NONE, ""   } }
    };

    container_seg_by_ctx (vb, &vb->contexts[ME23_TOPLEVEL], &top_level, 0, 0, 0);

    Container top_level_to_vcf = { 
        .repeats   = vb->lines.len,
        .flags     = CONTAINER_TOPLEVEL,
        .num_items = 5,
        .items     = { { (DictId)dict_id_fields[ME23_CHROM],    DID_I_NONE, "\t" },
                       { (DictId)dict_id_fields[ME23_POS],      DID_I_NONE, "\t" },
                       { (DictId)dict_id_fields[ME23_ID],       DID_I_NONE, "\t" },
                       { (DictId)dict_id_fields[ME23_GENOTYPE], DID_I_NONE, "\n", ME232VCF_GENOTYPE } }
    };

    container_seg_by_ctx (vb, &vb->contexts[ME23_TOP2VCF], &top_level_to_vcf, 0, 0, 0);
}

const char *me23_seg_txt_line (VBlock *vb, const char *field_start_line, bool *has_13)     // index in vb->txt_data where this line starts
{
    const char *next_field=field_start_line, *field_start;
    unsigned field_len=0;
    char separator;

    int32_t len = &vb->txt_data.data[vb->txt_data.len] - field_start_line;

    GET_NEXT_ITEM ("RSID");
    seg_id_field (vb, (DictId)dict_id_fields[ME23_ID], field_start, field_len, true);

    GET_NEXT_ITEM ("CHROM");
    seg_chrom_field (vb, field_start, field_len);

    GET_NEXT_ITEM ("POS");
    seg_pos_field (vb, ME23_POS, ME23_POS, false, field_start, field_len, 0, field_len+1);
    random_access_update_pos (vb, ME23_POS);

    // Genotype (a combination of one or two bases or "--")
    GET_LAST_ITEM ("GENOTYPE");
    
    ASSERT (field_len == 1 || field_len == 2, "%s: Error in %s: expecting all genotype data to be 1 or 2 characters, but found one with %u: %.*s",
            global_cmd, txt_name, field_len, field_len, field_start);

    seg_add_to_local_fixed (vb, &vb->contexts[ME23_GENOTYPE], field_start, field_len); 
        
    char lookup[2] = { SNIP_LOOKUP, '0' + field_len };
    seg_by_did_i (vb, lookup, 2, ME23_GENOTYPE, field_len + 1);

    SEG_EOL (ME23_EOL, false);
    
    return next_field;
}

//------------------------------------------------------------
// Translators for reconstructing 23andMe txxt into VCF format
//------------------------------------------------------------

// creates VCF file header
TXTHEADER_TRANSLATOR (txtheader_me232vcf)
{
    #define VCF_HEAD_1 "##fileformat=VCFv4.1\n" \
                       "##FILTER=<ID=PASS,Description=\"All filters passed\">\n" \
                       "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">\n" \
                       "##genozip_reference=%s\n"
                       
    #define VCF_HEAD_2 "##contig=<ID=%s,length=%"PRId64">\n"
    
    #define VCF_HEAD_3 "##co= Converted 23andMe to VCF format by genozip v%s: https://github.com/divonlan/genozip (also available on conda and DockerHub)\n"  \
                       "##genozipCommand=%s\n" \
                       "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\t%.*s\n"
    
    // move the me23 and header to the side for a sec
    buf_move (evb, &evb->compressed, evb, txt);
    ARRAY (char, header23, evb->compressed);

    Context *ctx = &z_file->contexts[ME23_CHROM];
    uint32_t num_chroms = (uint32_t)ctx->word_list.len;
    
    buf_alloc (evb, txt, 1.3*evb->compressed.len + strlen (VCF_HEAD_1) + strlen (VCF_HEAD_3)+50 +
               num_chroms * (strlen (VCF_HEAD_2) + 100), 1, "txt_data", 0);
    
    bufprintf (evb, txt, VCF_HEAD_1, ref_filename);
    
    // add contigs used in this file
    for (uint32_t chrom_i=0; chrom_i < num_chroms; chrom_i++) {
        
        // get contig length from loaded reference
        uint32_t chrom_name_len;
        const char *chrom_name = ctx_get_snip_by_word_index (&ctx->word_list, &ctx->dict, chrom_i, 0, &chrom_name_len);
        PosType contig_len = ref_contigs_get_contig_length (chrom_name, chrom_name_len);

        bufprintf (evb, txt, VCF_HEAD_2, chrom_name, contig_len);
    }

    // add original 23andMe header, prefixing lines with "##co=" instead of "#"
    uint64_t header23_line_start = txt->len;
    for (uint64_t i=0; i < evb->compressed.len; i++) {
        if (header23[i] == '#') {
            header23_line_start = txt->len;
            buf_add (txt, "##co=", 5);
        }
        else
            NEXTENT (char, *txt) = header23[i];
    }
    txt->len = header23_line_start; // remove last 23andme line ("# rsid chromosome position genotype")
    
    // attempt to get sample name from 23andMe file name
    char *sample_name = "Person"; // default
    unsigned sample_name_len = strlen (sample_name);
    
    #define ME23_FILENAME_BEFORE_SAMPLE "genome_"
    #define ME23_FILENAME_AFTER_SAMPLE  "_Full_"
    
    char *start, *after;
    if ((start = strstr (z_name, ME23_FILENAME_BEFORE_SAMPLE)) && 
        (after = strstr (start,  ME23_FILENAME_AFTER_SAMPLE ))) {
        start += strlen (ME23_FILENAME_BEFORE_SAMPLE);
        sample_name = start;;
        sample_name_len = after - start;
    }

    // add final lines
    bufprintf (evb, txt, VCF_HEAD_3, GENOZIP_CODE_VERSION, command_line, sample_name_len, sample_name);

    buf_free (&evb->compressed);
}

// reconstruct VCF GENOTYPE field as VCF - REF,ALT,QUAL,FILTER,INFO,FORMAT,Sample
TRANSLATOR_FUNC (sam_piz_m232vcf_GENOTYPE)
{
    // Genotype length expected to be 2 or 1 (for MT, Y)
    ASSERT (reconstructed_len==1 || reconstructed_len==2, "Error in sam_piz_m232vcf_GENOTYPE: bad reconstructed_len=%u", reconstructed_len);

    PosType pos = vb->contexts[ME23_POS].last_value.i;

    // get the value of the loaded reference at this position    
    const Range *range = ref_piz_get_range (vb, pos, 1);
    ASSERT (range, "Error: Failed to find the site chrom='%s' pos=%"PRId64, vb->chrom_name, pos);
    uint32_t idx = pos - range->first_pos;
    char ref_b = ref_get_nucleotide (range, idx);

    // get GENOTYPE from txt_data
    char b1 = reconstructed[0];
    char b2 = (reconstructed_len==2) ? reconstructed[1] : 0;
    vb->txt_data.len -= reconstructed_len; // rollback - we will reconstruct it differently

    
    if (b1 == '-' || b2 == '-' || // filter out variants if the genotype is not fully called
       (b1 == 'D' || b2 == 'D' || b1 == 'I' || b2 == 'I')) { // discard INDELs
        vb->dont_show_curr_line = true;
        return 0;
    }

    // REF
    RECONSTRUCT1 (ref_b);
    RECONSTRUCT1 ('\t');

    // ALT
    bool is_alt_1 = (b1 != ref_b) ;
    bool is_alt_2 = (b2 != ref_b) && (reconstructed_len==2);
    int num_uniq_alts = is_alt_1 + is_alt_2 - (is_alt_1 && (b1==b2));
    
    switch (num_uniq_alts) {
        case 0: RECONSTRUCT1 ('.'); break; // no alt allele
        case 1: RECONSTRUCT1 (is_alt_1 ? b1 : b2); break;
        case 2: RECONSTRUCT1 (b1);  // both are (different) alts
                RECONSTRUCT1 (','); 
                RECONSTRUCT1 (b2); 
    }

    #define FIXED_VCF_VARDATA "\t.\tPASS\t.\tGT\t"
    RECONSTRUCT (FIXED_VCF_VARDATA, strlen (FIXED_VCF_VARDATA));

    // Sample data
    RECONSTRUCT1 (is_alt_1 ? '1' : '0');
    
    if (reconstructed_len==2) {
        RECONSTRUCT1 ('/');
        RECONSTRUCT1 (is_alt_2 ? '0'+num_uniq_alts : '0');
    }
    
    return 0;
}
