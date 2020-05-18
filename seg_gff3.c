// ------------------------------------------------------------------
//   seg_gff3.c
//   Copyright (C) 2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "genozip.h"
#include "seg.h"
#include "vblock.h"
#include "move_to_front.h"
#include "random_access.h"
#include "file.h"
#include "strings.h"
#include "dict_id.h"
#include "optimize.h"

#define DATA_LINE(i) ENT (ZipDataLineGFF3, vb->lines, i)

// called from seg_all_data_lines
void seg_gff3_initialize (VBlock *vb)
{
    seg_init_mapper (vb, GFF3_ATTRS, &((VBlockGFF3 *)vb)->iname_mapper_buf, "iname_mapper_buf");    
}

// returns length of next expected item, and 0 if unsuccessful
static unsigned seg_gff3_get_aofs_item_len (const char *str, unsigned len, bool is_last_item)
{
    unsigned i=0; for (; i < len; i++) {
        bool is_comma = str[i] == ',';
        bool is_space = str[i] == ' ';

        if ((is_last_item && is_comma) || (!is_last_item && is_space)) return i; // we reached the end of the item - return its length

        if ((is_last_item && is_space) || (!is_last_item && is_comma)) return 0; // invalid string - unexpected seperator
    }

    if (is_last_item) return i; // last item of last entry
    else return 0; // the string ended prematurely - this is not yet the last item
}

#define MAX_AoS_ITEMS 10

void seg_gff3_array_of_struct_ctxs (VBlockGFF3 *vb, DictIdType dict_id, unsigned num_items, 
                                    MtfContext **ctx_array, MtfContext **enst_ctx) // out
{
    ASSERT (num_items <= MAX_AoS_ITEMS, "seg_gff3_create_ctx_sub_array: num_items=%u expected to be at most %u", num_items, MAX_AoS_ITEMS);

    *enst_ctx = mtf_get_ctx_by_dict_id_sf (vb, &vb->num_info_subfields, (DictIdType)dict_id_ENSTid); 

    // create new contexts - they are guaranteed to be sequential in mtf_ctx
    for (unsigned i=0; i < num_items; i++) {
        dict_id.id[1] = '0' + i; // change the 2nd char (the first two chars are used for hashing in dict_id_to_did_i_map)
        MtfContext *ctx = mtf_get_ctx_by_dict_id_sf (vb, &vb->num_info_subfields, dict_id); 

        if (i==0) *ctx_array = ctx;
        
        ASSERT0 ((*ctx_array) + i == ctx, "Error in seg_gff3_create_ctx_sub_array: expecting ctxs to be consecutive");                                                  
    }
}

// a field that looks like: "non_coding_transcript_variant 0 ncRNA ENST00000431238,intron_variant 0 primary_transcript ENST00000431238"
// we have an array (2 entires in this example) of items (4 in this examples) - the entries are separated by comma and the items by space
// observed in Ensembel generated GVF: Variant_effect, sift_prediction, polyphen_prediction, variant_peptide
// The last item is treated as an ENST_ID (format: ENST00000399012) while the other items are regular dictionaries
// the names of the dictionaries are the same as the ctx, with the 2nd character replaced by 1,2,3...
// the field itself will contain the number of entries
static void seg_gff3_array_of_struct (VBlockGFF3 *vb, MtfContext *subfield_ctx, 
                                      unsigned num_items_in_struct, 
                                      const char *snip, unsigned snip_len)
{
    unsigned num_entries = 0;
    bool is_last_entry = false;

    MtfContext *ctxs; // an array of length num_items_in_struct (pointer to start of sub-array in vb->mtf_ctx)
    MtfContext *enst_ctx;
    seg_gff3_array_of_struct_ctxs (vb, subfield_ctx->dict_id, num_items_in_struct, &ctxs, &enst_ctx);

    // set roll back point
    uint64_t saved_mtf_i_len[MAX_AoS_ITEMS], saved_local_len[MAX_AoS_ITEMS], saved_txt_len[MAX_AoS_ITEMS];
    for (unsigned item_i=0; item_i < num_items_in_struct ; item_i++) {
        saved_mtf_i_len[item_i] = ctxs[item_i].mtf_i.len;
        saved_local_len[item_i] = ctxs[item_i].local.len;
        saved_txt_len[item_i]   = ctxs[item_i].txt_len;
    }
    const char *saved_snip = snip;
    unsigned saved_snip_len = snip_len;

    while (snip_len) {
        
        for (unsigned item_i=0; item_i < num_items_in_struct +  1 /* +1 for enst */; item_i++) {
            bool is_last_item = item_i == num_items_in_struct;
            unsigned item_len = seg_gff3_get_aofs_item_len (snip, snip_len, is_last_item);
            if (!item_len) goto badly_formatted;

            if (!is_last_item)
                seg_one_subfield ((VBlockP)vb, snip, item_len, ctxs[item_i].dict_id, item_len+1); // include the separating space after
            else {
                is_last_entry = (snip_len - item_len == 0);
                seg_id_field ((VBlockP)vb, (DictIdType)dict_id_ENSTid, snip, item_len, !is_last_entry);
            }
    
            snip     += item_len + 1 - is_last_entry; // 1 for either the , or the ' ' (except in the last item of the last entry)
            snip_len -= item_len + 1 - is_last_entry;
        }

        if (!is_last_entry && snip[-1]!=',') goto badly_formatted; // expecting a , after the end of all items in this entry
        
        num_entries++;
    }

    // we successfully segged all items of all entries - now we enter into the subfields - the number of entries preceeded by a AOS_NUM_ENTRIES
    char str[30];
    unsigned str_len;
    str[0] = AOS_NUM_ENTRIES;
    str_int (num_entries, str+1, &str_len);

    seg_one_subfield ((VBlockP)vb, str, str_len+1, subfield_ctx->dict_id, 0); 

    return;

badly_formatted:
    // roll back all the changed data
    for (unsigned item_i=0; item_i < num_items_in_struct ; item_i++) {
        ctxs[item_i].mtf_i.len = saved_mtf_i_len[item_i];
        ctxs[item_i].local.len = saved_local_len[item_i];
        ctxs[item_i].txt_len   = saved_txt_len[item_i];
    }

    // now save the entire snip in the dictionary
    seg_one_subfield ((VBlockP)vb, saved_snip, saved_snip_len, subfield_ctx->dict_id, saved_snip_len); 
}                           

static bool seg_gff3_special_info_subfields(VBlockP vb_, MtfContextP ctx, const char **this_value, unsigned *this_value_len, char *optimized_snip)
{
    VBlockGFF3 *vb = (VBlockGFF3 *)vb_;

    // ID - this is a sequential number (at least in GRCh37/38)
    if (ctx->dict_id.num == dict_id_ATTR_ID) {
        vb->last_id = seg_pos_field ((VBlockP)vb, vb->last_id, NULL, true, ctx->did_i, 
                                     *this_value, *this_value_len, "ID", false);
        return false; // do not add to dictionary/b250 - we already did it
    }

    // Dbxref (example: "dbSNP_151:rs1307114892") - we divide to the non-numeric part which we store
    // in a dictionary and the numeric part which store in a NUMERICAL_ID_DATA section
    if (ctx->dict_id.num == dict_id_ATTR_Dbxref) {
        seg_id_field (vb_, ctx->dict_id, *this_value, *this_value_len, false); // discard the const as seg_id_field modifies

        return false; // do not add to dictionary/b250 - we already did it
    }

    // subfields that are arrays of structs, for example:
    // "non_coding_transcript_variant 0 ncRNA ENST00000431238,intron_variant 0 primary_transcript ENST00000431238"
    if (ctx->dict_id.num == dict_id_ATTR_Variant_effect      ||
        ctx->dict_id.num == dict_id_ATTR_sift_prediction     ||
        ctx->dict_id.num == dict_id_ATTR_polyphen_prediction ||
        ctx->dict_id.num == dict_id_ATTR_variant_peptide) {
        unsigned num_item_in_struct = (ctx->dict_id.num == dict_id_ATTR_variant_peptide ? 2 : 3); // num items excluding ENST Id
        seg_gff3_array_of_struct (vb, ctx, num_item_in_struct, *this_value, *this_value_len);
        return false;
    }

    // we store these 3 in one dictionary, as they are correlated and will compress better together
    if (ctx->dict_id.num == dict_id_ATTR_Variant_seq   ||
        ctx->dict_id.num == dict_id_ATTR_Reference_seq ||
        ctx->dict_id.num == dict_id_ATTR_ancestral_allele) {

        // note: all three are stored together in dict_id_ATTR_Variant_seq as they are correlated
        MtfContext *ctx = mtf_get_ctx_by_dict_id (vb, (DictIdType)dict_id_ATTR_Variant_seq); 

        seg_add_to_local_text (vb_, ctx, *this_value, *this_value_len, *this_value_len);
        return false; // do not add to dictionary/b250 - we already did it
    }

    // Optimize Variant_freq
    unsigned optimized_snip_len;
    if (flag_optimize_Vf && (ctx->dict_id.num == dict_id_ATTR_Variant_freq) &&
        optimize_float_2_sig_dig (*this_value, *this_value_len, 0, optimized_snip, &optimized_snip_len)) {
        
        vb->vb_data_size -= (int)(*this_value_len) - (int)optimized_snip_len;
        *this_value = optimized_snip;
        *this_value_len = optimized_snip_len;
        return true; // procedue with adding to dictionary/b250
    }

    return true; // all other cases -  procedue with adding to dictionary/b250
}

const char *seg_gff3_data_line (VBlock *vb_,   
                                const char *field_start_line)     // index in vb->txt_data where this line starts
{
    VBlockGFF3 *vb = (VBlockGFF3 *)vb_;
    ZipDataLineGFF3 *dl = DATA_LINE (vb->line_i);

    const char *next_field, *field_start;
    unsigned field_len=0;
    char separator;

    int32_t len = &vb->txt_data.data[vb->txt_data.len] - field_start_line;

    // SEQID
    field_start = field_start_line;
    next_field = seg_get_next_item (vb, field_start, &len, false, true, false, &field_len, &separator, NULL, "SEQID");
    seg_chrom_field (vb_, field_start, field_len);

    // SOURCE
    field_start = next_field;
    next_field = seg_get_next_item (vb, field_start, &len, false, true, false, &field_len, &separator, NULL, "SOURCE");
    seg_one_field (vb, field_start, field_len, GFF3_SOURCE, field_len + 1);

    // TYPE
    field_start = next_field;
    next_field = seg_get_next_item (vb, field_start, &len, false, true, false, &field_len, &separator, NULL, "TYPE");
    seg_one_field (vb, field_start, field_len, GFF3_TYPE, field_len + 1);

    // START - delta vs previous line 
    field_start = next_field;
    next_field = seg_get_next_item (vb, field_start, &len, false, true, false, &field_len, &separator, NULL, "START");
    vb->last_pos = seg_pos_field (vb_, vb->last_pos, NULL, false, GFF3_START, field_start, field_len, "START", true);
    random_access_update_pos (vb_, vb->last_pos);

    // END - delta vs START
    field_start = next_field;
    next_field = seg_get_next_item (vb, field_start, &len, false, true, false, &field_len, &separator, NULL, "END");
    seg_pos_field (vb_, vb->last_pos, NULL, false, GFF3_END, field_start, field_len, "END", true);

    // SCORE
    field_start = next_field;
    next_field = seg_get_next_item (vb, field_start, &len, false, true, false, &field_len, &separator, NULL, "SCORE");
    seg_one_field (vb, field_start, field_len, GFF3_SCORE, field_len + 1);

    // STRAND
    field_start = next_field;
    next_field = seg_get_next_item (vb, field_start, &len, false, true, false, &field_len, &separator, NULL, "STRAND");
    seg_one_field (vb, field_start, field_len, GFF3_STRAND, field_len + 1);

    // PHASE
    field_start = next_field;
    next_field = seg_get_next_item (vb, field_start, &len, false, true, false, &field_len, &separator, NULL, "PHASE");
    seg_one_field (vb, field_start, field_len, GFF3_PHASE, field_len + 1);

    // ATTRIBUTES
    field_start = next_field; 
    bool has_13 = false; // does this line end in Windows-style \r\n rather than Unix-style \n
    next_field = seg_get_next_item (vb, field_start, &len, true, false, false, &field_len, &separator, &has_13, 
                                    DTF(names)[GFF3_ATTRS] /* pointer to string to allow pointer comparison */); 

    seg_info_field (vb_, &dl->attrs_mtf_i, &vb->iname_mapper_buf, &vb->num_info_subfields, seg_gff3_special_info_subfields,
                    field_start, field_len, has_13, has_13); // we break the const bc seg_vcf_info_field might add a :#

    return next_field;
}

