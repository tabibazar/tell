#ifndef WHERE_H
#define WHERE_H

/*
 * Where spy is, from the cell it is on: the serving LTE cell (net.c's
 * AT+CPSI? reading) looked up in BeaconDB (api.beacondb.net, the community
 * successor to Mozilla's location service: no key, no account). Accuracy is
 * the cell's: tens of metres to a few kilometres. The modem's own AT+CLBS
 * answered only "10" (failure) on this SIM; there is no GNSS in use.
 */
#include <stdbool.h>
#include "net.h"

bool where_lookup(const net_cell_t *c, double *lat, double *lon, int *accuracy_m);

#endif /* WHERE_H */
