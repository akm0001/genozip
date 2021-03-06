// ------------------------------------------------------------------
//   vcf.h
//   Copyright (C) 2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef VCF_INCLUDED
#define VCF_INCLUDED

#include "genozip.h"
#include "digest.h"
#include "sections.h"

// default max number of samples in each sample block within a variant block. user configurable with --sblock
#define VCF_SAMPLES_PER_VBLOCK "4096" 

#define VCF_MAX_PLOIDY 100  // set to a reasonable 100 to avoid memory allocation explosion in case of an error in the VCF file
#if VCF_MAX_PLOIDY > 65535
#error "VCF_MAX_PLOIDY cannot go beyond 65535 VBlockVCF.ploidy are uint16_t"
#endif

// SEG stuff
extern const char *vcf_seg_txt_line (VBlockP vb_, const char *field_start_line, uint32_t remaining_txt_len, bool *has_special_eol);
extern void vcf_seg_initialize (VBlockP vb_);
extern void vcf_seg_finalize (VBlockP vb_);

// PIZ stuff
extern bool vcf_piz_is_skip_section (VBlockP vb, SectionType st, DictId dict_id);
CONTAINER_FILTER_FUNC (vcf_piz_filter);
CONTAINER_CALLBACK (vcf_piz_container_cb);

// VCF Header stuff
extern void vcf_header_initialize (void);
extern bool vcf_inspect_txt_header (BufferP txt_header);
extern bool vcf_header_set_globals (const char *filename, BufferP vcf_header, bool soft_fail);
extern void vcf_header_trim_header_line (BufferP vcf_header_buf);
extern void vcf_header_keep_only_last_line (BufferP vcf_header_buf);
extern uint32_t vcf_header_get_num_samples (void);

// VBlock stuff
extern void vcf_vb_release_vb();
extern void vcf_vb_destroy_vb();
extern void vcf_vb_cleanup_memory();
extern unsigned vcf_vb_size (void);
extern unsigned vcf_vb_zip_dl_size (void);
extern bool vcf_vb_has_haplotype_data (VBlockP vb);

// Samples stuff
extern void vcf_samples_add  (const char *samples_str);

#define VCF_SPECIAL { vcf_piz_special_REFALT, vcf_piz_special_FORMAT, vcf_piz_special_AC, vcf_piz_special_SVLEN, \
                      vcf_piz_special_DS, vcf_piz_special_BaseCounts, vcf_piz_special_SF }
SPECIAL (VCF, 0, REFALT,     vcf_piz_special_REFALT);
SPECIAL (VCF, 1, FORMAT,     vcf_piz_special_FORMAT)
SPECIAL (VCF, 2, AC,         vcf_piz_special_AC);
SPECIAL (VCF, 3, SVLEN,      vcf_piz_special_SVLEN);
SPECIAL (VCF, 4, DS,         vcf_piz_special_DS);
SPECIAL (VCF, 5, BaseCounts, vcf_piz_special_BaseCounts);
SPECIAL (VCF, 6, SF,         vcf_piz_special_SF);
#define NUM_VCF_SPECIAL 7

#define VCF_DICT_ID_ALIASES \
    /*         alias                           maps to this ctx          */  \
    { DT_VCF,  &dict_id_INFO_END,              &dict_id_fields[VCF_POS]    }, \

#define VCF_LOCAL_GET_LINE_CALLBACKS

#define dict_id_is_vcf_info_sf   dict_id_is_type_1
#define dict_id_is_vcf_format_sf dict_id_is_type_2

#define DTYPE_VCF_INFO   DTYPE_1
#define DTYPE_VCF_FORMAT DTYPE_2

#endif
