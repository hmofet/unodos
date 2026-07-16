/* PCI config-space access (pc64_pci.c) - mechanism #1 (ports 0xCF8/0xCFC). */
#ifndef PC64_PCI_H
#define PC64_PCI_H

typedef struct {
    int bus, dev, fn;
    unsigned short vendor, device;      /* filled by the class scan */
} pci_dev;

int  pci_find(unsigned short vendor, unsigned short device, pci_dev *out);
int  pci_find_class(unsigned char cls, unsigned char sub, pci_dev *out);
unsigned int   pci_cfg_read32(const pci_dev *d, int off);
unsigned short pci_cfg_read16(const pci_dev *d, int off);
void pci_cfg_write32(const pci_dev *d, int off, unsigned int v);
void pci_cfg_write16(const pci_dev *d, int off, unsigned short v);
void pci_enable_bus_master(const pci_dev *d);
unsigned long long pci_bar(const pci_dev *d, int n);

#endif
