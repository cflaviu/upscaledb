/**
 * Copyright (C) 2005-2008 Christoph Rupp (chris@crupp.de).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or 
 * (at your option) any later version.
 *
 * See files COPYING.* for License information.
 *
 */

#include "config.h"

#include <string.h>

#ifdef HAVE_MALLOC_H
#  include <malloc.h>
#else
#  include <stdlib.h>
#endif

#include "mem.h"
#include "error.h"
#include "db.h"

#if defined(_MSC_VER) && defined(HAM_DEBUG) && !defined(UNDER_CE)
#   define _CRTDBG_MAP_ALLOC
#   include <crtdbg.h>
#endif

void *
alloc_impl(mem_allocator_t *self, const char *file, int line, ham_u32_t size)
{
    (void)self;
    (void)file;
    (void)line;

#if defined(_CRTDBG_MAP_ALLOC)
    return (_malloc_dbg(size, _NORMAL_BLOCK, file, line));
#else
    return (malloc(size));
#endif
}

void 
free_impl(mem_allocator_t *self, const char *file, int line, void *ptr)
{
    (void)self;
    (void)file;
    (void)line;

    ham_assert(ptr, ("freeing NULL pointer in line %s:%d", file, line));
#if defined(_CRTDBG_MAP_ALLOC)
    _free_dbg(ptr, _NORMAL_BLOCK);
#else
    free(ptr);
#endif
}

void *
realloc_impl(mem_allocator_t *self, const char *file, int line, 
        void *ptr, ham_size_t size)
{
    (void)self;
    (void)file;
    (void)line;

#if defined(_CRTDBG_MAP_ALLOC)
    return (_realloc_dbg(ptr, size, _NORMAL_BLOCK, file, line));
#else
    return (realloc(ptr, size));
#endif
}

void 
close_impl(mem_allocator_t *self)
{
}

mem_allocator_t *
ham_default_allocator_new(void)
{
    static mem_allocator_t m;

    m.alloc  =alloc_impl;
    m.free   =free_impl;
    m.realloc=realloc_impl;
    m.close  =close_impl;
    m.priv   =0;
     
    return (&m);
}

void *
ham_mem_calloc(ham_db_t *db, ham_size_t size)
{
    void *p;

    if (!(p=ham_mem_alloc(db, size)))
        return (0);

    memset(p, 0, size);
    return (p);
}

void *
ham_mem_calloc_env(ham_env_t *env, ham_size_t size)
{
    void *p;

    p=allocator_alloc(env_get_allocator(env), size);
    if (!p)
        return (0);

    memset(p, 0, size);
    return (p);
}

