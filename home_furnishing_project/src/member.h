#ifndef MEMBER_H
#define MEMBER_H

#include "common.h"

/* Entry point for a forked team-member process. Never returns. */
void member_run(const member_ctx_t* ctx, const cfg_t* cfg);

#endif
