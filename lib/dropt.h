/** dropt.h -- single-header build
  *
  * A deliberately rudimentary command-line option parser.
  *
  * Version 2.0.0
  *
  * Copyright (C) 2006-2018 James D. Lin <jamesdlin@berkeley.edu>
  *
  * The latest version of the original (multi-file) library can be downloaded
  * from: <http://www.taenarum.com/software/dropt/>
  *
  * ALTERED SOURCE: this is a modified version of dropt. The original
  * dropt.h, dropt.c, dropt_string.h, dropt_string.c and dropt_handlers.c have
  * been merged into this single header, stb-style. No functional changes were
  * made to the library logic. To use it:
  *
  *     // in exactly ONE translation unit:
  *     #define DROPT_IMPLEMENTATION
  *     #include "dropt.h"
  *
  *     // everywhere else, just:
  *     #include "dropt.h"
  *
  * Optionally `#define DROPTDEF static inline` before the implementation
  * include to make the whole library header-only / file-local.
  *
  * This software is provided 'as-is', without any express or implied
  * warranty.  In no event will the authors be held liable for any damages
  * arising from the use of this software.
  *
  * Permission is granted to anyone to use this software for any purpose,
  * including commercial applications, and to alter it and redistribute it
  * freely, subject to the following restrictions:
  *
  * 1. The origin of this software must not be misrepresented; you must not
  *    claim that you wrote the original software. If you use this software
  *    in a product, an acknowledgment in the product documentation would be
  *    appreciated but is not required.
  *
  * 2. Altered source versions must be plainly marked as such, and must not be
  *    misrepresented as being the original software.
  *
  * 3. This notice may not be removed or altered from any source distribution.
  */

#ifndef DROPT_H
#define DROPT_H

#ifndef DROPTDEF
/* Goes before every public dropt declaration/definition. Override to control
   linkage or visibility, e.g. `#define DROPTDEF static inline`. */
#define DROPTDEF
#endif



#include <stdio.h>
#include <wchar.h>

#if __STDC_VERSION__ >= 199901L
    #include <stdint.h>
    typedef uintptr_t dropt_uintptr;
#else
    typedef size_t dropt_uintptr;
#endif


#ifdef __cplusplus
extern "C" {
#endif


#ifndef DROPT_USE_WCHAR
#if defined _UNICODE && (defined _MSC_VER || defined DROPT_NO_STRING_BUFFERS)
#define DROPT_USE_WCHAR 1
#endif
#endif

#ifdef DROPT_USE_WCHAR
    /* This may be used for both char and string literals. */
    #define DROPT_TEXT_LITERAL(s) L ## s

    typedef wchar_t dropt_char;
#else
    #define DROPT_TEXT_LITERAL(s) s

    typedef char dropt_char;
#endif


enum
{
    /* Errors in the range [0x00, 0x7F] are reserved for dropt. */
    dropt_error_none,
    dropt_error_unknown,
    dropt_error_bad_configuration,
    dropt_error_insufficient_memory,
    dropt_error_invalid_option,
    dropt_error_insufficient_arguments,
    dropt_error_mismatch,
    dropt_error_overflow,
    dropt_error_underflow,

    /* Errors in the range [0x80, 0xFFFF] are free for clients to use. */
    dropt_error_custom_start = 0x80,
    dropt_error_custom_last = 0xFFFF
};
typedef unsigned int dropt_error;

typedef unsigned char dropt_bool;

/* Opaque. */
typedef struct dropt_context dropt_context;

/* Forward declarations. */
typedef struct dropt_option dropt_option;


/** `dropt_option_handler_func` callbacks are responsible for parsing
  * individual options and storing the parsed value.
  *
  * `dropt_option_handler_decl` may be used for declaring the callback
  * functions (see the stock option handlers below for examples).
  * `dropt_option_handler_func` is the actual function pointer type.
  *
  * `option` points to the `dropt_option` entry that matched the option
  * supplied by the user.  This will never be `NULL` when dropt invokes the
  * handler.
  *
  * `optionArgument` will be `NULL` if no argument is specified for an option.
  * It will be the empty string if the user explicitly passed an empty string
  * as the argument (e.g. `--option=""`).
  *
  * An option that doesn't expect an argument still can receive a non-null
  * value for `optionArgument` if the user explicitly specified one (e.g.
  * `--option=arg`).
  *
  * If the option's argument is optional, the handler might be called twice:
  * once with a candidate argument, and if that argument is rejected by the
  * handler, again with no argument.  Handlers should be aware of this if they
  * have side-effects.
  *
  * `dest` is the client-specified pointer to a variable for the handler to
  * modify.
  */
typedef dropt_error dropt_option_handler_decl(dropt_context* context,
                                              const dropt_option* option,
                                              const dropt_char* optionArgument,
                                              void* dest);
typedef dropt_option_handler_decl* dropt_option_handler_func;

/** `dropt_error_handler_func` callbacks are responsible for generating error
  * messages.  The returned string must be allocated on the heap and must be
  * freeable with `free()`.
  */
typedef dropt_char* (*dropt_error_handler_func)(dropt_error error,
                                                const dropt_char* optionName,
                                                const dropt_char* optionArgument,
                                                void* handlerData);

/** `dropt_strncmp_func` callbacks allow callers to provide their own (possibly
  * case-insensitive) string comparison function.
  */
typedef int (*dropt_strncmp_func)(const dropt_char* s, const dropt_char* t,
                                  size_t n);


/** Properties defining each option:
  *
  * short_name:
  *     The option's short name (e.g. the 'h' in `-h`).
  *     Use '\0' if the option has no short name.
  *
  * long_name:
  *     The option's long name (e.g. "help" in `--help`).
  *     Use `NULL` if the option has no long name.
  *
  * description:
  *     The description shown when generating help.
  *     May be `NULL` for undocumented options.
  *
  * arg_description:
  *     The description for the option's argument (e.g. `--option=argument` or
  *     `--option argument`), printed when generating help.
  *     Use `NULL` if the option does not take an argument.
  *
  * handler:
  *     The handler callback and data invoked in response to encountering the
  *     option.
  *
  * dest:
  *     The address of a variable for the handler to modify, if necessary.
  *
  * attr:
  *     Miscellaneous attributes.  See below.
  *
  * extra_data:
  *     Additional callback data for the handler.
  */
struct dropt_option
{
    dropt_char short_name;
    const dropt_char* long_name;
    const dropt_char* description;
    const dropt_char* arg_description;
    dropt_option_handler_func handler;
    void* dest;
    unsigned int attr;
    dropt_uintptr extra_data;
};


/** Bitwise flags for option attributes:
  *
  * dropt_attr_halt:
  *     Stop processing when this option is encountered.
  *
  * dropt_attr_hidden:
  *     Don't list the option when generating help.  Use this for undocumented
  *     options.
  *
  * dropt_attr_optional_val:
  *     The option's argument is optional.  If an option has this attribute,
  *     the handler callback may be invoked twice (once with a potential
  *     argument, and if that fails, again with a `NULL` argument).
  */
enum
{
    dropt_attr_halt = (1 << 0),
    dropt_attr_hidden = (1 << 1),
    dropt_attr_optional_val = (1 << 2)
};


typedef struct dropt_help_params
{
    unsigned int indent;
    unsigned int description_start_column;
    dropt_bool blank_lines_between_options;
} dropt_help_params;


DROPTDEF dropt_context* dropt_new_context(const dropt_option* options);
DROPTDEF void dropt_free_context(dropt_context* context);

DROPTDEF const dropt_option* dropt_get_options(const dropt_context* context);

DROPTDEF void dropt_set_error_handler(dropt_context* context,
                             dropt_error_handler_func handler,
                             void* handlerData);
DROPTDEF void dropt_set_strncmp(dropt_context* context, dropt_strncmp_func cmp);

/* Use this only for backward compatibility purposes. */
DROPTDEF void dropt_allow_concatenated_arguments(dropt_context* context,
                                        dropt_bool allow);

DROPTDEF dropt_char** dropt_parse(dropt_context* context, int argc, dropt_char** argv);

DROPTDEF dropt_error dropt_get_error(const dropt_context* context);
DROPTDEF void dropt_get_error_details(const dropt_context* context,
                             dropt_char** optionName,
                             dropt_char** optionArgument);
DROPTDEF const dropt_char* dropt_get_error_message(dropt_context* context);
DROPTDEF void dropt_clear_error(dropt_context* context);

#ifndef DROPT_NO_STRING_BUFFERS
DROPTDEF dropt_char* dropt_default_error_handler(dropt_error error,
                                        const dropt_char* optionName,
                                        const dropt_char* optionArgument);

DROPTDEF void dropt_init_help_params(dropt_help_params* helpParams);
DROPTDEF dropt_char* dropt_get_help(const dropt_context* context,
                           const dropt_help_params* helpParams);
DROPTDEF void dropt_print_help(FILE* f, const dropt_context* context,
                      const dropt_help_params* helpParams);
#endif


/* Stock option handlers for common types. */
dropt_option_handler_decl dropt_handle_bool;
dropt_option_handler_decl dropt_handle_verbose_bool;
dropt_option_handler_decl dropt_handle_int;
dropt_option_handler_decl dropt_handle_uint;
dropt_option_handler_decl dropt_handle_double;
dropt_option_handler_decl dropt_handle_string;
dropt_option_handler_decl dropt_handle_const;

#define DROPT_MISUSE(message) dropt_misuse(message, __FILE__, __LINE__)
DROPTDEF void dropt_misuse(const char* message, const char* filename, int line);

#ifdef __cplusplus
} /* extern "C" */
#endif



#include <stdarg.h>


#ifdef DROPT_USE_WCHAR
    #define dropt_strlen wcslen
    #define dropt_strcmp wcscmp
    #define dropt_strncmp wcsncmp
    #define dropt_strchr wcschr
    #define dropt_strtol wcstol
    #define dropt_strtoul wcstoul
    #define dropt_strtod wcstod
    #define dropt_tolower towlower
    #define dropt_fputs fputws
#else
    #define dropt_strlen strlen
    #define dropt_strcmp strcmp
    #define dropt_strncmp strncmp
    #define dropt_strchr strchr
    #define dropt_strtol strtol
    #define dropt_strtoul strtoul
    #define dropt_strtod strtod
    #define dropt_tolower tolower
    #define dropt_fputs fputs
#endif


#ifdef __cplusplus
extern "C" {
#endif

DROPTDEF void* dropt_safe_malloc(size_t numElements, size_t elementSize);
DROPTDEF void* dropt_safe_realloc(void* p, size_t numElements, size_t elementSize);

DROPTDEF dropt_char* dropt_strdup(const dropt_char* s);
DROPTDEF dropt_char* dropt_strndup(const dropt_char* s, size_t n);
DROPTDEF int dropt_stricmp(const dropt_char* s, const dropt_char* t);
DROPTDEF int dropt_strnicmp(const dropt_char* s, const dropt_char* t, size_t n);


#ifndef DROPT_NO_STRING_BUFFERS
typedef struct dropt_stringstream dropt_stringstream;

DROPTDEF int dropt_vsnprintf(dropt_char* s, size_t n, const dropt_char* format, va_list args);
DROPTDEF int dropt_snprintf(dropt_char* s, size_t n, const dropt_char* format, ...);

DROPTDEF dropt_char* dropt_vasprintf(const dropt_char* format, va_list args);
DROPTDEF dropt_char* dropt_asprintf(const dropt_char* format, ...);

DROPTDEF dropt_stringstream* dropt_ssopen(void);
DROPTDEF void dropt_ssclose(dropt_stringstream* ss);

DROPTDEF void dropt_ssclear(dropt_stringstream* ss);
DROPTDEF dropt_char* dropt_ssfinalize(dropt_stringstream* ss);
DROPTDEF const dropt_char* dropt_ssgetstring(const dropt_stringstream* ss);

DROPTDEF int dropt_vssprintf(dropt_stringstream* ss, const dropt_char* format, va_list args);
DROPTDEF int dropt_ssprintf(dropt_stringstream* ss, const dropt_char* format, ...);
#endif /* DROPT_NO_STRING_BUFFERS */

#ifdef __cplusplus
} /* extern "C" */
#endif


#endif /* DROPT_H */

#ifdef DROPT_IMPLEMENTATION
#ifndef DROPT_IMPLEMENTATION_INCLUDED
#define DROPT_IMPLEMENTATION_INCLUDED

/* ===== dropt_string.c ===== */

#ifdef _MSC_VER
    #include <tchar.h>
#endif

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <wctype.h>
#include <stdio.h>
#include <assert.h>

#if __STDC_VERSION__ >= 199901L
    #include <stdint.h>
#else
    /* Compatibility junk for things that don't yet support ISO C99. */
    #if defined _MSC_VER || defined __BORLANDC__
        #ifndef va_copy
            #define va_copy(dest, src) (dest = (src))
        #endif
    #else
        #ifndef va_copy
            #error Unsupported platform.  va_copy is not defined.
        #endif
    #endif

    #ifndef SIZE_MAX
        #define SIZE_MAX ((size_t) -1)
    #endif
#endif


#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif

#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

#ifdef DROPT_DEBUG_STRING_BUFFERS
    enum { default_stringstream_buffer_size = 1 };
    #define GROWN_STRINGSTREAM_BUFFER_SIZE(oldSize, minAmount) \
        ((oldSize) + (minAmount))
#else
    enum { default_stringstream_buffer_size = 256 };
    #define GROWN_STRINGSTREAM_BUFFER_SIZE(oldSize, minAmount) \
        MAX((oldSize) * 2, (oldSize) + (minAmount))
#endif


#ifndef DROPT_NO_STRING_BUFFERS
struct dropt_stringstream
{
    /* The string buffer. */
    dropt_char* string;

    /* Size of the string buffer, in `dropt_char`s, including space for `NUL`.
     */
    size_t maxSize;

    /* Number of elements used in the string buffer, excluding `NUL`. */
    size_t used;
};
#endif


/** dropt_safe_malloc
  *
  *     A version of `malloc` that checks for integer overflow.
  *
  * PARAMETERS:
  *     IN numElements : The number of elements to allocate.
  *     IN elementSize : The size of each element, in bytes.
  *
  * RETURNS:
  *     A pointer to the allocated memory.
  *     Returns `NULL` if `numElements` is 0.
  *     Returns `NULL` on error.
  */
DROPTDEF void*
dropt_safe_malloc(size_t numElements, size_t elementSize)
{
    return dropt_safe_realloc(NULL, numElements, elementSize);
}


/** dropt_safe_realloc
  *
  *     Wrapper around `realloc` to check for integer overflow.
  *
  * PARAMETERS:
  *     IN/OUT p       : A pointer to the memory block to resize.
  *                      If `NULL`, a new memory block of the specified size
  *                        will be allocated.
  *     IN numElements : The number of elements to allocate.
  *                      If 0, frees `p`.
  *     IN elementSize : The size of each element, in bytes.
  *
  * RETURNS:
  *     A pointer to the allocated memory.
  *     Returns `NULL` if `numElements` is 0.
  *     Returns `NULL` on error.
  */
DROPTDEF void*
dropt_safe_realloc(void* p, size_t numElements, size_t elementSize)
{
    size_t numBytes;

    /* `elementSize` shouldn't legally be 0, but we check for it in case a
     * caller got the argument order wrong.
     */
    if (numElements == 0 || elementSize == 0)
    {
        /* The behavior of `realloc(p, 0)` is implementation-defined.  Let's
         * enforce a particular behavior.
         */
        free(p);

        assert(elementSize != 0);
        return NULL;
    }

    numBytes = numElements * elementSize;
    if (numBytes / elementSize != numElements)
    {
        /* Overflow. */
        return NULL;
    }

    return realloc(p, numBytes);
}


/** dropt_strdup
  *
  *     Duplicates a string.
  *
  * PARAMETERS:
  *     IN s : A `NUL`-terminated string to duplicate.
  *
  * RETURNS:
  *     The duplicated string.  The caller is responsible for calling `free()`
  *       on it when no longer needed.
  *     Returns `NULL` on error.
  */
DROPTDEF dropt_char*
dropt_strdup(const dropt_char* s)
{
    return dropt_strndup(s, SIZE_MAX);
}


/** dropt_strndup
  *
  *     Duplicates the first `n` characters of a string.
  *
  * PARAMETERS:
  *     IN s : The string to duplicate.
  *     IN n : The maximum number of `dropt_char`s to copy, excluding the
  *              `NUL`-terminator.
  *
  * RETURNS:
  *     The duplicated string, which is always `NUL`-terminated.  The caller is
  *       responsible for calling `free()` on it when no longer needed.
  *     Returns `NULL` on error.
  */
DROPTDEF dropt_char*
dropt_strndup(const dropt_char* s, size_t n)
{
    dropt_char* copy;
    size_t len = 0;

    assert(s != NULL);

    while (len < n && s[len] != DROPT_TEXT_LITERAL('\0'))
    {
        len++;
    }

    if (len + 1 < len)
    {
        /* This overflow check shouldn't be strictly necessary. `len` can be at
         * most `SIZE_MAX`, so `SIZE_MAX + 1` can wrap around to 0, but
         * `dropt_safe_malloc` will return `NULL` for a 0-sized allocation.
         * However, favor defensive paranoia.
         */
        return NULL;
    }

    copy = dropt_safe_malloc(len + 1 /* NUL */, sizeof *copy);
    if (copy != NULL)
    {
        memcpy(copy, s, len * sizeof *copy);
        copy[len] = DROPT_TEXT_LITERAL('\0');
    }

    return copy;
}


/** dropt_stricmp
  *
  *     Compares two `NUL`-terminated strings ignoring case differences.  Not
  *       recommended for non-ASCII strings.
  *
  * PARAMETERS:
  *     IN s, t : The strings to compare.
  *
  * RETURNS:
  *     0 if the strings are equivalent,
  *     < 0 if `s` should precede `t`,
  *     > 0 if `s` should follow `t`.
  */
DROPTDEF int
dropt_stricmp(const dropt_char* s, const dropt_char* t)
{
    assert(s != NULL);
    assert(t != NULL);
    return dropt_strnicmp(s, t, SIZE_MAX);
}


/** dropt_strnicmp
  *
  *     Compares the first `n` characters of two strings, ignoring case
  *       differences.  Not recommended for non-ASCII strings.
  *
  * PARAMETERS:
  *     IN s, t : The strings to compare.
  *     IN n    : The maximum number of `dropt_char`s to compare.
  *
  * RETURNS:
  *     0 if the strings are equivalent,
  *     < 0 if `s` should precede `t`,
  *     > 0 if `s` should follow `t`.
  */
DROPTDEF int
dropt_strnicmp(const dropt_char* s, const dropt_char* t, size_t n)
{
    assert(s != NULL);
    assert(t != NULL);

    if (s == t) { return 0; }

    while (n--)
    {
        if (*s == DROPT_TEXT_LITERAL('\0') && *t == DROPT_TEXT_LITERAL('\0'))
        {
            break;
        }
        else if (*s == *t || dropt_tolower(*s) == dropt_tolower(*t))
        {
            s++;
            t++;
        }
        else
        {
            return (dropt_tolower(*s) < dropt_tolower(*t))
                   ? -1
                   : +1;
        }
    }

    return 0;
}


#ifndef DROPT_NO_STRING_BUFFERS
/** dropt_vsnprintf
  *
  *     `vsnprintf` wrapper to provide ISO C99-compliant behavior.
  *
  * PARAMETERS:
  *     OUT s     : The destination buffer.  May be `NULL` if `n` is 0.
  *                 If non-`NULL`, always `NUL`-terminated.
  *     IN n      : The size of the destination buffer, measured in
  *                   `dropt_char`s.
  *     IN format : `printf`-style format specifier.  Must not be `NULL`.
  *     IN args   : Arguments to insert into the formatted string.
  *
  * RETURNS:
  *     The number of characters that would be written to the destination
  *       buffer if it's sufficiently large, excluding the `NUL`-terminator.
  *     Returns -1 on error.
  */
DROPTDEF int
dropt_vsnprintf(dropt_char* s, size_t n, const dropt_char* format, va_list args)
{
#if __STDC_VERSION__ >= 199901L || __GNUC__
    /* ISO C99-compliant.
     *
     * As far as I can tell, gcc's implementation of `vsnprintf` has always
     * matched the behavior required by the C99 standard (which is to return
     * the necessary buffer size).
     *
     * Note that this won't work with `wchar_t` because there is no true,
     * standard `wchar_t` equivalent of `snprintf`. `swprintf` comes close but
     * doesn't return the necessary buffer size (and the standard does not
     * provide a guaranteed way to test if truncation occurred), and its
     * format string can't be used interchangeably with `snprintf`.
     *
     * It's simpler not to support `wchar_t` on non-Windows platforms.
     */
    assert(format != NULL);
    return vsnprintf(s, n, format, args);
#elif defined __BORLANDC__
    /* Borland's compiler neglects to `NUL`-terminate. */
    int ret;
    assert(format != NULL);
    ret = vsnprintf(s, n, format, args);
    if (n != 0) { s[n - 1] = DROPT_TEXT_LITERAL('\0'); }
    return ret;
#elif defined _MSC_VER
    /* `_vsntprintf` and `_vsnprintf_s` on Windows don't have C99 semantics;
     * they return -1 if truncation occurs.
     */
    va_list argsCopy;
    int ret;

    assert(format != NULL);

    va_copy(argsCopy, args);
    ret = _vsctprintf(format, argsCopy);
    va_end(argsCopy);

    if (n != 0)
    {
        assert(s != NULL);

    #if _MSC_VER >= 1400
        (void) _vsntprintf_s(s, n, _TRUNCATE, format, args);
    #else
        /* This version doesn't necessarily `NUL`-terminate.  Sigh. */
        (void) _vsnprintf(s, n, format, args);
        s[n - 1] = DROPT_TEXT_LITERAL('\0');
    #endif
    }

    return ret;

#else
    #error Unsupported platform.  dropt_vsnprintf unimplemented.
    return -1;
#endif
}


/** See `dropt_vsnprintf`. */
DROPTDEF int
dropt_snprintf(dropt_char* s, size_t n, const dropt_char* format, ...)
{
    int ret;
    va_list args;
    va_start(args, format);
    ret = dropt_vsnprintf(s, n, format, args);
    va_end(args);
    return ret;
}


/** dropt_vasprintf
  *
  *     Allocates a formatted string with `vprintf` semantics.
  *
  * PARAMETERS:
  *     IN format : `printf`-style format specifier.  Must not be `NULL`.
  *     IN args   : Arguments to insert into the formatted string.
  *
  * RETURNS:
  *     The formatted string, which is always NUL-terminated.  The caller is
  *       responsible for calling `free()` on it when no longer needed.
  *     Returns `NULL` on error.
  */
DROPTDEF dropt_char*
dropt_vasprintf(const dropt_char* format, va_list args)
{
    dropt_char* s = NULL;
    int len;
    va_list argsCopy;
    assert(format != NULL);

    va_copy(argsCopy, args);
    len = dropt_vsnprintf(NULL, 0, format, argsCopy);
    va_end(argsCopy);

    if (len >= 0)
    {
        size_t n = len + 1 /* NUL */;
        s = dropt_safe_malloc(n, sizeof *s);
        if (s != NULL)
        {
            dropt_vsnprintf(s, n, format, args);
        }
    }

    return s;
}


/** See `dropt_vasprintf`. */
DROPTDEF dropt_char*
dropt_asprintf(const dropt_char* format, ...)
{
    dropt_char* s;

    va_list args;
    va_start(args, format);
    s = dropt_vasprintf(format, args);
    va_end(args);

    return s;
}


/** dropt_ssopen
  *
  *     Constructs a new `dropt_stringstream`.
  *
  * RETURNS:
  *     An initialized `dropt_stringstream`.  The caller is responsible for
  *       calling either `dropt_ssclose()` or `dropt_ssfinalize()` on it when
  *       no longer needed.
  *     Returns `NULL` on error.
  */
DROPTDEF dropt_stringstream*
dropt_ssopen(void)
{
    dropt_stringstream* ss = malloc(sizeof *ss);
    if (ss != NULL)
    {
        ss->used = 0;
        ss->maxSize = default_stringstream_buffer_size;
        ss->string = dropt_safe_malloc(ss->maxSize, sizeof *ss->string);
        if (ss->string == NULL)
        {
            free(ss);
            ss = NULL;
        }
        else
        {
            ss->string[0] = DROPT_TEXT_LITERAL('\0');
        }
    }
    return ss;
}


/** dropt_ssclose
  *
  *     Destroys a `dropt_stringstream`.
  *
  * PARAMETERS:
  *     IN/OUT ss : The `dropt_stringstream`.
  */
DROPTDEF void
dropt_ssclose(dropt_stringstream* ss)
{
    if (ss != NULL)
    {
        free(ss->string);
        free(ss);
    }
}


/** dropt_ssgetfreespace
  *
  * RETURNS:
  *     The amount of free space in the `dropt_stringstream`'s internal buffer,
  *       measured in `dropt_char`s.  Space used for the `NUL`-terminator is
  *       considered free. (The amount of free space therefore is always
  *       positive.)
  */
static size_t
dropt_ssgetfreespace(const dropt_stringstream* ss)
{
    assert(ss != NULL);
    assert(ss->maxSize > 0);
    assert(ss->maxSize > ss->used);
    return ss->maxSize - ss->used;
}


/** dropt_ssresize
  *
  *     Resizes a `dropt_stringstream`'s internal buffer.  If the requested
  *     size is less than the amount of buffer already in use, the buffer will
  *     be shrunk to the minimum size necessary.
  *
  * PARAMETERS:
  *     IN/OUT ss : The `dropt_stringstream`.
  *     IN n      : The desired buffer size, in `dropt_char`s.
  *
  * RETURNS:
  *     The new size of the `dropt_stringstream`'s buffer in `dropt_char`s,
  *       including space for a terminating `NUL`.
  */
static size_t
dropt_ssresize(dropt_stringstream* ss, size_t n)
{
    assert(ss != NULL);

    /* Don't allow shrinking if it will truncate the string. */
    if (n < ss->maxSize) { n = MAX(n, ss->used + 1 /* NUL */); }

    /* There should always be a buffer to point to. */
    assert(n > 0);

    if (n != ss->maxSize)
    {
        dropt_char* p = dropt_safe_realloc(ss->string, n, sizeof *ss->string);
        if (p != NULL)
        {
            ss->string = p;
            ss->maxSize = n;
            assert(ss->maxSize > 0);
         }
    }
    return ss->maxSize;
}


/** dropt_ssclear
  *
  *     Clears and re-initializes a `dropt_stringstream`.
  *
  * PARAMETERS:
  *     IN/OUT ss : The `dropt_stringstream`.
  */
DROPTDEF void
dropt_ssclear(dropt_stringstream* ss)
{
    assert(ss != NULL);

    ss->string[0] = DROPT_TEXT_LITERAL('\0');
    ss->used = 0;

    dropt_ssresize(ss, default_stringstream_buffer_size);
}


/** dropt_ssfinalize
  *
  *     Finalizes a `dropt_stringstream`; returns the contained string and
  *     destroys the `dropt_stringstream`.
  *
  * PARAMETERS:
  *     IN/OUT ss : The `dropt_stringstream`.
  *
  * RETURNS:
  *     The `dropt_stringstream`'s string, which is always `NUL`-terminated.
  *       Note that the caller assumes ownership of the returned string and is
  *       responsible for calling `free()` on it when no longer needed.
  */
DROPTDEF dropt_char*
dropt_ssfinalize(dropt_stringstream* ss)
{
    dropt_char* s;
    assert(ss != NULL);

    /* Shrink to fit. */
    dropt_ssresize(ss, 0);

    s = ss->string;
    ss->string = NULL;

    dropt_ssclose(ss);

    return s;
}


/** dropt_ssgetstring
  *
  * PARAMETERS:
  *     IN ss : The `dropt_stringstream`.
  *
  * RETURNS:
  *     The `dropt_stringstream`'s string, which is always `NUL`-terminated.
  *       The returned string will no longer be valid if further operations are
  *       performed on the `dropt_stringstream` or if the `dropt_stringstream`
  *       is closed.
  */
DROPTDEF const dropt_char*
dropt_ssgetstring(const dropt_stringstream* ss)
{
    assert(ss != NULL);
    return ss->string;
}


/** dropt_vssprintf
  *
  *     Appends a formatted string with `vprintf` semantics to a
  *     `dropt_stringstream`.
  *
  * PARAMETERS:
  *     IN/OUT ss : The `dropt_stringstream`.
  *     IN format : `printf`-style format specifier.  Must not be `NULL`.
  *     IN args   : Arguments to insert into the formatted string.
  *
  * RETURNS:
  *     The number of characters written to the `dropt_stringstream`, excluding
  *       the `NUL`-terminator.
  *     Returns a negative value on error.
  */
DROPTDEF int
dropt_vssprintf(dropt_stringstream* ss, const dropt_char* format, va_list args)
{
    int n;
    va_list argsCopy;
    assert(ss != NULL);
    assert(format != NULL);

    va_copy(argsCopy, args);
    n = dropt_vsnprintf(NULL, 0, format, argsCopy);
    va_end(argsCopy);

    if (n > 0)
    {
        size_t available = dropt_ssgetfreespace(ss);
        if ((unsigned int) n >= available)
        {
            /* It's possible that `newSize < ss->maxSize` if
             * `GROWN_STRINGSTREAM_BUFFER_SIZE()` overflows, but it should be
             * safe since we'll recompute the available space.
             */
            size_t newSize = GROWN_STRINGSTREAM_BUFFER_SIZE(ss->maxSize, n);
            dropt_ssresize(ss, newSize);
            available = dropt_ssgetfreespace(ss);
        }
        assert(available > 0); /* Space always is reserved for NUL. */

        /* `snprintf`'s family of functions return the number of characters
         * that would be output with a sufficiently large buffer, excluding
         * `NUL`.
         */
        n = dropt_vsnprintf(ss->string + ss->used, available, format, args);

        /* We couldn't allocate enough space. */
        if ((unsigned int) n >= available) { n = -1; }

        if (n > 0) { ss->used += n; }
    }
    return n;
}


/** See `dropt_vssprintf`. */
DROPTDEF int
dropt_ssprintf(dropt_stringstream* ss, const dropt_char* format, ...)
{
    int n;

    va_list args;
    va_start(args, format);
    n = dropt_vssprintf(ss, format, args);
    va_end(args);

    return n;
}
#endif /* DROPT_NO_STRING_BUFFERS */

/* ===== dropt.c ===== */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wctype.h>
#include <assert.h>


#if __STDC_VERSION__ >= 199901L
    #include <stdint.h>
    #include <stdbool.h>
#else
    /* Compatibility junk for things that don't yet support ISO C99. */
    #ifndef SIZE_MAX
        #define SIZE_MAX ((size_t) -1)
    #endif

#if __STDC_VERSION__ >= 199901L
    #include <stdbool.h>
#else
    typedef enum { false, true } bool;
#endif
#endif

#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

#ifndef ARRAY_LENGTH
#define ARRAY_LENGTH(array) (sizeof (array) / sizeof (array)[0])
#endif

#define IMPLIES(p, q) (!(p) || q)

#define OPTION_TAKES_ARG(option) ((option)->arg_description != NULL)


enum
{
    default_help_indent = 2,
    default_description_start_column = 6,
};


/** A string that might not be `NUL`-terminated. */
typedef struct
{
    const dropt_char* s;

    /* The length of s, excluding any `NUL` terminator. */
    size_t len;
} char_array;


/** A proxy for a `dropt_option` used for `qsort` and `bsearch`.  Instead of
  * sorting the `dropt_option` table directly, we sort arrays of `option_proxy`
  * structures.  This allows us to have separate arrays sorted by different
  * keys and allows passing along additional data.
  */
typedef struct
{
    const dropt_option* option;

    /* The `qsort` and `bsearch` comparison callbacks don't pass along any
     * client-supplied contextual data, so we have to embed it alongside the
     * regular data.
     */
    const dropt_context* context;
} option_proxy;


struct dropt_context
{
    const dropt_option* options;
    size_t numOptions;

    /* These may be NULL. */
    option_proxy* sortedByLong;
    option_proxy* sortedByShort;

    bool allowConcatenatedArgs;

    dropt_error_handler_func errorHandler;
    void* errorHandlerData;

    struct
    {
        dropt_error err;
        dropt_char* optionName;
        dropt_char* optionArgument;
        dropt_char* message;
    } errorDetails;

    /* This isn't named strncmp because platforms might provide a macro
     * version of strncmp, and we want to avoid a potential naming
     * conflict.
     */
    dropt_strncmp_func ncmpstr;
};


typedef struct
{
    const dropt_option* option;
    const dropt_char* optionArgument;
    dropt_char** argNext;
    int argsLeft;
} parse_state;


/** make_char_array
  *
  * PARAMETERS:
  *     IN s : A string.  Might not be NUL-terminated.
  *            May be `NULL`.
  *     len  : The length of `s`, excluding any `NUL` terminator.
  *
  * RETURNS:
  *     The constructed char_array structure.
  */
static char_array
make_char_array(const dropt_char* s, size_t len)
{
   char_array a;

   assert(IMPLIES(s == NULL, len == 0));

   a.s = s;
   a.len = len;
   return a;
}


/** cmp_key_option_proxy_long
  *
  *     Comparison callback for `bsearch`.  Compares a `char_array` structure
  *     against an `option_proxy` structure based on long option names.
  *
  * PARAMETERS:
  *     IN key  : A pointer to the `char_array` structure to search for.
  *     IN item : A pointer to the `option_proxy` structure being searched
  *                 against.
  *
  * RETURNS:
  *     0 if `key` and `item` are equivalent,
  *     < 0 if `key` should precede `item`,
  *     > 0 if `key` should follow `item`.
  */
static int
cmp_key_option_proxy_long(const void* key, const void* item)
{
    const char_array* longName = key;
    const option_proxy* op = item;

    size_t optionLen;
    int ret;

    assert(longName != NULL);
    assert(op != NULL);
    assert(op->option != NULL);
    assert(op->context != NULL);
    assert(op->context->ncmpstr != NULL);

    if (longName->s == op->option->long_name)
    {
        return 0;
    }
    else if (longName->s == NULL)
    {
        return -1;
    }
    else if (op->option->long_name == NULL)
    {
        return +1;
    }

    /* Although the `longName` key might not be `NUL`-terminated, the
     * `option_proxy` item we're searching against must be.
     */
    optionLen = dropt_strlen(op->option->long_name);
    ret = op->context->ncmpstr(longName->s,
                               op->option->long_name,
                               MIN(longName->len, optionLen));
    if (ret != 0)
    {
        return ret;
    }

    if (longName->len < optionLen)
    {
        return -1;
    }
    else if (longName->len > optionLen)
    {
        return +1;
    }

    return 0;
}


/** cmp_option_proxies_long
  *
  *     Comparison callback for `qsort`.  Compares two `option_proxy`
  *     structures based on long option names.
  *
  * PARAMETERS:
  *     IN p1, p2 : Pointers to the `option_proxy` structures to compare.
  *
  * RETURNS:
  *     0 if `p1` and `p2` are equivalent,
  *     < 0 if `p1` should precede `p2`,
  *     > 0 if `p1` should follow `p2`.
  */
static int
cmp_option_proxies_long(const void* p1, const void* p2)
{
    const option_proxy* o1 = p1;
    const option_proxy* o2 = p2;

    char_array ca1;

    assert(o1 != NULL);
    assert(o2 != NULL);
    assert(o1->option != NULL);
    assert(o1->context == o2->context);

    ca1 = make_char_array(o1->option->long_name,
                          (o1->option->long_name == NULL)
                          ? 0
                          : dropt_strlen(o1->option->long_name));
    return cmp_key_option_proxy_long(&ca1, o2);
}


/** cmp_key_option_proxy_short
  *
  *     Comparison callback for `bsearch`.  Compares a `dropt_char` against an
  *     `option_proxy` structure based on short option names.
  *
  * PARAMETERS:
  *     IN key  : A pointer to the `dropt_char` to search for.
  *     IN item : A pointer to the `option_proxy` structure being searched
  *                 against.
  *
  * RETURNS:
  *     0 if `key` and `item` are equivalent,
  *     < 0 if `key` should precede `item`,
  *     > 0 if `key` should follow `item`.
  */
static int
cmp_key_option_proxy_short(const void* key, const void* item)
{
    const dropt_char* shortName = key;
    const option_proxy* op = item;

    assert(shortName != NULL);
    assert(op != NULL);
    assert(op->option != NULL);
    assert(op->context != NULL);
    assert(op->context->ncmpstr != NULL);

    return op->context->ncmpstr(shortName,
                                &op->option->short_name,
                                1);
}


/** cmp_option_proxies_short
  *
  *     Comparison callback for `qsort`.  Compares two `option_proxy`
  *     structures based on short option names.
  *
  * PARAMETERS:
  *     IN p1, p2 : Pointers to the `option_proxy` structures to compare.
  *
  * RETURNS:
  *     0 if `p1` and `p2` are equivalent,
  *     < 0 if `p1` should precede `p2`,
  *     > 0 if `p1` should follow `p2`.
  */
static int
cmp_option_proxies_short(const void* p1, const void* p2)
{
    const option_proxy* o1 = p1;
    const option_proxy* o2 = p2;

    assert(o1 != NULL);
    assert(o2 != NULL);
    assert(o1->option != NULL);
    assert(o1->context == o2->context);

    return cmp_key_option_proxy_short(&o1->option->short_name, o2);
}


/** init_lookup_tables
  *
  *     Initializes the sorted lookup tables in a dropt context if not already
  *     initialized.
  *
  * PARAMETERS:
  *     IN/OUT context : The dropt context.
  *                      Must not be `NULL`.
  */
static void
init_lookup_tables(dropt_context* context)
{
    const dropt_option* options;
    size_t n;

    assert(context != NULL);

    options = context->options;
    n = context->numOptions;

    if (context->sortedByLong == NULL)
    {
        context->sortedByLong
            = dropt_safe_malloc(n, sizeof *(context->sortedByLong));
        if (context->sortedByLong != NULL)
        {
            size_t i;
            for (i = 0; i < n; i++)
            {
                context->sortedByLong[i].option = &options[i];
                context->sortedByLong[i].context = context;
            }

            qsort(context->sortedByLong,
                  n, sizeof *(context->sortedByLong),
                  cmp_option_proxies_long);
        }
    }

    if (context->sortedByShort == NULL)
    {
        context->sortedByShort
            = dropt_safe_malloc(n, sizeof *(context->sortedByShort));
        if (context->sortedByShort != NULL)
        {
            size_t i;
            for (i = 0; i < n; i++)
            {
                context->sortedByShort[i].option = &options[i];
                context->sortedByShort[i].context = context;
            }

            qsort(context->sortedByShort,
                  n, sizeof *(context->sortedByShort),
                  cmp_option_proxies_short);
        }
    }
}


/** free_lookup_tables
  *
  *     Frees the sorted lookup tables in a dropt context.
  *
  * PARAMETERS:
  *     IN/OUT context : The dropt context.
  *                      May be `NULL`.
  */
static void
free_lookup_tables(dropt_context* context)
{
    if (context != NULL)
    {
        free(context->sortedByLong);
        context->sortedByLong = NULL;

        free(context->sortedByShort);
        context->sortedByShort = NULL;
    }
}


/** is_valid_option
  *
  * PARAMETERS:
  *     IN option : Specification for an individual option.
  *
  * RETURNS:
  *     true if the specified option is valid, false if it's a sentinel value.
  */
static bool
is_valid_option(const dropt_option* option)
{
    return    option != NULL
           && !(   option->long_name == NULL
                && option->short_name == DROPT_TEXT_LITERAL('\0')
                && option->description == NULL
                && option->arg_description == NULL
                && option->handler == NULL
                && option->dest == NULL
                && option->attr == 0
                && option->extra_data == 0);
}


/** find_option_long
  *
  *     Finds the option specification for a long option name (i.e., an option
  *     of the form "--option").
  *
  * PARAMETERS:
  *     IN context     : The dropt context.
  *     IN longName    : The long option name to search for (excluding leading
  *                        dashes).
  *                      `longName.s` must not be `NULL`.
  *
  * RETURNS:
  *     A pointer to the corresponding option specification or `NULL` if not
  *       found.
  */
static const dropt_option*
find_option_long(const dropt_context* context,
                 char_array longName)
{
    assert(context != NULL);
    assert(longName.s != NULL);

    if (context->sortedByLong != NULL)
    {
        option_proxy* found = bsearch(&longName, context->sortedByLong,
                                      context->numOptions,
                                      sizeof *(context->sortedByLong),
                                      cmp_key_option_proxy_long);
        return (found == NULL) ? NULL : found->option;
    }

    /* Fall back to a linear search. */
    {
        option_proxy item = { 0 };
        item.context = context;
        for (item.option = context->options;
             is_valid_option(item.option);
             item.option++)
        {
            if (cmp_key_option_proxy_long(&longName, &item) == 0)
            {
                return item.option;
            }
        }
    }
    return NULL;
}


/** find_option_short
  *
  *     Finds the option specification for a short option name (i.e., an
  *     option of the form "-o").
  *
  * PARAMETERS:
  *     IN context   : The dropt context.
  *     IN shortName : The short option name to search for.
  *
  * RETURNS:
  *     A pointer to the corresponding option specification or `NULL` if not
  *       found.
  */
static const dropt_option*
find_option_short(const dropt_context* context, dropt_char shortName)
{
    assert(context != NULL);
    assert(shortName != DROPT_TEXT_LITERAL('\0'));
    assert(context->ncmpstr != NULL);

    if (context->sortedByShort != NULL)
    {
        option_proxy* found = bsearch(&shortName, context->sortedByShort,
                                      context->numOptions,
                                      sizeof *(context->sortedByShort),
                                      cmp_key_option_proxy_short);
        return (found == NULL) ? NULL : found->option;
    }

    /* Fall back to a linear search. */
    {
        const dropt_option* option;
        for (option = context->options; is_valid_option(option); option++)
        {
            if (context->ncmpstr(&shortName, &option->short_name, 1) == 0)
            {
                return option;
            }
        }
    }
    return NULL;
}


/** set_error_details
  *
  *     Generates error details in the dropt context.
  *
  * PARAMETERS:
  *     IN/OUT context    : The dropt context.
  *                         Must not be `NULL`.
  *     IN err            : The error code.
  *     IN optionName     : The name of the option we failed on.
  *                         `optionName.s` must not be `NULL`.
  *     IN optionArgument : The value of the option we failed on.
  *                         Pass `NULL` if unwanted.
  */
static void
set_error_details(dropt_context* context, dropt_error err,
                  char_array optionName,
                  const dropt_char* optionArgument)
{
    assert(context != NULL);
    assert(optionName.s != NULL);

    context->errorDetails.err = err;

    free(context->errorDetails.optionName);
    free(context->errorDetails.optionArgument);

    context->errorDetails.optionName = dropt_strndup(optionName.s,
                                                     optionName.len);
    context->errorDetails.optionArgument = (optionArgument == NULL)
                                           ? NULL
                                           : dropt_strdup(optionArgument);

    /* The message will be generated lazily on retrieval. */
    free(context->errorDetails.message);
    context->errorDetails.message = NULL;
}


/** set_short_option_error_details
  *
  *     Generates error details in the dropt context.
  *
  * PARAMETERS:
  *     IN/OUT context    : The dropt context.
  *     IN err            : The error code.
  *     IN shortName      : the "short" name of the option we failed on.
  *     IN optionArgument : The value of the option we failed on.
  *                         Pass `NULL` if unwanted.
  */
static void
set_short_option_error_details(dropt_context* context, dropt_error err,
                               dropt_char shortName,
                               const dropt_char* optionArgument)
{
    /* "-?" is just a placeholder. */
    dropt_char shortNameBuf[] = DROPT_TEXT_LITERAL("-?");

    assert(context != NULL);
    assert(shortName != DROPT_TEXT_LITERAL('\0'));

    shortNameBuf[1] = shortName;

    set_error_details(context, err,
                      make_char_array(shortNameBuf,
                                      ARRAY_LENGTH(shortNameBuf) - 1),
                      optionArgument);
}


/** dropt_get_error
  *
  * PARAMETERS:
  *     IN context : The dropt context.
  *                  Must not be NULL.
  *
  * RETURNS:
  *     The current error code waiting in the dropt context.
  */
DROPTDEF dropt_error
dropt_get_error(const dropt_context* context)
{
    if (context == NULL)
    {
        DROPT_MISUSE("No dropt context specified.");
        return dropt_error_bad_configuration;
    }
    return context->errorDetails.err;
}


/** dropt_get_error_details
  *
  *     Retrieves details about the current error.
  *
  * PARAMETERS:
  *     IN context         : The dropt context.
  *     OUT optionName     : On output, the name of the option we failed
  *                            on.  Do not free this string.
  *                          Pass `NULL` if unwanted.
  *     OUT optionArgument : On output, the value (possibly `NULL`) of the
  *                            option we failed on.  Do not free this
  *                            string.
  *                          Pass `NULL` if unwanted.
  */
DROPTDEF void
dropt_get_error_details(const dropt_context* context,
                        dropt_char** optionName, dropt_char** optionArgument)
{
    if (optionName != NULL)
    {
        *optionName = context->errorDetails.optionName;
    }

    if (optionArgument != NULL)
    {
        *optionArgument = context->errorDetails.optionArgument;
    }
}


/** dropt_get_error_message
  *
  * PARAMETERS:
  *     IN context : The dropt context.
  *                  Must not be `NULL`.
  *
  * RETURNS:
  *     The current error message waiting in the dropt context or the empty
  *       string if there are no errors.  Note that calling any dropt
  *       function other than `dropt_get_error`, `dropt_get_error_details`, and
  *       `dropt_get_error_message` may invalidate a previously-returned
  *       string.
  */
DROPTDEF const dropt_char*
dropt_get_error_message(dropt_context* context)
{
    if (context == NULL)
    {
        DROPT_MISUSE("No dropt context specified.");
        return DROPT_TEXT_LITERAL("");
    }

    if (context->errorDetails.err == dropt_error_none)
    {
        return DROPT_TEXT_LITERAL("");
    }

    if (context->errorDetails.message == NULL)
    {
        if (context->errorHandler != NULL)
        {
            context->errorDetails.message
                = context->errorHandler(context->errorDetails.err,
                                        context->errorDetails.optionName,
                                        context->errorDetails.optionArgument,
                                        context->errorHandlerData);
        }
        else
        {
#ifndef DROPT_NO_STRING_BUFFERS
            context->errorDetails.message
                = dropt_default_error_handler(context->errorDetails.err,
                                              context->errorDetails.optionName,
                                              context->errorDetails.optionArgument);
#endif
        }
    }

    return (context->errorDetails.message == NULL)
           ? DROPT_TEXT_LITERAL("Unknown error")
           : context->errorDetails.message;
}


/** dropt_clear_error
  *
  *     Clears the error waiting in the dropt context.
  *
  * PARAMETERS:
  *     IN/OUT context : The dropt context to free.
  *                      May be `NULL`.
  */
DROPTDEF void
dropt_clear_error(dropt_context* context)
{
    if (context != NULL)
    {
        context->errorDetails.err = dropt_error_none;

        free(context->errorDetails.optionName);
        context->errorDetails.optionName = NULL;

        free(context->errorDetails.optionArgument);
        context->errorDetails.optionArgument = NULL;

        free(context->errorDetails.message);
        context->errorDetails.message = NULL;
    }
}


#ifndef DROPT_NO_STRING_BUFFERS
/** dropt_default_error_handler
  *
  *     Default error handler.
  *
  * PARAMETERS:
  *     IN error          : The error code.
  *     IN optionName     : The name of the option we failed on.
  *     IN optionArgument : The value of the option we failed on.
  *                         Pass `NULL` if unwanted.
  *
  * RETURNS:
  *     An allocated string for the given error.  The caller is responsible for
  *       calling `free()` on it when no longer needed.
  *     May return `NULL`.
  */
DROPTDEF dropt_char*
dropt_default_error_handler(dropt_error error,
                            const dropt_char* optionName,
                            const dropt_char* optionArgument)
{
    dropt_char* s = NULL;

    const dropt_char* separator = DROPT_TEXT_LITERAL(": ");

    if (optionArgument == NULL)
    {
        separator = optionArgument = DROPT_TEXT_LITERAL("");
    }

    switch (error)
    {
        case dropt_error_none:
            /* This shouldn't happen (unless client code invokes this directly
             * with `dropt_error_none`), but it's here for completeness.
             */
            break;

        case dropt_error_bad_configuration:
            s = dropt_strdup(DROPT_TEXT_LITERAL("Invalid option configuration"));
            break;

        case dropt_error_invalid_option:
            s = dropt_asprintf(DROPT_TEXT_LITERAL("Invalid option: %s"),
                               optionName);
            break;
        case dropt_error_insufficient_arguments:
            s = dropt_asprintf(DROPT_TEXT_LITERAL("Value required after option %s"),
                               optionName);
            break;
        case dropt_error_mismatch:
            s = dropt_asprintf(DROPT_TEXT_LITERAL("Invalid value for option %s%s%s"),
                               optionName, separator, optionArgument);
            break;
        case dropt_error_overflow:
            s = dropt_asprintf(DROPT_TEXT_LITERAL("Value too large for option %s%s%s"),
                               optionName, separator, optionArgument);
            break;
        case dropt_error_underflow:
            s = dropt_asprintf(DROPT_TEXT_LITERAL("Value too small for option %s%s%s"),
                               optionName, separator, optionArgument);
            break;
        case dropt_error_insufficient_memory:
            s = dropt_strdup(DROPT_TEXT_LITERAL("Insufficient memory"));
            break;
        case dropt_error_unknown:
        default:
            s = dropt_asprintf(DROPT_TEXT_LITERAL("Unknown error handling option %s"),
                               optionName);
            break;
    }

    return s;
}


/** dropt_get_help
  *
  * PARAMETERS:
  *     IN context    : The dropt context.
  *                     Must not be `NULL`.
  *     IN helpParams : The help parameters.
  *                     Pass `NULL` to use the default help parameters.
  *
  * RETURNS:
  *     An allocated help string for the available options.  The caller is
  *       responsible for calling `free()` on it when no longer needed.
  *     Returns `NULL` on error.
  */
DROPTDEF dropt_char*
dropt_get_help(const dropt_context* context,
               const dropt_help_params* helpParams)
{
    dropt_char* helpText = NULL;
    dropt_stringstream* ss = dropt_ssopen();

    if (context == NULL)
    {
        DROPT_MISUSE("No dropt context specified.");
    }
    else if (ss != NULL)
    {
        const dropt_option* option;
        dropt_help_params hp;

        if (helpParams == NULL)
        {
            dropt_init_help_params(&hp);
        }
        else
        {
            hp = *helpParams;
        }

        for (option = context->options; is_valid_option(option); option++)
        {
            bool hasLongName =    option->long_name != NULL
                               && option->long_name[0] != DROPT_TEXT_LITERAL('\0');
            bool hasShortName = option->short_name != DROPT_TEXT_LITERAL('\0');

            /* The number of characters printed on the current line so far. */
            int n;

            if (   option->description == NULL
                || (option->attr & dropt_attr_hidden))
            {
                /* Undocumented option.  Ignore it and move on. */
                continue;
            }
            else if (hasLongName && hasShortName)
            {
                n = dropt_ssprintf(ss, DROPT_TEXT_LITERAL("%*s-%c, --%s"),
                                   hp.indent, DROPT_TEXT_LITERAL(""),
                                   option->short_name, option->long_name);
            }
            else if (hasLongName)
            {
                n = dropt_ssprintf(ss, DROPT_TEXT_LITERAL("%*s--%s"),
                                   hp.indent, DROPT_TEXT_LITERAL(""),
                                   option->long_name);
            }
            else if (hasShortName)
            {
                n = dropt_ssprintf(ss, DROPT_TEXT_LITERAL("%*s-%c"),
                                   hp.indent, DROPT_TEXT_LITERAL(""),
                                   option->short_name);
            }
            else
            {
                /* Comment text.  Don't bother with indentation. */
                assert(option->description != NULL);
                dropt_ssprintf(ss, DROPT_TEXT_LITERAL("%s\n"), option->description);
                goto next;
            }

            if (n < 0) { n = 0; }

            if (option->arg_description != NULL)
            {
                int m = dropt_ssprintf(ss,
                                       (option->attr & dropt_attr_optional_val)
                                       ? DROPT_TEXT_LITERAL("[=%s]")
                                       : DROPT_TEXT_LITERAL("=%s"),
                                       option->arg_description);
                if (m > 0) { n += m; }
            }

            /* Check for equality to make sure that there's at least one
             * space between the option name and its description.
             */
            if ((unsigned int) n >= hp.description_start_column)
            {
                dropt_ssprintf(ss, DROPT_TEXT_LITERAL("\n"));
                n = 0;
            }

            {
                const dropt_char* line = option->description;
                while (line != NULL)
                {
                    int lineLen;
                    const dropt_char* nextLine;
                    const dropt_char* newline = dropt_strchr(line, DROPT_TEXT_LITERAL('\n'));

                    if (newline == NULL)
                    {
                        lineLen = dropt_strlen(line);
                        nextLine = NULL;
                    }
                    else
                    {
                        lineLen = newline - line;
                        nextLine = newline + 1;
                    }

                    dropt_ssprintf(ss, DROPT_TEXT_LITERAL("%*s%.*s\n"),
                                   hp.description_start_column - n, DROPT_TEXT_LITERAL(""),
                                   lineLen, line);
                    n = 0;

                    line = nextLine;
                }
            }

        next:
            if (hp.blank_lines_between_options)
            {
                dropt_ssprintf(ss, DROPT_TEXT_LITERAL("\n"));
            }
        }
        helpText = dropt_ssfinalize(ss);
    }

    return helpText;
}


/** dropt_print_help
  *
  *     Prints help for the available options.
  *
  * PARAMETERS:
  *     IN/OUT f      : The file stream to print to.
  *     IN context    : The dropt context.
  *                     Must not be `NULL`.
  *     IN helpParams : The help parameters.
  *                     Pass `NULL` to use the default help parameters.
  */
DROPTDEF void
dropt_print_help(FILE* f, const dropt_context* context,
                 const dropt_help_params* helpParams)
{
    dropt_char* helpText = dropt_get_help(context, helpParams);
    if (helpText != NULL)
    {
        dropt_fputs(helpText, f);
        free(helpText);
    }
}
#endif /* DROPT_NO_STRING_BUFFERS */


/** set_option_value
  *
  *     Sets the value for a specified option by invoking the option's
  *     handler callback.
  *
  * PARAMETERS:
  *     IN/OUT context    : The dropt context.
  *     IN option         : The option.
  *     IN optionArgument : The option's value.  May be `NULL`.
  *
  * RETURNS:
  *     An error code.
  */
static dropt_error
set_option_value(dropt_context* context,
                 const dropt_option* option, const dropt_char* optionArgument)
{
    assert(option != NULL);

    if (option->handler == NULL)
    {
        DROPT_MISUSE("No option handler specified.");
        return dropt_error_bad_configuration;
    }

    return option->handler(context, option, optionArgument,
                           option->dest);
}


/** parse_option_arg
  *
  *     Helper function to `parse_long_option` and `parse_short_option` to
  *     deal with consuming possibly optional arguments.
  *
  * PARAMETERS:
  *     IN/OUT context : The dropt context.
  *     IN/OUT ps      : The current parse state.
  *
  * RETURNS:
  *     An error code.
  */
static dropt_error
parse_option_arg(dropt_context* context, parse_state* ps)
{
    dropt_error err;

    bool consumeNextArg = false;

    if (OPTION_TAKES_ARG(ps->option) && ps->optionArgument == NULL)
    {
        /* The option expects an argument, but none was specified with '='.
         * Try using the next item from the command-line.
         */
        if (ps->argsLeft > 0 && *(ps->argNext) != NULL)
        {
            consumeNextArg = true;
            ps->optionArgument = *(ps->argNext);
        }
        else if (!(ps->option->attr & dropt_attr_optional_val))
        {
            err = dropt_error_insufficient_arguments;
            goto exit;
        }
    }

    /* Even for options that don't ask for arguments, always parse and
     * consume an argument that was specified with '='.
     */
    err = set_option_value(context, ps->option, ps->optionArgument);

    if (   err != dropt_error_none
        && (ps->option->attr & dropt_attr_optional_val)
        && consumeNextArg
        && ps->optionArgument != NULL)
    {
        /* The option's handler didn't like the argument we fed it.  If the
         * argument was optional, try again without it.
         */
        consumeNextArg = false;
        ps->optionArgument = NULL;
        err = set_option_value(context, ps->option, NULL);
    }

exit:
    if (err == dropt_error_none && consumeNextArg)
    {
        ps->argNext++;
        ps->argsLeft--;
    }
    return err;
}


/** parse_long_option
  *
  *     Helper function to `dropt_parse` to deal with consuming a long option.
  *
  * PARAMETERS:
  *     IN/OUT context : The dropt context.
  *     IN/OUT ps      : The current parse state.
  *     IN arg         : The command-line argument with the long option.
  *                      (Examples: "--longName", "--longName=ARGUMENT")
  *
  * RETURNS:
  *     true if parsing should continue, false if it should halt.
  */
static bool
parse_long_option(dropt_context* context, parse_state* ps,
                  const dropt_char* arg)
{
    bool continueParsing = false;

    dropt_error err = dropt_error_none;

    const dropt_char* longName = arg + 2;
    const dropt_char* longNameEnd;
    if (longName[0] == DROPT_TEXT_LITERAL('\0'))
    {
        /* -- */

        /* This is used to mark the end of the option processing
         * to prevent some arguments with leading '-' characters
         * from being treated as options.
         *
         * Don't pass this back to the caller.
         */
        goto exit;
    }

    /* --longName */
    longNameEnd = dropt_strchr(longName, DROPT_TEXT_LITERAL('='));
    if (longNameEnd == longName)
    {
        /* Deal with the pathological case of a user supplying
         * "--=".
         */
        err = dropt_error_invalid_option;
        set_error_details(context, err,
                          make_char_array(arg, dropt_strlen(arg)),
                          NULL);
        goto exit;
    }
    else if (longNameEnd == NULL)
    {
        longNameEnd = longName + dropt_strlen(longName);
        assert(ps->optionArgument == NULL);
    }
    else
    {
        /* --longName=ARGUMENT */
        ps->optionArgument = longNameEnd + 1;
    }

    /* Pass the length of the option name so that we don't need
     * to mutate the original string by inserting a
     * `NUL`-terminator.
     */
    ps->option = find_option_long(context,
                                  make_char_array(longName,
                                                  longNameEnd - longName));
    if (ps->option == NULL)
    {
        err = dropt_error_invalid_option;
        set_error_details(context, err,
                          make_char_array(arg, longNameEnd - arg),
                          NULL);
    }
    else
    {
        err = parse_option_arg(context, ps);
        if (err != dropt_error_none)
        {
            set_error_details(context, err,
                              make_char_array(arg, longNameEnd - arg),
                              ps->optionArgument);
        }
    }

    if (   err != dropt_error_none
        || (ps->option->attr & dropt_attr_halt))
    {
        goto exit;
    }

    continueParsing = true;

exit:
    return continueParsing;
}


/** parse_short_option
  *
  *     Helper function to `dropt_parse` to deal with consuming one or more
  *     short options.
  *
  * PARAMETERS:
  *     IN/OUT context : The dropt context.
  *     IN/OUT ps      : The current parse state.
  *     IN arg         : The command-line argument with one or more short
  *                        options.
  *                      (Examples: "-a", "-abc", "-oARGUMENT", "-o=ARGUMENT")
  *
  * RETURNS:
  *     true if parsing should continue, false if it should halt.
  */
static bool
parse_short_option(dropt_context* context, parse_state* ps,
                   const dropt_char* arg)
{
    bool continueParsing = false;

    dropt_error err = dropt_error_none;

    size_t len;
    size_t j;

    const dropt_char* shortOptionGroup = arg + 1;
    const dropt_char* shortOptionGroupEnd
        = dropt_strchr(shortOptionGroup, DROPT_TEXT_LITERAL('='));

    if (shortOptionGroupEnd == shortOptionGroup)
    {
        /* Deal with the pathological case of a user supplying
         * "-=".
         */
        err = dropt_error_invalid_option;
        set_error_details(context, err,
                          make_char_array(arg, dropt_strlen(arg)),
                          NULL);
        goto exit;
    }
    else if (shortOptionGroupEnd != NULL)
    {
        /* -x=ARGUMENT */
        len = shortOptionGroupEnd - shortOptionGroup;
        ps->optionArgument = shortOptionGroupEnd + 1;
    }
    else
    {
        len = dropt_strlen(shortOptionGroup);
        assert(ps->optionArgument == NULL);
    }

    for (j = 0; j < len; j++)
    {
        ps->option = find_option_short(context, shortOptionGroup[j]);
        if (ps->option == NULL)
        {
            err = dropt_error_invalid_option;
            set_short_option_error_details(context, err,
                                           shortOptionGroup[j], NULL);
            goto exit;
        }
        else if (j + 1 == len)
        {
            /* The last short option in a condensed list gets
             * to use an argument.
             */
            err = parse_option_arg(context, ps);
            if (err != dropt_error_none)
            {
                set_short_option_error_details(context, err,
                                               shortOptionGroup[j],
                                               ps->optionArgument);
                goto exit;
            }
        }
        else if (   context->allowConcatenatedArgs
                 && OPTION_TAKES_ARG(ps->option)
                 && j == 0)
        {
            err = set_option_value(context, ps->option,
                                   &shortOptionGroup[j + 1]);

            if (   err != dropt_error_none
                && (ps->option->attr & dropt_attr_optional_val))
            {
                err = set_option_value(context, ps->option, NULL);
            }

            if (err != dropt_error_none)
            {
                set_short_option_error_details(context, err,
                                               shortOptionGroup[j],
                                               &shortOptionGroup[j + 1]);
                goto exit;
            }

            /* Skip to the next argument. */
            break;
        }
        else if (   OPTION_TAKES_ARG(ps->option)
                 && !(ps->option->attr & dropt_attr_optional_val))
        {
            /* Short options with required arguments can't be used
             * in condensed lists except in the last position.
             *
             * e.g. -abcd ARGUMENT
             *          ^
             */
            err = dropt_error_insufficient_arguments;
            set_short_option_error_details(context, err,
                                           shortOptionGroup[j], NULL);
            goto exit;
        }
        else
        {
            err = set_option_value(context, ps->option, NULL);
            if (err != dropt_error_none)
            {
                set_short_option_error_details(context, err,
                                               shortOptionGroup[j],
                                               NULL);
                goto exit;
            }
        }

        if (ps->option->attr & dropt_attr_halt) { goto exit; }
    }

    assert(err == dropt_error_none);
    continueParsing = true;

exit:
    return continueParsing;
}


/** dropt_parse
  *
  *     Parses command-line options.
  *
  * PARAMETERS:
  *     IN/OUT context : The dropt context.
  *                      Must not be `NULL`.
  *     IN argc        : The maximum number of arguments to parse from argv.
  *                      Pass -1 to parse all arguments up to a `NULL` sentinel
  *                        value.
  *     IN argv        : The list of command-line arguments, not including the
  *                        initial program name.
  *
  * RETURNS:
  *     A pointer to the first unprocessed element in `argv`.
  */
DROPTDEF dropt_char**
dropt_parse(dropt_context* context,
            int argc, dropt_char** argv)
{
    dropt_char* arg;
    parse_state ps;

    ps.option = NULL;
    ps.optionArgument = NULL;
    ps.argNext = argv;

    if (argv == NULL)
    {
        /* Nothing to do. */
        goto exit;
    }

    if (context == NULL)
    {
        DROPT_MISUSE("No dropt context specified.");
        set_error_details(context, dropt_error_bad_configuration,
                          make_char_array(DROPT_TEXT_LITERAL(""), 0),
                          NULL);
        goto exit;
    }

#ifdef DROPT_NO_STRING_BUFFERS
    if (context->errorHandler == NULL)
    {
        DROPT_MISUSE("No error handler specified.");
        set_error_details(context, dropt_error_bad_configuration,
                          make_char_array(DROPT_TEXT_LITERAL(""), 0),
                          NULL);
        goto exit;
    }
#endif

    if (argc == -1)
    {
        argc = 0;
        while (argv[argc] != NULL) { argc++; }
    }

    if (argc == 0)
    {
        /* Nothing to do. */
        goto exit;
    }

    init_lookup_tables(context);

    ps.argsLeft = argc;

    while (   ps.argsLeft-- > 0
           && (arg = *ps.argNext) != NULL
           && arg[0] == DROPT_TEXT_LITERAL('-'))
    {
        if (arg[1] == DROPT_TEXT_LITERAL('\0'))
        {
            /* - */

            /* This intentionally leaves "-" unprocessed for the caller to
             * deal with.  This allows construction of programs that treat
             * "-" to mean `stdin`.
             */
            goto exit;
        }

        ps.argNext++;

        if (arg[1] == DROPT_TEXT_LITERAL('-'))
        {
            if (!parse_long_option(context, &ps, arg)) { goto exit; }
        }
        else
        {
            /* Short name. (-x) */
            if (!parse_short_option(context, &ps, arg)) { goto exit; }
        }

        ps.option = NULL;
        ps.optionArgument = NULL;
    }

exit:
    return ps.argNext;
}


/** dropt_new_context
  *
  *     Creates a new dropt context.
  *
  * PARAMETERS:
  *     IN options : The list of option specifications.
  *                  Must not be `NULL`.
  *                  The list is *not* copied and must outlive the dropt
  *                    context.
  *
  * RETURNS:
  *     An allocated dropt context.  The caller is responsible for freeing
  *       it with `dropt_free_context` when no longer needed.
  *     Returns `NULL` on error.
  */
DROPTDEF dropt_context*
dropt_new_context(const dropt_option* options)
{
    dropt_context* context = NULL;
    size_t n;

    if (options == NULL)
    {
        DROPT_MISUSE("No option list specified.");
        goto exit;
    }

    /* Sanity-check the options. */
    for (n = 0; is_valid_option(&options[n]); n++)
    {
        if (   options[n].short_name == DROPT_TEXT_LITERAL('=')
            || (   options[n].long_name != NULL
                && dropt_strchr(options[n].long_name, DROPT_TEXT_LITERAL('=')) != NULL))
        {
            DROPT_MISUSE("Invalid option list. '=' may not be used in an option name.");
            goto exit;
        }
    }

    context = malloc(sizeof *context);
    if (context == NULL)
    {
        goto exit;
    }
    else
    {
        dropt_context emptyContext = { 0 };
        *context = emptyContext;

        context->options = options;
        context->numOptions = n;
        dropt_set_strncmp(context, NULL);
    }

exit:
    return context;
}


/** dropt_free_context
  *
  *     Frees a dropt context.
  *
  * PARAMETERS:
  *     IN/OUT context : The dropt context to free.
  *                      May be `NULL`.
  */
DROPTDEF void
dropt_free_context(dropt_context* context)
{
    dropt_clear_error(context);
    free_lookup_tables(context);
    free(context);
}


/** dropt_get_options
  *
  * PARAMETERS:
  *     IN context : The dropt context.
  *                  Must not be `NULL`.
  *
  * RETURNS:
  *     The context's list of option specifications.
  */
DROPTDEF const dropt_option*
dropt_get_options(const dropt_context* context)
{
    if (context == NULL)
    {
        DROPT_MISUSE("No dropt context specified.");
        return NULL;
    }

    return context->options;
}


/** dropt_init_help_params
  *
  *     Initializes a `dropt_help_params` structure with the default values.
  *
  * PARAMETERS:
  *     OUT helpParams : On output, set to the default help parameters.
  *                      Must not be `NULL`.
  */
DROPTDEF void
dropt_init_help_params(dropt_help_params* helpParams)
{
    if (helpParams == NULL)
    {
        DROPT_MISUSE("No dropt help parameters specified.");
        return;
    }

    helpParams->indent = default_help_indent;
    helpParams->description_start_column = default_description_start_column;
    helpParams->blank_lines_between_options = true;
}


/** dropt_set_error_handler
  *
  *     Sets the callback function used to generate error strings from error
  *     codes.
  *
  * PARAMETERS:
  *     IN/OUT context : The dropt context.
  *                      Must not be `NULL`.
  *     IN handler     : The error handler callback.
  *                      Pass `NULL` to use the default error handler.
  *     IN handlerData : Caller-defined callback data.
  */
DROPTDEF void
dropt_set_error_handler(dropt_context* context,
                        dropt_error_handler_func handler,
                        void* handlerData)
{
    if (context == NULL)
    {
        DROPT_MISUSE("No dropt context specified.");
        return;
    }

    context->errorHandler = handler;
    context->errorHandlerData = handlerData;
}


/** dropt_set_strncmp
  *
  *     Sets the callback function used to compare strings.
  *
  * PARAMETERS:
  *     IN/OUT context : The dropt context.
  *                      Must not be `NULL`.
  *     IN cmp         : The string comparison function.
  *                      Pass `NULL` to use the default string comparison
  *                        function.
  */
DROPTDEF void
dropt_set_strncmp(dropt_context* context, dropt_strncmp_func cmp)
{
    if (context == NULL)
    {
        DROPT_MISUSE("No dropt context specified.");
        return;
    }

    if (cmp == NULL) { cmp = dropt_strncmp; }
    context->ncmpstr = cmp;

    /* Changing the sort method invalidates our existing lookup tables. */
    free_lookup_tables(context);
}


/** dropt_allow_concatenated_arguments
  *
  *     Specifies whether "short" options are allowed to have concatenated
  *     arguments (i.e. without space or "=" separators, such as "-oARGUMENT").
  *
  *     (Concatenated arguments are disallowed by default.)
  *
  * PARAMETERS:
  *     IN/OUT context : The dropt context.
  *     IN allow       : Pass 1 if concatenated arguments should be allowed,
  *                        0 otherwise.
  */
DROPTDEF void
dropt_allow_concatenated_arguments(dropt_context* context, dropt_bool allow)
{
    if (context == NULL)
    {
        DROPT_MISUSE("No dropt context specified.");
        return;
    }

    context->allowConcatenatedArgs = (allow != 0);
}


/** dropt_misuse
  *
  *     Prints a diagnostic for logical errors caused by external clients
  *     calling into dropt improperly.
  *
  *     In debug builds, terminates the program and prints the filename and
  *     line number of the failure.
  *
  *     For logical errors entirely internal to dropt, use `assert()`
  *     instead.
  *
  * PARAMETERS:
  *     IN message  : The error message.
  *                   Must not be `NULL`.
  *     IN filename : The name of the file where the logical error occurred.
  *                   Must not be `NULL`.
  *     IN line     : The line number where the logical error occurred.
  */
DROPTDEF void
dropt_misuse(const char* message, const char* filename, int line)
{
#ifdef NDEBUG
    fprintf(stderr, "dropt: %s\n", message);
#else
    fprintf(stderr, "dropt: %s (%s: %d)\n", message, filename, line);
    abort();
#endif
}

/* ===== dropt_handlers.c ===== */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <float.h>
#include <errno.h>
#include <assert.h>



#define CONCAT(s, t) s ## t
#define XCONCAT(s, t) CONCAT(s, t)

#define STATIC_ASSERT(cond) \
  enum { XCONCAT(static_assert_line_,  __LINE__) = 1 / ((cond) != 0) }

#define ABS(x) (((x) < 0) ? -(x) : (x))

#if __STDC_VERSION__ >= 199901L
    #include <stdbool.h>
#else
    typedef enum { false, true } bool;
#endif


/** dropt_handle_bool
  *
  *     Stores a boolean value parsed from the given string if possible.
  *
  * PARAMETERS:
  *     IN/OUT context    : The options context.
  *     IN option         : The matched option.  For more information, see
  *                         `dropt_option_handler_decl`.
  *     IN optionArgument : A string representing a boolean value (0 or 1).
  *                         If `NULL`, the boolean value is assumed to be true.
  *     OUT dest          : A `dropt_bool*`.
  *                         On success, set to the interpreted boolean value.
  *                         On error, left untouched.
  *
  * RETURNS:
  *     dropt_error_none
  *     dropt_error_unknown
  *     dropt_error_bad_configuration
  *     dropt_error_mismatch
  */
DROPTDEF dropt_error
dropt_handle_bool(dropt_context* context,
                  const dropt_option* option,
                  const dropt_char* optionArgument,
                  void* dest)
{
    dropt_error err = dropt_error_none;
    bool val = false;
    dropt_bool* out = dest;

    if (out == NULL)
    {
        DROPT_MISUSE("No handler destination specified.");
        err = dropt_error_bad_configuration;
    }
    else if (optionArgument == NULL)
    {
        /* No explicit argument implies that the option is being turned on. */
        val = true;
    }
    else if (optionArgument[0] == DROPT_TEXT_LITERAL('\0'))
    {
        err = dropt_error_mismatch;
    }
    else
    {
        unsigned int i = 0;
        err = dropt_handle_uint(context, option, optionArgument, &i);
        if (err == dropt_error_none)
        {
            switch (i)
            {
                case 0:
                    val = false;
                    break;
                case 1:
                    val = true;
                    break;
                default:
                    err = dropt_error_mismatch;
                    break;
            }
        }
        else if (err == dropt_error_overflow)
        {
            err = dropt_error_mismatch;
        }
    }

    if (err == dropt_error_none) { *out = val; }
    return err;
}


/** dropt_handle_verbose_bool
  *
  *     Like `dropt_handle_bool` but accepts "true" and "false" string values.
  *
  * PARAMETERS:
  *     IN/OUT context    : The options context.
  *     IN option         : The matched option.  For more information, see
  *                         `dropt_option_handler_decl`.
  *     IN optionArgument : A string representing a boolean value.
  *                         If `NULL`, the boolean value is assumed to be true.
  *     OUT dest          : A `dropt_bool*`.
  *                         On success, set to the interpreted boolean value.
  *                         On error, left untouched.
  *
  * RETURNS:
  *     See dropt_handle_bool.
  */
DROPTDEF dropt_error
dropt_handle_verbose_bool(dropt_context* context,
                          const dropt_option* option,
                          const dropt_char* optionArgument,
                          void* dest)
{
    dropt_error err = dropt_handle_bool(context, option, optionArgument, dest);
    if (err == dropt_error_mismatch)
    {
        bool val = false;
        dropt_bool* out = dest;

        /* `dropt_handle_bool` already checks for this. */
        assert(out != NULL);

        if (dropt_stricmp(optionArgument, DROPT_TEXT_LITERAL("false")) == 0)
        {
            val = false;
            err = dropt_error_none;
        }
        else if (dropt_stricmp(optionArgument, DROPT_TEXT_LITERAL("true")) == 0)
        {
            val = true;
            err = dropt_error_none;
        }

        if (err == dropt_error_none) { *out = val; }
    }
    return err;
}


/** dropt_handle_int
  *
  *     Stores an integer parsed from the given string.
  *
  * PARAMETERS:
  *     IN/OUT context    : The options context.
  *     IN option         : The matched option.  For more information, see
  *                         `dropt_option_handler_decl`.
  *     IN optionArgument : A string representing a base-10 integer.
  *                         If `NULL`, returns
  *                           `dropt_error_insufficient_arguments`.
  *     OUT dest          : An `int*`.
  *                         On success, set to the interpreted integer.
  *                         On error, left untouched.
  *
  * RETURNS:
  *     dropt_error_none
  *     dropt_error_unknown
  *     dropt_error_bad_configuration
  *     dropt_error_insufficient_arguments
  *     dropt_error_mismatch
  *     dropt_error_overflow
  */
DROPTDEF dropt_error
dropt_handle_int(dropt_context* context,
                 const dropt_option* option,
                 const dropt_char* optionArgument,
                 void* dest)
{
    dropt_error err = dropt_error_none;
    int val = 0;
    int* out = dest;

    if (out == NULL)
    {
        DROPT_MISUSE("No handler destination specified.");
        err = dropt_error_bad_configuration;
    }
    else if (   optionArgument == NULL
             || optionArgument[0] == DROPT_TEXT_LITERAL('\0'))
    {
        err = dropt_error_insufficient_arguments;
    }
    else
    {
        dropt_char* end;
        long n;
        errno = 0;
        n = dropt_strtol(optionArgument, &end, 10);

        /* Check that we matched at least one digit.
         * (`strtol`/`strtoul` will return 0 if fed a string with no digits.)
         */
        if (*end == DROPT_TEXT_LITERAL('\0') && end > optionArgument)
        {
            if (errno == ERANGE || n < INT_MIN || n > INT_MAX)
            {
                err = dropt_error_overflow;
                val = (n < 0) ? INT_MIN : INT_MAX;
            }
            else if (errno == 0)
            {
                val = (int) n;
            }
            else
            {
                err = dropt_error_unknown;
            }
        }
        else
        {
            err = dropt_error_mismatch;
        }
    }

    if (err == dropt_error_none) { *out = val; }
    return err;
}


/** dropt_handle_uint
  *
  *     Stores an unsigned integer parsed from the given string.
  *
  * PARAMETERS:
  *     IN/OUT context    : The options context.
  *     IN option         : The matched option.  For more information, see
  *                         `dropt_option_handler_decl`.
  *     IN optionArgument : A string representing an unsigned base-10 integer.
  *                         If `NULL`, returns
  *                           `dropt_error_insufficient_arguments`.
  *     IN option         : The matched option.  For more information, see
  *                         dropt_option_handler_decl.
  *     OUT dest          : An `unsigned int*`.
  *                         On success, set to the interpreted integer.
  *                         On error, left untouched.
  *
  * RETURNS:
  *     dropt_error_none
  *     dropt_error_unknown
  *     dropt_error_bad_configuration
  *     dropt_error_insufficient_arguments
  *     dropt_error_mismatch
  *     dropt_error_overflow
  */
DROPTDEF dropt_error
dropt_handle_uint(dropt_context* context,
                  const dropt_option* option,
                  const dropt_char* optionArgument,
                  void* dest)
{
    dropt_error err = dropt_error_none;
    int val = 0;
    unsigned int* out = dest;

    if (out == NULL)
    {
        DROPT_MISUSE("No handler destination specified.");
        err = dropt_error_bad_configuration;
    }
    else if (   optionArgument == NULL
             || optionArgument[0] == DROPT_TEXT_LITERAL('\0'))
    {
        err = dropt_error_insufficient_arguments;
    }
    else if (optionArgument[0] == DROPT_TEXT_LITERAL('-'))
    {
        err = dropt_error_mismatch;
    }
    else
    {
        dropt_char* end;
        unsigned long n;
        errno = 0;
        n = dropt_strtoul(optionArgument, &end, 10);

        /* Check that we matched at least one digit.
         * (`strtol`/`strtoul` will return 0 if fed a string with no digits.)
         */
        if (*end == DROPT_TEXT_LITERAL('\0') && end > optionArgument)
        {
            if (errno == ERANGE || n > UINT_MAX)
            {
                err = dropt_error_overflow;
                val = UINT_MAX;
            }
            else if (errno == 0)
            {
                val = (unsigned int) n;
            }
            else
            {
                err = dropt_error_unknown;
            }
        }
        else
        {
            err = dropt_error_mismatch;
        }
    }

    if (err == dropt_error_none) { *out = val; }
    return err;
}


/** dropt_handle_double
  *
  *     Stores a `double` parsed from the given string.
  *
  * PARAMETERS:
  *     IN/OUT context    : The options context.
  *     IN option         : The matched option.  For more information, see
  *                         `dropt_option_handler_decl`.
  *     IN optionArgument : A string representing a base-10 floating-point
  *                           number.
  *                         If `NULL`, returns
  *                           `dropt_error_insufficient_arguments`.
  *     OUT dest          : A `double*`.
  *                         On success, set to the interpreted `double`.
  *                         On error, left untouched.
  *
  * RETURNS:
  *     dropt_error_none
  *     dropt_error_unknown
  *     dropt_error_bad_configuration
  *     dropt_error_insufficient_arguments
  *     dropt_error_mismatch
  *     dropt_error_overflow
  *     dropt_error_underflow
  */
DROPTDEF dropt_error
dropt_handle_double(dropt_context* context,
                    const dropt_option* option,
                    const dropt_char* optionArgument,
                    void* dest)
{
    dropt_error err = dropt_error_none;
    double val = 0.0;
    double* out = dest;

    if (out == NULL)
    {
        DROPT_MISUSE("No handler destination specified.");
        err = dropt_error_bad_configuration;
    }
    else if (   optionArgument == NULL
             || optionArgument[0] == DROPT_TEXT_LITERAL('\0'))
    {
        err = dropt_error_insufficient_arguments;
    }
    else
    {
        dropt_char* end;
        errno = 0;
        val = dropt_strtod(optionArgument, &end);

        /* Check that we matched at least one digit.
         * (`strtod` will return 0 if fed a string with no digits.)
         */
        if (*end == DROPT_TEXT_LITERAL('\0') && end > optionArgument)
        {
            if (errno == ERANGE)
            {
                /* Note that setting `errno` to `ERANGE` for underflow errors
                 * is implementation-defined behavior, but glibc, BSD's
                 * libc, and Microsoft's CRT all have implementations of
                 * `strtod` documented to return 0 and to set `errno` to
                 * `ERANGE` for such cases.
                 */
                err = (ABS(val) <= DBL_MIN)
                      ? dropt_error_underflow
                      : dropt_error_overflow;
            }
            else if (errno != 0)
            {
                err = dropt_error_unknown;
            }
        }
        else
        {
            err = dropt_error_mismatch;
        }
    }

    if (err == dropt_error_none) { *out = val; }
    return err;
}


/** dropt_handle_string
  *
  *     Stores a string.
  *
  * PARAMETERS:
  *     IN/OUT context    : The options context.
  *     IN option         : The matched option.  For more information, see
  *                         `dropt_option_handler_decl`.
  *     IN optionArgument : A string.
  *                         If `NULL`, returns
  *                           `dropt_error_insufficient_arguments`.
  *     OUT dest          : A `dropt_char**`.
  *                         On success, set to the input string.  The string is
  *                           NOT copied from the original `argv` array, so do
  *                           not free it.
  *                         On error, left untouched.
  *
  * RETURNS:
  *     dropt_error_none
  *     dropt_error_bad_configuration
  *     dropt_error_insufficient_arguments
  */
DROPTDEF dropt_error
dropt_handle_string(dropt_context* context,
                    const dropt_option* option,
                    const dropt_char* optionArgument,
                    void* dest)
{
    dropt_error err = dropt_error_none;
    const dropt_char** out = dest;

    if (out == NULL)
    {
        DROPT_MISUSE("No handler destination specified.");
        err = dropt_error_bad_configuration;
    }
    else if (optionArgument == NULL)
    {
        err = dropt_error_insufficient_arguments;
    }

    if (err == dropt_error_none) { *out = optionArgument; }
    return err;
}


/** dropt_handle_const
  *
  *     Stores a predefined value.  This can be used to set a single variable
  *     to different values in response to different boolean-like command-line
  *     options.
  *
  * PARAMETERS:
  *     IN/OUT context    : The options context.
  *     IN option         : The matched option.  For more information, see
  *                         `dropt_option_handler_decl`.
  *     IN optionArgument : Must be `NULL`.
  *     OUT dest          : A `dropt_uintptr*`.
  *                         On success, set to the constant value specified by
  *                           `option->extra_data`.
  *                         On error, left untouched.
  *
  * RETURNS:
  *     dropt_error_none
  *     dropt_error_bad_configuration
  *     dropt_error_mismatch
  */

DROPTDEF dropt_error
dropt_handle_const(dropt_context* context,
                   const dropt_option* option,
                   const dropt_char* optionArgument,
                   void* dest)
{
    dropt_error err = dropt_error_none;
    dropt_uintptr* out = dest;

    STATIC_ASSERT(sizeof (dropt_uintptr) >= sizeof (void*));

    if (out == NULL)
    {
        DROPT_MISUSE("No handler destination specified.");
        err = dropt_error_bad_configuration;
    }
    else if (option == NULL)
    {
        DROPT_MISUSE("No option entry given.");
        err = dropt_error_bad_configuration;
    }
    else if (optionArgument != NULL)
    {
        err = dropt_error_mismatch;
    }

    if (err == dropt_error_none) { *out = option->extra_data; }
    return err;
}

#endif /* DROPT_IMPLEMENTATION_INCLUDED */
#endif /* DROPT_IMPLEMENTATION */
