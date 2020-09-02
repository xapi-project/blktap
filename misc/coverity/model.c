/* Coverity Scan model
 *
 * This is a modelling file for Coverity Scan. Modelling helps to avoid false
 * positives.
 *
 * - A model file can't import any header files.
 * - Therefore only some built-in primitives like int, char and void are
 *   available but not NULL etc.
 * - Modelling doesn't need full structs and typedefs. Rudimentary structs
 *   and similar types are sufficient.
 * - An uninitialised local pointer is not an error. It signifies that the
 *   variable could be either NULL or have some data.
 *
 * Coverity Scan doesn't pick up modifications automatically. The model file
 * must be uploaded by an admin in the analysis.
 *
 * The Xen Coverity Scan modelling file used the cpython modelling file as a
 * reference to get started (suggested by Coverty Scan themselves as a good
 * example), but all content is Xen specific.
 *
 * Copyright (c) 2013-2014 Citrix Systems Ltd; All Right Reserved
 *
 * Based on:
 *     http://hg.python.org/cpython/file/tip/Misc/coverity_model.c
 * Copyright (c) 2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010,
 * 2011, 2012, 2013 Python Software Foundation; All Rights Reserved
 *
 */

/*
 * Useful references:
 *   https://scan.coverity.com/models
 */

/* Definitions */
#define NULL (void *)0

#define assert(cond) /* empty */

typedef void* va_list;

int asprintf(char **strp, const char *fmt, ...)
{
    char ch1;
    int success;
    unsigned int total_bytes_printed;

    /* fmt must be NUL terminated, and reasonably bounded */
    __coverity_string_null_sink__((void*)fmt);
    __coverity_string_size_sink__((void*)fmt);

    /* Reads fmt */
    ch1 = *fmt;

    if ( success )
    {
        /* Allocates a string.  Exact size is not calculable */
        char *str = __coverity_alloc_nosize__();

        /* Should be freed with free() */
        __coverity_mark_as_afm_allocated__(str, AFM_free);

        /* Returns memory via first parameter */
        *strp = str;

        /* Writes to all of the allocated string */
        __coverity_writeall__(str);

        /* Returns a positive number of bytes printed on success */
        return total_bytes_printed;
    }
    else
    {
        /* Return -1 on failure */
        return -1;
    }
}

int vasprintf(char **strp, const char *fmt, va_list ap)
{
    char ch1;
    int success;
    unsigned int total_bytes_printed;

    /* fmt must be NUL terminated, and reasonably bounded */
    __coverity_string_null_sink__((void*)fmt);
    __coverity_string_size_sink__((void*)fmt);

    /* Reads fmt */
    ch1 = *fmt;

    /* Reads ap */
    ch1 = *(char*)ap;

    if ( success )
    {
        /* Allocates a string.  Exact size is not calculable */
        char *str = __coverity_alloc_nosize__();

        /* Should be freed with free() */
        __coverity_mark_as_afm_allocated__(str, AFM_free);

        /* Returns memory via first parameter */
        *strp = str;

        /* Writes to all of the allocated string */
        __coverity_writeall__(str);

        /* Returns a positive number of bytes printed on success */
        return total_bytes_printed;
    }
    else
    {
        /* Return -1 on failure */
        return -1;
    }
}

int posix_memalign (void **__memptr, size_t __alignment, size_t __size)
{
    *__memptr = __coverity_alloc__(__size);
    return 0;
}


/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
