// ------------------------------------------------------------------
//   domqual.c
//   Copyright (C) 2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

// compression algorithm for QUAL value that has a dominant value ("dom") (roughly over 50%) - as typically does binned Illumina
// data with a dominant 'F'. We use two contexts' local buffer:
// QUAL - will contain an array of all values EXCEPT for doms. It is assumed that before any non-dom value,
// including at the beginning of the data, there is a run of doms. If there isn't a run of doms at that point, 
// we insert a NO_DOMS. In addition, if there is a run of doms at the end of the data, there will be a terminating NO_DOMS in qual
// QDOMRUNS - for each dom run we have a value 0-254 that is interpreted as a dom run of 1-255 doms,
// or a value of 255 means a run of 255 and then continue the run with the next dom value, 
// thereby allowing runs of over 255 (e.g. a run "255-255-5" would be a run of 255+255+5=515 doms)

/* feasibility test on Illumina binned data:
-rw-r--r-- 1 USER 197610 14236798 Jul 25 16:49 qual
-rw-r--r-- 1 USER 197610   924542 Jul 26 18:17 qual.bz2 

-rw-r--r-- 1 USER 197610   295298 Jul 26 17:07 qual.chars.bz2
-rw-r--r-- 1 USER 197610   600946 Jul 26 17:07 qdomruns.bz2
                           896244 3% better and 10% faster

-rw-r--r-- 1 USER 197610   274560 Jul 26 17:11 qual.chars.xz
-rw-r--r-- 1 USER 197610   545944 Jul 26 17:11 qdomruns.xz
                           820504 12% better and 3.5X more time
*/

#include "vblock.h"
#include "data_types.h"
#include "piz.h"

#define NO_DOMS '\x1'

//--------------
// ZIP side
//--------------

// Sample a few lines, and check that at least 50% of the Phred scores are a single character. 
// This is typically with Illumina binning and "normal" samples where most scores are F
// but might apply with other technologies too, including in combination with our optimize-QUAL
// Returns the character that appears more than 50% of the sample lines tested, or -1 if there isn't one.
static char domqual_has_dominant_value (VBlock *vb, LocalGetLineCallback get_line)
{
#   define NUM_LINES_IN_SAMPLE 4

    uint32_t char_counter[256] = { 0 };
    uint32_t total_len = 0;
    for (unsigned line_i=0; line_i < MIN (NUM_LINES_IN_SAMPLE, vb->lines.len); line_i++) {   
        char *qual_data, *unused;
        uint32_t qual_data_len, unused_len;
        get_line (vb, line_i, &qual_data, &qual_data_len, &unused, &unused_len);
        total_len += qual_data_len;

        for (unsigned j=0; j < qual_data_len; j++)
            char_counter[(uint8_t)qual_data[j]]++;
    }

    // shortcut for the common case of binned Illumina quality scores
    if (char_counter['F'] > total_len / 2) return 'F'; 

    // get most frequent value
#   define DOMQUAL_MINIMUM_THREADSHOLD 5 // not worth it if less than this (and will fail in SAM with 1)
    for (unsigned c=32; c <= 126; c++)  // printable ASCII only
        if (char_counter[c] > total_len / 2 && char_counter[c] > DOMQUAL_MINIMUM_THREADSHOLD) 
            return c; // this will be 'F' in case of binned illumina

    return -1; // no value has more than 50% in the tested 4-line sample
}

static inline void domqual_add_runs (Buffer *qdomruns_buf, uint32_t runlen)
{
    // add one more bytes to represent the run
    while (runlen) {
        uint8_t subrun_len = (uint8_t)MIN (runlen, 254);

        NEXTENT (uint8_t, *qdomruns_buf) = (runlen <= 254 ? subrun_len : 255);
        runlen -= subrun_len;
    }
}

bool domqual_convert_qual_to_domqual (VBlockP vb, LocalGetLineCallback get_line, int qual_field)
{
    const char dom = domqual_has_dominant_value (vb, get_line);
    if (dom == -1) return false; // we can't do domqual - just go ahead and compress the qual data directly

    Context *qual_ctx = &vb->contexts[qual_field];
    Buffer *qual_buf  = &qual_ctx->local;

    Context *qdomruns_ctx = qual_ctx + 1; // the qdomruns context is always one after qual context
    Buffer *qdomruns_buf  = &qdomruns_ctx->local;

    // this is usually enough, but might not be in some edge cases
    // note: qual_buf->len is the total length of all qual lines
    buf_alloc (vb, qual_buf, qual_buf->len / 5, 1, "context->local", dom); // dom goes into param, and eventually into SectionHeaderCtx.local_param
    buf_alloc (vb, qdomruns_buf, qual_buf->len / 10, 1, "context->local", qual_ctx->did_i);

    qual_ctx->local.len = 0; 
    qual_ctx->inst |= CTX_INST_NO_CALLBACK | CTX_INST_LOCAL_PARAM;
    qual_ctx->ltype = LT_DOMQUAL;
    qual_ctx->lcomp = COMP_LZMA;

    qdomruns_ctx->ltype = LT_UINT8;
    qdomruns_ctx->lcomp = COMP_LZMA;

    uint32_t runlen = 0;
    
    for (uint32_t line_i=0; line_i < vb->lines.len; line_i++) {   
        char *qual[2];
        uint32_t qual_len[2];
        get_line (vb, line_i, &qual[0], &qual_len[0], &qual[1], &qual_len[1]);

        // grow if needed
        buf_alloc (vb, qual_buf, qual_buf->len + 2 * (qual_len[0] + qual_len[1]), 1.5, "context->local", dom); // theoretical worst case is 2 characters (added NO_DOMS) per each original character
        buf_alloc (vb, qdomruns_buf, qdomruns_buf->len + qual_len[0] + qual_len[1], 1, "context->local", qdomruns_ctx->did_i);

        for (uint32_t side=0; side < 2; side++) {

            if (!qual[side]) continue;

            for (uint32_t i=0; i < qual_len[side]; i++) {
                if (qual[side][i] == dom) 
                    runlen++;
                
                else {
                    // this non-dom value terminates a run of doms
                    if (runlen) {
                        domqual_add_runs (qdomruns_buf, runlen);
                        runlen = 0;
                    }

                    // this non-dom does not terminate a run of doms - add NO_DOMs to indicate the missing dom run
                    else 
                        NEXTENT (char, *qual_buf) = NO_DOMS;

                    // add the non-dom character
                    NEXTENT (char, *qual_buf) = qual[side][i];
                }
            }
        }
    }

    // case: we have a final dom run. note: we mark the terminating run, for example to avoid a situation
    // where QUAL is empty if qual is just one run. We use NO_DOMS rather than another marker, to avoid introducing
    // another letter into the compressed alphabet
    if (runlen) {
        domqual_add_runs (qdomruns_buf, runlen); // add final dom runs
        NEXTENT (char, *qual_buf) = NO_DOMS;
    }
    return true;
}

//--------------
// PIZ side
//--------------

// shorten a run, including handling multi-bytes run
static inline bool shorten_run (uint8_t *run, uint32_t dec)
{
    // count bytes of this run
    uint32_t bytes=1; 
    for (uint8_t *b=run; *b == 255; b++) bytes++;

    uint32_t new_runlen = (bytes-1) * 254 + run[bytes-1] - dec;

    bool increment_start = false;
    uint32_t new_bytes = MAX (1, (new_runlen + 253) / 254); // roundup (if runlen=0, we still need 1 byte)
    if (new_bytes < bytes) {
        increment_start = true;
        run++;
    }

    // update run
    for (uint32_t i=0; i < new_bytes; i++) {
        run[i] = (new_runlen > 254 ? 255 : new_runlen);
        new_runlen -= 254;
    }

    return increment_start;
}

// reconstructed a run of the dominant character
static inline uint32_t domqual_reconstruct_dom_run (VBlockP vb, Context *qdomruns_ctx, char dom, uint32_t max_len)
{
    ASSERT (qdomruns_ctx->next_local < qdomruns_ctx->local.len, "Error in domqual_reconstruct_dom_run: unexpectedly reached the end of qdomruns_ctx in vb_i=%u", vb->vblock_i);

    uint32_t runlen=0;
    uint8_t this_byte;
    do {
        this_byte = NEXTLOCAL (uint8_t, qdomruns_ctx); // might be 0 in beginning of line if previous line consumed entire run
        runlen += (this_byte < 255 ? this_byte : 254); // 0-254 is the length, 255 means length 254 continued in next byte

        // case: a run spans multiple lines - take only what we need, and leave the rest for the next line
        // note: if we use max_len exactly, then we still leave a run of 0 length, so next line can start with a "run" as usual
        if (runlen >= max_len) { 
            bool increment_start = shorten_run (ENT (uint8_t, qdomruns_ctx->local, qdomruns_ctx->next_local-1), max_len);
            if (!increment_start)
                qdomruns_ctx->next_local--; // unconsume this run as we will consume it again in the next line (but shorter)
            
            runlen = max_len;
            break;
        }
    } while (this_byte == 255);

    memset (AFTERENT (char, vb->txt_data), dom, runlen);
    vb->txt_data.len += runlen;

    return runlen;
}

void domqual_reconstruct (VBlockP vb, ContextP qual_ctx)
{
    Context *qdomruns_ctx = qual_ctx + 1; // the qdomruns context is always one after qual context
    char dom = (char)qual_ctx->local.param; // passed from SectionHeaderCtx.local_param

    uint32_t qual_len=0;
    while (qual_len < vb->seq_len) {

        char c = NEXTLOCAL (char, qual_ctx);
        if (c != NO_DOMS) {
            qual_len += domqual_reconstruct_dom_run (vb, qdomruns_ctx, dom, vb->seq_len - qual_len);

            // case: we're at an end of a line that ended with a run
            if (qual_len == vb->seq_len) {
                qual_ctx->next_local--; // unconsume c
                break;
            }
        }

        else if ((uint32_t)qual_ctx->local.len == qual_ctx->next_local) { // this is an final-run indicator
            qual_len += domqual_reconstruct_dom_run (vb, qdomruns_ctx, dom, vb->seq_len - qual_len);
            qual_ctx->next_local--; // leave it unconsumed as it might be needed by the next lines
            break;
        }
        else
            c = NEXTLOCAL (char, qual_ctx);

        RECONSTRUCT1 (c==' ' ? '*' : c); // in SAM, we re-wrote a '*' marking 'unavailable' as ' ' to avoid confusing with '*' as a valid quality store during optimizaition
        qual_len++;
    }

    ASSERT (qual_len == vb->seq_len, "Error in domqual_reconstruct: expecting qual_len(%u) == vb->seq_len(%u) in vb_i=%u line_i=%u", 
            qual_len, vb->seq_len, vb->vblock_i, vb->line_i);   
}