#ifndef GUI_H
#define GUI_H

#include "referee.h"

/* Initialize GLUT, set callbacks, enter main loop (does not return).
 * The world pointer is saved internally so callbacks can read it. */
void gui_run(int* argc, char** argv, world_t* world);

/* Headless fallback: just polls referee_tick() in a sleep loop until winner. */
void headless_run(world_t* world);

#endif
