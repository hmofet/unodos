/* attached-mode USB transport over EFI_USB_IO_PROTOCOL - see usbio.h. */
#include "usbio.h"
#include "uefi.h"

void *uno_pc64_st(void);                 /* EFI_SYSTEM_TABLE (uefi_main.c) */
int   uno_pc64_detached(void);

#define USBIO_MAX 32

static EFI_USB_IO_PROTOCOL *g_dev[USBIO_MAX];
static int g_n;

static EFI_BOOT_SERVICES *bs(void)
{
    EFI_SYSTEM_TABLE *st;
    if (uno_pc64_detached()) return 0;   /* boot services are gone */
    st = (EFI_SYSTEM_TABLE *)uno_pc64_st();
    return st ? st->BootServices : 0;
}

int uno_usbio_count(void)
{
    static EFI_GUID guid = EFI_USB_IO_PROTOCOL_GUID;
    EFI_BOOT_SERVICES *BS = bs();
    EFI_HANDLE *hs = 0;
    UINTN n = 0, i;
    g_n = 0;
    if (!BS) return 0;
    if (EFI_ERROR(BS->LocateHandleBuffer(EFI_LOCATE_BY_PROTOCOL, &guid, 0, &n, &hs)))
        return 0;
    for (i = 0; i < n && g_n < USBIO_MAX; i++) {
        void *p;
        if (EFI_ERROR(BS->HandleProtocol(hs[i], &guid, &p))) continue;
        g_dev[g_n++] = (EFI_USB_IO_PROTOCOL *)p;
    }
    BS->FreePool(hs);
    return g_n;
}

int uno_usbio_info(int i, unsigned short *vid, unsigned short *pid,
                   unsigned char *if_class, unsigned char *if_subclass)
{
    EFI_USB_DEVICE_DESCRIPTOR dd;
    EFI_USB_INTERFACE_DESCRIPTOR id;
    if (i < 0 || i >= g_n || !bs()) return -1;
    if (EFI_ERROR(g_dev[i]->UsbGetDeviceDescriptor(g_dev[i], &dd))) return -1;
    if (vid) *vid = dd.IdVendor;
    if (pid) *pid = dd.IdProduct;
    if (if_class || if_subclass) {
        if (EFI_ERROR(g_dev[i]->UsbGetInterfaceDescriptor(g_dev[i], &id)))
            return -1;
        if (if_class)    *if_class    = id.InterfaceClass;
        if (if_subclass) *if_subclass = id.InterfaceSubClass;
    }
    return 0;
}

int uno_usbio_control(int i, unsigned char bmRequestType, unsigned char bRequest,
                      unsigned short wValue, unsigned short wIndex,
                      void *data, int len)
{
    EFI_USB_DEVICE_REQUEST rq;
    EFI_USB_DATA_DIRECTION dir;
    UINT32 st = 0;
    if (i < 0 || i >= g_n || !bs()) return -1;
    rq.RequestType = bmRequestType;
    rq.Request     = bRequest;
    rq.Value       = wValue;
    rq.Index       = wIndex;
    rq.Length      = (UINT16)(len > 0 ? len : 0);
    dir = (len <= 0) ? EfiUsbNoData
        : (bmRequestType & 0x80) ? EfiUsbDataIn : EfiUsbDataOut;
    if (EFI_ERROR(g_dev[i]->UsbControlTransfer(g_dev[i], &rq, dir, 1000,
                                               len > 0 ? data : 0,
                                               (UINTN)(len > 0 ? len : 0), &st)))
        return -1;
    return len > 0 ? len : 0;
}

int uno_usbio_bulk_eps(int i, int *in_ep, int *out_ep)
{
    EFI_USB_INTERFACE_DESCRIPTOR id;
    int k;
    if (in_ep)  *in_ep  = 0;
    if (out_ep) *out_ep = 0;
    if (i < 0 || i >= g_n || !bs()) return -1;
    if (EFI_ERROR(g_dev[i]->UsbGetInterfaceDescriptor(g_dev[i], &id))) return -1;
    for (k = 0; k < (int)id.NumEndpoints; k++) {
        EFI_USB_ENDPOINT_DESCRIPTOR ed;
        if (EFI_ERROR(g_dev[i]->UsbGetEndpointDescriptor(g_dev[i], (UINT8)k, &ed)))
            continue;
        if ((ed.Attributes & 0x03) != 0x02) continue;          /* bulk only */
        if ((ed.EndpointAddress & 0x80) && in_ep && !*in_ep)   *in_ep  = ed.EndpointAddress;
        if (!(ed.EndpointAddress & 0x80) && out_ep && !*out_ep) *out_ep = ed.EndpointAddress;
    }
    return ((!in_ep || *in_ep) && (!out_ep || *out_ep)) ? 0 : -1;
}

int uno_usbio_bulk_out(int i, int ep, const void *data, int len)
{
    UINTN n = (UINTN)len;
    UINT32 st = 0;
    if (i < 0 || i >= g_n || len <= 0 || !bs()) return -1;
    if (EFI_ERROR(g_dev[i]->UsbBulkTransfer(g_dev[i], (UINT8)ep, (void *)data,
                                            &n, 2000, &st)))
        return -1;
    return (int)n;
}

int uno_usbio_bulk_in(int i, int ep, void *data, int cap, int timeout_ms)
{
    UINTN n = (UINTN)cap;
    UINT32 st = 0;
    EFI_STATUS r;
    if (i < 0 || i >= g_n || cap <= 0 || !bs()) return -1;
    if (timeout_ms < 1) timeout_ms = 1;      /* 0 means "wait forever" in UEFI */
    r = g_dev[i]->UsbBulkTransfer(g_dev[i], (UINT8)ep, data, &n,
                                  (UINTN)timeout_ms, &st);
    if (EFI_ERROR(r)) return (st & 0x40 /* EFI_USB_ERR_TIMEOUT */) ? 0 : -1;
    return (int)n;
}
