// ------------------------------------------------------------------
//   fast.c
//   Copyright (C) 2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "fast_private.h"
#include "seg.h"
#include "context.h"
#include "file.h"
#include "strings.h"
#include "piz.h"
#include "optimize.h"
#include "dict_id.h"
#include "refhash.h"
#include "endianness.h"
#include "zfile.h"
#include "piz.h"
#include "buffer.h"
#include "domqual.h"

// used in case of flag_optimize_DESC to count the number of lines, as we need it for the description
static void fastq_txtfile_count_lines (VBlockP vb)
{
    uint32_t num_lines = 0;
    ARRAY (const char, txt, vb->txt_data);

    for (uint32_t i=0; i < vb->txt_data.len; i++)
        if (txt[i] == '\n') num_lines++;

    ASSERT (num_lines % 4 == 0, "Error in fastq_txtfile_count_lines: expecting number of txt lines in VB to be a multiple of 4, but found %u", num_lines);

    vb->first_line = txt_file->num_lines + 1; // this is normally not used in ZIP
    txt_file->num_lines += num_lines / 4;     // update here instead of in zip_update_txt_counters;
}

void fastq_zip_read_one_vb (VBlockP vb)
{
    // in case we're optimizing DESC in FASTQ, we need to know the number of lines
    if (flag_optimize_DESC)
        fastq_txtfile_count_lines (vb);
}

static void fastq_initialize_pair_iterators (VBlockFAST *vb)
{
    // initialize pair iterators
    for (DidIType did_i=0; did_i < vb->num_dict_ids; did_i++) {
        Context *ctx = &vb->contexts[did_i];
        if (buf_is_allocated (&ctx->pair) && dict_id_is_type_1 (ctx->dict_id)) // DESC components
            ctx->pair_b250_iter = (SnipIterator){ .next_b250 = FIRSTENT (uint8_t, ctx->pair),
                                                  .prev_word_index = -1 };
    }
}

// case of --optimize-DESC: generate the prefix of the read name from the txt file name
// eg. "../../fqs/sample.1.fq.gz" -> "@sample-1:"
static void fastq_get_optimized_desc_read_name (VBlockFAST *vb)
{
    vb->optimized_desc = malloc (strlen (txt_file->basename) + 30); // leave room for the line number to follow
    vb->optimized_desc[0] = '@';
    strcpy (&vb->optimized_desc[1], txt_file->basename);
    file_get_raw_name_and_type (&vb->optimized_desc[1], NULL, NULL); // remove file type extension
    vb->optimized_desc_len = strlen (vb->optimized_desc) + 1; // +1 :
    vb->optimized_desc[vb->optimized_desc_len-1] = ':';

    // replace '.' in the filename with '-' as '.' is a separator in seg_compound_field and would needless inflate the number of contexts
    for (unsigned i=0; i < vb->optimized_desc_len; i++)
        if (vb->optimized_desc[i] == '.') vb->optimized_desc[i] = '-';
}

void fastq_seg_initialize (VBlockFAST *vb)
{
    // thread safety: this will be initialized by vb_i=1, while it holds a mutex in zip_compress_one_vb
    static bool structured_initialized = false;
    if (!structured_initialized) {
        seg_initialize_compound_structured ((VBlockP)vb, "D?ESC", &structured_DESC); 
        structured_initialized = true;
    }

    if (flag_reference == REF_EXTERNAL || flag_reference == REF_EXT_STORE) {
        vb->contexts[FASTQ_STRAND].ltype   = LT_BITMAP;
        vb->contexts[FASTQ_STRAND].lcomp   = COMP_NONE; // bz2 and lzma only make it bigger

        vb->contexts[FASTQ_GPOS].ltype     = LT_UINT32;
        vb->contexts[FASTQ_GPOS].flags     = CTX_FL_STORE_INT;
        vb->contexts[FASTQ_GPOS].lcomp     = COMP_LZMA;

    }

    vb->contexts[FASTQ_SQBITMAP].ltype = LT_BITMAP; // used in non-reference too

    vb->contexts[FASTQ_NONREF].ltype   = LT_SEQUENCE;
    vb->contexts[FASTQ_NONREF].lcomp   = flag_optimize_SEQ ? COMP_LZMA : COMP_ACGT; // ACGT is a lot faster, but ~5% less good

    vb->contexts[FASTQ_QUAL].ltype     = LT_SEQUENCE; // might be overridden by domqual_convert_qual_to_domqual
    vb->contexts[FASTQ_QUAL].inst      = 0; // don't inherit from previous file (we will set CTX_INST_NO_CALLBACK if needed, later)

     if (flag_pair == PAIR_READ_2) {
        vb->contexts[FASTQ_GPOS]  .inst  = CTX_INST_PAIR_LOCAL;
        vb->contexts[FASTQ_STRAND].inst  = CTX_INST_PAIR_LOCAL; 
        vb->contexts[FASTQ_STRAND].lcomp = COMP_BZ2; // pair2 is expected to contain long runs, so BZ2 is good. Cancel the COMP_NONE possibly inherited from the last vb of the previous bound file

        piz_uncompress_all_ctxs ((VBlockP)vb, vb->pair_vb_i);
        vb->z_data.len = 0; // we've finished reading the pair file z_data, next, we're going to write to z_data our compressed output

        fastq_initialize_pair_iterators (vb);
    }

    if (flag_optimize_DESC) 
        fastq_get_optimized_desc_read_name (vb);
}

void fastq_seg_finalize (VBlockP vb)
{
    // check if domqual compression is applicable to this quality data, and prepare QUAL/QDOMRUNS local data if it is
    domqual_convert_qual_to_domqual (vb, fastq_zip_get_start_len_line_i_qual, FASTQ_QUAL);
}

// called by txtfile_read_vblock when reading the 2nd file in a fastq pair - counts the number of fastq "lines" (each being 4 textual lines),
// comparing to the number of lines in the first file of the pair
// returns true if we have at least as much as needed, and sets unconsumed_len to the amount of excess characters read
// returns false is we don't yet have pair_1_num_lines lines - we need to read more
bool fastq_txtfile_have_enough_lines (VBlockP vb_, uint32_t *unconsumed_len)
{
    VBlockFAST *vb = (VBlockFAST *)vb_;

    const char *next  = FIRSTENT (const char, vb->txt_data);
    const char *after = AFTERENT (const char, vb->txt_data);

    for (uint32_t line_i=0; line_i < vb->pair_num_lines * 4; line_i++) {
        while (*next != '\n' && next < after) next++; // note: next[-1] in the first iteration an underflow byte of the buffer, so no issue
        if (next >= after) 
            return false;
        next++; // skip newline
    }

    *unconsumed_len = after - next;
    return true;
}

// called from I/O thread ahead of zip or piz a pair 2 vb - to read data we need from the previous pair 1 file
// returns true if successful, false if there isn't a vb with vb_i in the previous file
bool fastq_read_pair_1_data (VBlockP vb_, uint32_t first_vb_i_of_pair_1, uint32_t last_vb_i_of_pair_1)
{
    VBlockFAST *vb = (VBlockFAST *)vb_;
    uint64_t save_offset = file_tell (z_file);
    uint64_t save_disk_so_far = z_file->disk_so_far;

    vb->pair_vb_i = first_vb_i_of_pair_1 + (vb->vblock_i - last_vb_i_of_pair_1 - 1);

    SectionListEntry *sl = sections_vb_first (vb->pair_vb_i, true);
    if (!sl) return false;

    // get num_lines from vb header
    SectionHeaderVbHeader *vb_header = zfile_read_section_header (sl->offset, sizeof (SectionHeaderVbHeader));
    ASSERT0 (vb_header, "Error in fastq_read_pair_1_data: failed to read vb_header of pair file");

    vb->pair_num_lines = BGEN32 (vb_header->num_lines);

    // read into ctx->pair the data we need from our pair: DESC and its components, GPOS and STRAND
    sl++;
    buf_alloc (vb, &vb->z_section_headers, MAX ((MAX_DICTS * 2 + 50),  vb->z_section_headers.len + MAX_SUBFIELDS + 10) * sizeof(uint32_t), 0, "z_section_headers", 1); // room for section headers  
    while (sl->section_type == SEC_B250 || sl->section_type == SEC_LOCAL) {
        
        if (((dict_id_is_type_1 (sl->dict_id) || sl->dict_id.num == dict_id_fields[FASTQ_DESC]) && sl->section_type == SEC_B250) ||
            ((sl->dict_id.num == dict_id_fields[FASTQ_GPOS] || sl->dict_id.num == dict_id_fields[FASTQ_STRAND]) && sl->section_type == SEC_LOCAL)) { // these are local sections
            
            *ENT (uint32_t, vb->z_section_headers, vb->z_section_headers.len) = vb->z_data.len; 
            int32_t ret = zfile_read_section (z_file, (VBlockP)vb, vb->pair_vb_i, NO_SB_I, &vb->z_data, "data", sizeof(SectionHeaderCtx), 
                                              sl->section_type, sl); // returns 0 if section is skipped
            ASSERT (ret != EOF, "Error in fastq_read_pair_1_data: vb_i=%u failed to read from pair_vb=%u dict_id=%s", vb->vblock_i, vb->pair_vb_i, err_dict_id (sl->dict_id));
            vb->z_section_headers.len++;
        }
        
        sl++;
    }

    file_seek (z_file, save_offset, SEEK_SET, false); // restore
    z_file->disk_so_far = save_disk_so_far;
    
    return true;
}

// I/O thread: called from piz_read_one_vb as DTPZ(piz_read_one_vb)
bool fastq_piz_read_one_vb (VBlockP vb, SectionListEntryP sl)
{
    // if we're grepping we we uncompress and reconstruct the DESC from the I/O thread, and terminate here if this VB is to be skipped
    if (flag_grep && !fast_piz_test_grep ((VBlockFAST *)vb)) return false; 

    // in case of this is a paired fastq file, and this is the 2nd file of a pair - 
    // we also read the equivalent sections from the first (bound) file
    ARRAY (const unsigned, section_index, vb->z_section_headers);
    for (uint32_t sec_i=1; sec_i < vb->z_section_headers.len; sec_i++) {
        SectionHeaderCtx *header = (SectionHeaderCtx *)ENT (char, vb->z_data, section_index[sec_i]);
        if (header->h.flags & CTX_FL_PAIRED) {
            uint32_t prev_file_first_vb_i, prev_file_last_vb_i;
            sections_get_prev_file_vb_i (sl, &prev_file_first_vb_i, &prev_file_last_vb_i);

            fastq_read_pair_1_data (vb, prev_file_first_vb_i, prev_file_last_vb_i);
            break;
        }
    }

    return true;
}

uint32_t fastq_get_pair_vb_i (VBlockP vb)
{
    return ((VBlockFAST *)vb)->pair_vb_i;
}

// concept: we treat every 4 lines as a "line". the Description/ID is stored in DESC dictionary and segmented to subfields D?ESC.
// The sequence is stored in SEQ data. In addition, we utilize the TEMPLATE dictionary for metadata on the line, namely
// the length of the sequence and whether each line has a \r.
const char *fastq_seg_txt_line (VBlockFAST *vb, const char *field_start_line, bool *has_13)     // index in vb->txt_data where this line starts
{
    ZipDataLineFAST *dl = DATA_LINE (vb->line_i);

    const char *next_field, *field_start=field_start_line;
    unsigned field_len=0;
    char separator;

    int32_t len = (int32_t)(AFTERENT (char, vb->txt_data) - field_start_line);

    // the leading @ - just verify it (it will be included in D0ESC subfield)
    ASSSEG (*field_start != '\n', field_start, "%s: Invalid FASTQ file format: unexpected newline", global_cmd);

    ASSSEG (*field_start == '@', field_start, "%s: Invalid FASTQ file format: expecting description line to start with @ but it starts with %c",
            global_cmd, *field_start);

    // DESC - the description/id line is vendor-specific. example:
    // @A00910:85:HYGWJDSXX:1:1101:3025:1000 1:N:0:CAACGAGAGC+GAATTGAGTG (<-- this is Illumina format)
    // See here for details of Illumina subfields: https://help.basespace.illumina.com/articles/descriptive/fastq-files/
    next_field = seg_get_next_line (vb, field_start, &len, &field_len, has_13, "DESC");
 
    // if flag_optimize_DESC is on, we replace the description with filename:line_i 
    unsigned unoptimized_len = 0; // 0 unless optimized
    if (flag_optimize_DESC) {
        unoptimized_len = field_len;
        field_start = vb->optimized_desc;
        field_len = vb->optimized_desc_len + str_int (vb->first_line + vb->line_i, &vb->optimized_desc[vb->optimized_desc_len]);        
    }

    // we segment it using / | : and " " as separators. 
    seg_compound_field ((VBlockP)vb, &vb->contexts[FASTQ_DESC], field_start, field_len, &vb->desc_mapper, structured_DESC, true, unoptimized_len, 0);
    SEG_EOL (FASTQ_E1L, true);

    // SEQ - just get the whole line
    const char *seq_start = next_field;
    dl->seq_data_start = next_field - vb->txt_data.data;
    next_field = seg_get_next_item (vb, next_field, &len, true, false, false, &dl->seq_len, &separator, has_13, "SEQ");

    fast_seg_seq (vb, seq_start, dl->seq_len, FASTQ_SQBITMAP);

    // Add LOOKUP snip with seq_len
    char snip[10];
    snip[0] = SNIP_LOOKUP;
    unsigned seq_len_str_len = str_int (dl->seq_len, &snip[1]);
    seg_by_did_i (vb, snip, 1 + seq_len_str_len, FASTQ_SQBITMAP, 0); 
    vb->contexts[FASTQ_NONREF].txt_len += dl->seq_len; // account for the txt data in NONREF

    SEG_EOL (FASTQ_E1L, true);

    // PLUS - next line is expected to be a "+"
    GET_LAST_ITEM ("+");
    ASSSEG (*field_start=='+' && field_len==1, field_start, "%s: Invalid FASTQ file format: expecting middle line to be a \"+\" (with no spaces) but it is \"%.*s\"",
            global_cmd, field_len, field_start);

    seg_by_did_i (vb, "+", 1, FASTQ_PLUS, 1);
    SEG_EOL (FASTQ_E1L, true);

    // QUAL - just get the whole line and make sure its length is the same as SEQ
    dl->qual_data_start = next_field - vb->txt_data.data;
    GET_LAST_ITEM ("QUAL");
    vb->contexts[FASTQ_QUAL].local.len += dl->seq_len;
    vb->contexts[FASTQ_QUAL].txt_len   += dl->seq_len;

    // End Of Line    
    SEG_EOL (FASTQ_E1L, true);

    ASSSEG (field_len == dl->seq_len, field_start, "%s: Invalid FASTQ file format: sequence_len=%u and quality_len=%u. Expecting them to be the same.\nSEQ=%.*s\nQUAL==%.*s",
            global_cmd, dl->seq_len, field_len, dl->seq_len, seq_start, field_len, field_start);
 
    return next_field;
}

// callback function for compress to get data of one line (called by comp_compress_bzlib)
void fastq_zip_get_start_len_line_i_qual (VBlock *vb, uint32_t vb_line_i, 
                                          char **line_qual_data, uint32_t *line_qual_len, // out
                                          char **unused_data,   uint32_t *unused_len) 
{
    ZipDataLineFAST *dl = DATA_LINE (vb_line_i);
     
    *line_qual_data = ENT (char, vb->txt_data, dl->qual_data_start);
    *line_qual_len  = dl->seq_len;
    *unused_data    = NULL;
    *unused_len     = 0;

    // note - we optimize just before compression - likely the string will remain in CPU cache
    // removing the need for a separate load from RAM
    if (flag_optimize_QUAL) optimize_phred_quality_string (*line_qual_data, *line_qual_len);
}

// returns true if section is to be skipped reading / uncompressing
bool fastq_piz_is_skip_section (VBlockP vb, SectionType st, DictId dict_id)
{
    if (!vb) return false; // we don't skip reading any SEC_DICT sections

    // note that piz_read_global_area rewrites --header-only as flag_header_one
    if (flag_header_one && 
        (dict_id.num == dict_id_fields[FASTQ_NONREF] || dict_id.num == dict_id_fields[FASTQ_QUAL] || dict_id.num == dict_id_fields[FASTQ_PLUS]))
        return true;
        
    // when grepping by I/O thread - skipping all sections but DESC
    if (flag_grep && (vb->grep_stages == GS_TEST) && 
        dict_id.num != dict_id_fields[FASTQ_DESC] && !dict_id_is_fast_desc_sf (dict_id))
        return true;

    // if grepping, compute thread doesn't need to decompressed DESC again
    if (flag_grep && (vb->grep_stages == GS_UNCOMPRESS) && 
        (dict_id.num == dict_id_fields[FASTQ_DESC] || dict_id_is_fast_desc_sf (dict_id)))
        return true;

//    if (dump_one_b250_dict_id.num && dump_one_b250_dict_id.num != dict_id.num)
//        return true;
    
//    if (dump_one_local_dict_id.num && dump_one_local_dict_id.num != dict_id.num)
//        return true;

    return false;
}

// PIZ: SEQ reconstruction 
void fastq_piz_reconstruct_seq (VBlock *vb_, Context *bitmap_ctx, const char *seq_len_str, unsigned seq_len_str_len)
{
    VBlockFAST *vb = (VBlockFAST *)vb_;
    ASSERT0 (bitmap_ctx && bitmap_ctx->did_i == FASTQ_SQBITMAP, "Error in fastq_piz_reconstruct_seq: context is not FASTQ_SQBITMAP");

    if (piz_is_skip_section (vb, SEC_LOCAL, bitmap_ctx->dict_id)) return; // if case we need to skip the SEQ field (for the entire file)

    Context *nonref_ctx = &vb->contexts[FASTQ_NONREF];
    Context *gpos_ctx =   &vb->contexts[FASTQ_GPOS];
    Context *strand_ctx = &vb->contexts[FASTQ_STRAND];

    // get seq_len, gpos and direction
    vb->seq_len = atoi (seq_len_str); // terminated by SNIP_SEP=0
    
    if (buf_is_allocated (&bitmap_ctx->local)) { // not all non-ref

        bool forward_strand;
        int64_t gpos;

        // first file of a pair ("pair 1") or a non-pair fastq
        if (!vb->pair_vb_i) {
            uint32_t gpos_bgen = NEXTLOCAL (uint32_t, gpos_ctx);
            gpos = BGEN32 (gpos_bgen); 
            forward_strand = NEXTLOCALBIT (strand_ctx);        
        }

        // 2nd file of a pair ("pair 2")
        else {
            // forward_strand: the strand bit is a 1 iff the strand is the same as the pair
            bool pair_forward_strand = PAIRBIT (strand_ctx);
            forward_strand = NEXTLOCALBIT (strand_ctx) ? pair_forward_strand : !pair_forward_strand;

            // gpos: reconstruct, then cancel the reconstruction and just use last_value
            uint32_t reconstructed_len = piz_reconstruct_from_ctx (vb, FASTQ_GPOS, 0);
            vb->txt_data.len -= reconstructed_len; // roll back reconstruction
            gpos = gpos_ctx->last_value.i;
        }
        
        if (forward_strand)  // normal (note: this condition test is outside of the tight loop)
            for (uint32_t i=0; i < vb->seq_len; i++)
                if (NEXTLOCALBIT (bitmap_ctx))  // get base from reference
                    RECONSTRUCT1 (ACGT_DECODE (&genome.ref, gpos + i));
                else  // get base from nonref
                    RECONSTRUCT1 (NEXTLOCAL (char, nonref_ctx));

        else // reverse complement
            for (uint32_t i=0; i < vb->seq_len; i++) 
                if (NEXTLOCALBIT (bitmap_ctx))  // case: get base from reference
                    RECONSTRUCT1 (complement [(uint8_t)ACGT_DECODE (&genome.ref, gpos + vb->seq_len-1 - i)]);
                else  // case: get base from nonref
                    RECONSTRUCT1 (NEXTLOCAL (char, nonref_ctx));
    }
    else {
        RECONSTRUCT (ENT (char, nonref_ctx->local, nonref_ctx->next_local), vb->seq_len);
        nonref_ctx->next_local += vb->seq_len;
    }
}

void fastq_piz_reconstruct_vb (VBlockFAST *vb)
{
    ASSERT0 (!flag_reference || genome.ref.num_of_bits, "Error in fastq_piz_reconstruct_vb: reference is not loaded correctly");

    fastq_initialize_pair_iterators (vb);

    if (!flag_grep) piz_map_compound_field ((VBlockP)vb, dict_id_is_fast_desc_sf, &vb->desc_mapper); // it not already done during grep

    for (uint32_t vb_line_i=0; vb_line_i < vb->lines.len; vb_line_i++) {

        vb->line_i = 4 * (vb->first_line + vb_line_i); // each vb line is a fastq record which is 4 txt lines
        vb->dont_show_curr_line = false; // might become true due --regions or --grep
        
        uint32_t txt_data_start_line = vb->txt_data.len;

        piz_reconstruct_from_ctx (vb, FASTQ_DESC, 0);
        piz_reconstruct_from_ctx (vb, FASTQ_E1L,  0);

        // minor bug here: since all the EOL fields are aliases, in case header_one, we dont consume the
        // missing line EOLs and they will show wrong ones to the remaining lines. minor bc in virtually all files,
        // the EOL is identical in all lines
        if (!flag_header_one) { // note that piz_read_global_area rewrites --header-only as flag_header_one
            piz_reconstruct_from_ctx (vb, FASTQ_SQBITMAP, 0);
            piz_reconstruct_from_ctx (vb, FASTQ_E2L,  0);
            piz_reconstruct_from_ctx (vb, FASTQ_PLUS, 0);
            piz_reconstruct_from_ctx (vb, FASTQ_E3L,  0);
            piz_reconstruct_from_ctx (vb, FASTQ_QUAL, 0);
            piz_reconstruct_from_ctx (vb, FASTQ_E4L,  0);
        }

        // case: we're grepping, and this line doesn't match
        *AFTERENT (char, vb->txt_data) = 0; // for strstr
        if (vb->dont_show_curr_line || (flag_grep && !strstr (ENT (char, vb->txt_data, txt_data_start_line), flag_grep)))
            vb->txt_data.len = txt_data_start_line; // rollback
    }
}
