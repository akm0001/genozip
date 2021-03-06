// ------------------------------------------------------------------
//   strings.c
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "genozip.h"
#include "strings.h"
#include "flags.h"
#ifndef WIN32
#include <sys/ioctl.h>
#else
#include <windows.h>
#endif

char *str_tolower (const char *in, char *out /* out allocated by caller - can be the same as in */)
{
    char *startout = out;

    for (; *in; in++, out++) 
        *out = (*in >= 'A' && *in <= 'Z') ? *in + 32 : *in;
    
    return startout;
}

char *str_toupper (const char *in, char *out /* out allocated by caller - can be the same as in */)
{
    char *startout = out;

    for (; *in; in++, out++) 
        *out = (*in >= 'a' && *in <= 'z') ? *in - 32 : *in;
    
    return startout;
}

StrText char_to_printable (char c) 
{
    switch ((uint8_t)c) {
        case 32 ... 127 : return (StrText) { .s = {c, 0} }; // printable ASCII
        case '\t'       : 
        case '\n'       : 
        case '\r'       : return (StrText) { .s = {' ', 0} }; // whitespace
        default         : { // unprintable - output eg \xf 
            StrText p;
            sprintf (p.s, "\\x%x", (uint8_t)c);
            return p;
        }
    }
}

StrText str_size (uint64_t size)
{
    StrText s;

    if      (size > (1LL << 50)) sprintf (s.s, "%3.1lf PB", ((double)size) / (double)(1LL << 50));
    else if (size > (1LL << 40)) sprintf (s.s, "%3.1lf TB", ((double)size) / (double)(1LL << 40));
    else if (size > (1LL << 30)) sprintf (s.s, "%3.1lf GB", ((double)size) / (double)(1LL << 30));
    else if (size > (1LL << 20)) sprintf (s.s, "%3.1lf MB", ((double)size) / (double)(1LL << 20));
    else if (size > (1LL << 10)) sprintf (s.s, "%3.1lf KB", ((double)size) / (double)(1LL << 10));
    else if (size > 0          ) sprintf (s.s, "%3d B"    ,     (int)size)                       ;
    else                         sprintf (s.s, "-"                       )                       ;

    return s;
}

// returns length
unsigned str_int (int64_t n, char *str /* out */)
{
    unsigned len=0;

    if (n==0) {
        str[0] = '0';
        len=1;
    }

    else {
        bool is_negative = (n<0);
        if (is_negative) n = -n;

        char rev[50] = {}; // "initialize" to avoid compiler warning
        while (n) {
            rev[len++] = '0' + n % 10;
            n /= 10;
        }
        // now reverse it
        for (int i=0; i < len; i++) str[i + is_negative] = rev[len-i-1];

        if (is_negative) {
            str[0] = '-';
            len++;
        }
    }

    str[len] = '\0'; // string terminator
    return len;
}

StrText str_int_s (int64_t n)
{
    StrText s;
    str_int (n, s.s);
    return s;
}

// similar to strtoull, except it rejects numbers that are shorter than str_len, or that their reconstruction would be different
// the the original string
bool str_get_int (const char *str, unsigned str_len, 
                  int64_t *value) // out - modified only if str is an integer
{
    int64_t out = 0;

     // edge cases - "" ; "-" ; "030" ; "-0" ; "-030" - false, because if we store these as an integer, we can't reconstruct them
    if (!str_len || 
        (str_len == 1 && str[0] == '-') || 
        (str_len >= 2 && str[0] == '0') || 
        (str_len >= 2 && str[0] == '-' && str[1] == '0')) return false;

    unsigned negative = (str[0] == '-');

    for (unsigned i=negative; i < str_len; i++) {
        if (!IS_DIGIT(str[i])) return false;

        int64_t prev_out = out;
        out = (out * 10) + (str[i] - '0');

        if (out < prev_out) return false; // number overflowed beyond maximum int64_t
    }

    if (negative) out *= -1;

    if (value) *value = out; // update only if successful
    return true;
}

bool str_get_int_range (const char *str, unsigned str_len, int64_t min_val, int64_t max_val, int64_t *value)
{
    if (!str_get_int (str, str_len, value)) return false;
    
    return *value >= min_val && *value <= max_val;
}

StrText str_uint_commas (int64_t n)
{
    StrText s;

    unsigned len = 0, orig_len=0;

    if (n==0) {
        s.s[0] = '0';
        len = 1;
    }
    else {
        char rev[50] = {}; // "initialize" to avoid compiler warning
        while (n) {
            if (orig_len && orig_len % 3 == 0) rev[len++] = ',';    
            rev[len++] = '0' + n % 10;
            orig_len++;
            n /= 10;
        }
        // now reverse it
        for (int i=0; i < len; i++) s.s[i] = rev[len-i-1];
    }

    s.s[len] = '\0'; // string terminator
    return s;
}

// "3.123" -> "%5.3f" ;  returns length of format string
unsigned str_get_float_format (const char *float_str, unsigned float_str_len, char *str /* out */)
{
    unsigned decimal_digits=0;
    for (int i=(int)float_str_len-1; i >= 0; i--)
        if (float_str[i] == '.') {
            decimal_digits = (float_str_len-1) - i;
            break;
        }

    unsigned next=0;
    str[next++] = '%';
    next += str_int (float_str_len, &str[next]);    
    str[next++] = '.';
    next += str_int (decimal_digits, &str[next]);
    str[next++] = 'f';
    str[next] = 0;

    return next;
}

// returns float value, or -1 if not a positive float of at most 9 digits after the decimal point
double str_get_positive_float (const char *float_str, unsigned float_str_len)
{
    bool in_decimals=false;
    unsigned num_decimals=0;
    double val = 0;

    for (unsigned i=0; i < float_str_len; i++) {
        if (float_str[i] == '.' && !in_decimals)
            in_decimals = true;
    
        else if (IS_DIGIT (float_str[i])) {
            val = (val * 10) + (float_str[i] - '0');
            if (in_decimals) num_decimals++;
        }
    
        else return -1; // not a positive float
    }

    static const double pow10[10] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000 };
    if (num_decimals >= sizeof (pow10) / sizeof (pow10[0])) return -1; // too many decimals

    return val / pow10[num_decimals];
}

StrText str_pointer (const void *p)
{
    StrText s;
    sprintf (s.s, "0x%"PRIx64, (uint64_t)p);
    return s;
}

bool str_is_in_range (const char *str, uint32_t str_len, char first_c, char last_c)
{
    for (; str_len ; str++, str_len--)
        if (*str < first_c || *str > last_c) return false;
    return true;
}

const char *type_name (unsigned item, 
                       const char * const *name, // the address in which a pointer to name is found, if item is in range
                       unsigned num_names)
{
    if (item > num_names) {
        static char str[50];
        sprintf (str, "%d (out of range)", item);
        return str;
    }
    
    return *name;    
}

void str_print_null_seperated_data (const char *data, unsigned len, bool add_newline, bool remove_equal_asterisk)
{
    for (unsigned i=0; i < len; i++) {
        // in case we are showing chrom data in --list-chroms in SAM - don't show * and =
        if (remove_equal_asterisk && (data[i]=='*' || data[i]=='=') && !data[i+1]) {
            i++;
            continue; // skip character and following separator
        }

        switch (data[i]) {
            case 32 ... 127 : fputc (data[i], info_stream);      break;
            case 0          : fputc (add_newline ? '\n' : ' ', info_stream); break; // snip separator
            case '\t'       : fwrite ("\\t", 1, 2, info_stream); break;
            case '\n'       : fwrite ("\\n", 1, 2, info_stream); break;
            case '\r'       : fwrite ("\\r", 1, 2, info_stream); break;
            default         : iprintf ("\\x%x", data[i]);
        }
        }
}

int str_print_text (const char **text, unsigned num_lines,
                    const char *wrapped_line_prefix, 
                    const char *newline_separator, 
                    unsigned line_width /* 0=calcuate optimal */)
{                       
    ASSERTE0 (text, "text is NULL");
    
    if (!line_width) {
#ifdef _WIN32
        line_width = 120; // default width of cmd window in Windows 10
#else
        // in Linux and Mac, we can get the actual terminal width
        struct winsize w;
        ioctl(0, TIOCGWINSZ, &w);
        line_width = MAX (40, w.ws_col); // our wrapper cannot work with to small line widths 
#endif
    }

    for (unsigned i=0; i < num_lines; i++)  {
        const char *line = text[i];
        unsigned line_len = strlen (line);
        
        // print line with line wraps
        bool wrapped = false;
        while (line_len + (wrapped ? strlen (wrapped_line_prefix) : 0) > line_width) {
            int c; for (c=line_width-1 - (wrapped ? strlen (wrapped_line_prefix) : 0); 
                        c>=0 && (IS_LETTER(line[c]) || IS_DIGIT (line[c])); // wrap lines at - and | too, so we can break very long regex strings like in genocat
                        c--); // find 
            printf ("%s%.*s\n", wrapped ? wrapped_line_prefix : "", c, line);
            line += c + (line[c]==' '); // skip space too
            line_len -= c + (line[c]==' ');
            wrapped = true;
        }
        printf ("%s%s%s", wrapped ? wrapped_line_prefix : "", line, newline_separator);
    }
    return 0;
}

// receives a user response, a default "Y" or "N" (or NULL) and modifies the response to be "Y" or "N"
bool str_verify_y_n (char *response, unsigned response_size, const char *y_or_n)
{
    ASSERTE0 (!y_or_n || (strlen (y_or_n)==1 && (y_or_n[0]=='Y' || y_or_n[0]=='N')), 
              "y_or_n needs to be NULL, \"Y\" or \"N\"");

    // default is N (or no default) and first character of the user's response is y or Y
    if ((!y_or_n || y_or_n[0] == 'N') && (response[0] == 'y' || response[0] == 'Y')) response[0] = 'Y'; 
    
    // default is Y (or no default) and first character of the user's response is n or N
    else if ((!y_or_n || y_or_n[0] == 'Y') && (response[0] == 'n' || response[0] == 'N')) response[0] = 'N';

    // we have a default, and the response of user is not opposite of the default - return default
    else if (y_or_n) response[0] = y_or_n[0]; 

    // we don't have a default - we request the user to respond again
    else return false;

    response[1] = 0;

    return true; // always accept the response
}

bool str_verify_not_empty (char *response, unsigned response_size, const char *unused)
{ 
    unsigned len = strlen (response);

    return !(len==0 || (len==1 && response[0]=='\r')); // not \n or \r\n only
}

void str_query_user (const char *query, char *response, unsigned response_size, 
                     ResponseVerifier verifier, const char *verifier_param)
{
    do {
        fprintf (stderr, "%s", query);

        unsigned bytes = read (STDIN_FILENO, response, response_size); 
        response[bytes-1] = '\0'; // string terminator instead of the newline

    } while (verifier && !verifier (response, response_size, verifier_param));
}

const char *str_win_error (void)
{
    static char msg[100];
#ifdef _WIN32
    FormatMessageA (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,                   
                    NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), msg, sizeof (msg), NULL);
#endif
    return msg;
}
