#include "dp.h"

pqueue_pri_t EC(const struct nand_block *blk)
{
    return (pqueue_pri_t)blk->erase_cnt;
}

dp_pool_entry *dp_peek_entry(pqueue_t *q)
{
    if (!q) {
        return NULL;
    }

    return (dp_pool_entry *)pqueue_peek(q);
}

dp_pool_entry *dp_peek_tail(pqueue_t *q)
{
    dp_pool_entry *entry = NULL;

    if (!q || q->size <= 1) {
        return NULL;
    }

    for (size_t i = 1; i < q->size; i++) {
        dp_pool_entry *candidate = (dp_pool_entry *)q->d[i];
        if (!entry || candidate->blk->erase_cnt < entry->blk->erase_cnt) {
            entry = candidate;
        }
    }

    return entry;
}

struct nand_block *H_minus(pqueue_t *q)
{
    dp_pool_entry *entry = dp_peek_tail(q);
    return entry ? entry->blk : NULL;
}

struct nand_block *H_plus(pqueue_t *q)
{
    dp_pool_entry *entry = dp_peek_entry(q);
    return entry ? entry->blk : NULL;
}

static inline int cold_pool_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline int hot_pool_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t block_pool_get_pri(void *a)
{
    return EC(((dp_pool_entry *)a)->blk);
}

static inline void block_pool_set_pri(void *a, pqueue_pri_t pri)
{
    ((dp_pool_entry *)a)->blk->erase_cnt = (int)pri;
}

static inline size_t cold_pool_get_pos(void *a)
{
    return ((dp_pool_entry *)a)->wear_pos;
}

static inline void cold_pool_set_pos(void *a, size_t pos)
{
    ((dp_pool_entry *)a)->wear_pos = pos;
}

static inline size_t hot_pool_get_pos(void *a)
{
    return ((dp_pool_entry *)a)->wear_pos;
}

static inline void hot_pool_set_pos(void *a, size_t pos)
{
    ((dp_pool_entry *)a)->wear_pos = pos;
}

static inline size_t dp_entry_index(struct ssdparams *spp,
                                    const struct ppa *base)
{
    size_t idx = base->g.ch;
    idx = idx * spp->luns_per_ch + base->g.lun;
    idx = idx * spp->pls_per_lun + base->g.pl;
    idx = idx * spp->blks_per_pl + base->g.blk;
    return idx;
}

static inline pqueue_pri_t eff_pool_get_pri(void *a)
{
    return ((dp_pool_entry *)a)->effective_ec;
}

static inline void eff_pool_set_pri(void *a, pqueue_pri_t pri)
{
    ((dp_pool_entry *)a)->effective_ec = (uint32_t)pri;
}

static inline size_t eff_pool_get_pos(void *a)
{
    return ((dp_pool_entry *)a)->eff_pos;
}

static inline void eff_pool_set_pos(void *a, size_t pos)
{
    ((dp_pool_entry *)a)->eff_pos = pos;
}

static void dp_update_eff_queue(struct block_pools *bp, dp_pool_entry *entry)
{
    pqueue_t *pq;

    if (!bp || !entry) {
        return;
    }

    pq = entry->in_hot ? bp->hot_pool_eff : bp->cold_pool_eff;
    if (pq && entry->eff_pos) {
        pqueue_change_priority(pq, entry->effective_ec, entry);
    }
}

static void dp_remove_entry_from_wear_pool(pqueue_t *pq, dp_pool_entry *entry)
{
    if (pq && entry && entry->wear_pos) {
        pqueue_remove(pq, entry);
        entry->wear_pos = 0;
    }
}

static void dp_remove_entry_from_eff_pool(pqueue_t *pq, dp_pool_entry *entry)
{
    if (pq && entry && entry->eff_pos) {
        pqueue_remove(pq, entry);
        entry->eff_pos = 0;
    }
}

void dp_switch_pool_membership(struct block_pools *bp, dp_pool_entry *entry,
                               bool to_hot)
{
    if (!bp || !entry) {
        return;
    }

    if (entry->in_hot == to_hot) {
        return;
    }

    if (entry->in_hot) {
        dp_remove_entry_from_wear_pool(bp->hot_pool, entry);
        dp_remove_entry_from_eff_pool(bp->hot_pool_eff, entry);
        entry->in_hot = false;
        ftl_assert(pqueue_insert(bp->cold_pool, entry) == 0);
        ftl_assert(pqueue_insert(bp->cold_pool_eff, entry) == 0);
    } else {
        dp_remove_entry_from_wear_pool(bp->cold_pool, entry);
        dp_remove_entry_from_eff_pool(bp->cold_pool_eff, entry);
        entry->in_hot = true;
        ftl_assert(pqueue_insert(bp->hot_pool, entry) == 0);
        ftl_assert(pqueue_insert(bp->hot_pool_eff, entry) == 0);
    }
}

void dp_update_block_pools(struct ssd *ssd, struct nand_block *blk,
                           const struct ppa *base)
{
    struct block_pools *bp = &ssd->bp;
    struct ssdparams *spp = &ssd->sp;
    pqueue_pri_t pri = EC(blk);
    size_t idx;
    dp_pool_entry *entry;

    if (!bp->entries || !base) {
        return;
    }

    idx = dp_entry_index(spp, base);
    ftl_assert(idx < bp->entry_count);

    entry = &bp->entries[idx];
    ftl_assert(entry->blk == blk);

    entry->effective_ec++;

    if (entry->in_hot) {
        if (bp->hot_pool && entry->wear_pos) {
            pqueue_change_priority(bp->hot_pool, pri, entry);
        }
    } else {
        if (bp->cold_pool && entry->wear_pos) {
            pqueue_change_priority(bp->cold_pool, pri, entry);
        }
    }

    dp_update_eff_queue(bp, entry);
}

void dp_reset_effective_ec(struct block_pools *bp, dp_pool_entry *entry)
{
    if (!entry) {
        return;
    }

    entry->effective_ec = 0;
    dp_update_eff_queue(bp, entry);
}

void dp_check_cold_pool_resize(struct ssd *ssd)
{
    struct block_pools *bp = &ssd->bp;
    dp_pool_entry *cold_head_entry;
    dp_pool_entry *hot_tail_entry;

    if (!bp->cold_pool_eff || !bp->hot_pool_eff) {
        return;
    }

    cold_head_entry = dp_peek_entry(bp->cold_pool_eff);
    hot_tail_entry = dp_peek_tail(bp->hot_pool_eff);

    if (!cold_head_entry || !hot_tail_entry) {
        return;
    }

    if ((int)cold_head_entry->effective_ec - (int)hot_tail_entry->effective_ec
        > ssd->th) {
        dp_switch_pool_membership(bp, cold_head_entry, true);
    }
}

void dp_init_block_pools(struct ssd *ssd)
{
    struct block_pools *bp = &ssd->bp;
    struct ssdparams *spp = &ssd->sp;
    size_t idx = 0;

    bp->cold_pool = pqueue_init(spp->tt_blks, cold_pool_cmp_pri,
                                block_pool_get_pri, block_pool_set_pri,
                                cold_pool_get_pos, cold_pool_set_pos);
    ftl_assert(bp->cold_pool);

    bp->hot_pool = pqueue_init(spp->tt_blks, hot_pool_cmp_pri,
                               block_pool_get_pri, block_pool_set_pri,
                               hot_pool_get_pos, hot_pool_set_pos);
    ftl_assert(bp->hot_pool);

    bp->cold_pool_eff = pqueue_init(spp->tt_blks, cold_pool_cmp_pri,
                                    eff_pool_get_pri, eff_pool_set_pri,
                                    eff_pool_get_pos, eff_pool_set_pos);
    ftl_assert(bp->cold_pool_eff);

    bp->hot_pool_eff = pqueue_init(spp->tt_blks, hot_pool_cmp_pri,
                                   eff_pool_get_pri, eff_pool_set_pri,
                                   eff_pool_get_pos, eff_pool_set_pos);
    ftl_assert(bp->hot_pool_eff);

    bp->entry_count = spp->tt_blks;
    bp->entries = g_malloc0(sizeof(dp_pool_entry) * bp->entry_count);

    for (int ch = 0; ch < spp->nchs; ch++) {
        struct ssd_channel *channel = &ssd->ch[ch];
        for (int lun = 0; lun < channel->nluns; lun++) {
            struct nand_lun *lunp = &channel->lun[lun];
            for (int pl = 0; pl < lunp->npls; pl++) {
                struct nand_plane *plp = &lunp->pl[pl];
                for (int blk = 0; blk < plp->nblks; blk++) {
                    struct nand_block *block = &plp->blk[blk];
                    dp_pool_entry *entry = &bp->entries[idx++];

                    entry->blk = block;
                    entry->base.ppa = 0;
                    entry->base.g.ch = ch;
                    entry->base.g.lun = lun;
                    entry->base.g.pl = pl;
                    entry->base.g.blk = blk;
                    entry->base.g.pg = 0;
                    entry->base.g.sec = 0;
                    entry->effective_ec = 0;
                    entry->wear_pos = 0;
                    entry->eff_pos = 0;
                    entry->in_hot = false;

                    if (g_random_boolean()) {
                        entry->in_hot = true;
                        pqueue_insert(bp->hot_pool, entry);
                        ftl_assert(pqueue_insert(bp->hot_pool_eff, entry) == 0);
                    } else {
                        entry->in_hot = false;
                        ftl_assert(pqueue_insert(bp->cold_pool, entry) == 0);
                        ftl_assert(pqueue_insert(bp->cold_pool_eff, entry) == 0);
                    }
                }
            }
        }
    }
}

