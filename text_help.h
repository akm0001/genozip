// ------------------------------------------------------------------
//   help-text.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "genozip.h"

static const char *help_genozip[] = {
    "",
    "Compress VCF (Variant Call Format) files",
    "",
    "Usage: genozip [options]... [files]...",
    "",
    "See also: genounzip genocat genols",
    "",
    "Actions - use at most one of these actions:",
    "   -z --compress     This is the default action. Compress a .vcf, .vcf.gz or .vcf.bz2 file (Yes! You can compress an already-compressed file). The source file is left unchanged. This is the default action of genozip",
    "   -d --decompress   Same as running genounzip. For more details, run: genounzip --help",
    "   -l --list         Same as running genols. For more details, run: genols --help",
    "   -t --test         Test genozip. Compress the .vcf file(s), uncompress, and then compare the result to the original .vcf - all in memory without writing to any file",
    "   -h --help         Show this help page",
    "   -L --license      Show the license terms and conditions for this product",
    "   -V --version      Display version number",
    "",    
    "Flags:",    
    "   -c --stdout       Send output to standard output instead of a file",    
    "   -f --force        Force overwrite of the output file, or force writing .vcf" GENOZIP_EXT " data to standard output",    
    "   -^ --replace      Replace the source file with the result file, rather than leaving it unchanged",    
    "   -o --output       Output file name. This option can also be used to concatenate multiple input files with the same individuals, into a single concatenated output file",
    "   -p --password     Password-protected - encrypted with 256-bit AES",
    "   -m --md5          Records the MD5 hash of the VCF file in the genozip file header. When the resulting file is decompressed, this MD5 will be compared to the MD5 of the decompressed VCF",
    "   -q --quiet        Don't show the progress indicator",    
    "   -@ --threads      Specify the maximum number of threads. By default, this is set to the number of cores available. The number of threads actually used may be less, if sufficient to balance CPU and I/O",
    "   --show-content    Show the information content of VCF files and the compression ratios of each component",
    "   --show-alleles    Output allele values to stdout. Each row corresponds to a row in the VCF file. Mixed-ploidy regions are padded, and 2-digit allele values are replaced by an ascii character",
    "   --show-time       Show what functions are consuming the most time (useful mostly for developers of genozip)",
    "   --show-memory     Show what buffers are consuming the most memory (useful mostly for developers of genozip)",
    "",
    "One or more file names may be given, or if omitted, standard input is used instead",
    "",
    "Genozip is available for free for non-commercial use. Commercial use requires a commercial license.",
};

static const char *help_genounzip[] = {
    "",
    "Uncompress VCF (Variant Call Format) files previously compressed with genozip",
    "",
    "Usage: genounzip [options]... [files]...",
    "",
    "See also: genozip genocat genols",
    "",
    "Options:",
    "   -c --stdout       Send output to standard output instead of a file",
    "   -f --force        Force overwrite of the output file",
    "   -^ --replace      Replace the source file with the result file, rather than leaving it unchanged",    
    "   -o --output       Output file name",
    "   -p --password     Provide password to access file(s) that were compressed with --password",
    "   -q --quiet        Don't show the progress indicator",    
    "   -@ --threads      Specify the maximum number of threads. By default, this is set to the number of cores available. The number of threads actually used may be less, if sufficient to balance CPU and I/O",
    "   --show-time       Show what functions are consuming the most time (useful mostly for developers of genozip)",
    "   --show-memory     Show what buffers are consuming the most memory (useful mostly for developers of genozip)",
    "   -h --help         Show this help page",
    "   -L --license      Show the license terms and conditions for this product",
    "   -V --version      Display version number",
    "",
    "One or more file names may be given, or if omitted, standard input is used instead",
};

static const char *help_genols[] = {
    "",
    "View metadata of VCF (Variant Call Format) files previously compressed with genozip",
    "",
    "Usage: genols [options]... [files or directories]...",
    "",
    "See also: genozip genounzip genocat",
    "",
    "Options:",
    "   -p --password     Provide password to access file(s) that were compressed with --password",
    "   -m --md5          Shows the MD5 of files that were compressed with --md5",
    "   -h --help         Show this help page",
    "   -L --license      Show the license terms and conditions for this product",
    "   -V --version      Display version number",
    "",
    "One or more file or directory names may be given, or if omitted, genols runs on the current directory",
};

static const char *help_genocat[] = {
    "",
    "Print VCF (Variant Call Format) file(s) previously compressed with genozip",
    "",
    "Usage: genocat [options]... [files]...",
    "",
    "See also: genozip genounzip genols",
    "",
    "Actions - use at most one of these actions:",
    "",    
    "Flags:",    
    "   -p --password     Provide password to access file(s) that were compressed with --password",
    "   -@ --threads      Specify the maximum number of threads. By default, this is set to the number of cores available. The number of threads actually used may be less, if sufficient to balance CPU and I/O",
    "   -h --help         Show this help page",
    "   -L --license      Show the license terms and conditions for this product",
    "   -V --version      Display version number",
    "",
    "One or more file names may be given, or if omitted, standard input is used instead",
};

static const char *help_footer[] = {
    "",
    "For bug reports: bugs@genozip.com and license inquiries: sales@genozip.com",
    "",
    "THIS SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.",
    ""
};
