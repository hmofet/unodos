/* unobus — service registry + bus enumerate/bind (CONTRACT-ARCH §7, Phase 11).
 *
 * A driver is a backend BOUND AT RUNTIME instead of compile time, implementing one
 * of the §6 service interfaces (block/input/fb/nic/audio). Buses enumerate device
 * nodes; drivers claim by class and PUBLISH a service into the registry; the
 * Primitive-Vtable slots are then filled FROM the registry instead of hard-coded.
 * Static-link first (no loadable modules in 3.1). On fixed-hardware machines this
 * degenerates to "hard-bound services" at zero cost — and a soldered "detect pin"
 * (e.g. the Famicom Disk System) is the SAME binding question as a bus walk.
 */
#ifndef UNOBUS_H
#define UNOBUS_H

enum { SVC_BLOCK = 0, SVC_INPUT, SVC_FB, SVC_NIC, SVC_AUDIO, SVC__COUNT };

typedef struct { const char *driver; void *iface; } uno_service_t;

void  uno_registry_reset(void);
void  uno_publish(int svc_class, const char *driver, void *iface);   /* a driver publishes */
void *uno_lookup(int svc_class);                                     /* vtable slot fill */
const char *uno_provider(int svc_class);

/* a device node a bus enumerator emits */
typedef struct { const char *bus; unsigned vendor_device; int svc_class; const char *id; } uno_node_t;

/* a static-linked driver: claims nodes of match_class and publishes a service */
typedef struct {
    int match_class;
    const char *name;
    void *(*bind)(const uno_node_t *node);    /* returns the service iface */
} uno_driver_t;

/* walk nodes, match drivers by class, publish. Returns count bound. */
int uno_bus_bind(const uno_node_t *nodes, int nnodes, const uno_driver_t *drivers, int ndrivers);

#endif /* UNOBUS_H */
