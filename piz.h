// ------------------------------------------------------------------
//   piz.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef PIZ_INCLUDED
#define PIZ_INCLUDED

#include "genozip.h"

// returns true is successfully outputted a vcf file
extern bool piz_dispatcher (const char *z_basename, FileP z_file, FileP vcf_file, 
                            bool test_mode, unsigned max_threads);

extern void piz_reconstruct_line_components (VariantBlockP vb);
extern void piz_merge_all_lines (VariantBlockP vb);

#endif