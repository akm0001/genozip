// ------------------------------------------------------------------
//   fastq.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef FASTQ_INCLUDED
#define FASTQ_INCLUDED

#include "genozip.h"
#include "sections.h"

// Txtfile stuff
extern int32_t fastq_unconsumed (VBlockP vb, uint32_t first_i, int32_t *i);
extern bool fastq_txtfile_have_enough_lines (VBlockP vb, uint32_t *unconsumed_len);

// ZIP Stuff
extern void fastq_zip_initialize (void);
extern void fastq_zip_read_one_vb (VBlockP vb);
extern bool fastq_zip_dts_flag (void);
COMPRESSOR_CALLBACK(fastq_zip_qual)

// SEG Stuff
extern void fastq_seg_initialize();
extern void fastq_seg_finalize();
extern const char *fastq_seg_txt_line();

// PIZ Stuff
extern void fastq_piz_initialize (void);
extern bool fastq_piz_is_paired (void);
extern bool fastq_piz_read_one_vb (VBlockP vb, ConstSectionListEntryP sl);
CONTAINER_FILTER_FUNC (fastq_piz_filter);
extern bool fastq_piz_is_skip_section (VBlockP vb, SectionType st, DictId dict_id);
extern void fastq_reconstruct_seq (VBlockP vb, ContextP bitmap_ctx, const char *seq_len_str, unsigned seq_len_str_len);
extern void fastq_txtfile_write_one_vblock_interleave (VBlockP vb1, VBlockP vb2);

// VBlock stuff
extern void fastq_vb_release_vb();
extern void fastq_vb_destroy_vb();
extern unsigned fastq_vb_size (void);
extern unsigned fastq_vb_zip_dl_size (void);

// file pairing (--pair) stuff
extern bool fastq_read_pair_1_data (VBlockP vb, uint32_t first_vb_i_of_pair_1, uint32_t last_vb_i_of_pair_1);
extern uint32_t fastq_get_pair_vb_i (VBlockP vb);

// --genobwa stuff
extern WordIndex fastq_get_genobwa_chrom (void);

#define FASTQ_DICT_ID_ALIASES 

#define FASTQ_LOCAL_GET_LINE_CALLBACKS  \
    { DT_FASTQ, &dict_id_fields[FASTQ_QUAL], fastq_zip_qual },

#endif
