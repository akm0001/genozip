// ------------------------------------------------------------------
//   codec.c
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "codec.h"
#include "vblock.h"
#include "strings.h"
#include "dict_id.h"
#include "file.h"
#include "zfile.h"
#include "profiler.h"
#include "bgzf.h"

// --------------------------------------
// memory functions that serve the codecs
// --------------------------------------

// memory management for bzlib - tesing shows that compress allocates 4 times, and decompress 2 times. Allocations are the same set of sizes
// every call to compress/decompress with the same parameters, independent on the contents or size of the compressed/decompressed data.
void *codec_alloc (VBlock *vb, int size, double grow_at_least_factor)
{
    // get the next buffer - allocations are always in the same order in bzlib and lzma -
    // so subsequent VBs will allocate roughly the same amount of memory for each buffer
    for (unsigned i=0; i < NUM_CODEC_BUFS ; i++) 
        if (!buf_is_allocated (&vb->codec_bufs[i])) {
            buf_alloc (vb, &vb->codec_bufs[i], size, grow_at_least_factor, "codec_bufs");
            //printf ("codec_alloc: %u bytes buf=%u\n", size, i);
            return vb->codec_bufs[i].data;
        }

    ABORT_R ("Error: codec_alloc could not find a free buffer. vb_i=%d", vb->vblock_i);
}

void codec_free (void *vb_, void *addr)
{
    VBlockP vb = (VBlockP)vb_;

    if (!addr) return; // already freed

    for (unsigned i=0; i < NUM_CODEC_BUFS ; i++) 
        if (vb->codec_bufs[i].data == addr) {
            buf_free (&vb->codec_bufs[i]);
            //printf ("codec_free: buf=%u\n", i);
            return;
        }

    ABORT ("Error: codec_free failed to find buffer to free. vb_i=%d addr=%s", 
           vb->vblock_i, str_pointer (addr).s);
}

void codec_free_all (VBlock *vb)
{
    for (unsigned i=0; i < NUM_CODEC_BUFS ; i++) 
        buf_free (&vb->codec_bufs[i]);
}

static bool codec_compress_error (VBlock *vb, SectionHeader *header, const char *uncompressed, uint32_t *uncompressed_len, LocalGetLineCB callback,
                                  char *compressed, uint32_t *compressed_len, bool soft_fail) 
{
    ABORT_R ("Error in comp_compress: Unsupported codec: %s", codec_name (header->codec));
}


static void codec_uncompress_error (VBlock *vb, Codec codec, uint8_t param,
                                    const char *compressed, uint32_t compressed_len,
                                    Buffer *uncompressed_buf, uint64_t uncompressed_len,
                                    Codec sub_codec)
{
    ABORT ("Error in comp_uncompress: Unsupported codec: %s", codec_name (codec));
}

static void codec_reconstruct_error (VBlockP vb, Codec codec, ContextP ctx)
{
    ABORT ("Error in reconstruct_from_ctx_do: in ctx=%s - codec %s has no LT_CODEC reconstruction", 
           dis_dict_id (ctx->dict_id).s, codec_name (codec));
}

static uint32_t codec_est_size_default (Codec codec, uint64_t uncompressed_len)
{
    return (uint32_t)MAX (uncompressed_len / 2, 500);
}

// returns 4-character codec name
const char *codec_name (Codec codec)
{
    switch (codec) {
        case 0 ... NUM_CODECS : return codec_args[codec].name;
        default               : return "BAD!";    
    }
}

void codec_initialize (void)
{
    codec_bsc_initialize();
    bgzf_libdeflate_initialize();
}

// ------------------------------
// Automatic codec selection
// ------------------------------

typedef struct {
    Codec codec;
    double size;
    double clock;
} CodecTest;

static int codec_assign_sorter (const CodecTest *t1, const CodecTest *t2)
{
    // in --fast mode - if one if significantly faster with a modest size hit, take it. Otherwise, take the best.
    if (flag.fast) {
        if (t1->clock < t2->clock * 0.90 && t1->size < t2->size * 1.3) return -1; // t1 has 10% or more better time with at most 30% size hit
        if (t2->clock < t1->clock * 0.90 && t2->size < t1->size * 1.3) return  1; 
    }

    // case: select for significant difference in size (more than 2%)
    if (t1->size  < t2->size  * 0.98) return -1; // t1 has significantly better size
    if (t2->size  < t1->size  * 0.98) return  1; // t2 has significantly better size

    // case: size is similar, select for significant difference in time (more than 50%)
    if (t1->clock < t2->clock * 0.50) return -1; // t1 has significantly better time
    if (t2->clock < t1->clock * 0.50) return  1; // t2 has significantly better time

    // case: size and time are quite similar, check 2nd level 

    // case: select for smaller difference in size (more than 1%)
    if (t1->size  < t2->size  * 0.99) return -1; // t1 has significantly better size
    if (t2->size  < t1->size  * 0.99) return  1; // t2 has significantly better size

    // case: select for smaller difference in time (more than 15%)
    if (t1->clock < t2->clock * 0.85) return -1; // t1 has significantly better time
    if (t2->clock < t1->clock * 0.85) return  1; // t2 has significantly better time

    // time and size are very similar (within %1 and 15% respectively) - select for smaller size
    return t1->size - t2->size;
}

// this function tests each of our generic codecs on a 100KB sample of local or b250 data, and assigns the best one based on 
// compression ratio, or if the ratio is very similar, and the time is quite different, then based on time.
// the codec is then committed to zf_ctx, so that future VBs that clone recieve it and needn't test again.
// This function is called from two places:
// 1. For contexts with generic codecs, left as CODEC_UNKNOWN by the segmenter, we are called from zip_assign_best_codec.
//    For vb=1, this is called while holding the vb=1 lock, so that for all such contexts that appear in vb=1, they are
//    guaranteed to be tested only once. For contexts that make a first appearance in a later VB, parallel VBs might test
//    in parallel. A bit wasteful, but no harm.
// 2. For "specific" codecs (DOMQUAL, HAPMAT, GTSHARK...), subordinate contexts generated during compression of the primary
//    context (compression runs after zip_assign_best_codec is completed already) - those codecs explicitly call us to get the
//    codec for the subordinate context. Multiple of the early VBs may call in parallel, but future VBs will receive
//    the codec during cloning    
Codec codec_assign_best_codec (VBlockP vb, 
                               ContextP ctx, /* for b250, local, dict */ 
                               BufferP data, /* non-context data */
                               SectionType st)
{
    START_TIMER;

    uint64_t save_section_list = vb->section_list_buf.len; // save section list as comp_compress adds to it
    uint64_t save_z_data       = vb->z_data.len;
    bool is_local = (st == SEC_LOCAL);
    bool is_b250  = (st == SEC_B250 );

    CodecTest tests[] = { { CODEC_BZ2 }, { CODEC_NONE }, { CODEC_BSC }, { CODEC_LZMA } };
    const unsigned num_tests = 4;

    Codec non_ctx_codec = CODEC_UNKNOWN; // used for non-b250, non-local sections

    Codec *selected_codec = is_local ? &ctx->lcodec : 
                            is_b250  ? &ctx->bcodec :
                                       &non_ctx_codec;
    // set data
    switch (st) {
        case SEC_DICT  : data = &ctx->dict  ; break;
        case SEC_B250  : data = &ctx->b250  ; break; 
        case SEC_LOCAL : data = &ctx->local ; break;
        default: ASSERTE (data, "expecting non-NULL data for section=%s", st_name (st));
    }

    uint64_t save_data_len = data->len;
    data->len = MIN (data->len * (is_local ? lt_desc[ctx->ltype].width : 1), CODEC_ASSIGN_SAMPLE_SIZE);

    if (data->len < MIN_LEN_FOR_COMPRESSION ||       // if too small - don't assign - compression will use the default BZ2 and the next VB can try to select
        *selected_codec != CODEC_UNKNOWN) goto done; // if already selected - don't assign
     
    // last attempt to avoid double checking of the same context by parallel threads (as we're not locking, 
    // it doesn't prevent double testing 100% of time, but that's good enough) 
    Codec zf_codec = is_local ? z_file->contexts[ctx->did_i].lcodec :  // read without locking (1 byte)
                     is_b250  ? z_file->contexts[ctx->did_i].bcodec :
                                CODEC_UNKNOWN;
    
    if (zf_codec != CODEC_UNKNOWN) {
        *selected_codec = zf_codec;
        goto done;
    }

    // measure the compressed size and duration for a small sample of of the local data, for each codec
    for (unsigned t=0; t < num_tests; t++) {
        *selected_codec = tests[t].codec;

        if (flag.show_time) codec_show_time (vb, "Assign", ctx->name, *selected_codec);

        clock_t start_time = clock();

        if (*selected_codec == CODEC_NONE) tests[t].size = data->len;
        else {
            LocalGetLineCB *callback = zfile_get_local_data_callback (vb->data_type, ctx);

            uint64_t z_data_before = vb->z_data.len;
            zfile_compress_section_data_ex (vb, SEC_NONE, callback ? NULL : data, callback, data->len, *selected_codec, SECTION_FLAGS_NONE);
            tests[t].size = vb->z_data.len - z_data_before;
        }
                                                           
        tests[t].clock = (clock() - start_time);
    }
    
    // sort codec by our selection criteria
    qsort (tests, num_tests, sizeof (CodecTest), (int (*)(const void *, const void*))codec_assign_sorter);

    if (flag.show_codec) {
        iprintf ("vb_i=%-2u %-12s %-5s [%-4s %5d %4.1f] [%-4s %5d %4.1f] [%-4s %5d %4.1f] [%-4s %5d %4.1f]\n", 
                 vb->vblock_i, ctx ? ctx->name : "", &st_name (st)[4],
                 codec_name (tests[0].codec), (int)tests[0].size, tests[0].clock,
                 codec_name (tests[1].codec), (int)tests[1].size, tests[1].clock,
                 codec_name (tests[2].codec), (int)tests[2].size, tests[2].clock,
                 codec_name (tests[3].codec), (int)tests[3].size, tests[3].clock);
        fflush (info_stream);
    }

    // assign the best codec - the first one in the sorted array - and commit it to zf_ctx
    *selected_codec = tests[0].codec;

    if (is_b250 || is_local) ctx_commit_codec_to_zf_ctx (vb, ctx, is_local);

done:
    // roll back
    data->len                = save_data_len;
    vb->z_data.len           = save_z_data;
    vb->section_list_buf.len = save_section_list; 

    COPY_TIMER (codec_assign_best_codec);

    return *selected_codec;
}

// print prefix to the string to be printed by COPY_TIMER in the codec compression functions in case
// of eg. --show-time=compressor_lzma
void codec_show_time (VBlock *vb, const char *name, const char *subname, Codec codec)
{
    if ((strcmp (flag.show_time, "compressor_lzma"  ) && codec==CODEC_LZMA) ||
        (strcmp (flag.show_time, "compressor_bsc"   ) && codec==CODEC_BSC ) || 
        (strcmp (flag.show_time, "compressor_acgt"  ) && codec==CODEC_ACGT) || 
        (strcmp (flag.show_time, "compressor_domq"  ) && codec==CODEC_DOMQ) || 
        (strcmp (flag.show_time, "compressor_hapmat") && codec==CODEC_HAPM) || 
        (strcmp (flag.show_time, "compressor_bz2"   ) && codec==CODEC_BZ2 )) {

        vb->profile.next_name    = name;
        vb->profile.next_subname = subname;
    }
}

// needs to be after all the functions as it refers to them
CodecArgs codec_args[NUM_CODECS] = CODEC_ARGS;

