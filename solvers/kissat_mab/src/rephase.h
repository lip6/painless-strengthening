#ifndef _rephase_h_INCLUDED
#define _rephase_h_INCLUDED

#include <stdbool.h>
#include <stdint.h>

typedef struct rephased rephased;

struct rephased
{
	char type;
	uint64_t count;
	uint64_t last;
};

struct kissat;

bool
kissat_mab_rephasing(struct kissat*);
void
kissat_mab_rephase(struct kissat*);
char
kissat_mab_rephase_best(struct kissat*);

void
kissat_mab_reset_rephased(struct kissat*);
void
kissat_mab_reset_target_assigned(struct kissat*);
void
kissat_mab_reset_consistently_assigned(struct kissat*);

#endif
