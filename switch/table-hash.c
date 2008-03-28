/* Copyright (C) 2008 Board of Trustees, Leland Stanford Jr. University.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "table.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "crc32.h"
#include "flow.h"
#include "datapath.h"

struct sw_table_hash {
    struct sw_table swt;
    struct crc32 crc32;
    unsigned int n_flows;
    unsigned int bucket_mask; /* Number of buckets minus 1. */
    struct sw_flow **buckets;
};

static struct sw_flow **find_bucket(struct sw_table *swt,
                                    const struct sw_flow_key *key)
{
    struct sw_table_hash *th = (struct sw_table_hash *) swt;
    unsigned int crc = crc32_calculate(&th->crc32, key, sizeof *key);
    return &th->buckets[crc & th->bucket_mask];
}

static struct sw_flow *table_hash_lookup(struct sw_table *swt,
                                         const struct sw_flow_key *key)
{
    struct sw_flow *flow = *find_bucket(swt, key);
    return flow && !memcmp(&flow->key, key, sizeof *key) ? flow : NULL;
}

static int table_hash_insert(struct sw_table *swt, struct sw_flow *flow)
{
    struct sw_table_hash *th = (struct sw_table_hash *) swt;
    struct sw_flow **bucket;
    int retval;

    if (flow->key.wildcards != 0)
        return 0;

    bucket = find_bucket(swt, &flow->key);
    if (*bucket == NULL) {
        th->n_flows++;
        *bucket = flow;
        retval = 1;
    } else {
        struct sw_flow *old_flow = *bucket;
        if (!memcmp(&old_flow->key, &flow->key, sizeof flow->key)) {
            *bucket = flow;
            flow_free(old_flow);
            retval = 1;
        } else {
            retval = 0;
        }
    }
    return retval;
}

/* Caller must update n_flows. */
static void
do_delete(struct sw_flow **bucket)
{
    flow_free(*bucket);
    *bucket = NULL;
}

/* Returns number of deleted flows. */
static int table_hash_delete(struct sw_table *swt,
                             const struct sw_flow_key *key, int strict)
{
    struct sw_table_hash *th = (struct sw_table_hash *) swt;
    unsigned int count = 0;

    if (key->wildcards == 0) {
        struct sw_flow **bucket = find_bucket(swt, key);
        struct sw_flow *flow = *bucket;
        if (flow && !memcmp(&flow->key, key, sizeof *key)) {
            do_delete(bucket);
            count = 1;
        }
    } else {
        unsigned int i;

        for (i = 0; i <= th->bucket_mask; i++) {
            struct sw_flow **bucket = &th->buckets[i];
            struct sw_flow *flow = *bucket;
            if (flow && flow_del_matches(&flow->key, key, strict)) {
                do_delete(bucket);
                count++;
            }
        }
    }
    th->n_flows -= count;
    return count;
}

static int table_hash_timeout(struct datapath *dp, struct sw_table *swt)
{
    struct sw_table_hash *th = (struct sw_table_hash *) swt;
    unsigned int i;
    int count = 0;

    for (i = 0; i <= th->bucket_mask; i++) {
        struct sw_flow **bucket = &th->buckets[i];
        struct sw_flow *flow = *bucket;
        if (flow && flow_timeout(flow)) {
            dp_send_flow_expired(dp, flow);
            do_delete(bucket);
            count++;
        }
    }
    th->n_flows -= count;
    return count;
}

static void table_hash_destroy(struct sw_table *swt)
{
    struct sw_table_hash *th = (struct sw_table_hash *) swt;
    unsigned int i;
    for (i = 0; i <= th->bucket_mask; i++) {
        if (th->buckets[i]) {
            flow_free(th->buckets[i]); 
        }
    }
    free(th->buckets);
    free(th);
}

struct swt_iterator_hash {
    struct sw_table_hash *th;
    unsigned int bucket_i;
};

static struct sw_flow *next_flow(struct swt_iterator_hash *ih)
{
    for (;ih->bucket_i <= ih->th->bucket_mask; ih->bucket_i++) {
        struct sw_flow *f = ih->th->buckets[ih->bucket_i];
        if (f != NULL)
            return f;
    }

    return NULL;
}

static int table_hash_iterator(struct sw_table *swt,
                               struct swt_iterator *swt_iter)
{
    struct swt_iterator_hash *ih;

    swt_iter->private = ih = malloc(sizeof *ih);

    if (ih == NULL)
        return 0;

    ih->th = (struct sw_table_hash *) swt;

    ih->bucket_i = 0;
    swt_iter->flow = next_flow(ih);

    return 1;
}

static void table_hash_next(struct swt_iterator *swt_iter)
{
    struct swt_iterator_hash *ih;

    if (swt_iter->flow == NULL)
        return;

    ih = (struct swt_iterator_hash *) swt_iter->private;

    ih->bucket_i++;
    swt_iter->flow = next_flow(ih);
}

static void table_hash_iterator_destroy(struct swt_iterator *swt_iter)
{
    free(swt_iter->private);
}

static void table_hash_stats(struct sw_table *swt,
                             struct sw_table_stats *stats) 
{
    struct sw_table_hash *th = (struct sw_table_hash *) swt;
    stats->name = "hash";
    stats->n_flows = th->n_flows;
    stats->max_flows = th->bucket_mask + 1;
}

struct sw_table *table_hash_create(unsigned int polynomial,
                                   unsigned int n_buckets)
{
    struct sw_table_hash *th;
    struct sw_table *swt;

    th = malloc(sizeof *th);
    if (th == NULL)
        return NULL;

    assert(!(n_buckets & (n_buckets - 1)));
    th->buckets = calloc(n_buckets, sizeof *th->buckets);
    if (th->buckets == NULL) {
        printf("failed to allocate %u buckets\n", n_buckets);
        free(th);
        return NULL;
    }
    th->bucket_mask = n_buckets - 1;

    swt = &th->swt;
    swt->lookup = table_hash_lookup;
    swt->insert = table_hash_insert;
    swt->delete = table_hash_delete;
    swt->timeout = table_hash_timeout;
    swt->destroy = table_hash_destroy;
    swt->iterator = table_hash_iterator;
    swt->iterator_next = table_hash_next;
    swt->iterator_destroy = table_hash_iterator_destroy;
    swt->stats = table_hash_stats;

    crc32_init(&th->crc32, polynomial);

    return swt;
}

/* Double-hashing table. */

struct sw_table_hash2 {
    struct sw_table swt;
    struct sw_table *subtable[2];
};

static struct sw_flow *table_hash2_lookup(struct sw_table *swt,
                                          const struct sw_flow_key *key)
{
    struct sw_table_hash2 *t2 = (struct sw_table_hash2 *) swt;
    int i;
        
    for (i = 0; i < 2; i++) {
        struct sw_flow *flow = *find_bucket(t2->subtable[i], key);
        if (flow && !memcmp(&flow->key, key, sizeof *key))
            return flow;
    }
    return NULL;
}

static int table_hash2_insert(struct sw_table *swt, struct sw_flow *flow)
{
    struct sw_table_hash2 *t2 = (struct sw_table_hash2 *) swt;

    if (table_hash_insert(t2->subtable[0], flow))
        return 1;
    return table_hash_insert(t2->subtable[1], flow);
}

static int table_hash2_delete(struct sw_table *swt,
                              const struct sw_flow_key *key, int strict)
{
    struct sw_table_hash2 *t2 = (struct sw_table_hash2 *) swt;
    return (table_hash_delete(t2->subtable[0], key, strict)
            + table_hash_delete(t2->subtable[1], key, strict));
}

static int table_hash2_timeout(struct datapath *dp, struct sw_table *swt)
{
    struct sw_table_hash2 *t2 = (struct sw_table_hash2 *) swt;
    return (table_hash_timeout(dp, t2->subtable[0])
            + table_hash_timeout(dp, t2->subtable[1]));
}

static void table_hash2_destroy(struct sw_table *swt)
{
    struct sw_table_hash2 *t2 = (struct sw_table_hash2 *) swt;
    table_hash_destroy(t2->subtable[0]);
    table_hash_destroy(t2->subtable[1]);
    free(t2);
}

struct swt_iterator_hash2 {
    struct sw_table_hash2 *th2;
    struct swt_iterator ih;
    uint8_t table_i;
};

static int table_hash2_iterator(struct sw_table *swt,
                                struct swt_iterator *swt_iter)
{
    struct swt_iterator_hash2 *ih2;

    swt_iter->private = ih2 = malloc(sizeof *ih2);
    if (ih2 == NULL)
        return 0;

    ih2->th2 = (struct sw_table_hash2 *) swt;
    if (!table_hash_iterator(ih2->th2->subtable[0], &ih2->ih)) {
        free(ih2);
        return 0;
    }

    if (ih2->ih.flow != NULL) {
        swt_iter->flow = ih2->ih.flow;
        ih2->table_i = 0;
    } else {
        table_hash_iterator_destroy(&ih2->ih);
        ih2->table_i = 1;
        if (!table_hash_iterator(ih2->th2->subtable[1], &ih2->ih)) {
            free(ih2);
            return 0;
        }
        swt_iter->flow = ih2->ih.flow;
    }

    return 1;
}

static void table_hash2_next(struct swt_iterator *swt_iter) 
{
    struct swt_iterator_hash2 *ih2;

    if (swt_iter->flow == NULL)
        return;

    ih2 = (struct swt_iterator_hash2 *) swt_iter->private;
    table_hash_next(&ih2->ih);

    if (ih2->ih.flow != NULL) {
        swt_iter->flow = ih2->ih.flow;
    } else {
        if (ih2->table_i == 0) {
            table_hash_iterator_destroy(&ih2->ih);
            ih2->table_i = 1;
            if (!table_hash_iterator(ih2->th2->subtable[1], &ih2->ih)) {
                ih2->ih.private = NULL;
                swt_iter->flow = NULL;
            } else {
                swt_iter->flow = ih2->ih.flow;
            }
        } else {
            swt_iter->flow = NULL;
        }
    }
}

static void table_hash2_iterator_destroy(struct swt_iterator *swt_iter)
{
    struct swt_iterator_hash2 *ih2;

    ih2 = (struct swt_iterator_hash2 *) swt_iter->private;
    if (ih2->ih.private != NULL)
        table_hash_iterator_destroy(&ih2->ih);
    free(ih2);
}

static void table_hash2_stats(struct sw_table *swt,
                              struct sw_table_stats *stats)
{
    struct sw_table_hash2 *t2 = (struct sw_table_hash2 *) swt;
    struct sw_table_stats substats[2];
    int i;

    for (i = 0; i < 2; i++)
        table_hash_stats(t2->subtable[i], &substats[i]);
    stats->name = "hash2";
    stats->n_flows = substats[0].n_flows + substats[1].n_flows;
    stats->max_flows = substats[0].max_flows + substats[1].max_flows;
}

struct sw_table *table_hash2_create(unsigned int poly0, unsigned int buckets0,
                                    unsigned int poly1, unsigned int buckets1)

{
    struct sw_table_hash2 *t2;
    struct sw_table *swt;

    t2 = malloc(sizeof *t2);
    if (t2 == NULL)
        return NULL;

    t2->subtable[0] = table_hash_create(poly0, buckets0);
    if (t2->subtable[0] == NULL)
        goto out_free_t2;

    t2->subtable[1] = table_hash_create(poly1, buckets1);
    if (t2->subtable[1] == NULL)
        goto out_free_subtable0;

    swt = &t2->swt;
    swt->lookup = table_hash2_lookup;
    swt->insert = table_hash2_insert;
    swt->delete = table_hash2_delete;
    swt->timeout = table_hash2_timeout;
    swt->destroy = table_hash2_destroy;
    swt->stats = table_hash2_stats;

    swt->iterator = table_hash2_iterator;
    swt->iterator_next = table_hash2_next;
    swt->iterator_destroy = table_hash2_iterator_destroy;

    return swt;

out_free_subtable0:
    table_hash_destroy(t2->subtable[0]);
out_free_t2:
    free(t2);
    return NULL;
}