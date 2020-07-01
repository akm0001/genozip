// ------------------------------------------------------------------
//   sam_zip.c
//   Copyright (C) 2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "sam_private.h"
#include "reference.h"
#include "seg.h"
#include "context.h"
#include "file.h"
#include "random_access.h"
#include "endianness.h"
#include "strings.h"
#include "zip.h"
#include "optimize.h"
#include "dict_id.h"
#include "arch.h"

static Structured structured_QNAME;

// called by I/O thread 
void sam_zip_initialize (void)
{
    // if there is no external reference provided, then we create our internal one, and store it
    // (if an external reference IS provided, the user can decide whether to store it or not, with --store-reference)
    if (flag_reference == REF_NONE) flag_reference = REF_INTERNAL;

    // in case of internal reference, we need to initialize. in case of --reference, it was initialized by ref_read_external_reference()
    if (!flag_reference || flag_reference == REF_INTERNAL) ref_initialize_ranges (false); // it will be REF_INTERNAL if this is the 2nd+ non-conatenated file

    // evb buffers must be alloced by I/O threads, since other threads cannot modify evb's buf_list
    random_access_alloc_ra_buf (evb, 0);
}

// callback function for compress to get data of one line (called by comp_compress_bzlib)
void sam_zip_get_start_len_line_i_qual (VBlock *vb, uint32_t vb_line_i, 
                                        char **line_qual_data, uint32_t *line_qual_len, // out
                                        char **line_u2_data,   uint32_t *line_u2_len) 
{
    ZipDataLineSAM *dl = DATA_LINE (vb_line_i);
     
    *line_qual_data = ENT (char, vb->txt_data, dl->qual_data_start);
    *line_qual_len  = dl->qual_data_len;
    *line_u2_data   = dl->u2_data_start ? ENT (char, vb->txt_data, dl->u2_data_start) : NULL;
    *line_u2_len    = dl->u2_data_len;

    // if QUAL is just "*" (i.e. unavailable) replace it by " " because '*' is a legal PHRED quality value that will confuse PIZ
    if (dl->qual_data_len == 1 && (*line_qual_data)[0] == '*') 
        *line_qual_data = " "; // pointer to static string

    // note - we optimize just before compression - likely the string will remain in CPU cache
    // removing the need for a separate load from RAM
    else if (flag_optimize_QUAL) {
        optimize_phred_quality_string (*line_qual_data, *line_qual_len);
        if (*line_u2_data) optimize_phred_quality_string (*line_u2_data, *line_u2_len);
    }
}

// callback function for compress to get data of one line
void sam_zip_get_start_len_line_i_bd (VBlock *vb, uint32_t vb_line_i, 
                                      char **line_bd_data, uint32_t *line_bd_len,  // out 
                                      char **unused1,  uint32_t *unused2)
{
    ZipDataLineSAM *dl = DATA_LINE (vb_line_i);

    *line_bd_data = dl->bd_data_len ? ENT (char, vb->txt_data, dl->bd_data_start) : NULL;
    *line_bd_len  = dl->bd_data_len;
    *unused1 = NULL;
    *unused2 = 0;
}   

// callback function for compress to get BI data of one line
// if BD data is available for this line too - we output the character-wise delta as they are correlated
// we expect the length of BI and BD to be the same, the length of the sequence. this is enforced by sam_seg_optional_field.
void sam_zip_get_start_len_line_i_bi (VBlock *vb, uint32_t vb_line_i, 
                                      char **line_bi_data, uint32_t *line_bi_len,  // out 
                                      char **unused1,  uint32_t *unused2)
{
    ZipDataLineSAM *dl = DATA_LINE (vb_line_i);

    if (dl->bi_data_len && dl->bd_data_len) {

        ASSERT (dl->bi_data_len == dl->bd_data_len, "Error: expecting dl->bi_data_len=%u to be equal to dl->bd_data_len=%u",
                dl->bi_data_len, dl->bd_data_len);

        char *bi       = ENT (char, vb->txt_data, dl->bi_data_start);
        const char *bd = ENT (char, vb->txt_data, dl->bd_data_start);

        // calculate character-wise delta
        for (unsigned i=0; i < dl->bi_data_len; i++) *(bi++) -= *(bd++);
    }

    *line_bi_data = dl->bi_data_len ? ENT (char, vb->txt_data, dl->bi_data_start) : NULL;
    *line_bi_len  = dl->bi_data_len;
    *unused1 = NULL;
    *unused2 = 0;
}   

void sam_seg_initialize (VBlock *vb)
{
    // thread safety: this will be initialized by vb_i=1, while it holds a mutex in zip_compress_one_vb
    static bool structured_initialized = false;
    if (!structured_initialized) {
        seg_initialize_compound_structured ((VBlockP)vb, "Q?NAME", &structured_QNAME);
        structured_initialized = true;
    }

    vb->contexts[SAM_RNAME].flags      = CTX_FL_NO_STONS; // needs b250 node_index for random access
    vb->contexts[SAM_SEQ_BITMAP].ltype = CTX_LT_SEQ_BITMAP;
    vb->contexts[SAM_SEQNOREF].flags   = CTX_FL_LOCAL_LZMA;
    vb->contexts[SAM_SEQNOREF].ltype   = CTX_LT_SEQUENCE;
    vb->contexts[SAM_QUAL].ltype       = CTX_LT_SEQUENCE;
    vb->contexts[SAM_TLEN].flags       = CTX_FL_STORE_VALUE;
    vb->contexts[SAM_OPTIONAL].flags   = CTX_FL_STRUCTURED;
}

// TLEN - 3 cases: 
// 1. if a non-zero value that is the negative of the previous line - a SNIP_DELTA & "-" (= value negation)
// 2. else, tlen>0 and pnext_pos_delta>0 and seq_len>0 tlen is stored as SNIP_SPECIAL & tlen-pnext_pos_delta-seq_len
// 3. else, stored as is
static inline void sam_seg_tlen_field (VBlockSAM *vb, const char *tlen, unsigned tlen_len, int64_t pnext_pos_delta, int32_t cigar_seq_len)
{
    ASSSEG (tlen_len, tlen, "%s: empty TLEN", global_cmd);
    ASSSEG (str_is_int (tlen, tlen_len), tlen, "%s: expecting TLEN to be an integer", global_cmd);

    Context *ctx = &vb->contexts[SAM_TLEN];

    int64_t tlen_value = (int64_t)strtoull (tlen, NULL, 10 /* base 10 */); // strtoull can handle negative numbers, despite its name
    
    // case 1
    if (tlen_value && tlen_value == -ctx->last_value) {
        char snip_delta[2] = { SNIP_SELF_DELTA, '-'};
        seg_by_ctx ((VBlockP)vb, snip_delta, 2, ctx, tlen_len + 1, NULL);
    }
    // case 2:
    else if (tlen_value > 0 && pnext_pos_delta > 0 && cigar_seq_len > 0) {
        char tlen_by_calc[50];
        tlen_by_calc[0] = SNIP_SPECIAL;
        tlen_by_calc[1] = SAM_SPECIAL_TLEN;
        unsigned tlen_by_calc_len = str_int (tlen_value - pnext_pos_delta - (int64_t)cigar_seq_len, &tlen_by_calc[2]);
        seg_by_ctx ((VBlockP)vb, tlen_by_calc, tlen_by_calc_len + 2, ctx, tlen_len+1, NULL);
    }
    // case default: add as is
    else 
        seg_by_ctx ((VBlockP)vb, tlen, tlen_len, ctx, tlen_len+1, NULL);

    ctx->last_value = tlen_value;
}

// returns length of string ending with separator, or -1 if separator was not found
static inline int sam_seg_get_next_subitem (const char *str, int str_len, char separator)
{
    for (int i=0; i < str_len; i++) {
        if (str[i] == separator) return i;
        if (str[i] == ',' || str[i] == ';') return -1; // wrong separator encountered
    }
    return -1;
}

#define DO_SSF(ssf,sep) \
        ssf = &field[i]; \
        ssf##_len = sam_seg_get_next_subitem (&field[i], field_len-i, sep); \
        if (ssf##_len == -1) goto error; /* bad format */ \
        i += ssf##_len + 1; /* skip snip and separator */        

#define DEC_SSF(ssf) const char *ssf; \
                     int ssf##_len;

// Creates a bitmap from seq data - exactly one bit per base that is mapped to the reference (e.g. not for INSERT bases)
// - Normal SEQ: tracking CIGAR, we compare the sequence to the reference, indicating in a SAM_SEQ_BITMAP whether this
//   base in the same as the reference or not. In case of REF_INTERNAL, if the base is not already in the reference, we add it.
//   bases that differ from the reference are stored in SAM_SEQNOREF
// - Edge case: no POS (i.e. unaligned read) - we just store the sequence in SAM_SEQNOREF
// - Edge case: no CIGAR (it is "*") - we just treat it as an M and compare to the reference
// - Edge case: no SEQ (it is "*") - we '*' in SAM_SEQNOREF and indicate "different from reference" in the bitmap. We store a
//   single entry, regardless of the number of entries indicated by CIGAR
static void sam_seg_seq_field (VBlockSAM *vb, const char *seq, uint32_t seq_len, int64_t pos, const char *cigar, unsigned recursion_level)
{
    Context *bitmap_ctx = &vb->contexts[SAM_SEQ_BITMAP];
    Context *nonref_ctx = &vb->contexts[SAM_SEQNOREF];

    if (!recursion_level)
        bitmap_ctx->txt_len += seq_len + 1; // byte counts for --show-sections - +1 for terminating \t (note: E2 will be accounted in SEQ as its an alias)

    ASSERT0 (recursion_level < 4, "Error in sam_seg_seq_field: excess recursion"); // this would mean a read of about 4M bases... in 2020, this looks unlikely

    // allocate bitmap - provide name only if buffer is not allocated, to avoid re-writing param which would overwrite num_of_bits that overlays it
    buf_alloc (vb, &bitmap_ctx->local, MAX (bitmap_ctx->local.len + seq_len + sizeof(int64_t), vb->lines.len * seq_len / 5), CTX_GROWTH, 
               buf_is_allocated (&bitmap_ctx->local) ? NULL : "context->local", 0); 
    
    buf_alloc (vb, &nonref_ctx->local, MAX (nonref_ctx->local.len + seq_len, vb->lines.len * seq_len / 40), CTX_GROWTH, "context->local", nonref_ctx->did_i); 

    // we can't compare to the reference if POS is 0: we store the seqeuence in SEQNOREF without an indication in the bitmap
    if (!pos) {
        buf_add (&nonref_ctx->local, seq, seq_len);
        return; 
    }

    if (seq[0] == '*') return; // we already handled a missing seq (SEQ="*") by adding a '-' to CIGAR - no data added here

    Range *range = ref_zip_get_locked_range ((VBlockP)vb, pos);

    // when using an external refernce, pos has to be within the reference range (we already checked pos ref_zip_get_locked_range, now we check the end pos)
    int64_t final_seq_pos = pos + vb->ref_consumed - 1;
    ASSSEG (flag_reference == REF_INTERNAL || final_seq_pos <= range->last_pos, cigar,
            "Error: file has contig \"%.*s\", POS=%"PRId64" CIGAR=\"%s\", implying a final reference pos for this read at %"PRId64". However, the reference's last pos for this contig is %"PRId64,
            range->chrom_name_len, range->chrom_name, pos, cigar, final_seq_pos, range->last_pos);

    // Note: if range is NULL, it cannot be diffed against a reference, as the hash entry for this range is unfortunately already
    // occupied by another range (can only happen with REF_INTERNAL). We treat this as if all the bases don't match the ref
    if (!range) {
        buf_add (&nonref_ctx->local, seq, seq_len);
                
        for (uint32_t i=0; i < vb->ref_and_seq_consumed; i++) 
            buf_add_clear_bit (&bitmap_ctx->local); // not very efficient, but quiet rare

        random_access_update_last_pos ((VBlockP)vb, pos + vb->ref_consumed - 1);

        return; 
    }    

    uint32_t pos_index = pos - range->first_pos;
    uint32_t next_ref = pos_index;

    // if cigar is "*" we make it an M eg "151M"
    char alt_cigar[30];
    if (cigar[0] == '*' && cigar[1] == 0) {
        sprintf (alt_cigar, "%uM", seq_len);
        cigar = alt_cigar;
        vb->ref_consumed = vb->ref_and_seq_consumed = seq_len;
    }

    const char *next_cigar = cigar;

    uint32_t i=0;
    int subcigar_len=0;
    char cigar_op;

    // get exact ref lengths to be consumed in this recursion level
    uint32_t ref_len_this_level = MIN (vb->ref_consumed, range->last_pos - pos + 1);

    uint32_t range_len = (range->last_pos - range->first_pos + 1);
    
    while (i < seq_len || next_ref < pos_index + ref_len_this_level) {

        ASSERT0 (i <= seq_len && next_ref <= pos_index + ref_len_this_level, "Error in sam_seg_seq_field: i or next_ref are out of range");

        subcigar_len = strtod (next_cigar, (char **)&next_cigar); // get number and advance next_cigar
        
        cigar_op = *(next_cigar++);

        if (cigar_op == 'M' || cigar_op == '=' || cigar_op == 'X') { // alignment match or sequence match or mismatch

            ASSERT (subcigar_len > 0 && subcigar_len <= (seq_len - i), 
                    "Error in sam_seg_seq_field: CIGAR %s implies seq_len longer than actual seq_len=%u", cigar, seq_len);

            while (subcigar_len && next_ref < pos_index + ref_len_this_level) {

                // when we have an X we don't enter it into our internal ref, and we wait for a read with a = or M for that site,
                // as we assume that on average, more reads will have the reference base, leading to better compression
            
                bool normal_base = (seq[i] == 'A' || seq[i] == 'C' || seq[i] == 'G' || seq[i] == 'T');

                // case: we have not yet set a value for this site - we set it now. note: in ZIP, is_set means that the site
                // will be needed for pizzing. With REF_INTERNAL, this is equivalent to saying we have set the ref value for the site
                if (flag_reference == REF_INTERNAL && range && normal_base && !ref_is_nucleotide_set (range, next_ref)) { 
                    ref_set_nucleotide (range, next_ref, seq[i]);
                    bit_array_set (&range->is_set, next_ref); // we will need this ref to reconstruct
                    buf_add_set_bit (&bitmap_ctx->local);
                }

                // case our seq is identical to the reference at this site
                else if (range && normal_base && seq[i] == ref_get_nucleotide (range, next_ref)) {
                    buf_add_set_bit (&bitmap_ctx->local); // we will need this site in the reference for pizzing
                    bit_array_set (&range->is_set, next_ref); // we will need this ref to reconstruct
                }
                
                // case: ref is set to a different value - we store our value in nonref_ctx
                else {
                    NEXTENT (char, nonref_ctx->local) = seq[i];
                    buf_add_clear_bit (&bitmap_ctx->local);
                } 

                subcigar_len--;
                next_ref++;
                i++;
                vb->ref_and_seq_consumed--;
            }
        } // end if 'M', '=', 'X'

        // for Insertion or Soft clipping - this SEQ segment doesn't align with the reference - we leave it as is 
        else if (cigar_op == 'I' || cigar_op == 'S') {

            ASSSEG (subcigar_len > 0 && subcigar_len <= (seq_len - i), seq,
                    "Error in sam_seg_seq_field: CIGAR %s implies seq_len longer than actual seq_len=%u", cigar, seq_len);

            buf_add (&nonref_ctx->local, &seq[i], subcigar_len);
            i += subcigar_len;
        }

        // for Deletion or Skipping - we move the next_ref ahead
        else if (cigar_op == 'D' || cigar_op == 'N') {
            unsigned ref_consumed = MIN (subcigar_len, range_len - next_ref);

            next_ref += ref_consumed;
            subcigar_len -= ref_consumed;
        }

        // Hard clippping (H) or padding (P) we do nothing
        else if (cigar_op == 'H' || cigar_op == 'P') {}

        else {
            ASSSEG (cigar_op, vb->last_cigar, "Error in sam_seg_seq_field: End of CIGAR reached but we still have %u reference and %u sequence bases to consume",
                    pos_index + ref_len_this_level - next_ref, seq_len-i);        

            ASSSEG (false, vb->last_cigar, "Error in sam_seg_seq_field: Invalid CIGAR op: '%c' (ASCII %u)", cigar_op, cigar_op);        
        }

        // case: we're at the end of the reference AND we want more of it
        if (next_ref == pos_index + ref_len_this_level && subcigar_len) break;
    }

    if (range) mutex_unlock (range->mutex);       

    uint32_t this_seq_last_pos = pos + (next_ref - pos_index) - 1;

    // in REF_INTERNAL, the sequence can flow over to the next range as each range is 1M bases. this cannot happen
    // in REF_EXTERNAL as each range is the entire contig
    ASSERT (flag_reference == REF_INTERNAL || i == seq_len, "Error in sam_seg_seq_field: expecting i(%u) == seq_len(%u)", i, seq_len);

    // case: we have reached the end of the current reference range, but we still have sequence left - 
    // call recursively with remaining sequence and next reference range 
    if (i < seq_len) {

        ASSSEG (this_seq_last_pos <= MAX_POS, cigar, "%s: Error: POS=%"PRId64" and the consumed reference implied by CIGAR=\"%s\", exceeding MAX_POS=%"PRId64,
                global_cmd, pos, cigar, MAX_POS);

        vb->ref_consumed -= ref_len_this_level;

        char updated_cigar[100];
        if (subcigar_len) sprintf (updated_cigar, "%u%c%s", subcigar_len, cigar_op, next_cigar);

        sam_seg_seq_field (vb, seq + i, seq_len - i, range->last_pos + 1, subcigar_len ? updated_cigar : next_cigar, recursion_level + 1);
    }
    else // update RA of the VB with last pos of this line as implied by the CIGAR string
        random_access_update_last_pos ((VBlockP)vb, this_seq_last_pos);
}

static void sam_seg_SA_or_OA_field (VBlockSAM *vb, DictId subfield_dict_id, 
                                    const char *field, unsigned field_len, const char *field_name)
{
    // OA and SA format is: (rname ,pos ,strand ,CIGAR ,mapQ ,NM ;)+ . in OA - NM is optional (but its , is not)
    // Example SA:Z:chr13,52863337,-,56S25M70S,0,0;chr6,145915118,+,97S24M30S,0,0;chr18,64524943,-,13S22M116S,0,0;chr7,56198174,-,20M131S,0,0;chr7,87594501,+,34S20M97S,0,0;chr4,12193416,+,58S19M74S,0,0;
    // See: https://samtools.github.io/hts-specs/SAMtags.pdf
    static const Structured structured_SA_OA = {
        .repeats     = 0, 
        .num_items   = 6, 
        .flags       = 0,
        .repsep      = {0,0},
        .items       = { { .dict_id = {.id="@RNAME" }, .seperator = {','}, .did_i = DID_I_NONE},  // we don't mix with primary as primary is often sorted, and mixing will ruin its b250 compression
                         { .dict_id = {.id="@POS"   }, .seperator = {','}, .did_i = DID_I_NONE},  // we don't mix with primary as these are local-stored random numbers anyway - no advantage for mixing, and it would obscure the stats
                         { .dict_id = {.id="@STRAND"}, .seperator = {','}, .did_i = DID_I_NONE},
                         { .dict_id = {.id={'C'&0x3f,'I','G','A','R'}}, .seperator = {','}, .did_i = DID_I_NONE}, // we mix with primary - CIGAR tends to be a rather large dictionary, so better not have two copies of it
                         { .dict_id = {.id="@MAPQ"  }, .seperator = {','}, .did_i = DID_I_NONE},  // we don't mix with primary as primary often has a small number of values, and mixing will ruin its b250 compression
                         { .dict_id = {.id="NM:i"   }, .seperator = {';'}, .did_i = DID_I_NONE} } // we mix together with the NM option field
    };

    DEC_SSF(rname); DEC_SSF(pos); DEC_SSF(strand); DEC_SSF(cigar); DEC_SSF(mapq); DEC_SSF(nm); 

    Structured sa_oa = structured_SA_OA;

    for (uint32_t i=0; i < field_len; sa_oa.repeats++) {

        ASSSEG (sa_oa.repeats <= STRUCTURED_MAX_REPEATS, field, "Error in sam_seg_SA_or_OA_field - exceeded maximum repeats allowed (%lu) while parsing %s",
                STRUCTURED_MAX_REPEATS, err_dict_id (subfield_dict_id));

        DO_SSF (rname,  ','); // these also do sanity checks
        DO_SSF (pos,    ','); 
        DO_SSF (strand, ','); 
        DO_SSF (cigar,  ','); 
        DO_SSF (mapq,   ','); 
        DO_SSF (nm,     ';'); 

        // sanity checks before adding to any dictionary
        if (strand_len != 1 || (strand[0] != '+' && strand[0] != '-')) goto error; // invalid format
        
        int64_t pos_value = seg_scan_pos_snip ((VBlockP)vb, pos, pos_len, true);
        if (pos_value < 0) goto error;

        seg_by_dict_id (vb, rname,  rname_len,  structured_SA_OA.items[0].dict_id, 1 + rname_len);
        seg_by_dict_id (vb, strand, strand_len, structured_SA_OA.items[2].dict_id, 1 + strand_len);
        seg_by_dict_id (vb, cigar,  cigar_len,  structured_SA_OA.items[3].dict_id, 1 + cigar_len);
        seg_by_dict_id (vb, mapq,   mapq_len,   structured_SA_OA.items[4].dict_id, 1 + mapq_len);
        seg_by_dict_id (vb, nm,     nm_len,     structured_SA_OA.items[5].dict_id, 1 + nm_len);
        
        Context *pos_ctx = mtf_get_ctx (vb, structured_SA_OA.items[1].dict_id);
        pos_ctx->flags = CTX_FL_LOCAL_LZMA;
        pos_ctx->ltype = CTX_LT_UINT32;
        seg_add_to_local_uint32 ((VBlockP)vb, pos_ctx, pos_value, 1 + pos_len);
    }

    seg_structured_by_dict_id (vb, subfield_dict_id, &sa_oa, 1 /* \t */);
    return;

error:
    // if the error occurred on on the first repeat - this file probably has a different
    // format - we just store as a normal subfield
    // if it occurred on the 2nd+ subfield, after the 1st one was fine - we reject the file
    ASSSEG (!sa_oa.repeats, field, "Invalid format in repeat #%u of field %s. snip: %.*s",
            sa_oa.repeats+1, err_dict_id (subfield_dict_id), field_len, field);

    seg_by_dict_id (vb, field, sizeof(field_len), subfield_dict_id, field_len + 1 /* \t */); 
}

static void sam_seg_XA_field (VBlockSAM *vb, const char *field, unsigned field_len)
{
    // XA format is: (chr,pos,CIGAR,NM;)*  pos starts with +- which is strand
    // Example XA:Z:chr9,-60942781,150M,0;chr9,-42212061,150M,0;chr9,-61218415,150M,0;chr9,+66963977,150M,1;
    // See: http://bio-bwa.sourceforge.net/bwa.shtml
    static const Structured structured_XA = {
        .repeats     = 0, 
        .num_items   = 5, 
        .flags       = 0,
        .repsep      = {0,0},
        .items       = { { .dict_id = {.id="@RNAME"  }, .seperator = {','}, .did_i = DID_I_NONE },
                         { .dict_id = {.id="@STRAND" }, .seperator = { 0 }, .did_i = DID_I_NONE },
                         { .dict_id = {.id="@POS"    }, .seperator = {','}, .did_i = DID_I_NONE },
                         { .dict_id = {.id={'C'&0x3f,'I','G','A','R'}}, .seperator = {','}, .did_i = DID_I_NONE},
                         { .dict_id = {.id="NM:i"    }, .seperator = {';'}, .did_i = DID_I_NONE } }     
    };

    Structured xa = structured_XA;

    DEC_SSF(rname); DEC_SSF(pos); DEC_SSF(cigar); DEC_SSF(nm); 

    for (uint32_t i=0; i < field_len; xa.repeats++) {

        ASSSEG (xa.repeats <= STRUCTURED_MAX_REPEATS, field, "Error in sam_seg_XA_field - exceeded maximum repeats allowed (%lu) while parsing XA",
                STRUCTURED_MAX_REPEATS);

        DO_SSF (rname,  ','); 
        DO_SSF (pos,    ','); 
        DO_SSF (cigar,  ','); 
        DO_SSF (nm,     ';'); 

        // sanity checks before adding to any dictionary
        if (pos_len < 2 || (pos[0] != '+' && pos[0] != '-')) goto error; // invalid format - expecting pos to begin with the strand

        int64_t pos_value = seg_scan_pos_snip ((VBlockP)vb, &pos[1], pos_len-1, true);
        if (pos_value < 0) goto error;

        seg_by_dict_id (vb, rname,  rname_len, structured_XA.items[0].dict_id, 1 + rname_len);
        seg_by_dict_id (vb, pos,    1,         structured_XA.items[1].dict_id, 1); // strand is first character of pos
        seg_by_dict_id (vb, cigar,  cigar_len, structured_XA.items[3].dict_id, 1 + cigar_len);
        seg_by_dict_id (vb, nm,     nm_len,    structured_XA.items[4].dict_id, 1 + nm_len);
        
        Context *pos_ctx = mtf_get_ctx (vb, structured_XA.items[2].dict_id);
        pos_ctx->ltype = CTX_LT_UINT32;
        pos_ctx->flags = CTX_FL_LOCAL_LZMA;

        seg_add_to_local_uint32 ((VBlockP)vb, pos_ctx, pos_value, pos_len); // +1 for seperator, -1 for strand
    }

    seg_structured_by_dict_id (vb, (DictId)dict_id_OPTION_XA, &xa, 1 /* \t */);
    return;

error:
    // if the error occurred on on the first repeat - this file probably has a different
    // format - we just store as a normal subfield
    // if it occurred on the 2nd+ subfield, after the 1st one was fine - we reject the file
    ASSSEG (!xa.repeats, field, "Invalid format in repeat #%u of field XA. snip: %.*s", xa.repeats+1, field_len, field);

    seg_by_dict_id (vb, field, field_len, (DictId)dict_id_OPTION_XA, field_len + 1 /* \t */); 
}

uint32_t sam_seg_get_seq_len_by_MD_field (const char *md_str, unsigned md_str_len)
{
    uint32_t result=0, curr_num=0;

    for (unsigned i=0; i < md_str_len; i++) {   
        if (IS_DIGIT (md_str[i])) 
            curr_num = curr_num * 10 + (md_str[i] - '0');

        else {
            result += curr_num + 1; // number terminates here + one character
            curr_num = 0;
        }
    }

    result += curr_num; // in case the string ends with a number

    return result;
}

// in the case where sequence length as calculated from the MD is the same as that calculated
// from the CIGAR/SEQ/QUAL (note: this is required by the SAM spec but nevertheless genozip doesn't require it):
// MD is shortened to replace the last number with a *, since it can be calculated knowing the length. The result is that
// multiple MD values collapse to one, e.g. "MD:Z:119C30" and "MD:Z:119C31" both become "MD:Z:119C*" hence improving compression.
// In the case where the MD is simply a number "151" and drop it altogether and keep just an empty string.
static inline bool sam_seg_get_shortened_MD (const char *md_str, unsigned md_str_len, uint32_t seq_len,
                                             char *new_md_str, unsigned *new_md_str_len)
{
    uint32_t seq_len_by_md = sam_seg_get_seq_len_by_MD_field (md_str, md_str_len);

    if (seq_len_by_md != seq_len) return false;  // MD string doesn't comply with SAM spec and is therefore not changed
    
    // case - MD ends with a number eg "119C31" - we replace it with prefix+"119C". if its all digits then just prefix
    if (IS_DIGIT (md_str[md_str_len-1])) {

        int i=md_str_len-1; for (; i>=0; i--)
            if (!IS_DIGIT (md_str[i])) break;

        new_md_str[0] = SNIP_SPECIAL;
        new_md_str[1] = SAM_SPECIAL_MD;
        if (i >= 0) memcpy (&new_md_str[2], md_str, i+1);
        
        *new_md_str_len = i+3;
        return true;
    }

    return false; // MD doesn't end with a number and is hence unchanged (this normally doesn't occur as the MD would finish with 0)
}

// AS and XS are values (at least as set by BWA) at most the seq_len, and AS is often equal to it. we modify
// it to be new_value=(value-seq_len) 
static inline void sam_seg_AS_field (VBlockSAM *vb, ZipDataLineSAM *dl, DictId dict_id, 
                                     const char *snip, unsigned snip_len)
{
    bool positive_delta = true;

    // verify that its a unsigned number
    for (unsigned i=0; i < snip_len; i++)
        if (!IS_DIGIT (snip[i])) positive_delta = false;

    int32_t as;
    if (positive_delta) {
        as = atoi (snip); // type i is signed 32 bit by SAM specification
        if (dl->seq_len < as) positive_delta=false;
    }

    // if possible, store a special snip with the positive delta
    if (positive_delta) {
        char new_snip[20];    
        new_snip[0] = SNIP_SPECIAL;
        new_snip[1] = SAM_SPECIAL_AS; 
        unsigned delta_len = str_int (dl->seq_len-as, &new_snip[2]);

        seg_by_dict_id (vb, new_snip, delta_len+2, dict_id, snip_len + 1); 
    }

    // not possible - just store unmodified
    else
        seg_by_dict_id (vb, snip, snip_len, dict_id, snip_len + 1); // +1 for \t
}

// optimization for Ion Torrent flow signal (ZM) - negative values become zero, positives are rounded to the nearest 10
static void sam_optimize_ZM (const char **snip, unsigned *snip_len, char *new_str)
{
    char *after;
    int number = strtoul (*snip, &after, 10);

    if ((unsigned)(after - *snip) > 0) {
        if (number >= 0) number = ((number + 5) / 10) * 10;
        else             number = 0;

        *snip_len = str_int (number, new_str);
        *snip = new_str;
    }    
}

// process an optional subfield, that looks something like MX:Z:abcdefg. We use "MX" for the field name, and
// the data is abcdefg. The full name "MX:Z:" is stored as part of the OPTIONAL dictionary entry
static DictId sam_seg_optional_field (VBlockSAM *vb, ZipDataLineSAM *dl, const char *field, unsigned field_len)
{
    ASSSEG0 (field_len, field, "Error: line invalidly ends with a tab");

    ASSSEG (field_len >= 6 && field[2] == ':' && field[4] == ':', field, "Error: invalid optional field format: %.*s",
            field_len, field);

    DictId dict_id = sam_dict_id_optnl_sf (dict_id_make (field, 4));
    const char *value = &field[5]; // the "abcdefg" part of "MX:Z:abcdefg"
    unsigned value_len = field_len - 5;

    if (dict_id.num == dict_id_OPTION_SA || dict_id.num == dict_id_OPTION_OA)
        sam_seg_SA_or_OA_field (vb, dict_id, value, value_len, dict_id.num == dict_id_OPTION_SA ? "SA" : "OA");

    else if (dict_id.num == dict_id_OPTION_XA) 
        sam_seg_XA_field (vb, value, value_len);

    // fields containing CIGAR format data
    else if (dict_id.num == dict_id_OPTION_MC || dict_id.num == dict_id_OPTION_OC) 
        seg_by_did_i (vb, value, value_len, SAM_CIGAR, value_len+1)

    // MD's logical length is normally the same as seq_len, we use this to optimize it.
    // In the common case that it is just a number equal the seq_len, we replace it with an empty string.
    else if (dict_id.num == dict_id_OPTION_MD) {
        // if MD value can be derived from the seq_len, we don't need to store - store just an empty string

#define MAX_SAM_MD_LEN 1000 // maximum length of MD that is shortened.
        char new_md[MAX_SAM_MD_LEN];
        unsigned new_md_len = 0;
        bool md_is_special  = (value_len-2 <= MAX_SAM_MD_LEN);

        if (md_is_special) 
            md_is_special = sam_seg_get_shortened_MD (value, value_len, dl->seq_len, new_md, &new_md_len);

        // not sure which of these two is better....
        seg_by_dict_id (vb,                                 
                        md_is_special ? new_md : value, 
                        md_is_special ? new_md_len : value_len,
                        dict_id, value_len+1);
    }

    // BD and BI set by older versions of GATK's BQSR is expected to be seq_len (seen empircally, documentation is lacking)
    else if (dict_id.num == dict_id_OPTION_BD) { 
        ASSSEG (value_len == dl->seq_len, field,
                "Error in %s: Expecting BD data to be of length %u as indicated by CIGAR, but it is %u. BD=%.*s",
                txt_name, dl->seq_len, value_len, value_len, value);

        dl->bd_data_start = value - vb->txt_data.data;
        dl->bd_data_len   = value_len;

        Context *ctx = mtf_get_ctx (vb, dict_id);
        ctx->local.len += value_len;
        ctx->txt_len   += value_len + 1; // +1 for \t
        ctx->ltype      = CTX_LT_SEQUENCE;
        ctx->flags      = CTX_FL_LOCAL_LZMA;
    }

    else if (dict_id.num == dict_id_OPTION_BI) { 
        ASSSEG (value_len == dl->seq_len, field,
                "Error in %s: Expecting BI data to be of length %u as indicated by CIGAR, but it is %u. BI=%.*s",
                txt_name, dl->seq_len, value_len, value_len, value);

        dl->bi_data_start = value - vb->txt_data.data;
        dl->bi_data_len   = value_len;

        Context *ctx = mtf_get_ctx (vb, dict_id);
        ctx->local.len += value_len;
        ctx->txt_len   += value_len + 1; // +1 for \t
        ctx->ltype      = CTX_LT_SEQUENCE;
        ctx->flags      = CTX_FL_LOCAL_LZMA;

        // BI requires a special algorithm to reconstruct from the delta from BD (if one exists)
        if (dl->bd_data_len) {
            const char bi_special_snip[2] = { SNIP_SPECIAL, SAM_SPECIAL_BI };
            seg_by_ctx ((VBlockP)vb, bi_special_snip, 2, ctx, 0, NULL);
        }
        else {
            char bi_lookup_snip = SNIP_LOOKUP;
            seg_by_ctx ((VBlockP)vb, &bi_lookup_snip, 1, ctx, 0, NULL);
        }
    }

    // AS is a value (at least as set by BWA) at most the seq_len, and often equal to it. we modify
    // it to be new_AS=(AS-seq_len) 
    else if (dict_id.num == dict_id_OPTION_AS) 
        sam_seg_AS_field (vb, dl, dict_id, value, value_len);
    
    // mc:i: (output of bamsormadup? - mc in small letters) appears to a pos value usually close to POS.
    // we encode as a delta.
    else if (dict_id.num == dict_id_OPTION_mc) {
        uint8_t mc_did_i = mtf_get_ctx (vb, dict_id)->did_i;

        seg_pos_field ((VBlockP)vb, mc_did_i, SAM_POS, true, value, value_len, true);
    }

    // E2 - SEQ data (note: E2 doesn't have a dictionary)
    else if (dict_id.num == dict_id_OPTION_E2) { 
        ASSERT (value_len == dl->seq_len, 
                "Error in %s: Expecting E2 data to be of length %u as indicated by CIGAR, but it is %u. E2=%.*s",
                txt_name, dl->seq_len, value_len, value_len, value);

        int64_t this_pos = vb->contexts[SAM_POS].last_value;
        sam_seg_seq_field (vb, (char *)value, value_len, this_pos, vb->last_cigar, 0); // remove const bc SEQ data is actually going to be modified
    }

    // U2 - QUAL data (note: U2 doesn't have a dictionary)
    else if (dict_id.num == dict_id_OPTION_U2) {
        ASSERT (value_len == dl->seq_len, 
                "Error in %s: Expecting U2 data to be of length %u as indicated by CIGAR, but it is %u. E2=%.*s",
                txt_name, dl->seq_len, value_len, value_len, value);

        dl->u2_data_start = value - vb->txt_data.data;
        dl->u2_data_len   = value_len;
        vb->contexts[SAM_QUAL].txt_len   += value_len + 1; // +1 for \t
        vb->contexts[SAM_QUAL].local.len += value_len;
    }

    // Numeric array array
    else if (field[3] == 'B') {
        SegOptimize optimize = NULL;

        if (flag_optimize_ZM && dict_id.num == dict_id_OPTION_ZM && value_len > 3 && value[0] == 's')  // XM:B:s,
            optimize = sam_optimize_ZM;

        seg_array_field ((VBlockP)vb, dict_id, value, value_len, optimize);
    }

    // All other subfields - have their own dictionary
    else        
        seg_by_dict_id (vb, value, value_len, dict_id, (value_len) + 1); // +1 for \t

    return dict_id;
}

static void sam_seg_cigar_field (VBlockSAM *vb, ZipDataLineSAM *dl, unsigned last_cigar_len,
                                 const char *seq,  uint32_t seq_data_len, 
                                 const char *qual, uint32_t qual_data_len)
{
    bool qual_is_available = qual_data_len != 1 || *qual != '*';
    bool seq_is_available  = seq_data_len != 1  || *seq  != '*';

    ASSSEG (!(seq_is_available && *seq=='*'), seq, "seq=%.*s (seq_len=%u), but expecting a missing seq to be \"*\" only (1 character)", seq_data_len, seq, seq_data_len);

    char cigar_snip[100] = { SNIP_SPECIAL, SAM_SPECIAL_CIGAR };
    unsigned cigar_snip_len=2;

    // case: SEQ is "*" - we add a '-' to the CIGAR
    if (!seq_is_available) cigar_snip[cigar_snip_len++] = '-';

    // case: CIGAR is "*" - we get the dl->seq_len directly from SEQ or QUAL, and add the length to CIGAR eg "151*"
    if (!dl->seq_len) { // CIGAR is not available
        ASSSEG (!seq_data_len || !qual_is_available || seq_data_len==dl->qual_data_len, seq,
                "Bad line: SEQ length is %u, QUAL length is %u, unexpectedly differ. SEQ=%.*s QUAL=%.*s", 
                seq_data_len, dl->qual_data_len, seq_data_len, seq, dl->qual_data_len, qual);    

        dl->seq_len = MAX (seq_data_len, dl->qual_data_len); // one or both might be not available and hence =1

        cigar_snip_len += str_int (dl->seq_len, &cigar_snip[cigar_snip_len]);
    } 
    else { // CIGAR is available - just check the seq and qual lengths
        ASSSEG (!seq_is_available || seq_data_len == dl->seq_len, seq,
                "Bad line: according to CIGAR, expecting SEQ length to be %u but it is %u. SEQ=%.*s", 
                dl->seq_len, seq_data_len, seq_data_len, seq);

        ASSSEG (!qual_is_available || qual_data_len == dl->seq_len, qual,
                "Bad line: according to CIGAR, expecting QUAL length to be %u but it is %u. QUAL=%.*s", 
                dl->seq_len, dl->qual_data_len, dl->qual_data_len, qual);    
    }

    memcpy (&cigar_snip[cigar_snip_len], vb->last_cigar, last_cigar_len);

    seg_by_did_i (vb, cigar_snip, cigar_snip_len + last_cigar_len, SAM_CIGAR, last_cigar_len+1); // +1 for \t
}

static void sam_seg_qual_field (VBlockSAM *vb, ZipDataLineSAM *dl, const char *qual, uint32_t qual_data_len)
{
    dl->qual_data_start = qual - vb->txt_data.data;
    dl->qual_data_len   = qual_data_len;

    Context *qual_ctx = &vb->contexts[SAM_QUAL];
    qual_ctx->local.len += dl->qual_data_len;
    qual_ctx->txt_len   += dl->qual_data_len + 1;
}

const char *sam_seg_txt_line (VBlock *vb_, const char *field_start_line, bool *has_13)     // index in vb->txt_data where this line starts
{
    VBlockSAM *vb = (VBlockSAM *)vb_;
    ZipDataLineSAM *dl = DATA_LINE (vb->line_i);

    const char *next_field=field_start_line, *field_start;
    unsigned field_len=0;
    char separator;

    int32_t len = AFTERENT (char, vb->txt_data) - field_start_line;

    // QNAME - We break down the QNAME into subfields separated by / and/or : - these are vendor-defined strings. Examples:
    // Illumina: <instrument>:<run number>:<flowcell ID>:<lane>:<tile>:<x-pos>:<y-pos> for example "A00488:61:HMLGNDSXX:4:1101:15374:1031" see here: https://help.basespace.illumina.com/articles/descriptive/fastq-files/
    // PacBio BAM: {movieName}/{holeNumber}/{qStart}_{qEnd} see here: https://pacbiofileformats.readthedocs.io/en/3.0/BAM.html
    GET_NEXT_ITEM ("QNAME");
    seg_compound_field ((VBlockP)vb, &vb->contexts[SAM_QNAME], field_start, field_len, &vb->qname_mapper, structured_QNAME, false, 1 /* \n */);

    SEG_NEXT_ITEM (SAM_FLAG);

    GET_NEXT_ITEM ("RNAME");
    seg_chrom_field (vb_, field_start, field_len);
    bool rname_is_missing = (*field_start == '*' && field_len == 1);

    GET_NEXT_ITEM ("POS");
    int64_t this_pos = seg_pos_field (vb_, SAM_POS, SAM_POS, false, field_start, field_len, true);
    ASSSEG (!(rname_is_missing && this_pos), field_start, "Error: RNAME=\"*\" - expecting POS to be 0 but it is %"PRId64, this_pos);

    random_access_update_pos (vb_, SAM_POS);

    SEG_NEXT_ITEM (SAM_MAPQ);

    // CIGAR - we wait to get more info from SEQ and QUAL
    GET_NEXT_ITEM ("CIGAR");
    sam_analyze_cigar (field_start, field_len, &dl->seq_len, &vb->ref_consumed, &vb->ref_and_seq_consumed);
    vb->last_cigar = field_start;
    unsigned last_cigar_len = field_len;
    ((char *)vb->last_cigar)[field_len] = 0; // null-terminate CIGAR string

    GET_NEXT_ITEM ("RNEXT");
    seg_by_did_i (vb, field_start, field_len, SAM_RNAME, field_len+1); // add to RNAME dictionary
    
    GET_NEXT_ITEM ("PNEXT");
    seg_pos_field (vb_, SAM_PNEXT, SAM_POS, false, field_start, field_len, true);

    GET_NEXT_ITEM ("TLEN");
    sam_seg_tlen_field (vb, field_start, field_len, vb->contexts[SAM_PNEXT].last_delta, dl->seq_len);

    // SEQ 
    GET_NEXT_ITEM ("SEQ");

    // calculate diff vs. reference (self or imported) - only if aligned (pos!=0) and CIGAR and SEQ values are available
    sam_seg_seq_field (vb, field_start, field_len, this_pos, vb->last_cigar, 0);
    const char *seq = field_start;
    uint32_t seq_data_len = field_len;

    GET_MAYBE_LAST_ITEM ("QUAL");
    sam_seg_qual_field (vb, dl, field_start, field_len); 

    // finally we can seg CIGAR now
    sam_seg_cigar_field (vb, dl, last_cigar_len, seq, seq_data_len, field_start, field_len);
    
    // OPTIONAL fields - up to MAX_SUBFIELDS of them
    Structured st = { .repeats=1, .num_items=0, .flags=0 };
    char prefixes[MAX_SUBFIELDS * 6 + 2]; // each name is 5 characters per SAM specification, eg "MC:Z:" followed by SNIP_STRUCTURED ; +2 for the initial SNIP_STRUCTURED
    prefixes[0] = prefixes[1] = SNIP_STRUCTURED; // initial SNIP_STRUCTURED follow by seperator of empty Structured-wide prefix
    unsigned prefixes_len=2;

    while (separator != '\n') {
        GET_MAYBE_LAST_ITEM ("OPTIONAL-subfield");

        StructuredItem *si = &st.items[st.num_items];
        si->dict_id      = sam_seg_optional_field (vb, dl, field_start, field_len);
        si->seperator[0] = '\t';
        si->seperator[1] = 0;
        si->did_i        = DID_I_NONE; // seg always puts NONE, PIZ changes it
        st.num_items++;

        ASSSEG (st.num_items <= MAX_SUBFIELDS, field_start, "Error: too many optional fields, limit is %u", MAX_SUBFIELDS);

        memcpy (&prefixes[prefixes_len], field_start, 5);
        prefixes[prefixes_len+5] = SNIP_STRUCTURED;
        prefixes_len += 6;
    }

    if (st.num_items)
        seg_structured_by_ctx (vb_, &vb->contexts[SAM_OPTIONAL], &st, prefixes, prefixes_len, 5 * st.num_items); // account for prefixes eg MX:i:
    else
        seg_by_did_i (vb, "", 0, SAM_OPTIONAL, 0); // empty regular snip in case this line has no OPTIONAL

    SEG_EOL (SAM_EOL, false); /* last field accounted for \n */

    return next_field;
}
