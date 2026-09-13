#ifndef ENVFLASH_H
#define ENVFLASH_H

#include "envstore.h"

/* Finds the envlog partition and fills in the flash interface envstore wants.
   False when there is no such partition, which is every board but wave. */
bool envflash_open(envflash_t *out);

#endif /* ENVFLASH_H */
