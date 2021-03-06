#include <errno.h>

#include "libdeflate/libdeflate.h"
#include "zlib/zlib.h"
#include "bgzf.h"
#include "endianness.h"
#include "buffer.h"
#include "vblock.h"
#include "arch.h"
#include "strings.h"
#include "file.h"
#include "zfile.h"
#include "zip.h"
#include "arch.h"
#include "txtfile.h"
#include "codec.h"

#define uncomp_size param // for vb->compressed, we store the uncompressed size in param

// all data in Little Endian
typedef struct __attribute__ ((__packed__)) BgzfHeader {
    uint8_t id1, id2, cm, flg;
    uint32_t mtime;
    uint8_t xfl, os;
    uint16_t xlen;
    uint8_t si1, si2;
    uint16_t slen, bsize;
} BgzfHeader;

typedef struct __attribute__ ((__packed__)) BgzfFooter {
    uint32_t crc32;
    uint32_t isize;
} BgzfFooter;

// possible return values, see libdeflate_result in libdeflate.h
static const char *libdeflate_error (int err)
{
    switch (err) {
        case LIBDEFLATE_SUCCESS            : return "SUCCESS";
        case LIBDEFLATE_BAD_DATA           : return "BAD DATA";
        case LIBDEFLATE_SHORT_OUTPUT       : return "SHORT OUTPUT";
        case LIBDEFLATE_INSUFFICIENT_SPACE : return "INSUFFICIENT SPACE";
        default                            : return "Undefined libdeflate error";
    }
}

typedef struct { char s[100]; } BgzfBlockStr;
static BgzfBlockStr display_bb (BgzfBlockZip *bb)
{
    BgzfBlockStr s;
    sprintf (s.s, "{txt_index=%u txt_size=%u compressed_index=%u comp_size=%u is_decompressed=%u}",
             bb->txt_index, bb->txt_size, bb->compressed_index, bb->comp_size, bb->is_decompressed);
    return s;
}

//--------------------------------------------------------------------
// ZIP SIDE - decompress BGZF-compressed file and prepare BGZF section
//--------------------------------------------------------------------

static inline uint32_t bgzf_fread (File *file, void *dst_buf, uint32_t count)
{
    return fread (dst_buf, 1, count, (FILE *)file->file);
}

// ZIP: reads and validates a BGZF block, and returns the uncompressed size or (only if soft_fail) an error
int32_t bgzf_read_block (File *file, // txt_file is not yet assigned when called from file_open_txt_read
                         uint8_t *block /* must be BGZF_MAX_BLOCK_SIZE in size */, uint32_t *block_size /* out */,
                         bool soft_fail)
{
    BgzfHeader *h = (BgzfHeader *)block;

    *block_size = bgzf_fread (file, h, sizeof (struct BgzfHeader)); // read the header
    if (! *block_size) return 0; // EOF without an EOF block

    if (*block_size < 12) {
        ASSERTE (soft_fail, "file %s appears truncated - it ends with a partial gzip block header", file->basename); // less than the minimal gz block header size
        return BGZF_BLOCK_IS_NOT_GZIP;
    } 

    // case: this is not a GZ / BGZF block at all (see: https://tools.ietf.org/html/rfc1952)
    if (h->id1 != 31 || h->id2 != 139) {
        ASSERTE (soft_fail, "expecting %s to be compressed with gzip format, but it is not", file->basename);
        return BGZF_BLOCK_IS_NOT_GZIP;
    }

#ifdef _WIN32
    // On Windows, we can't pipe binary files, bc Windows converts \n to \r\n
    ASSINP0 (!file->redirected, "genozip on Windows supports piping in only plain (uncompressed) data");
#endif
    // case: this is GZIP block that is NOT a valid BGZF block (see: https://samtools.github.io/hts-specs/SAMv1.pdf)
    if (!(*block_size == 18 && !memcmp (h, BGZF_PREFIX, BGZF_PREFIX_LEN))) {
        ASSERTE (soft_fail, "invalid BGZF block while reading %s", file->basename);
        return BGZF_BLOCK_GZIP_NOT_BGZIP;
    }

    *block_size = LTEN16 (h->bsize) + 1;
    
    uint32_t body_size = *block_size - sizeof (struct BgzfHeader);
    uint32_t bytes = bgzf_fread (file, h+1, body_size);
    ASSERTE (bytes == body_size, "failed to read body of BGZF block in %s - expecting %u bytes but read %u: %s", 
             file->basename, body_size, bytes, strerror (errno));

    uint32_t isize_lt32 = *(uint32_t *)&block[*block_size - 4];
    uint32_t isize = LTEN32 (isize_lt32); // 0...65536 per spec

    // add isize to buffer that will be written to SEC_BGZF
    if (isize) { // don't store EOF block (bc isize=0 cannot be represented as (isize-1) )
        buf_alloc_more (evb, &file->bgzf_isizes, 1, flag.vblock_memory / 63000, uint16_t, 2, "bgzf_isizes");
        NEXTENT (uint16_t, file->bgzf_isizes) = BGEN16 ((uint16_t)(isize - 1)); // -1 to make the range 0..65535
    }
    else 
        txt_file->bgzf_flags.has_eof_block = true;
    
    return isize;
}

// ZIP
void bgzf_compress_bgzf_section (void)
{
    if (!txt_file->bgzf_isizes.len) return; // this txt file is not compressed with BGZF - we don't need a BGZF section

    // we don't write a BGZF block if we have optimized, as the file has changed and we can't reconstruct to the same blocks
    if (flag.optimize) return; 

    // sanity check
    int64_t total_isize = 0;
    ARRAY (uint16_t, isizes, txt_file->bgzf_isizes);
    for (uint64_t i=0; i < txt_file->bgzf_isizes.len; i++) 
        total_isize += BGEN16 (isizes[i]) + 1; // values 0-65535 correspond to isize 1-65536
    ASSERTE (total_isize == txt_file->txt_data_size_single, "Expecting total_isize=%"PRId64" == txt_file->txt_data_size_single=%"PRId64,
             total_isize, txt_file->txt_data_size_single);

    // usually BZ2 is the best, but if the sizes are highly random (happens in long read SAM/BAM), then BZ2 can be bloated
    // and even produce an error. that's why we test.
    txt_file->bgzf_isizes.len *= sizeof (uint16_t);
    Codec codec = codec_assign_best_codec (evb, NULL, &txt_file->bgzf_isizes, SEC_BGZF);

    zfile_compress_section_data_ex (evb, SEC_BGZF, &txt_file->bgzf_isizes, NULL, 0, codec, (SectionFlags)txt_file->bgzf_flags);
    txt_file->bgzf_isizes.len /= sizeof (uint16_t); // restore
}

// de-compresses a BGZF block in vb->compressed referred to by bb, into its place in vb->txt_data as prescribed by bb
void bgzf_uncompress_one_block (VBlock *vb, BgzfBlockZip *bb)
{
    if (bb->is_decompressed) return; // already decompressed - nothing to do

    ASSERTE0 (vb->gzip_compressor, "vb->gzip_compressor=NULL");

    BgzfHeader *h = (BgzfHeader *)ENT (char, vb->compressed, bb->compressed_index);

    // verify that entire block is within vb->compressed
    ASSERTE (bb->compressed_index + sizeof (BgzfHeader) < vb->compressed.len && // we have at least the header - we can access bsize
             bb->compressed_index + (uint32_t)LTEN16 (h->bsize) + 1 <= vb->compressed.len, 
             "bgzf block size goes past the end of in vb->compressed: bb=%s vb=%u compressed_index=%u vb->compressed.len=%"PRIu64, 
             display_bb (bb).s, vb->vblock_i, bb->compressed_index, vb->compressed.len);

    ASSERTE (h->id1==31 && h->id2==139, "not a valid bgzf block in vb->compressed: vb=%u compressed_index=%u", vb->vblock_i, bb->compressed_index);

    if (flag.show_bgzf)
        iprintf ("%-7s vb=%u i=%u compressed_index=%u size=%u txt_index=%u size=%u ",
                 arch_am_i_io_thread() ? "IO" : "COMPUTE", vb->vblock_i, 
                 ENTNUM (vb->bgzf_blocks, bb), bb->compressed_index, bb->comp_size, bb->txt_index, bb->txt_size);

    enum libdeflate_result ret = 
        libdeflate_deflate_decompress (vb->gzip_compressor, 
                                       h+1, bb->comp_size - sizeof(BgzfHeader) - sizeof (BgzfFooter), // compressed
                                       ENT (char, vb->txt_data, bb->txt_index), bb->txt_size, NULL);  // uncompressed

    ASSERTE (ret == LIBDEFLATE_SUCCESS, "libdeflate_deflate_decompress failed: %s", libdeflate_error(ret));

    bb->is_decompressed = true;

    if (flag.show_bgzf)
        #define C(i) ((bb->txt_index + i < vb->txt_data.len) ? char_to_printable (*ENT (char, vb->txt_data, bb->txt_index + (i))).s : "") 
        iprintf ("txt_data[5]=%1s%1s%1s%1s%1s %s\n", C(0), C(1), C(2), C(3), C(4), bb->comp_size == BGZF_EOF_LEN ? "EOF" : "");
        #undef C
}

// ZIP: called from the compute thread: zip_compress_one_vb and I/O thread: txtfile_read_block_bgzf
void bgzf_uncompress_vb (VBlock *vb)
{
    START_TIMER;

    vb->gzip_compressor = libdeflate_alloc_decompressor(vb);

    for (uint32_t block_i=0; block_i < vb->bgzf_blocks.len ; block_i++) {
        BgzfBlockZip *bb = ENT (BgzfBlockZip, vb->bgzf_blocks, block_i);
        bgzf_uncompress_one_block (vb, bb);
    } 

    libdeflate_free_decompressor ((struct libdeflate_decompressor **)&vb->gzip_compressor);

    buf_free (&vb->compressed); // now that we are finished decompressing we can free it

    if (flag.show_time) {
        if (arch_am_i_io_thread ()) COPY_TIMER (bgzf_io_thread)
        else                        COPY_TIMER (bgzf_compute_thread);
    }
}

static void *bgzf_alloc (void *vb_, unsigned items, unsigned size)
{
    return codec_alloc ((VBlock *)vb_, items * size, 1); // all bzlib buffers are constant in size between subsequent compressions
}

void bgzf_libdeflate_initialize (void)
{
    libdeflate_set_memory_allocator (bgzf_alloc, codec_free);
}

// ZIP: tests a BGZF block against libdeflate's level 0-12
// returns the level 0-12 if detected, or BGZF_COMP_LEVEL_UNKNOWN if not
// NOTE: even if we detected the level based on the first block of the file - it's not certain that the file was compressed with this 
// level. This is because multiple levels might result in the same compression for this block, but maybe not for other blocks in the file.
struct FlagsBgzf bgzf_get_compression_level (const char *filename, const uint8_t *comp_block, uint32_t comp_block_size, uint32_t uncomp_block_size)
{
    static const struct { BgzfLibraryType library; int level; } levels[] =  // test in the order of likelihood of observing them
        { {BGZF_LIBDEFLATE,6}, {BGZF_ZLIB,6},  {BGZF_ZLIB,4},  {BGZF_LIBDEFLATE,9},  {BGZF_LIBDEFLATE,8}, {BGZF_LIBDEFLATE,7}, 
          {BGZF_LIBDEFLATE,5}, {BGZF_LIBDEFLATE,4}, {BGZF_LIBDEFLATE,3}, {BGZF_LIBDEFLATE,2}, {BGZF_LIBDEFLATE,1}, 
          {BGZF_LIBDEFLATE,0}, {BGZF_LIBDEFLATE,12}, {BGZF_LIBDEFLATE,11}, {BGZF_LIBDEFLATE,10}, {BGZF_ZLIB,9}, {BGZF_ZLIB,7}, 
          {BGZF_ZLIB,8}, {BGZF_ZLIB,5}, {BGZF_ZLIB,3}, {BGZF_ZLIB,2}, {BGZF_ZLIB,1} };

    // ignore the header and footer of the block
    comp_block      += sizeof (BgzfHeader);
    comp_block_size -= sizeof (BgzfHeader) + sizeof (BgzfFooter);

    // decompress block
    if (!evb->gzip_compressor) evb->gzip_compressor = libdeflate_alloc_decompressor(evb);

    uint8_t uncomp_block[uncomp_block_size]; // at most 64K - fits on stack
    enum libdeflate_result ret = 
        libdeflate_deflate_decompress (evb->gzip_compressor, comp_block, comp_block_size, uncomp_block, uncomp_block_size, NULL);

    ASSERTE (ret == LIBDEFLATE_SUCCESS, "unable to read file %s. It appears to be compressed with BGZF, however decompression failed: %s", filename, libdeflate_error(ret));
    
    libdeflate_free_decompressor ((struct libdeflate_decompressor **)&evb->gzip_compressor);

    // now, re-compress with each level until the compressed size matches
    uint32_t recomp_size;
    uint8_t recomp_block[BGZF_MAX_BLOCK_SIZE]; 

    for (int level_i=0; level_i < sizeof (levels) / sizeof (levels[0]); level_i++) { 
        struct FlagsBgzf l = { .library = levels[level_i].library, .level = levels[level_i].level };

        if (l.library == BGZF_LIBDEFLATE) {
            void *compressor = libdeflate_alloc_compressor (l.level, evb);
            recomp_size = (uint32_t)libdeflate_deflate_compress (compressor, uncomp_block, uncomp_block_size, recomp_block, BGZF_MAX_BLOCK_SIZE);

            libdeflate_free_compressor (compressor);
        }
        else { // BGZF_ZLIB
            z_stream strm = { .zalloc = bgzf_alloc, .zfree  = codec_free, .opaque = evb };
            // deflateInit2 with the default zlib parameters, with is also the same as htslib does
            ASSERTE0 (deflateInit2 (&strm, l.level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK, "deflateInit2 failed");

            strm.next_in   = uncomp_block;
            strm.avail_in  = uncomp_block_size;
            strm.next_out  = recomp_block;
            strm.avail_out = sizeof (recomp_block);
            ASSERTE (deflate (&strm, Z_FINISH) == Z_STREAM_END, "deflate failed: msg=%s", strm.msg);

            recomp_size = sizeof (recomp_block) - strm.avail_out;
            
            ASSERTE0 (deflateEnd (&strm) == Z_OK, "deflateEnd failed");
        }

        bool identical = recomp_size == comp_block_size && !memcmp (comp_block, recomp_block, comp_block_size);

        if (flag.show_bgzf) 
            iprintf ("Testing library %s level %u: size_in_file=%u size_in_test=%u identical=%s\n", 
                     l.library == BGZF_LIBDEFLATE ? "libdeflate" : "zlib", l.level, comp_block_size, recomp_size, identical ? "Yes" : "No");

        if (identical) {
            if (flag.show_bgzf) 
                iprintf ("File %s: Identified as compressed with %s level %u\n", 
                         filename, l.library == BGZF_LIBDEFLATE ? "libdeflate" : "zlib", l.level);
            return l; // this still might be wrong, see comment in function header ^
        }       
    }

    if (flag.show_bgzf) iprintf ("File %s: Could not identify compression library and level\n", filename);
    return (struct FlagsBgzf){ .level = BGZF_COMP_LEVEL_UNKNOWN };
}

//---------
// PIZ SIDE
//---------

bool bgzf_load_isizes (const SectionListEntry *sl_ent) 
{
    // skip passed all VBs of this component, and read SEC_BGZF - the last section of the component - if it exists
    // but stop if we encounter the next component's SEC_TXT_HEADER before seeing the BGZF
    if (!sections_get_next_section_of_type2 (&sl_ent, SEC_BGZF, SEC_TXT_HEADER, false, false) // updates sl_ent, but its a local var, doesn't affect our caller
        || sl_ent->section_type == SEC_TXT_HEADER)
        return false; // this component doesn't contain a BGZF section

    int32_t offset = zfile_read_section (z_file, evb, 0, &evb->z_data, "z_data", SEC_BGZF, sl_ent);

    SectionHeader *header = (SectionHeader *)ENT (char, evb->z_data, offset);
    txt_file->bgzf_flags = header->flags.bgzf;

    // if we don't know the compression level, or if original file had compression level 0 (no compression), go with the default
    if (txt_file->bgzf_flags.level == BGZF_COMP_LEVEL_UNKNOWN ||
        !txt_file->bgzf_flags.level) // the user can override this behaviour with --bgzf
        txt_file->bgzf_flags.level = BGZF_COMP_LEVEL_DEFAULT;

    zfile_uncompress_section (evb, header, &txt_file->bgzf_isizes, "txt_file->bgzf_isizes", 0, SEC_BGZF);
    txt_file->bgzf_isizes.len /= 2;

    // convert to native endianity from big endian
    ARRAY (uint16_t, isizes, txt_file->bgzf_isizes);
    for (uint64_t i=0; i < txt_file->bgzf_isizes.len; i++) 
        isizes[i] = BGEN16 (isizes[i]); // now it contains isize-1 - a value 0->65535 representing an isize 1->65536

    return true; // bgzf_isizes successfully loaded
}                

// PIZ: I/O thread ahead of dispatching - calculate the BGZF blocks within this VB that need to be compressed by
// the compute thread - i.e. excluding the flanking regions of txt_data that might share a BGZF block with the adjacent VB
// and will be compressed by bgzf_compress_and_write_split_blocks.
void bgzf_calculate_blocks_one_vb (VBlock *vb, uint32_t vb_txt_data_len)
{
    ARRAY (uint16_t, isizes, txt_file->bgzf_isizes);
    #define next_isize txt_file->bgzf_isizes.param // during PIZ, we use bgzf_isizes.param as "next" - iterator on bgzf_isizes
     
    // if we don't have isize (either source is not BGZF, or compressed with --optimize, or --bgzf or data-modifying piz options, or v8)
    // then we don't prescribe blocks, and let the VB compress based on the actual length of data reconstructed
    if (!txt_file->bgzf_isizes.len) return;

    // if we have an initial region of txt_data that has a split block with the previous VB - 
    // that will be our first block, with a negative index. the previous VBs final data is now in bzgf_passed_down_len
    int32_t index = -txt_file->bzgf_passed_down_len; // first block should cover passed down data too

    while (next_isize < txt_file->bgzf_isizes.len) { 
        
        int32_t isize = (int32_t)isizes[next_isize] + 1;// +1 bc the array values are (isize-1)

        ASSERTE (index + isize > 0, "expecting index=%d + isize=%d > 0", index, isize); // if isize is small than the unconsumed data, then this block should have belonged to the previous VB

        if (index + isize > (int32_t)vb_txt_data_len) {
            txt_file->bzgf_passed_down_len = (int32_t)vb_txt_data_len - index; // pass down to next vb 
            break; // this VB doesn't have enough data to fill up this BGZF block - pass it down to the next VB
        }
        buf_alloc_more (vb, &vb->bgzf_blocks, 1, flag.vblock_memory / 63000, BgzfBlockPiz, 1.5, "bgzf_blocks");

        NEXTENT (BgzfBlockPiz, vb->bgzf_blocks) = (BgzfBlockPiz){ .txt_index = index, .txt_size = isize };

        index += isize; // definitely postive after this increment
        next_isize++;
    }

    #undef next_isize
}

static void bgzf_alloc_compressor (VBlock *vb, struct FlagsBgzf bgzf_flags)
{
    ASSERTE0 (!vb->gzip_compressor, "expecting vb->gzip_compressor=NULL");

    if (bgzf_flags.library == BGZF_LIBDEFLATE)  // libdeflate
        vb->gzip_compressor = libdeflate_alloc_compressor (bgzf_flags.level, vb);

    else { // zlib
        vb->gzip_compressor = bgzf_alloc (vb, 1, sizeof (z_stream));
        *(z_stream *)vb->gzip_compressor = (z_stream){ .zalloc = bgzf_alloc, .zfree  = codec_free, .opaque = vb };
    }
}

static void bgzf_free_compressor (VBlock *vb, struct FlagsBgzf bgzf_flags)
{
    if (bgzf_flags.library == BGZF_LIBDEFLATE)  // libdeflate
        libdeflate_free_compressor (vb->gzip_compressor);
    else
        codec_free (vb, vb->gzip_compressor);

    vb->gzip_compressor = NULL;
}

static uint32_t bgzf_compress_one_block (VBlock *vb, const char *in, uint32_t isize,
                                         int32_t block_i, int32_t txt_index) // for show_bgzf (both may be negative - indicating previous VB)
{
    START_TIMER;

    ASSERTE0 (vb->gzip_compressor, "vb->gzip_compressor=NULL");

    #define BGZF_MAX_CDATA_SIZE (BGZF_MAX_BLOCK_SIZE - sizeof (BgzfHeader) - sizeof (BgzfFooter))

    buf_alloc_more (vb, &vb->compressed, BGZF_MAX_BLOCK_SIZE, 0, char, 1.2, "compressed");

    BgzfHeader *header = (BgzfHeader *)AFTERENT (char, vb->compressed);
    buf_add (&vb->compressed, BGZF_EOF, sizeof (BgzfHeader)); // template of header - only bsize needs updating

    uint32_t comp_index = vb->compressed.len;
    int out_size;

    if (txt_file->bgzf_flags.library == BGZF_LIBDEFLATE) { // libdeflate

        out_size = (int)libdeflate_deflate_compress (vb->gzip_compressor, in, isize, AFTERENT (char, vb->compressed), BGZF_MAX_CDATA_SIZE);

        // in case the compressed data doesn't fit in one BGZF block, move to compressing at the maximum level. this can
        // happen theoretically (maybe) if the original data was compressed with a higher level, and an uncompressible 64K block was
        // "compressed" to just under 64K while in our compression level it is just over 64K.
        if (!out_size) {
            void *high_compressor = libdeflate_alloc_compressor (12, vb); // libdefate's highest level
            out_size = libdeflate_deflate_compress (vb->gzip_compressor, in, isize, AFTERENT (char, vb->compressed), BGZF_MAX_CDATA_SIZE);
            libdeflate_free_compressor (high_compressor);
        }
    }
    else { // zlib
        #define strm ((z_stream *)vb->gzip_compressor)

        ASSERTE0 (deflateInit2 (vb->gzip_compressor, txt_file->bgzf_flags.level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK, 
                  "deflateInit2 failed");

        strm->next_in   = (uint8_t *)in;
        strm->avail_in  = isize;
        strm->next_out  = AFTERENT (uint8_t, vb->compressed);
        strm->avail_out = BGZF_MAX_CDATA_SIZE;
        ASSERTE (deflate (vb->gzip_compressor, Z_FINISH) == Z_STREAM_END, "deflate failed: msg=%s", strm->msg);

        out_size = BGZF_MAX_CDATA_SIZE - strm->avail_out;
        
        ASSERTE0 (deflateEnd (vb->gzip_compressor) == Z_OK, "deflateEnd failed");
        #undef strm
    }

    if (flag.show_bgzf)
        #define C(i) (i < isize ? char_to_printable (in[i]).s : "")
        iprintf ("%-7s vb=%u i=%d compressed_index=%u size=%u txt_index=%d size=%u txt_data[5]=%1s%1s%1s%1s%1s %s\n",
                arch_am_i_io_thread() ? "IO" : "COMPUTE", vb->vblock_i, block_i,
                comp_index, (unsigned)out_size, txt_index, isize, C(0), C(1), C(2), C(3), C(4),
                out_size == BGZF_EOF_LEN ? "EOF" : "");
        #undef C

    ASSERTE (out_size, "cannot compress block with %u bytes into a BGZF block with %u bytes", isize, BGZF_MAX_BLOCK_SIZE);
    vb->compressed.len += out_size;

    header->bsize = LTEN16 ((uint16_t)(sizeof (BgzfHeader) + out_size + sizeof (BgzfFooter) - 1));

    BgzfFooter footer = { .crc32 = LTEN32 (libdeflate_crc32 (0, in, isize)),
                          .isize = LTEN32 (isize) };
    buf_add (&vb->compressed, &footer, sizeof (BgzfFooter));

    if (flag.show_time) {
        if (arch_am_i_io_thread ()) COPY_TIMER (bgzf_io_thread)
        else                        COPY_TIMER (bgzf_compute_thread);
    }

    return (uint32_t)out_size;
} 

// Called in the Compute Thread for VBs and in I/O thread for the Txt Header - fallback from 
// bgzf_compress_vb in case no bgzf blocks are available and we need to make our own
static void bgzf_compress_vb_no_blocks (VBlock *vb)
{
    #define BGZF_CREATED_BLOCK_SIZE 65280 // same size as observed in htslib-created files

    // estimated size, we will increase later if needed
    buf_alloc (vb, &vb->compressed, vb->txt_data.len/2, 1, "compressed"); // alloc based on estimated size
    buf_alloc (vb, &vb->bgzf_blocks, (1 + vb->txt_data.len / BGZF_CREATED_BLOCK_SIZE) * sizeof (BgzfBlockPiz), 1, "bgzf_blocks");
    bgzf_alloc_compressor (vb, txt_file->bgzf_flags);

    uint32_t next=0, block_i=0;
    while (next < vb->txt_data.len) {
        uint32_t block_isize = MIN (BGZF_CREATED_BLOCK_SIZE, vb->txt_data.len - next);
        buf_alloc_more (vb, &vb->compressed, BGZF_MAX_BLOCK_SIZE, 0, uint8_t, 1.5, "compressed");

        bgzf_compress_one_block (vb, ENT (char, vb->txt_data, next), block_isize, block_i++, next);

        NEXTENT (BgzfBlockPiz, vb->bgzf_blocks) = (BgzfBlockPiz){ .txt_index = next, .txt_size = block_isize };
        next += block_isize;
    }

    vb->compressed.uncomp_size = (uint32_t)vb->txt_data.len;
    
    bgzf_free_compressor (vb, txt_file->bgzf_flags);
}

// Called in the Compute Thread for VBs and in I/O thread for the Txt Header.
// bgzf-compress vb->txt_data into vb->compressed - reconstructing the same-isize BGZF blocks as the original txt file
// note: data at the beginning and end of txt_data that doesn't fit into a whole BGZF block (i.e. the block is shared
// with the adjacent VB) - we don't compress here, but rather in bgzf_write_to_disk().
// Note: we hope to reconstruct the exact same byte-level BGZF blocks, but that will only happen if the GZIP library 
// (eg libdeflate), version and parameters are the same 
void bgzf_compress_vb (VBlock *vb)
{
    ASSERTE0 (!vb->compressed.len, "expecting vb->compressed to be free, but its not");

    // case: we don't have perscribed bgzf blocks bc we're modifying the data or the source file didn't provided them
    if (!buf_is_allocated (&txt_file->bgzf_isizes)) {
        bgzf_compress_vb_no_blocks (vb);
        return;
    }

    // we have no bgzf blocks in this VB, possibly bc it is too small to fill a BGZF block and all data was passed up to next vb
    if (!vb->bgzf_blocks.len) return;

    buf_alloc (vb, &vb->compressed, vb->bgzf_blocks.len * BGZF_MAX_BLOCK_SIZE/2, 1, "compressed"); // alloc based on estimated size
    bgzf_alloc_compressor (vb, txt_file->bgzf_flags);

    ARRAY (BgzfBlockPiz, blocks, vb->bgzf_blocks);
    for (uint64_t i=0; i < vb->bgzf_blocks.len; i++) {

        ASSERTE (blocks[i].txt_index + blocks[i].txt_size <= vb->txt_data.len, 
                 "block=%"PRIu64" out of range: expecting txt_index=%u txt_size=%u <= txt_data.len=%u",
                 i, blocks[i].txt_index, blocks[i].txt_size, (uint32_t)vb->txt_data.len);

        // case: all the data is from the current VB. if there is any data from the previous VB, the index will be negative 
        // and we will compress it in bgzf_write_to_disk() instead
        if (blocks[i].txt_index >= 0) {
            bgzf_compress_one_block (vb, ENT (char, vb->txt_data, blocks[i].txt_index), blocks[i].txt_size, i, blocks[i].txt_index);
            vb->compressed.uncomp_size += blocks[i].txt_size;
        }
    }

    bgzf_free_compressor (vb, txt_file->bgzf_flags);
}

// PIZ: Called by I/O thread to complete the work Compute Thread cannot do - see 1,2,3 below
void bgzf_write_to_disk (VBlock *vb)
{
    // Step 1. bgzf-compress the BGZF block that is split between end the previous VB(s) (data currently in txt_file->unconsumed_txt)
    //    and the beginning of this VB, and write the compressed data to disk
    if (txt_file->unconsumed_txt.len) {

        BgzfBlockPiz *first_block = FIRSTENT (BgzfBlockPiz, vb->bgzf_blocks); // this block contains both uncosumed data and the first data of this VB
        int32_t first_data_len = first_block->txt_index + first_block->txt_size; // hopefully the VB has this much, but possibly not...

        ASSERTE (txt_file->unconsumed_txt.len + first_block->txt_index == 0, "Expecting txt_file->unconsumed_txt.len=%"PRId64" + first_block->txt_index=%d == 0",
                 txt_file->unconsumed_txt.len, first_block->txt_index);
        
        // case: if we have enough data in this VB to complete the BGZF block, we compress it now
        if (first_data_len <= vb->txt_data.len) {
            char block[BGZF_MAX_BLOCK_SIZE];
            memcpy (block, txt_file->unconsumed_txt.data, txt_file->unconsumed_txt.len);
            memcpy (&block[txt_file->unconsumed_txt.len], FIRSTENT (char, vb->txt_data), first_data_len); 

            // note: if we're writing the txt_header, then evb->compressed will contain the first BGZF blocks of the txt_header. 
            // Luckily, there will be no unconsumed_data as there is nothing before the header, so Step 1 is not entered
            ASSERTE0 (!evb->compressed.len, "expecting evb->compressed to be empty");

            bgzf_alloc_compressor (evb, txt_file->bgzf_flags);
            bgzf_compress_one_block (evb, block, first_block->txt_size, 0, first_block->txt_index); // compress into evb->compressed
            bgzf_free_compressor (evb, txt_file->bgzf_flags);

            if (!flag.test) file_write (txt_file, evb->compressed.data, evb->compressed.len);

            txt_file->txt_data_so_far_single += first_block->txt_size;
            txt_file->disk_so_far            += evb->compressed.len;

            buf_free (&txt_file->unconsumed_txt);
            buf_free (&evb->compressed);
        }

        // case: we don't have enough data in this VB to complete the BGZF block
        else {
            // santiy check: this case, we are not expecting any compressed data
            ASSERTE0 (!vb->compressed.len, "not expecting compressed data, if VB is too small to complete a BGZF block");
        }
    }

    // Step 2. Write all the "middle" blocks of this VB, compressed by bgzf_compress_vb, currently in vb->compressed, to disk
    if (vb->compressed.len) {
        if (!flag.test) 
            file_write (txt_file, vb->compressed.data, vb->compressed.len);

        txt_file->txt_data_so_far_single += vb->compressed.uncomp_size;
        txt_file->disk_so_far            += vb->compressed.len; // evb->compressed.len;
        buf_free (&vb->compressed);
    }

    // Step 3. move the final part (if any) of vb->txt_data, not compressed because its split with the subsequent VB, 
    //    into txt_file->unconsumed_txt, to be compressed in #1 of the next call to this function.
    //    special case: the previous unconsumed txt, plus all the data in this VB, is not enough to complete the block.
    //    in this case, we just add all the data to unconsumed text.
    BgzfBlockPiz *last_block = LASTENT (BgzfBlockPiz, vb->bgzf_blocks); // bad pointer if we have no blocks
    uint32_t last_data_index = vb->bgzf_blocks.len ? last_block->txt_index + last_block->txt_size : 0;
    uint32_t last_data_len   = (uint32_t)vb->txt_data.len - last_data_index;

    if (last_data_len) {                             
        buf_alloc_more (evb, &txt_file->unconsumed_txt, last_data_len, 0, char, 0, "txt_file->unconsumed_txt");
        buf_add (&txt_file->unconsumed_txt, ENT (char, vb->txt_data, last_data_index), last_data_len);
    }
}

void bgzf_write_finalize (File *file)
{
    // write EOF block if needed
    if (file->bgzf_flags.has_eof_block) {
        if (!flag.test) file_write (file, BGZF_EOF, BGZF_EOF_LEN);
        file->disk_so_far += BGZF_EOF_LEN;
    
        if (flag.show_bgzf) iprintf ("%-7s vb=%u   EOF\n", "IO", 0);
    }

    // if we attempted to reconstruct the BGZF lock to the original file's bgzf_isizes - warn if we were unlucky and failed
    if (file->bgzf_isizes.len) {
        uint8_t signature[3];
        bgzf_sign (file->disk_so_far, signature);

        ASSERTW (!memcmp (signature, file->bgzf_signature, 3), 
                 "FYI: %s was recompressed with BGZF (.gz). However, it seems that the original file was compressed with a different compression library than genozip uses, resulting in a slightly different level of compression. Rest assured that the actual data is identical.", file->name);
    }
}

void bgzf_sign (uint64_t disk_size, uint8_t *signature)
{
    signature[0] = (disk_size      ) & 0xff; // LSB of size
    signature[1] = (disk_size >> 8 ) & 0xff;
    signature[2] = (disk_size >> 16) & 0xff;
}
