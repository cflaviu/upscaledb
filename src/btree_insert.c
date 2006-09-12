/**
 * Copyright 2005, 2006 Christoph Rupp (chris@crupp.de)
 * see file LICENSE for license and copyright information
 *
 * btree inserting
 *
 */

#include <string.h>
#include <ham/config.h>
#include "db.h"
#include "error.h"
#include "keys.h"
#include "page.h"
#include "btree.h"
#include "blob.h"
#include "mem.h"
#include "util.h"

/*
 * the insert_scratchpad_t structure helps us to propagate return values
 * from the bottom of the tree to the root.
 */
typedef struct
{
    /*
     * the backend pointer
     */
    ham_btree_t *be;

    /*
     * the flags of the ham_insert()-call
     */
    ham_u32_t flags;

    /*
     * the transaction object
     */
    ham_txn_t *txn;

    /*
     * the record which is inserted
     */
    ham_record_t *record;

    /*
     * a key; this is used to propagate SMOs (structure modification
     * operations) from a child page to a parent page
     */
    ham_key_t key;

    /*
     * a RID; this is used to propagate SMOs (structure modification
     * operations) from a child page to a parent page
     */
    ham_offset_t rid;

} insert_scratchpad_t;

/*
 * return values
 */
#define SPLIT     1

/*
 * flags for my_insert_nosplit()
 */
#define NOFLUSH   1
#define OVERWRITE 2

/*
 * this is the function which does most of the work - traversing to a 
 * leaf, inserting the key using my_insert_in_page() 
 * and performing necessary SMOs. it works recursive.
 */
static ham_status_t
my_insert_recursive(ham_page_t *page, ham_key_t *key, 
        ham_offset_t rid, insert_scratchpad_t *scratchpad);

/*
 * this function inserts a key in a page
 */
static ham_status_t
my_insert_in_page(ham_page_t *page, ham_key_t *key, 
        ham_offset_t rid, ham_u32_t flags, 
        insert_scratchpad_t *scratchpad);

/*
 * insert a key in a page; the page MUST have free slots
 */
ham_status_t
my_insert_nosplit(ham_page_t *page, ham_txn_t *txn, ham_key_t *key, 
        ham_offset_t rid, ham_record_t *record, ham_u32_t flags);

/*
 * split a page and insert the new element
 */
static ham_status_t
my_insert_split(ham_page_t *page, ham_key_t *key, 
        ham_offset_t rid, ham_u32_t flags, 
        insert_scratchpad_t *scratchpad);


ham_status_t
btree_insert(ham_btree_t *be, ham_txn_t *txn, ham_key_t *key, 
        ham_record_t *record, ham_u32_t flags)
{
    ham_status_t st;
    ham_page_t *root;
    ham_db_t *db=btree_get_db(be);
    insert_scratchpad_t scratchpad;

    /* 
     * initialize the scratchpad 
     */
    memset(&scratchpad, 0, sizeof(scratchpad));
    scratchpad.be=be;
    scratchpad.txn=txn;
    scratchpad.flags=flags;
    scratchpad.record=record;

    /* 
     * get the root-page...
     */
    ham_assert(btree_get_rootpage(be)!=0, 0, 0);
    root=db_fetch_page(db, scratchpad.txn, btree_get_rootpage(be), 0);

    /* 
     * ... and start the recursion 
     */
    st=my_insert_recursive(root, key, 0, &scratchpad);

    /*
     * if the root page was split, we have to create a new
     * root page.
     */
    if (st==SPLIT) {
        ham_page_t *newroot;
        btree_node_t *node;

        /*
         * allocate a new root page
         */
        newroot=db_alloc_page(db, PAGE_TYPE_ROOT, txn, 0); 
        if (!newroot)
            return (db_get_error(db));
        page_set_type(newroot, PAGE_TYPE_ROOT);

        /* 
         * insert the pivot element and the ptr_left
         */ 
        node=ham_page_get_btree_node(newroot);
        btree_node_set_ptr_left(node, btree_get_rootpage(be));
        st=my_insert_nosplit(newroot, scratchpad.txn, &scratchpad.key, 
                scratchpad.rid, scratchpad.record, NOFLUSH);
        if (st) {
            if (scratchpad.key.data)
                ham_mem_free(scratchpad.key.data);
            return (st);
        }

        /*
         * set the new root page
         *
         * !!
         * do NOT delete the old root page - it's still in use!
         */
        btree_set_rootpage(be, page_get_self(newroot));
        db_set_dirty(db, 1);
        page_set_type(root, PAGE_TYPE_INDEX);
    }

    /*
     * release the scratchpad-memory and return to caller
     */
    if (scratchpad.key.data)
        ham_mem_free(scratchpad.key.data);

    return (st);
}

static ham_status_t
my_insert_recursive(ham_page_t *page, ham_key_t *key, 
        ham_offset_t rid, insert_scratchpad_t *scratchpad)
{
    ham_status_t st;
    ham_page_t *child;
    ham_db_t *db=page_get_owner(page);
    btree_node_t *node=ham_page_get_btree_node(page);

    /*
     * if we've reached a leaf: insert the key
     */
    if (btree_node_is_leaf(node)) 
        return (my_insert_in_page(page, key, rid, 0, scratchpad));

    /*
     * otherwise traverse the root down to the leaf
     */
    child=btree_traverse_tree(db, scratchpad->txn, page, key, 0);
    ham_assert(child!=0, "guru meditation error", 0);

    /*
     * and call this function recursively
     */
    st=my_insert_recursive(child, key, rid, scratchpad);
    switch (st) {
        /*
         * if we're done, we're done
         */
        case HAM_SUCCESS:
            break;

        /*
         * if we tried to insert a duplicate key, we're done, too
         */
        case HAM_DUPLICATE_KEY:
            break;

        /*
         * the child was split, and we have to insert a new (key/rid)-tuple.
         */
        case SPLIT:
            st=my_insert_in_page(page, &scratchpad->key, 
                        scratchpad->rid, OVERWRITE, scratchpad);
            break;

        /*
         * every other return value is unexpected and shouldn't happen
         */
        default:
            db_set_error(db, st);
            break;
    }

    return (st);
}

static ham_status_t
my_insert_in_page(ham_page_t *page, ham_key_t *key, 
        ham_offset_t rid, ham_u32_t flags, 
        insert_scratchpad_t *scratchpad)
{
    ham_size_t maxkeys=btree_get_maxkeys(scratchpad->be);
    btree_node_t *node=ham_page_get_btree_node(page);

    ham_assert(maxkeys>1, "invalid result of db_get_maxkeys(): %d", maxkeys);

    /*
     * if we can insert the new key without splitting the page: 
     * my_insert_nosplit() will do the work for us
     */
    if (btree_node_get_count(node)<maxkeys) 
        return (my_insert_nosplit(page, scratchpad->txn, key, rid, 
                    scratchpad->record, flags));

    /*
     * otherwise, we have to split the page.
     * but BEFORE we split, we check if the key already exists!
     */
    if (btree_node_is_leaf(node)) {
        if (btree_node_search_by_key(page_get_owner(page), page, key)>=0) {
            if (flags&OVERWRITE) 
                return (my_insert_nosplit(page, scratchpad->txn, key, rid, 
                        scratchpad->record, flags));
            else
                return (HAM_DUPLICATE_KEY);
        }
    }

    return (my_insert_split(page, key, rid, flags, scratchpad));
}

ham_status_t
my_insert_nosplit(ham_page_t *page, ham_txn_t *txn, ham_key_t *key, 
        ham_offset_t rid, ham_record_t *record, ham_u32_t flags)
{
    int cmp;
    ham_size_t i, count, keysize;
    key_t *bte=0;
    btree_node_t *node;
    ham_db_t *db=page_get_owner(page);

    node=ham_page_get_btree_node(page);
    count=btree_node_get_count(node);
    keysize=db_get_keysize(db);

    /*
     * TODO this is subject to optimization...
     */
    for (i=0; i<count; i++) {
        bte=btree_node_get_key(db, node, i);

        cmp=key_compare_int_to_pub(page, i, key);
        if (db_get_error(db))
            return (db_get_error(db));

        /*
         * key exists already
         */
        if (cmp==0) {
            if (flags&OVERWRITE) {
                /* 
                 * TODO no need to overwrite the key - it already exists! 
                 * ATTENTION with extended keys! need to be overwritten, too 
                key_set_key(bte, key->data, keysize);
                btree_entry_set_size(bte, key->size);
                page_set_dirty(page, 1);
                 */
                return (HAM_SUCCESS);
            }
            else
                return (HAM_DUPLICATE_KEY);
        }

        /*
         * we found the first key which is > then the new key
         */
        if (cmp>0) {
            /* shift all keys one position to the right */
            memmove(((char *)bte)+sizeof(key_t)-1+keysize, bte,
                    (sizeof(key_t)-1+keysize)*(count-i));
            break;
        }

    }

    if (i==count)
        bte=btree_node_get_key(db, node, count);

    /*
     * if we're in the leaf: insert the blob, and append the blob-id 
     * in this node; otherwise just append the entry (rid)
     *
     * if the record's size is <= sizeof(ham_offset_t), we don't allocate
     * a blob but store the record data directly in the offset
     *
     * in an in-memory-database, we don't use the blob management, but 
     * allocate a single chunk of memory, and store the memory address
     * in rid
     */
    if (btree_node_is_leaf(node) && record->size>sizeof(ham_offset_t)) {
        ham_status_t st;
        if ((st=blob_allocate(db, txn, record->data, record->size, 0, &rid)))
            return (st);
    }

    /*
     * if the record's size is <= sizeof(ham_offset_t), store the data
     * directly in the offset
     *
     * if the record's size is < sizeof(ham_offset_t), the last byte
     * in &rid is the size of the data. if the record is empty, we just
     * set the "empty"-flag.
     */
    key_set_flags(bte, 0);
    if (btree_node_is_leaf(node)) {
        if (record->size==0) {
            key_set_flags(bte, key_get_flags(bte)|KEY_BLOB_SIZE_EMPTY);
        }
        else if (record->size<=sizeof(ham_offset_t)) {
            memcpy(&rid, record->data, record->size);
            if (record->size<sizeof(ham_offset_t)) {
                char *p=(char *)&rid;
                p[sizeof(ham_offset_t)-1]=record->size;
                key_set_flags(bte, key_get_flags(bte)|KEY_BLOB_SIZE_TINY);
            }
            else {
                key_set_flags(bte, key_get_flags(bte)|KEY_BLOB_SIZE_SMALL);
            }
        }
    }

    /*
     * we insert the extended key, if necessary
     */
    key_set_key(bte, key->data, key->size);
    /* @@@
    if (btree_node_is_leaf(node) && key->size>db_get_keysize(db)) {
        ham_offset_t *p;
        ham_u8_t *prefix=bte->_key;
        blobid=db_ext_key_insert(db, txn, page, key);
        if (!blobid)
            return (db_get_error(db));
        p=(ham_offset_t *)(prefix+(db_get_keysize(db)-
                sizeof(ham_offset_t)));
        *p=ham_db2h_offset(blobid);
    }
    */
    key_set_ptr(bte, rid);
    key_set_size(bte, key->size);
    page_set_dirty(page, 1);
    btree_node_set_count(node, count+1);

    return (0);
}

static ham_status_t
my_insert_split(ham_page_t *page, ham_key_t *key, 
        ham_offset_t rid, ham_u32_t flags, 
        insert_scratchpad_t *scratchpad)
{
    int cmp;
    ham_status_t st;
    ham_page_t *newpage, *oldsib;
    key_t *nbte, *obte;
    btree_node_t *nbtp, *obtp, *sbtp;
    ham_size_t count, pivot, keysize;
    ham_db_t *db=page_get_owner(page);
    ham_key_t pivotkey, oldkey;
    ham_offset_t pivotrid;

    keysize=db_get_keysize(db);

    /*
     * allocate a new page
     */
    newpage=db_alloc_page(db, PAGE_TYPE_INDEX, scratchpad->txn, 0); 
    if (!newpage)
        return (db_get_error(db));

    /*
     * move half of the key/rid-tuples to the new page
     */
    nbtp=ham_page_get_btree_node(newpage);
    nbte=btree_node_get_key(db, nbtp, 0);
    obtp=ham_page_get_btree_node(page);
    obte=btree_node_get_key(db, obtp, 0);
    count=btree_node_get_count(obtp);
    pivot=count/2;

    /*
     * if we split a leaf, we'll insert the pivot element in the leaf
     * page, too. in internal nodes, we don't insert it, but propagate
     * it to the parent node only.
     */
    if (btree_node_is_leaf(obtp)) {
        memcpy((char *)nbte,
               ((char *)obte)+(sizeof(key_t)-1+keysize)*pivot, 
               (sizeof(key_t)-1+keysize)*(count-pivot));
    }
    else {
        memcpy((char *)nbte,
               ((char *)obte)+(sizeof(key_t)-1+keysize)*(pivot+1), 
               (sizeof(key_t)-1+keysize)*(count-pivot-1));
    }
    
    /* 
     * store the pivot element, we'll need it later to propagate it 
     * to the parent page
     */
    nbte=btree_node_get_key(db, obtp, pivot);

    oldkey.data=key_get_key(nbte);
    oldkey.size=key_get_size(nbte);
    if (!util_copy_key(&oldkey, &pivotkey)) {
        (void)db_free_page(db, scratchpad->txn, newpage, 0);
        /* @@@ TODO page_delete(newpage);*/
        db_set_error(db, HAM_OUT_OF_MEMORY);
        return (HAM_OUT_OF_MEMORY);
    }
    pivotrid=page_get_self(newpage);

    /*
     * adjust the page count
     */
    if (btree_node_is_leaf(obtp)) {
        btree_node_set_count(obtp, pivot);
        btree_node_set_count(nbtp, count-pivot);
    }
    else {
        btree_node_set_count(obtp, pivot);
        btree_node_set_count(nbtp, count-pivot-1);
    }

    /*
     * if we're in an internal page: fix the ptr_left of the new page
     * (it points to the ptr of the pivot key)
     */ 
    if (!btree_node_is_leaf(obtp)) {
        /* 
         * nbte still contains the pivot key 
         */
        btree_node_set_ptr_left(nbtp, key_get_ptr(nbte));
    }

    /*
     * insert the new element
     */
    cmp=key_compare_int_to_pub(page, pivot, key);
    if (db_get_error(db)) 
        return (db_get_error(db));

    if (cmp<=0)
        st=my_insert_nosplit(newpage, scratchpad->txn, key, rid, 
                scratchpad->record, flags|NOFLUSH);
    else
        st=my_insert_nosplit(page, scratchpad->txn, key, rid, 
                scratchpad->record, flags|NOFLUSH);
    if (st) 
        return (st);

    /*
     * fix the double-linked list of pages, and mark the pages as dirty
     */
    if (btree_node_get_right(obtp)) 
        oldsib=db_fetch_page(db, scratchpad->txn, 
                btree_node_get_right(obtp), 0);
    else
        oldsib=0;
    btree_node_set_left (nbtp, page_get_self(page));
    btree_node_set_right(nbtp, btree_node_get_right(obtp));
    btree_node_set_right(obtp, page_get_self(newpage));
    if (oldsib) {
        sbtp=ham_page_get_btree_node(oldsib);
        btree_node_set_left(sbtp, page_get_self(newpage));
        page_set_dirty(oldsib, 1);
    }
    page_set_dirty(newpage, 1);
    page_set_dirty(page, 1);

    /* 
     * propagate the pivot key to the parent page
     */
    if (scratchpad->key.data)
        ham_mem_free(scratchpad->key.data);
    scratchpad->key=pivotkey;
    scratchpad->rid=pivotrid;

    return (SPLIT);
}

