/* unobus — registry + binder. See unobus.h. */
#include "unobus.h"
#include <stddef.h>

static uno_service_t g_reg[SVC__COUNT];

void uno_registry_reset(void) { for (int i = 0; i < SVC__COUNT; i++) { g_reg[i].driver = NULL; g_reg[i].iface = NULL; } }
void uno_publish(int c, const char *drv, void *iface) { if (c >= 0 && c < SVC__COUNT) { g_reg[c].driver = drv; g_reg[c].iface = iface; } }
void *uno_lookup(int c) { return (c >= 0 && c < SVC__COUNT) ? g_reg[c].iface : NULL; }
const char *uno_provider(int c) { return (c >= 0 && c < SVC__COUNT) ? g_reg[c].driver : NULL; }

int uno_bus_bind(const uno_node_t *nodes, int nn, const uno_driver_t *drv, int nd) {
    int bound = 0;
    for (int i = 0; i < nn; i++)
        for (int j = 0; j < nd; j++)
            if (drv[j].match_class == nodes[i].svc_class) {
                void *iface = drv[j].bind(&nodes[i]);
                if (iface) { uno_publish(nodes[i].svc_class, drv[j].name, iface); bound++; }
                break;
            }
    return bound;
}
