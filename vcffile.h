// ------------------------------------------------------------------
//   vcffile.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef VCFFILE_INCLUDED
#define VCFFILE_INCLUDED

#include "genozip.h"
#include "buffer.h"

extern bool vcffile_get_line (VariantBlockP vb, unsigned line_i_in_file, bool skip_md5_vcf_header, Buffer *line, const char *buf_name);
extern void vcffile_write_one_variant_block (FileP vcf_file, VariantBlockP vb);
extern unsigned vcffile_write_to_disk(FileP vcf_file, const Buffer *buf);
extern void vcffile_compare_pipe_to_file (FILE *from_pipe, FileP vcf_file);

#endif