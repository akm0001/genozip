// ------------------------------------------------------------------
//   me23.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt


#ifndef ME23_INCLUDED
#define ME23_INCLUDED

#include "genozip.h"

extern const char *me23_seg_txt_line (VBlockP vb_, const char *field_start_line, uint32_t remaining_txt_len, bool *has_special_eol);
extern void me23_seg_initialize (VBlockP vb);
extern void me23_seg_finalize (VBlockP vb);
extern bool me23_header_inspect (BufferP txt_header);

#define ME23_DICT_ID_ALIASES

// translator numbers must start from 1 - 0 is reserved for "none"
TRANSLATOR (ME23, VCF, 1, GENOTYPE, sam_piz_m232vcf_GENOTYPE)   // reconstruct VCF GENOTYPE field as VCF - REF,ALT,QUAL,FILTER,INFO,FORMAT,Sample
#define NUM_ME23_TRANS 2 // including "none"
#define ME23_TRANSLATORS { NULL /* none */, sam_piz_m232vcf_GENOTYPE }

TXTHEADER_TRANSLATOR (txtheader_me232vcf);

#endif
