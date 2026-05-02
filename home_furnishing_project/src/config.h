#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

/* 
 * If path is NULL or unreadable, defaults are kept and a warning is printed.
 * Returns 0 on success, nonzero only if the config is *invalid* (e.g. negative team size). */
int  config_load(cfg_t* cfg, const char* path);
void config_print(const cfg_t* cfg, FILE* f);

#endif
