// ------------------------------------------------------------------
//   txtfile.c
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt
 
#include "profiler.h"

#ifdef __APPLE__
#define off64_t __int64_t // needed for for conda mac - otherwise zlib.h throws compilation errors
#endif
#define Z_LARGE64
#include <errno.h>
#include <bzlib.h>
#include "genozip.h"
#include "txtfile.h"
#include "vblock.h"
#include "vcf.h"
#include "zfile.h"
#include "file.h"
#include "strings.h"
#include "endianness.h"
#include "crypt.h"
#include "progress.h"
#include "codec.h"
#include "bgzf.h"
#include "mutex.h"
#include "digest.h"
#include "zlib/zlib.h"
#include "libdeflate/libdeflate.h"

static bool is_first_txt = true; 
static uint32_t total_bound_txt_headers_len = 0;

static uint32_t vb1_txt_data_comp_len = 0; // ZIP: approximate size of the source BZ2/gz/bgzf-compressed vb->txt_data

const char *txtfile_dump_filename (VBlockP vb, const char *base_name, const char *ext) 
{
    char *dump_filename = MALLOC (strlen (base_name) + 100); // we're going to leak this allocation
    sprintf (dump_filename, "%s.vblock-%u.start-%"PRIu64".len-%u.%s", 
             base_name, vb->vblock_i, vb->vb_position_txt_file, (uint32_t)vb->txt_data.len, ext);
    return dump_filename;
}

// dump bad vb to disk
const char *txtfile_dump_vb (VBlockP vb, const char *base_name)
{
    const char *dump_filename = txtfile_dump_filename (vb, base_name, "bad");
    buf_dump_to_file (dump_filename, &vb->txt_data, 1, false, false, true);

    return dump_filename;
}

uint32_t txtfile_get_bound_headers_len(void) { return total_bound_txt_headers_len; }

static inline uint32_t txtfile_read_block_plain (VBlock *vb, uint32_t max_bytes)
{
    char *data = AFTERENT (char, vb->txt_data);
    int32_t bytes_read;

    // case: we have data passed to us from file_open_txt_read - handle it first
    if (!vb->txt_data.len && evb->compressed.len) {
        memcpy (data, evb->compressed.data, (bytes_read = evb->compressed.len));
        buf_free (&evb->compressed);
    }

    // case: normal read
    else {
        bytes_read = read (fileno((FILE *)txt_file->file), data, max_bytes); // -1 if error in libc
        ASSERTE (bytes_read >= 0, "read failed from %s: %s", txt_name, strerror(errno));

        // bytes_read=0 and we're using an external decompressor - it is either EOF or
        // there is an error. In any event, the decompressor is done and we can suck in its stderr to inspect it
        if (!bytes_read && file_is_read_via_ext_decompressor (txt_file)) {
            file_assert_ext_decompressor();
            txt_file->is_eof = true;
            return 0; // all is good - just a normal end-of-file
        }
    }

    txt_file->disk_so_far += (int64_t)bytes_read;

#ifdef _WIN32
    // in Windows using Powershell, the first 3 characters on an stdin pipe are BOM: 0xEF,0xBB,0xBF https://en.wikipedia.org/wiki/Byte_order_mark
    // these charactes are not in 7-bit ASCII, so highly unlikely to be present natrually in a textual txt file
    if (txt_file->redirected && 
        txt_file->disk_so_far == (int64_t)bytes_read &&  // start of file
        bytes_read >= 3  && 
        (uint8_t)data[0] == 0xEF && 
        (uint8_t)data[1] == 0xBB && 
        (uint8_t)data[2] == 0xBF) {

        // Bomb the BOM
        bytes_read -= 3;
        memcpy (data, data + 3, bytes_read);
        txt_file->disk_so_far -= 3;
    }
#endif
    vb->txt_data.len += bytes_read;

    return (uint32_t)bytes_read;
}

static inline uint32_t txtfile_read_block_gz (VBlock *vb, uint32_t max_bytes)
{
    uint32_t bytes_read = gzfread (AFTERENT (char, vb->txt_data), 1, max_bytes, (gzFile)txt_file->file);
    vb->txt_data.len += bytes_read;

    if (bytes_read)
        txt_file->disk_so_far = gzconsumed64 ((gzFile)txt_file->file); // this is actually all the data uncompressed so far, some of it not yet read by us and still waiting in zlib's output buffer
    else
        txt_file->is_eof = true;

    return bytes_read;
}

static inline uint32_t txtfile_read_block_bz2 (VBlock *vb, uint32_t max_bytes)
{
    uint32_t bytes_read = BZ2_bzread ((BZFILE *)txt_file->file, AFTERENT (char, vb->txt_data), max_bytes);
    vb->txt_data.len += bytes_read;

    if (bytes_read)
        txt_file->disk_so_far = BZ2_consumed ((BZFILE *)txt_file->file); 
    else
        txt_file->is_eof = true;

    return bytes_read;
}

// BGZF: we read *compressed* data into vb->compressed - that will be decompressed now or later, depending on uncompress. 
// We read data with a *decompressed* size up to max_uncomp. vb->compressed always contains only full BGZF blocks
static inline uint32_t txtfile_read_block_bgzf (VBlock *vb, int32_t max_uncomp /* must be signed */, bool uncompress)
{
    #define uncomp_len param // we use vb->compress.param to hold the uncompressed length of the bgzf data in vb->compress

    uint32_t block_comp_len, block_uncomp_len, this_uncomp_len=0;

    if (uncompress)
        vb->gzip_compressor = libdeflate_alloc_decompressor(vb);
        
    while (vb->compressed.uncomp_len < max_uncomp - BGZF_MAX_BLOCK_SIZE) {

        buf_alloc_more (vb, &vb->compressed, BGZF_MAX_BLOCK_SIZE, max_uncomp/4, char, 1.5, "compressed")

        // case: we have data passed to us from file_open_txt_read - handle it first
        if (!vb->txt_data.len && evb->compressed.len) {
            block_uncomp_len = evb->compressed.uncomp_len;
            block_comp_len   = evb->compressed.len;

            // if we're reading a VB (not the txt header) - copy the compressed data from evb to vb
            if (evb != vb) {
                buf_copy (vb, &vb->compressed, &evb->compressed, 0,0,0,0);
                buf_free (&evb->compressed);
            }

            // add block to list
            buf_alloc_more (vb, &vb->bgzf_blocks, 1, 1.2 * max_uncomp / BGZF_MAX_BLOCK_SIZE, BgzfBlockZip, 2, "bgzf_blocks");
            NEXTENT (BgzfBlockZip, vb->bgzf_blocks) = (BgzfBlockZip)
                { .txt_index        = 0,
                  .compressed_index = 0,
                  .txt_size         = block_uncomp_len,
                  .comp_size        = block_comp_len,
                  .is_decompressed  = false };           
        }
        else {
            block_uncomp_len = (uint32_t)bgzf_read_block (txt_file, AFTERENT (uint8_t, vb->compressed), &block_comp_len, false);
            
            // check for corrupt data - at this point we've already confirm the file is BGZF so not expecting a different block
            if (block_uncomp_len == BGZF_BLOCK_GZIP_NOT_BGZIP || block_uncomp_len == BGZF_BLOCK_IS_NOT_GZIP) {
                // dump to file
                char dump_fn[strlen(txt_name)+100];
                sprintf (dump_fn, "%s.vb-%u.bad-bgzf.bad-offset-0x%X", txt_name, vb->vblock_i, (uint32_t)vb->compressed.len);
                Buffer dump_buffer = vb->compressed; // a copy
                dump_buffer.len   += block_comp_len; // compressed size
                buf_dump_to_file (dump_fn, &dump_buffer, 1, false, false, true);

                ABORT ("Error in txtfile_read_block_bgzf: Invalid BGZF block in vb=%u block_comp_len=%u. Entire BGZF data of this vblock dumped to %s, bad block stats at offset 0x%X",
                       vb->vblock_i, block_comp_len, dump_fn, (uint32_t)vb->compressed.len);
            }

            // add block to list - including the EOF block (block_comp_len=BGZF_EOF_LEN block_uncomp_len=0)
            if (block_comp_len) {
                buf_alloc_more (vb, &vb->bgzf_blocks, 1, 1.2 * max_uncomp / BGZF_MAX_BLOCK_SIZE, BgzfBlockZip, 2, "bgzf_blocks");
                NEXTENT (BgzfBlockZip, vb->bgzf_blocks) = (BgzfBlockZip)
                    { .txt_index        = vb->txt_data.len, // after passed-down data and all previous blocks
                      .compressed_index = vb->compressed.len,
                      .txt_size         = block_uncomp_len,
                      .comp_size        = block_comp_len,
                      .is_decompressed  = !block_uncomp_len }; // EOF block is always considered decompressed           

                vb->compressed.len += block_comp_len;   // compressed size
            }

            // case EOF - happens in 2 cases: 1. EOF block (block_comp_len=BGZF_EOF_LEN) or 2. no EOF block (block_comp_len=0)
            if (!block_uncomp_len) {
                txt_file->is_eof = true;
                if (flag.show_bgzf && txt_file->bgzf_flags.has_eof_block) 
                    iprint0 ("IO      vb=0 EOF\n");
                break;
            }
        }

        this_uncomp_len           += block_uncomp_len; // total uncompressed length of data read by this function call
        vb->compressed.uncomp_len += block_uncomp_len; // total uncompressed length of data in vb->compress
        vb->txt_data.len          += block_uncomp_len; // total length of txt_data after adding decompressed vb->compressed (may also include pass-down data)
        txt_file->disk_so_far     += block_comp_len;   

        // we decompress one block a time in the loop so that the decompression is parallel with the disk reading into cache
        if (uncompress) bgzf_uncompress_one_block (vb, LASTENT (BgzfBlockZip, vb->bgzf_blocks));  
    }

    if (uncompress) {
        buf_free (&evb->compressed); 
        libdeflate_free_decompressor ((struct libdeflate_decompressor **)&vb->gzip_compressor);
    }

    return this_uncomp_len;
#undef param
}

// performs a single I/O read operation - returns number of bytes read
// data is placed in vb->txt_data, except if its BGZF and uncompress=false - compressed data is placed in vb->compressed
static uint32_t txtfile_read_block (VBlock *vb, uint32_t max_bytes,
                                    bool uncompress) // in BGZF, whether to uncompress the data. ignored if not BGZF
{
    START_TIMER;

    if (txt_file->is_eof) return 0; // nothing more to read

    uint32_t bytes_read=0;

    if (file_is_plain_or_ext_decompressor (txt_file)) 
        bytes_read = txtfile_read_block_plain (vb, max_bytes);
    
    // BGZF: we read *compressed* data into vb->compressed - that will be decompressed later. we read
    // data with a *decompressed* size up to max_bytes. vb->compressed always contains only full BGZF blocks
    else if (txt_file->codec == CODEC_BGZF) 
        bytes_read = txtfile_read_block_bgzf (vb, max_bytes, uncompress); // bytes_read is in uncompressed terms

    else if (txt_file->codec == CODEC_GZ) 
        bytes_read = txtfile_read_block_gz (vb, max_bytes);

    else if (txt_file->codec == CODEC_BZ2) 
        bytes_read = txtfile_read_block_bz2 (vb, max_bytes);
    
    else 
        ABORT ("txtfile_read_block: Invalid file type %s (codec=%s)", ft_name (txt_file->type), codec_name (txt_file->codec));

    COPY_TIMER_VB (evb, read);
    return bytes_read;
}

// default callback from DataTypeProperties.is_header_done: 
// returns header length if header read is complete + sets lines.len, 0 if complete but not existant, -1 not complete yet 
int32_t def_is_header_done (void)
{
    ARRAY (char, header, evb->txt_data);
    evb->lines.len = 0; // reset in case we've called this function a number of times (in case of a very large header)
    char prev_char = '\n';

    // check stop condition - a line not beginning with a 'first_char'
    for (int i=0; i < evb->txt_data.len; i++) { // start from 1 back just in case it is a newline, and end 1 char before bc our test is 2 chars
        if (header[i] == '\n') 
            evb->lines.len++;   

        if (prev_char == '\n' && header[i] != DTPT (txt_header_1st_char)) {
            // if we have no header, its an error if we require one
            ASSINP (i || DTPT (txt_header_required) != HDR_MUST, "Error: %s is missing a %s header", txt_name, dt_name (txt_file->data_type));
            return i; // return header length
        }
        prev_char = header[i];
    }

    return -1; // not end of header yet
}

// ZIP I/O thread: returns the hash of the header
static Digest txtfile_read_header (bool is_first_txt)
{
    START_TIMER;

    ASSERT_DT_FUNC (txt_file, is_header_done);

    int32_t header_len;
    uint32_t bytes_read=1 /* non-zero */;

    // read data from the file until either 1. EOF is reached 2. end of txt header is reached
    #define HEADER_BLOCK (256*1024) // we have no idea how big the header will be... read this much at a time
    while ((header_len = (DT_FUNC (txt_file, is_header_done)())) < 0) { // we might have data here from txtfile_test_data
        
        if (!bytes_read) {
            if (flags_pipe_in_process_died()) // only works for Linux
                ABORTINP ("Pipe-in process %s (pid=%u) died before the %s header was fully read; only %"PRIu64" bytes were read",
                          flags_pipe_in_process_name(), flags_pipe_in_pid(), dt_name(txt_file->data_type), evb->txt_data.len);
            else
                ABORT ("Error in txtfile_read_header: unexpected end-of-file while reading the %s header of %s (header_len=%"PRIu64")", 
                       dt_name(txt_file->data_type), txt_name, evb->txt_data.len);
        }

        buf_alloc_more (evb, &evb->txt_data, HEADER_BLOCK, 0, char, 1.15, "txt_data");    
        bytes_read = txtfile_read_block (evb, HEADER_BLOCK, true);
    }

    // the excess data is for the next vb to read 
    buf_copy (evb, &txt_file->unconsumed_txt, &evb->txt_data, 1, header_len, 0, "txt_file->unconsumed_txt");

    // account for the passed up vb=1 data - using the compression ratio of this block
    vb1_txt_data_comp_len = ((double)(evb->txt_data.len - header_len) / (double)evb->txt_data.len) * (double)txt_file->disk_so_far;

    txt_file->txt_data_so_far_single = evb->txt_data.len = header_len; // trim to uncompressed length of txt header

    // md5 header - always digest_ctx_single, digest_ctx_bound only if first component 
    if (flag.bind && is_first_txt) digest_update (&z_file->digest_ctx_bound, &evb->txt_data, "txt_header:digest_ctx_bound");
    digest_update (&z_file->digest_ctx_single, &evb->txt_data, "txt_header:digest_ctx_single");

    Digest header_digest = digest_snapshot (&z_file->digest_ctx_single);

    COPY_TIMER_VB (evb, txtfile_read_header); // same profiler entry as txtfile_read_header

    return header_digest;
}

// default "unconsumed" function file formats where we need to read whole \n-ending lines. returns the unconsumed data length
int32_t def_unconsumed (VBlockP vb, uint32_t first_i, int32_t *i)
{
    ASSERTE (*i >= 0 && *i < vb->txt_data.len, "*i=%d is out of range [0,%"PRIu64"]", *i, vb->txt_data.len);

    for (; *i >= (int32_t)first_i; (*i)--) {
        if (vb->txt_data.data[*i] == '\n') 
            return vb->txt_data.len-1 - *i;
    }

    return -1; // cannot find \n in the data starting first_i
}

static uint32_t txtfile_get_unconsumed_to_pass_up (VBlock *vb, bool testing_memory)
{
    int32_t passed_up_len;
    int32_t i=vb->txt_data.len-1; // next index to test (going backwards)

    // case: the data is BGZF-compressed in vb->compressed, except for passed down data from prev VB        
    // uncompress one block at a time to see if its sufficient. usually, one block is enough
    if (txt_file->codec == CODEC_BGZF && vb->compressed.len) {

        vb->gzip_compressor = libdeflate_alloc_decompressor(vb);

        for (int block_i=vb->bgzf_blocks.len-1; block_i >= 0; block_i--) {
            BgzfBlockZip *bb = ENT (BgzfBlockZip, vb->bgzf_blocks, block_i);
            bgzf_uncompress_one_block (vb, bb);

            passed_up_len = DT_FUNC(txt_file, unconsumed)(vb, bb->txt_index, &i);
            if (passed_up_len >= 0) goto done; // we have the answer (callback returns -1 if no it needs more data)
        }

        libdeflate_free_decompressor ((struct libdeflate_decompressor **)&vb->gzip_compressor);

        // if not found - fall through to test the passed-down data too now
    }

    // test remaining txt_data including passed-down data from previous VB
    passed_up_len = DT_FUNC(txt_file, unconsumed)(vb, 0, &i);

    // case: we're testing memory and this VB is too small for a single line - return and caller will try again with a larger VB
    if (testing_memory && passed_up_len < 0) return (uint32_t)-1;

    ASSERTE (passed_up_len >= 0, "failed to find a single complete line in the entire vb in vb=%u data_type=%s codec=%s. Sometimes this happens when the file is missing a newline on the last line. VB dumped: %s", 
             vb->vblock_i, dt_name (txt_file->data_type), codec_name (txt_file->codec), txtfile_dump_vb (vb, txt_name));

done:
    return (uint32_t)passed_up_len;
}

// ZIP I/O threads
void txtfile_read_vblock (VBlock *vb, bool testing_memory)
{
    START_TIMER;

    ASSERT_DT_FUNC (txt_file, unconsumed);

    uint64_t pos_before = 0;
    if (vb->vblock_i==1 && file_is_read_via_int_decompressor (txt_file))
        pos_before = file_tell (txt_file);

    buf_alloc (vb, &vb->txt_data, flag.vblock_memory, 1, "txt_data");    

    // start with using the data passed down from the previous VB (note: copy & free and not move! so we can reuse txt_data next vb)
    if (buf_is_allocated (&txt_file->unconsumed_txt)) {
        buf_copy (vb, &vb->txt_data, &txt_file->unconsumed_txt, 0 ,0 ,0, "txt_data");
        buf_free (&txt_file->unconsumed_txt);
    }

    // read data from the file until either 1. EOF is reached 2. end of block is reached
    uint64_t max_memory_per_vb = flag.vblock_memory;
    uint32_t passed_up_len=0;

    bool always_uncompress = flag.pair == PAIR_READ_2 || // if we're reading the 2nd paired file, fastq_txtfile_have_enough_lines needs the whole data
                             flag.make_reference      || // unconsumed callback for make-reference needs to inspect the whole data
                             testing_memory;

    for (int32_t block_i=0; ; block_i++) {

        uint32_t len = max_memory_per_vb > vb->txt_data.len ? txtfile_read_block (vb, MIN (max_memory_per_vb - vb->txt_data.len, 1<<30 /* read() can't handle moer */), always_uncompress) 
                                                            : 0;

        if (!len || vb->txt_data.len >= max_memory_per_vb) {  // EOF or we have filled up the allocted memory

            // case: this is the 2nd file of a fastq pair - make sure it has at least as many fastq "lines" as the first file
            if (flag.pair == PAIR_READ_2 &&  // we are reading the second file of a fastq file pair (with --pair)
                !fastq_txtfile_have_enough_lines (vb, &passed_up_len)) { // we don't yet have all the data we need

                ASSINP (len, "File %s has less FASTQ reads than its R1 counterpart", txt_name);

                ASSERTE (vb->txt_data.len, "txt_data.len=0 when reading pair-2 vb=%u", vb->vblock_i);

                // if we need more lines - increase memory and keep on reading
                max_memory_per_vb *= 1.1; 
                buf_alloc (vb, &vb->txt_data, max_memory_per_vb, 1, "txt_data");    
            }
            else
                break;
        }
    }

    if (always_uncompress) buf_free (&vb->compressed); // tested by txtfile_get_unconsumed_to_pass_up

    // callback to decide what part of txt_data to pass up to the next VB (usually partial lines, but sometimes more)
    // note: even if we haven't read any new data (everything was passed down), we still might data to pass up - eg
    // in FASTA with make-reference if we have a lots of small contigs, each VB will take one contig and pass up the remaining
    if (!passed_up_len && vb->txt_data.len) {
        passed_up_len = txtfile_get_unconsumed_to_pass_up (vb, testing_memory);

        // case: return if we're testing memory, and there is not even one line of text  
        if (testing_memory && passed_up_len == (uint32_t)-1) {
            buf_copy (evb, &txt_file->unconsumed_txt, &vb->txt_data, 0, 0, 0, "txt_file->unconsumed_txt"); 
            buf_free (&vb->txt_data);
            return;
        }
    }

    // if we have some unconsumed data, pass it up to the next vb
    if (passed_up_len) {
        buf_copy (evb, &txt_file->unconsumed_txt, &vb->txt_data, 1, // evb, because dst buffer belongs to File
                  vb->txt_data.len - passed_up_len, passed_up_len, "txt_file->unconsumed_txt");

        // now, if our data is bgzf-compressed, txt_data.len becomes shorter than indicated by vb->bgzf_blocks. that's ok - all that data
        // is decompressed and passed-down to the next VB. because it has been decompressed, the compute thread won't try to decompress it again
        vb->txt_data.len -= passed_up_len; 

        // if is possible we reached eof but still have pass_up_data - this happens eg in make-reference when a
        // VB takes only one contig from txt_data and pass up the rest - reset eof so that we come back here to process the rest
        txt_file->is_eof = false;
    }

    vb->vb_position_txt_file = txt_file->txt_data_so_far_single;

    vb->vb_data_size = vb->txt_data.len; // initial value. it may change if --optimize is used.

    if (!testing_memory) {

        txt_file->txt_data_so_far_single += vb->txt_data.len;
    
        // update vb1_txt_data_comp_len used by txtfile_estimate_txt_data_size(). Note: it already includes
        // the part of vb=1 passed up from txtfile_read_header()
        if (vb->vblock_i==1 && file_is_read_via_int_decompressor (txt_file)) {
            vb1_txt_data_comp_len += file_tell (txt_file) - pos_before; // bgzf/gz/bz2 compressed bytes read

            // deduct the amount of compressed data due to passed up data that actually belongs to vb>=2
            // assume the same compression ratio for the passup part as the (vb data + passed up)
            if (passed_up_len) {
                double comp_ratio = (double)vb1_txt_data_comp_len / (double)(vb->txt_data.len + passed_up_len);
                uint32_t pass_up_len_comp = (double)passed_up_len * comp_ratio;
                vb1_txt_data_comp_len -= pass_up_len_comp;
            }
        }
    }

    if (DTPT(zip_read_one_vb)) DTPT(zip_read_one_vb)(vb);

   COPY_TIMER (txtfile_read_vblock);
}

// read num_lines of the txtfile (after the header), and call test_func for each line. true iff the proportion of lines
// that past the test is at least success_threashold
bool txtfile_test_data (char first_char,            // first character in every header line
                        unsigned num_lines_to_test, // number of lines to test
                        double success_threashold,  // proportion of lines that need to pass the test, for this function to return true
                        TxtFileTestFunc test_func)
{
    uint32_t line_start_i = 0;
    unsigned num_lines_so_far = 0; // number of data (non-header) lines
    unsigned successes = 0;

    #define TEST_BLOCK_SIZE (256 * 1024)

    while (1) {      // read data from the file until either 1. EOF is reached 2. we pass the header + num_lines_to_test lines
        buf_alloc_more (evb, &evb->txt_data, TEST_BLOCK_SIZE + 1 /* for \0 */, 0, char, 1.2, "txt_data");    

        uint64_t start_read = evb->txt_data.len;
        txtfile_read_block (evb, TEST_BLOCK_SIZE, true);
        if (start_read == evb->txt_data.len) break; // EOF

        ARRAY (char, str, evb->txt_data); // declare here, in case of a realloc ^ 
        for (uint64_t i=start_read; i < evb->txt_data.len; i++) {

            if (str[i] == '\n') { 
                if (str[line_start_i] != first_char) {  // data line
                    successes += test_func (&str[line_start_i], i - line_start_i);
                    num_lines_so_far++;

                    if (num_lines_so_far == num_lines_to_test) goto done;
                }
                line_start_i = i+1; 
            }
        }
    }
    // note: read data is left in evb->txt_data for the use of txtfile_read_header

done:
    return (double)successes / (double)num_lines_so_far >= success_threashold;
}

// PIZ
void txtfile_write_to_disk (Buffer *buf)
{
    if (!buf->len) return;
    
    if (!flag.test) file_write (txt_file, buf->data, buf->len);

    txt_file->txt_data_so_far_single += buf->len;
    txt_file->disk_so_far            += buf->len;
}

void txtfile_write_one_vblock (VBlockP vb)
{
    START_TIMER;

    if (txt_file->codec == CODEC_BGZF)
        bgzf_write_to_disk (vb); 
    else
        txtfile_write_to_disk (&vb->txt_data);

    ASSERTW (vb->txt_data.len == vb->vb_data_size || // files are the same size, expected
             exe_type == EXE_GENOCAT ||              // many genocat flags modify the output file, so don't compare
             !dt_get_translation().is_src_dt,        // we are translating between data types - the source and target txt files have different sizes
             "Warning: vblock_i=%u (num_lines=%u vb_start_line_in_file=%u) had %s bytes in the original %s file but %s bytes in the reconstructed file (diff=%d)", 
             vb->vblock_i, (uint32_t)vb->lines.len, vb->first_line,
             str_uint_commas (vb->vb_data_size).s, dt_name (txt_file->data_type), 
             str_uint_commas (vb->txt_data.len).s, 
             (int32_t)vb->txt_data.len - (int32_t)vb->vb_data_size);

    COPY_TIMER (write);
}

// PIZ - called from fastq_txtfile_write_one_vblock_interleave
void txtfile_write_4_lines (VBlockP vb, 
                            unsigned pair) // 1 or 2 to add /1 or /2 to the end of the qname
{
    static const char *suffixes[3] = { "", "/1", "/2" }; // suffixes for pair 1 and pair 2 reads

    ARRAY (char, txt, vb->txt_data);
    #define start_line (vb->txt_data.param) // we use param as "start_line"

    for (unsigned nl=0; nl < 4; nl++) {
        int64_t last_in_line = start_line; 
        while (txt[last_in_line] != '\n') last_in_line++;
        
        int64_t line_len = last_in_line - start_line + 1;

        if (nl || !pair)
            file_write (txt_file, &txt[start_line], line_len);

        else {
            int64_t after_qname = last_in_line;
            for (int64_t i=start_line; i <= last_in_line; i++)
                if (txt[i] == ' ' || txt[i] == '\t') {
                    after_qname = i;
                    break;
                }

            int qname_len = after_qname - start_line;
            file_write (txt_file, &txt[start_line], qname_len);

            // write suffix if requested, and suffix is not already present
            if (pair && (qname_len < 3 || txt[after_qname-2] != '/' || txt[after_qname-1] != '0' + pair))
                file_write (txt_file, suffixes[pair], 2);

            file_write (txt_file, &txt[after_qname], line_len - qname_len);
        }
        
        txt_file->txt_data_so_far_single += line_len;
        txt_file->disk_so_far            += line_len;
        start_line                       += line_len;
    }

    #undef start_line
}

// ZIP only - estimate the size of the txt data in this file. affects the hash table size and the progress indicator.
int64_t txtfile_estimate_txt_data_size (VBlock *vb)
{
    uint64_t disk_size = txt_file->disk_size; 

    // case: we don't know the disk file size (because its stdin or a URL where the server doesn't provide the size)
    if (!disk_size) { 
        if (flag.stdin_size) disk_size = flag.stdin_size; // use the user-provided size, if there is one
        else return 0; // we're unable to estimate if the disk size is not known
    } 
    
    double ratio=1;

    bool is_no_ht_vcf = (txt_file->data_type == DT_VCF && vcf_vb_has_haplotype_data(vb));

    switch (txt_file->codec) {
        // if we decomprssed gz/bz2 data directly - we extrapolate from the observed compression ratio
        case CODEC_GZ:
        case CODEC_BGZF:
        case CODEC_BZ2:  
            if (vb1_txt_data_comp_len) {
                ratio = (double)vb->vb_data_size / (double)vb1_txt_data_comp_len; 
                vb1_txt_data_comp_len = 0;
                break;
            }
            // case: small file, and all data in first VB was passed up from the txt_header 
            else 
                return txt_file->txt_data_so_far_single;

        // for compressed files for which we don't have their size (eg streaming from an http server) - we use
        // estimates based on a benchmark compression ratio of files with and without genotype data

        // note: .bcf files might be compressed or uncompressed - we have no way of knowing as 
        // "bcftools view" always serves them to us in plain VCF format. These ratios are assuming
        // the bcf is compressed as it normally is.
        case CODEC_BCF:  ratio = is_no_ht_vcf ? 55 : 8.5; break;

        case CODEC_XZ:   ratio = is_no_ht_vcf ? 171 : 12.7; break;

        case CODEC_CRAM: ratio = 25; break;

        case CODEC_ZIP:  ratio = 3; break;

        case CODEC_NONE: ratio = 1; break;

        default: ABORT ("Error in txtfile_estimate_txt_data_size: unspecified txt_file->codec=%s (%u)", codec_name (txt_file->codec), txt_file->codec);
    }

     return disk_size * ratio;
}

// PIZ: called before reading each genozip file
void txtfile_header_initialize(void)
{
    is_first_txt = true;
    vcf_header_initialize(); // we don't yet know the data type, but we initialize the VCF stuff just in case, no harm.
}

// ZIP: reads txt header and writes its compressed form to the GENOZIP file
bool txtfile_header_to_genozip (uint32_t *txt_line_i)
{    
    Digest header_digest = DIGEST_NONE;
    digest_initialize(); 

    z_file->disk_at_beginning_of_this_txt_file = z_file->disk_so_far;

    if (DTPT(txt_header_required) == HDR_MUST || DTPT (txt_header_required) == HDR_OK)
        header_digest = txtfile_read_header (is_first_txt); // reads into evb->txt_data and evb->lines.len
    
    *txt_line_i += (uint32_t)evb->lines.len;

    // for VCF, we need to check if the samples are the same before approving binding (other data types can bind without restriction)
    // for SAM, we check that the contigs specified in the header are consistent with the reference given in --reference/--REFERENCE
    if (!(DT_FUNC_OPTIONAL (txt_file, inspect_txt_header, true)(&evb->txt_data))) { 
        // this is the second+ file in a bind list, but its samples are incompatible
        buf_free (&evb->txt_data);
        return false;
    }

    if (z_file && !flag.seg_only)       
        // we always write the txt_header section, even if we don't actually have a header, because the section
        // header contains the data about the file
        zfile_write_txt_header (&evb->txt_data, header_digest, is_first_txt); // we write all headers in bound mode too, to support --unbind

    // for stats: combined length of txt headers in this bound file, or only one file if not bound
    if (!flag.bind) total_bound_txt_headers_len=0;
    total_bound_txt_headers_len += evb->txt_data.len; 

    z_file->num_txt_components_so_far++; // when compressing

    buf_free (&evb->txt_data);
    
    is_first_txt = false;

    return true; // everything's good
}

// PIZ: reads the txt header from the genozip file and outputs it to the reconstructed txt file
void txtfile_genozip_to_txt_header (const SectionListEntry *sl, 
                                    Digest *digest) // NULL if we're just skipped this header (2nd+ header in bound file)
{
    bool show_headers_only = (flag.show_headers && exe_type == EXE_GENOCAT);

    digest_initialize();

    z_file->disk_at_beginning_of_this_txt_file = z_file->disk_so_far;

    zfile_read_section (z_file, evb, 0, &evb->z_data, "header_section", SEC_TXT_HEADER, sl);

    // handle the GENOZIP header of the txt header section
    SectionHeaderTxtHeader *header = (SectionHeaderTxtHeader *)evb->z_data.data;

    ASSERTE (!digest || BGEN32 (header->h.compressed_offset) == crypt_padded_len (sizeof(SectionHeaderTxtHeader)), 
             "invalid txt header's header size: header->h.compressed_offset=%u, expecting=%u", BGEN32 (header->h.compressed_offset), (unsigned)sizeof(SectionHeaderTxtHeader));

    // 1. in unbind mode - we open the output txt file of the component
    // 2. when reading a reference file - we create txt_file here (but don't actually open the physical file)
    if (flag.unbind || flag.reading_reference) {
        ASSERTE0 (!txt_file, "not expecting txt_file to be open already in unbind mode or when reading reference");
        
        const char *filename = txtfile_piz_get_filename (header->txt_filename, flag.unbind, false);
        txt_file = file_open (filename, WRITE, TXT_FILE, z_file->data_type);
        FREE (filename); // file_open copies the names
    }

    txt_file->txt_data_size_single = BGEN64 (header->txt_data_size); 
    txt_file->max_lines_per_vb     = BGEN32 (header->max_lines_per_vb);
    
    if (txt_file->codec == CODEC_BGZF)
        memcpy (txt_file->bgzf_signature, header->codec_info, 3);
    
    if (is_first_txt || flag.unbind) 
        z_file->num_lines = BGEN64 (header->num_lines);

    if (flag.unbind) *digest = header->digest_single; // override md5 from genozip header

    // case: we need to reconstruct (or not) the BGZF following the instructions from the z_file
    if (flag.bgzf == FLAG_BGZF_BY_ZFILE) {

        // load the source file isize if we have it and we are attempting to reconstruct an unmodifed file identical to the source
        bool loaded = false;
        if (!flag.data_modified &&   
            (z_file->num_components == 1 || flag.unbind))  // not concatenating multiple files
            loaded = bgzf_load_isizes (sl); // also sets txt_file->bgzf_flags

        // case: user wants to see this section header, despite not needing BGZF data
        else if (exe_type == EXE_GENOCAT && (flag.show_headers == SEC_BGZF+1 || flag.show_headers == -1)) {
            bgzf_load_isizes (sl); 
            buf_free (&txt_file->bgzf_isizes);
        }

        // case: we need to reconstruct back to BGZF, but we don't have a SEC_BGZF to guide us - we'll creating our own BGZF blocks
        if (!loaded && z_file->z_flags.bgzf)
            txt_file->bgzf_flags = (struct FlagsBgzf){ // case: we're creating our own BGZF blocks
                .has_eof_block = true, // add an EOF block at the end
                .library       = BGZF_LIBDEFLATE, // default - libdeflate level 6
                .level         = BGZF_COMP_LEVEL_DEFAULT 
            };

        header = (SectionHeaderTxtHeader *)evb->z_data.data; // re-assign after possible realloc of z_data in bgzf_load_isizes
    }

    // case: the user wants us to reconstruct (or not) the BGZF blocks in a particular way, this overrides the z_file instructions 
    else 
        txt_file->bgzf_flags = (struct FlagsBgzf){ // case: we're creating our own BGZF blocks
            .has_eof_block = true, // add an EOF block at the end
            .library       = BGZF_LIBDEFLATE, 
            .level         = flag.bgzf 
        };

    // sanity        
    ASSERTE (txt_file->bgzf_flags.level >= 0 && txt_file->bgzf_flags.level <= 12, "txt_file->bgzf_flags.level=%u out of range [0,12]", 
             txt_file->bgzf_flags.level);

    ASSERTE (txt_file->bgzf_flags.library >= 0 && txt_file->bgzf_flags.library < NUM_BGZF_LIBRARIES, "txt_file->bgzf_flags.library=%u out of range [0,%u]", 
             txt_file->bgzf_flags.level, NUM_BGZF_LIBRARIES-1);

    // now get the text of the txt header itself
    if (!show_headers_only)
        zfile_uncompress_section (evb, header, &evb->txt_data, "txt_data", 0, SEC_TXT_HEADER);
    
    // write txt header if it is needed:
    if ((is_first_txt || flag.unbind) &&  // this is the first component, or we are unbinding (all components get a header)
        (!flag.no_header || z_file->z_flags.txt_is_bin) && // user didn't specify --no-header (or ignore the request if this is a binary file, eg BAM)
        !flag.reading_reference &&        // nothing is written when reading a reference
        (!flag.genocat_info_only || z_file->data_type == DT_VCF)) {  // nothing is written when we are just showing info (but in VCF we need the header as we need to count the samples)

        if (evb->txt_data.len)
            DT_FUNC_OPTIONAL (z_file, inspect_txt_header, true)(&evb->txt_data); // ignore return value

        if (!flag.genocat_info_only) {
            // if we're translating from one data type to another (SAM->BAM, BAM->FASTQ, ME23->VCF etc) translate the txt header 
            // note: in a header-less SAM, after translating to BAM, we will have a header
            DtTranslation trans = dt_get_translation();
            if (trans.txtheader_translator && !show_headers_only) trans.txtheader_translator (&evb->txt_data); 

            if (!evb->txt_data.len) goto done; // still no header... nothing more for us to do!

            bool test_digest = !digest_is_zero (header->digest_header) && // in v8 without --md5, we had no digest
                            !flag.data_modified; // no point calculating digest if we know already the file will be different

            if (test_digest) digest_update (&txt_file->digest_ctx_bound, &evb->txt_data, "txt_header:digest_ctx_bound");

            // compress the txt header with BGZF if needed
            if (txt_file->codec == CODEC_BGZF) { 
                bgzf_calculate_blocks_one_vb (evb, evb->txt_data.len);
                bgzf_compress_vb (evb); // compress data (but not if we are re-creating SEC_BGZF blocks and header is too small to fit into the first block)
                bgzf_write_to_disk (evb); // write blocks to disk and/or move unconsumed data to the next vb
            } 
            else
                txtfile_write_to_disk (&evb->txt_data);

            if (test_digest && z_file->genozip_version >= 9) {  // backward compatability with v8: we don't test against v8 MD5 for the header, as we had a bug in v8 in which we included a junk MD5 if they user didn't --md5 or --test. any file integrity problem will be discovered though on the whole-file MD5 so no harm in skipping this.
                Digest reconstructed_header_digest = digest_do (evb->txt_data.data, evb->txt_data.len);
                
                ASSERTW (flag.data_modified || digest_is_equal (reconstructed_header_digest, header->digest_header),
                        "%s of reconstructed %s header (%s) differs from original file (%s)\n"
                        "Bad reconstructed header has been dumped to: %s\n", digest_name(),
                        dt_name (z_file->data_type), digest_display (reconstructed_header_digest).s, digest_display (header->digest_header).s,
                        txtfile_dump_vb (evb, z_name));
            }
        }
    }

done:
    buf_free (&evb->z_data);
    buf_free (&evb->txt_data);

    z_file->num_txt_components_so_far++;
    is_first_txt = false;
}

DataType txtfile_get_file_dt (const char *filename)
{
    FileType ft = file_get_stdin_type(); // check for --input option

    if (ft == UNKNOWN_FILE_TYPE) // no --input - get file type from filename
        ft = file_get_type (filename);

    return file_get_data_type (ft, true);
}

// get filename of output txt file in genounzip if user didn't specific it with --output
// case 1: outputing a single file - generate txt_filename based on the z_file's name
// case 2: unbinding a genozip into multiple txt files - generate txt_filename of a component file from the
//         component name in SEC_TXT_HEADER 
const char *txtfile_piz_get_filename (const char *orig_name,const char *prefix, bool is_orig_name_genozip)
{
    unsigned fn_len = strlen (orig_name);
    unsigned genozip_ext_len = is_orig_name_genozip ? strlen (GENOZIP_EXT) : 0;
    char *txt_filename = (char *)MALLOC(fn_len + 10);

    #define EXT2_MATCHES_TRANSLATE(from,to,ext)  \
        ((z_file->data_type==(from) && flag.out_dt==(to) && \
         fn_len >= genozip_ext_len+strlen(ext) && \
         !strcmp (&txt_filename[fn_len-genozip_ext_len-strlen(ext)], (ext))) ? (int)strlen(ext) : 0) 

    // length of extension to remove if translating, eg remove ".sam" if .sam.genozip->.bam */
    int old_ext_removed_len = EXT2_MATCHES_TRANSLATE (DT_SAM,  DT_BAM,   ".sam") +
                              EXT2_MATCHES_TRANSLATE (DT_SAM,  DT_SAM,   ".bam") +
                              EXT2_MATCHES_TRANSLATE (DT_SAM,  DT_FASTQ, ".sam") +
                              EXT2_MATCHES_TRANSLATE (DT_SAM,  DT_FASTQ, ".bam") +
                              EXT2_MATCHES_TRANSLATE (DT_VCF,  DT_BCF,   ".vcf") +
                              EXT2_MATCHES_TRANSLATE (DT_ME23, DT_VCF,   ".txt");

    sprintf ((char *)txt_filename, "%s%.*s%s%s", prefix,
                fn_len - genozip_ext_len - old_ext_removed_len, orig_name,
                old_ext_removed_len ? file_plain_ext_by_dt (flag.out_dt) : "", // add translated extension if needed
                (z_file->z_flags.bgzf && flag.out_dt != DT_BAM) ? ".gz" : ""); // add .gz if --bgzf (except in BAM where it is implicit)

    return txt_filename;
}