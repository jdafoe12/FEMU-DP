
#ifndef __FEMU_DP_H
#define __FEMU_DP_H

#include "ftl.h"

pqueue_pri_t EC(const struct nand_block *blk);
struct nand_block *H_minus(pqueue_t *q);
struct nand_block *H_plus(pqueue_t *q);
dp_pool_entry *dp_peek_entry(pqueue_t *q);
dp_pool_entry *dp_peek_tail(pqueue_t *q);
void dp_switch_pool_membership(struct block_pools *bp, dp_pool_entry *entry,
                               bool to_hot);

void dp_init_block_pools(struct ssd *ssd);
void dp_update_block_pools(struct ssd *ssd, struct nand_block *blk,
                           const struct ppa *base);
void dp_reset_effective_ec(struct block_pools *bp, dp_pool_entry *entry);
void dp_perform_wear_leveling(struct ssd *ssd);
void dp_check_cold_pool_resize(struct ssd *ssd);

#endif

