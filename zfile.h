// ------------------------------------------------------------------
//   zfile.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef ZFILE_INCLUDED
#define ZFILE_INCLUDED

#include "genozip.h"
#include "sections.h"
#include "dict_id.h"
#include "md5.h"

extern int16_t zfile_read_genozip_header (VariantBlockP vb, Md5Hash *digest);
extern SectionHeaderGenozipHeader *zfile_compress_genozip_header (VariantBlockP vb, uint16_t data_type, const Md5Hash *single_component_md5);
extern bool zfile_get_genozip_header (FileP z_file, SectionHeaderGenozipHeader *header);

extern void zfile_write_vcf_header (VariantBlockP vb, BufferP vcf_header_text, bool is_first_vcf);

extern void zfile_compress_section_data (VariantBlockP vb, SectionType section_type, BufferP section_data);
extern void zfile_compress_vb_header (VariantBlockP vb);
extern void zfile_update_compressed_vb_header (VariantBlockP vb, uint32_t pos, uint32_t num_info_subfields);

extern void zfile_read_all_dictionaries (VariantBlockP pseudo_vb, uint32_t last_vb_i /* 0 means all VBs */);
extern void zfile_compress_dictionary_data (VariantBlockP vb, MtfContextP ctx, 
                                            uint32_t num_words, const char *data, uint32_t num_chars);
extern void zfile_compress_b250_data (VariantBlockP vb, MtfContextP ctx);

extern bool zfile_read_one_vb (VariantBlockP vb);

// returns offset of header within data, EOF if end of file (or end of VCF component in the case of flag_split)
#define MAYBE_V1 (-2) // zfile_read_one_section returns this if the first section cannot be read
extern int zfile_read_one_section (VariantBlockP vb, 
                                   BufferP data /* buffer to append */, const char *buf_name,
                                   unsigned header_size, SectionType expected_sec_type);

extern void zfile_uncompress_section (VariantBlockP vb, void *section_header, 
                                      BufferP uncompressed_data, const char *uncompressed_data_buf_name,
                                      SectionType expected_section_type);

extern void zfile_compress_terminator_section (VariantBlockP vb);

#ifdef __APPLE__
#define off64_t __int64_t // needed for conda mac - otherwise zlib.h throws compilation errors
#endif
extern bool zfile_update_vcf_header_section_header (VariantBlockP vb, off64_t pos_of_current_vcf_header, Md5Hash *md5);

// v1 compatability
extern bool v1_zfile_read_one_vb (VariantBlockP vb);
extern int v1_zfile_read_one_section (VariantBlockP vb, BufferP data, const char *buf_name, unsigned header_size, SectionType expected_sec_type, bool allow_eof);


#endif
