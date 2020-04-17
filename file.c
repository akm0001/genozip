// ------------------------------------------------------------------
//   file.c
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#define Z_LARGE64
#ifdef __APPLE__
    #define off64_t __int64_t
#endif
#include <bzlib.h>
#include "zlib/zlib.h"
#include "genozip.h"
#include "move_to_front.h"
#include "file.h"
#include "stream.h"
#include "url.h"
#include "compressor.h"

// globals
File *z_file   = NULL;
File *txt_file = NULL;

static StreamP input_decompressor  = NULL; // bcftools or xz or samtools - only one at a time
static StreamP output_compressor   = NULL; // bgzip, samtools, bcftools

static FileType stdin_type = UNKNOWN_FILE_TYPE; // set by the --stdin command line option

// global pointers - so the can be compared eg "if (mode == READ)"
const char *READ  = "rb";  // use binary mode (b) in read and write so Windows doesn't add \r
const char *WRITE = "wb";

char *file_exts[] = FILE_EXTS;

void file_set_stdin_type (const char *type_str)
{
    if (type_str)
        for (stdin_type=UNKNOWN_FILE_TYPE+1; stdin_type < AFTER_LAST_FILE_TYPE; stdin_type++)
            if (!strcmp (type_str, &file_exts[stdin_type][1])) break; // compare to file extension without the leading .

    ASSERT (stdin_type==SAM || stdin_type==VCF, "%s: --stdin/-i option can only accept 'vcf' or 'sam'", global_cmd);
}

FileType file_get_stdin_type (void)
{
    return stdin_type;
}

static FileType file_get_type (const char *filename)
{
    for (FileType ft=UNKNOWN_FILE_TYPE+1; ft < AFTER_LAST_FILE_TYPE; ft++)
        if (file_has_ext (filename, file_exts[ft])) return ft;

    return UNKNOWN_FILE_TYPE;
}

static void file_ask_user_to_confirm_overwrite (const char *filename)
{
    fprintf (stderr, "%s: output file %s already exists: in the future, you may use --force to overwrite\n", global_cmd, filename);
    
    if (!isatty(0) || !isatty(2)) exit_on_error(); // if we stdin or stderr is redirected - we cannot ask the user an interactive question
    
    fprintf (stderr, "Do you wish to overwrite it now? (y or [n]) ");

    // read all chars available on stdin, so that if we're processing multiple files - and we ask this question
    // for a subsequent file later - we don't get left overs of this response
    char read_buf[1000];
    read_buf[0] = 0;
    read (STDIN_FILENO, read_buf, 1000);

    if (read_buf[0] != 'y' && read_buf[0] != 'Y') {
        fprintf (stderr, "No worries, I'm stopping here - no damage done!\n");
        exit(0);
    }
}

static void file_redirect_output_to_stream (File *file, char *exec_name, char *stdout_option, char *format_option)
{
    char threads_str[20];
    sprintf (threads_str, "%u", global_max_threads);

    FILE *redirected_stdout_file = NULL;
    if (!flag_stdout) {
        redirected_stdout_file = fopen (file->name, file->mode); // exec_name will redirect its output to this file
        ASSERT (redirected_stdout_file, "%s: cannot open file %s: %s", global_cmd, file->name, strerror(errno));
    }
    char reason[100];
    sprintf (reason, "To output a %s file", file_exts[file->type]);
    output_compressor = stream_create (0, 0, 0, global_max_memory_per_vb, 
                                        redirected_stdout_file, // output is redirected unless flag_stdout
                                        0, reason,
                                        exec_name, 
                                        stdout_option, // either to the terminal or redirected to output file
                                        "--threads", threads_str,
                                        format_option,
                                        NULL);
    file->file = stream_to_stream_stdin (output_compressor);
}

// returns true if successful
bool file_open_txt (File *file)
{
    // for READ, set data_type
    if (file->mode == READ) {
        if      (file_is_vcf (file)) file->data_type = DATA_TYPE_VCF;
        else if (file_is_sam (file)) file->data_type = DATA_TYPE_SAM;
        else ABORT ("%s: input file must have one of the following extensions: " COMPRESSIBLE_EXTS, global_cmd);
    }
    else { // WRITE - data_type is already set by file_open
        if (file->data_type == DATA_TYPE_VCF) {
            
            ASSERT (file->type == VCF || file->type == VCF_GZ || file->type == VCF_BGZ || file->type == BCF, 
                    "%s: output file must have a .vcf or .bcf or .vcf.gz or .vcf.bgz extension", global_cmd); 
        }
        else if (file->data_type == DATA_TYPE_SAM) {
            ASSERT (file->type == SAM || file->type == BAM, 
                    "%s: output file must have a .sam or .bam", global_cmd); 
        }
        // if we don't know our type based on our own name, consult z_file (this happens when opening 
        // from piz_dispacher)
        else if (file->data_type == DATA_TYPE_NONE && z_file && z_file->data_type != DATA_TYPE_NONE) {
            file->data_type = z_file->data_type;

            #define ENFORCE_TYPE_FLAG(flag,dt) ASSERT (!flag_##flag, "%s: the --" #flag " flag cannot be used with files containing %s data like %s", global_cmd, dt, z_name);
            
            switch (file->data_type) {
                case DATA_TYPE_VCF : 
                    ENFORCE_TYPE_FLAG (bam, "VCF");
                    file->type = (flag_bgzip ? VCF_GZ : (flag_bcf ? BCF : VCF)); 
                    break;
                
                case DATA_TYPE_SAM : 
                    ENFORCE_TYPE_FLAG (bcf, "SAM");
                    ENFORCE_TYPE_FLAG (bgzip, "SAM");
                    file->type = (flag_bam ? BAM : SAM); 
                    break;
                
                default: ABORT ("Error in file_open_txt: unknown data_type=%u", file->data_type);
            }
        }
    }

    switch (file->type) {
    case VCF:
    case SAM:
        // don't actually open the file if we're just testing in genounzip
        if (flag_test && file->mode == WRITE) return true;

        file->file = file->is_remote ? url_open (NULL, file->name) : fopen (file->name, file->mode);
        break;

    case VCF_GZ:
    case VCF_BGZ:
        if (file->mode == READ) {
            if (file->is_remote) { 
                FILE *url_fp = url_open (NULL, file->name);
                file->file = gzdopen (fileno(url_fp), file->mode); // we're abandoning the FILE structure (and leaking it, if libc implementation dynamically allocates it) and working only with the fd
            }
            else
                file->file = gzopen64 (file->name, file->mode); // for local files we decompress ourselves
        }
        else 
            file_redirect_output_to_stream (file, "bgzip", "--stdout", NULL);
        break;

    case VCF_BZ2:
        if (file->is_remote) {            
            FILE *url_fp = url_open (NULL, file->name);
            file->file = BZ2_bzdopen (fileno(url_fp), file->mode); // we're abandoning the FILE structure (and leaking it, if libc implementation dynamically allocates it) and working only with the fd
        }
        else
            file->file = BZ2_bzopen (file->name, file->mode);  // for local files we decompress ourselves   
        break;

    case VCF_XZ:
        input_decompressor = stream_create (0, global_max_memory_per_vb, DEFAULT_PIPE_SIZE, 0, 0, 
                                            file->is_remote ? file->name : NULL,     // url
                                            "To compress an .xz file", "xz",         // reason, exec_name
                                            file->is_remote ? SKIP_ARG : file->name, // local file name 
                                            "--threads=8", "--decompress", "--keep", "--stdout", 
                                            flag_quiet ? "--quiet" : SKIP_ARG, 
                                            NULL);            
        file->file = stream_from_stream_stdout (input_decompressor);
        break;

    case BAM:
    case BCF:
    case BCF_GZ:
    case BCF_BGZ: {
        if (file->mode == READ) {
            char reason[100];
            sprintf (reason, "To compress a %s file", file_exts[file->type]);
            input_decompressor = stream_create (0, global_max_memory_per_vb, DEFAULT_PIPE_SIZE, 0, 0, 
                                                file->is_remote ? file->name : NULL,         // url                                        
                                                reason, 
                                                file->type == BAM ? "samtools" : "bcftools", // exec_name
                                                "view", 
                                                "--threads", "8", 
                                                file->type == BAM ? "-OSAM" : "-Ov",
                                                file->is_remote ? SKIP_ARG : file->name,    // local file name 
                                                file->type == BAM ? "-h" : NULL, // include header in SAM output
                                                NULL);
            file->file = stream_from_stream_stdout (input_decompressor);
        }
        else { // write
            file_redirect_output_to_stream (file, 
                                            file->type==BAM ? "samtools" : "bcftools", 
                                            "view", 
                                            file->type==BAM ? "-OBAM" : "-Ob");            

        }
        break;
    }

    default:
        if (file->mode == WRITE && file->data_type == DATA_TYPE_NONE) {
            // user is trying to genounzip a .genozip file that is not a recognized extension -
            // that's ok - we will discover its type after reading the genozip header in zip_dispatcher
            return true; // let's call this a success anyway
        }
        else {
            ABORT ("%s: unrecognized file type: %s", global_cmd, file->name);
        }
    }

    if (file->mode == READ) 
        file->txt_data_size_single = file->disk_size; 

    return file->file != 0;
}

// returns true if successful
static bool file_open_z (File *file)
{
    ASSERT (file_has_ext (file->name, GENOZIP_EXT), "%s: file %s must have a " GENOZIP_EXT " extension", 
            global_cmd, file->name);

    // for READ, set data_type
    if (file->mode == READ) {
        switch (file->type) {
            case VCF_GENOZIP : file->data_type = DATA_TYPE_VCF; break;
            case SAM_GENOZIP : file->data_type = DATA_TYPE_SAM; break; 
            default          : file->data_type = DATA_TYPE_NONE; // to be determined after the genozip header is read
        }
    }
    else { // WRITE - data_type is already set by file_open
        // set file->type according to the data type, overriding the previous setting - i.e. if the user
        // uses the --output option, he is unrestricted in the choice of a file name
        switch (file->data_type) {
            case DATA_TYPE_VCF : file->type = VCF_GENOZIP; break;
            case DATA_TYPE_SAM : file->type = SAM_GENOZIP; break; 
            default: ABORT ("Error in file_open_z: unknown data type %u", file->data_type);
        }   
    }

    file->file = file->is_remote ? url_open (NULL, file->name) 
                                 : fopen (file->name, file->mode);

    // initialize read buffer indices
    if (file->mode == READ) 
        file->z_last_read = file->z_next_read = READ_BUFFER_SIZE;

    return file->file != 0;
}

File *file_open (const char *filename, FileMode mode, FileSupertype supertype, DataType data_type /* only needed for WRITE */)
{
    ASSERT0 (filename, "Error: filename is null");

    File *file = (File *)calloc (1, sizeof(File) + ((mode == READ && supertype == Z_FILE) ? READ_BUFFER_SIZE : 0));

    file->supertype = supertype;
    file->is_remote = url_is_url (filename);
    bool file_exists;

    // is_remote is only possible in READ mode
    ASSERT (mode != WRITE || !file->is_remote, "%s: expecting output file %s to be local, not a URL", global_cmd, filename);

    int64_t url_file_size = 0; // will be -1 if the web/ftp site does not provide the file size
    const char *error = NULL;
    if (file->is_remote) {
        error = url_get_status (filename, &file_exists, &url_file_size); // accessing is expensive - get existance and size in one call
        if (url_file_size >= 0) file->disk_size = (uint64_t)url_file_size;
    }
    else {
        file_exists = (access (filename, F_OK) == 0);
        error = strerror (errno);
        if (file_exists && mode == READ) file->disk_size = file_get_size (filename);
    }

    // return null if genozip input file size is known to be 0, so we can skip it. note: file size of url might be unknown
    if (mode == READ && supertype == TXT_FILE && file_exists && !file->disk_size && !url_file_size) {
        FREE (file);
        return NULL; 
    }

    ASSERT (mode != READ  || file_exists, "%s: cannot open %s for reading: %s", global_cmd, filename, error);

    if (mode == WRITE && file_exists && !flag_force && !(supertype==TXT_FILE && flag_test))
        file_ask_user_to_confirm_overwrite (filename); // function doesn't return if user responds "no"

    // copy filename 
    unsigned fn_size = strlen (filename) + 1; // inc. \0
    file->name = malloc (fn_size);
    memcpy (file->name, filename, fn_size);

    file->mode = mode;

    if (mode==READ || data_type != DATA_TYPE_NONE) // if its NONE, we will not open now, and try again from piz_dispatch after reading the genozip header
        file->type = file_get_type (file->name);

    if (file->mode == WRITE) 
        file->data_type = data_type; // for READ, data_type is set by file_open_*

    bool success=false;
    switch (supertype) {
        case TXT_FILE: success = file_open_txt (file); break;
        case Z_FILE:   success = file_open_z (file);   break;
        default:       ABORT ("Error: invalid supertype: %u", supertype);
    }

    ASSERT (success, "%s: cannot open file %s: %s", global_cmd, file->name, strerror(errno)); // errno will be retrieve even the open() was called through zlib and bzlib 

    return file;
}

File *file_open_redirect (FileMode mode, FileSupertype supertype, DataType data_type /* only used for WRITE */)
{
    ASSERT (mode==WRITE || stdin_type != UNKNOWN_FILE_TYPE, 
            "%s: to redirect from standard input use the --stdin option eg '--stdin vcf'. See '%s --help' for more details", global_cmd, global_cmd);

    File *file = (File *)calloc (1, sizeof(File) + ((mode == READ && supertype == Z_FILE) ? READ_BUFFER_SIZE : 0));

    file->file = (mode == READ) ? fdopen (STDIN_FILENO,  "rb")
                                : fdopen (STDOUT_FILENO, "wb");
    ASSERT (file->file, "%s: Failed to redirect %s: %s", global_cmd, (mode==READ ? "stdin" : "stdout"), strerror (errno));

    file->supertype = supertype;
    
    file->data_type = (mode==READ) ? (stdin_type == VCF ? DATA_TYPE_VCF : DATA_TYPE_SAM)
                                   : data_type;

    file->type = (mode==READ) ? stdin_type 
                              : (data_type == DATA_TYPE_VCF ? VCF : SAM);
    file->redirected = true;
    
    return file;
}

void file_close (File **file_p, 
                 bool cleanup_memory) // optional - used to destroy buffers in the file is closed NOT near the end of the execution, eg when dealing with splitting concatenated files
{
    File *file = *file_p;
    *file_p = NULL;

    if (file->file) {

        if (file->mode == READ && (file->type == VCF_GZ || file->type == VCF_BGZ)) {
            int ret = gzclose_r((gzFile)file->file);
            ASSERTW (!ret, "Warning: failed to close vcf.gz file: %s", file_printname (file));
        }
        else if (file->mode == READ && file->type == VCF_BZ2) 
            BZ2_bzclose((BZFILE *)file->file);
        
        else if (file->mode == READ && 
                 (file->type == BCF || file->type == BCF_GZ || file->type == BCF_BGZ || file->type == VCF_XZ)) 
            stream_close (&input_decompressor, STREAM_WAIT_FOR_PROCESS);

        else if (file->mode == WRITE && (file->type == VCF_GZ || file->type == VCF_BGZ)) 
            stream_close (&output_compressor, STREAM_WAIT_FOR_PROCESS);

        else 
            FCLOSE (file->file, file_printname (file));
    }

    // free resources if we are NOT near the end of the execution. If we are at the end of the execution
    // it is faster to just let the process die
    if (cleanup_memory) {
            
        if (file->supertype == Z_FILE && !file->redirected) { // reading or writing a .genozip. redirected files are by definition a single file - so cleaned up when process exits
            for (unsigned i=0; i < file->num_dict_ids; i++)
                mtf_destroy_context (&file->mtf_ctx[i]);

            if (file->dicts_mutex_initialized) 
                pthread_mutex_destroy (&file->dicts_mutex);

            buf_destroy (&file->dict_data);
            buf_destroy (&file->ra_buf);
            buf_destroy (&file->section_list_buf);
            buf_destroy (&file->section_list_dict_buf);
            buf_destroy (&file->v1_next_vcf_header);
        }

        if (file_is_zip_read(file))
            buf_destroy (&file->unconsumed_txt);

        if (file->name) FREE (file->name);
        
        FREE (file);
    }
}

size_t file_write (File *file, const void *data, unsigned len)
{
    size_t bytes_written = fwrite (data, 1, len, (FILE *)file->file); // use fwrite - let libc manage write buffers for us

    // if we're streaming our genounzip/genocat/genols output to another process and that process has 
    // ended prematurely then exit quietly. in genozip we display an error because this means the resulting
    // .genozip file will be corrupted
    if (!file->name && command != ZIP && errno == EINVAL) exit(0);

    ASSERT (bytes_written, "Error: failed to write %u bytes to %s: %s", 
            len, file->name ? file->name : "(stdout)", strerror(errno));
    return bytes_written;
}

void file_remove (const char *filename, bool fail_quietly)
{
    int ret = remove (filename); 
    ASSERTW (!ret || fail_quietly, "Warning: failed to remove %s: %s", filename, strerror (errno));
}

bool file_has_ext (const char *filename, const char *extension)
{
    if (!filename) return false;

    unsigned ext_len = strlen (extension);
    unsigned fn_len  = strlen (filename);
    
    return fn_len > ext_len && !strncmp (&filename[fn_len-ext_len], extension, ext_len);
}

// get basename of a filename - we write our own basename for Visual C and Windows compatibility
const char *file_basename (const char *filename, bool remove_exe, const char *default_basename,
                           char *basename /* optional pre-allocated memory */, unsigned basename_size /* basename bytes */)
{
    if (!filename) filename = default_basename;

    unsigned len = strlen (filename);
    if (remove_exe && file_has_ext (filename, ".exe")) len -= 4; // for Windows

    // get start of basename
    const char *start = filename;
    for (int i=len-1; i >= 0; i--)
        if (filename[i]=='/' || filename[i]=='\\') {
            start = &filename[i+1];
            break;
        }

    len = len - (start-filename);

    if (!basename) 
        basename = (char *)malloc (len + 1); // +1 for \0
    else
        len = MIN (len, basename_size-1);

    sprintf (basename, "%.*s", (int)len, start);

    return basename;
}

// returns true if successful. depending on soft_fail, a failure will either emit an error 
// (and exit) or a warning (and return).
bool file_seek (File *file, int64_t offset, 
                int whence, 
                bool soft_fail)
{
    ASSERT0 (file == z_file, "Error: file_seek only works for z_file");

    // check if we can just move the read buffers rather than seeking
    if (file->mode == READ && file->z_next_read != file->z_last_read && whence == SEEK_SET) {
#ifdef __APPLE__
        int64_t move_by = offset - ftello ((FILE *)file->file);
#else
        int64_t move_by = offset - ftello64 ((FILE *)file->file);
#endif

        // case: move is within read buffer already in memory (ftello shows the disk location after read of the last buffer)
        // we just change the buffer pointers rather than discarding the buffer and re-reading
        if (move_by <= 0 && move_by >= -(int64_t)file->z_last_read) {
            file->z_next_read = file->z_last_read + move_by; // only z_file uses next/last_Read
            return true;
        }
    }

#ifdef __APPLE__
    int ret = fseeko ((FILE *)file->file, offset, whence);
#else
    int ret = fseeko64 ((FILE *)file->file, offset, whence);
#endif

    if (soft_fail) {
        if (!flag_stdout) {
            ASSERTW (!ret, errno == EINVAL ? "Error while reading file %s: it is too small%s" 
                                           : "Warning: fseeko failed on file %s: %s", 
                    file_printname (file),  errno == EINVAL ? "" : strerror (errno));
        }
    } 
    else {
        ASSERT (!ret, "Error: fseeko failed on file %s: %s", file_printname (file), strerror (errno));
    }

    // reset the read buffers
    if (!ret) file->z_next_read = file->z_last_read = READ_BUFFER_SIZE;

    return !ret;
}

uint64_t file_tell (File *file)
{
    if (command == ZIP && (file->type == VCF_GZ || file->type == VCF_BGZ))
        return gzconsumed64 ((gzFile)txt_file->file); 
    
    if (command == ZIP && file->type == VCF_BZ2)
        return BZ2_consumed ((BZFILE *)txt_file->file); 

#ifdef __APPLE__
    return ftello ((FILE *)file->file);
#else
    return ftello64 ((FILE *)file->file);
#endif
}

uint64_t file_get_size (const char *filename)
{
    struct stat64 st;
    
    int ret = stat64(filename, &st);
    ASSERT (!ret, "Error: failed accessing %s: %s", filename, strerror(errno));
    
    return st.st_size;
}

bool file_is_dir (const char *filename)
{
    struct stat64 st;
    
    int ret = stat64(filename, &st);
    ASSERT (!ret, "Error: failed accessing %s: %s", filename, strerror(errno));
    
    return S_ISDIR (st.st_mode);
}

void file_get_file (VBlockP vb, const char *filename, Buffer *buf, const char *buf_name, unsigned buf_param,
                    bool add_string_terminator)
{
    uint64_t size = file_get_size (filename);

    buf_alloc (vb, buf, size + add_string_terminator, 1, buf_name, buf_param);

    FILE *file = fopen (filename, "rb");
    ASSERT (file, "Error: cannot open %s: %s", filename, strerror (errno));

    size_t bytes_read = fread (buf->data, 1, size, file);
    ASSERT (bytes_read == (size_t)size, "Error reading file %s: %s", filename, strerror (errno));

    buf->len = size;

    if (add_string_terminator) buf->data[size] = 0;

    FCLOSE (file, filename);
}

void file_assert_ext_decompressor (void)
{
    if (!stream_wait_for_exit (input_decompressor)) return; // just a normal EOF - all good!

    // read error from stderr
    #define INPUT_DECOMPRESSOR_RESPSONSE_LEN 4096
    char error_str[INPUT_DECOMPRESSOR_RESPSONSE_LEN];

    FILE *stderr_pipe = stream_from_stream_stderr (input_decompressor);
    int bytes_read = fread (error_str, 1, INPUT_DECOMPRESSOR_RESPSONSE_LEN-1, stderr_pipe);
    error_str[bytes_read] = 0; // string terminator

    ABORT ("%s: failed to read file: %s", global_cmd, error_str);
}

// used when aborting due to an error. avoid the compressors outputting their own errors after our process is gone
void file_kill_external_compressors (void)
{
    stream_close (&input_decompressor, STREAM_KILL_PROCESS);
    stream_close (&output_compressor,  STREAM_KILL_PROCESS);
}
