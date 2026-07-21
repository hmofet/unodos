/* ===========================================================================
 * UnoDOS/pc64 - minimal UEFI surface (handwritten, no gnu-efi / EDK2).
 *
 * Exactly the slice of UEFI the port touches, transcribed from the UEFI 2.x
 * spec: the System Table, the Boot Services entries we call (Stall,
 * SetWatchdogTimer, LocateProtocol, HandleProtocol, LocateHandleBuffer,
 * FreePool), Simple Text Input (keyboard), Graphics Output (framebuffer),
 * and the Simple/Absolute Pointer protocols (mouse). Struct layouts follow
 * the spec field-for-field - the unused function-pointer slots are typed
 * void* so offsets stay exact without dragging in every signature.
 *
 * All UEFI calls are MS x64 ABI, which is the mingw target's native calling
 * convention - so no EFIAPI attribute gymnastics are needed here.
 * ===========================================================================
 */
#ifndef UNO_UEFI_H
#define UNO_UEFI_H

typedef unsigned char       UINT8;
typedef unsigned short      UINT16;
typedef unsigned int        UINT32;
typedef unsigned long long  UINT64;
typedef signed int          INT32;
typedef unsigned long long  UINTN;
typedef long long           INTN;
typedef unsigned short      CHAR16;
typedef unsigned char       EFI_BOOLEAN;
typedef UINTN               EFI_STATUS;
typedef void               *EFI_HANDLE;
typedef void               *EFI_EVENT;
typedef UINT64              EFI_PHYSICAL_ADDRESS;

#define EFI_SUCCESS   0
#define EFI_NOT_READY 0x8000000000000006ULL
#define EFI_ERROR(s)  ((INTN)(s) < 0)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ---- Simple Text Input --------------------------------------------------- */
typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

/* UEFI scan codes the port maps */
enum {
    SCAN_UP = 0x01, SCAN_DOWN = 0x02, SCAN_RIGHT = 0x03, SCAN_LEFT = 0x04,
    SCAN_DELETE = 0x08, SCAN_F9 = 0x13, SCAN_F10 = 0x14, SCAN_ESC = 0x17
};

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (*Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
                        EFI_BOOLEAN ExtendedVerification);
    EFI_STATUS (*ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
                                EFI_INPUT_KEY *Key);
    EFI_EVENT WaitForKey;
};

/* ---- Simple Text Input Ex (modifier state - Ctrl becomes the Mac cmdKey) -- */
#define EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID \
    { 0xdd9e7534, 0x7762, 0x4698, { 0x8c, 0x14, 0xf5, 0x85, 0x17, 0xa6, 0x25, 0xaa } }

#define EFI_SHIFT_STATE_VALID 0x80000000u
#define EFI_RIGHT_SHIFT_PRESSED   0x00000001u
#define EFI_LEFT_SHIFT_PRESSED    0x00000002u
#define EFI_RIGHT_CONTROL_PRESSED 0x00000004u
#define EFI_LEFT_CONTROL_PRESSED  0x00000008u

typedef struct {
    UINT32 KeyShiftState;
    UINT8  KeyToggleState;
} EFI_KEY_STATE;

typedef struct {
    EFI_INPUT_KEY Key;
    EFI_KEY_STATE KeyState;
} EFI_KEY_DATA;

typedef struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL;
struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL {
    EFI_STATUS (*Reset)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                        EFI_BOOLEAN ExtendedVerification);
    EFI_STATUS (*ReadKeyStrokeEx)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                  EFI_KEY_DATA *KeyData);
    EFI_EVENT WaitForKeyEx;
    void *SetState;
    void *RegisterKeyNotify;
    void *UnregisterKeyNotify;
};

/* ---- Simple Text Output (boot-time diagnostics only) --------------------- */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (*OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                               CHAR16 *String);
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    void *ClearScreen;
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
};

/* ---- Graphics Output ------------------------------------------------------ */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,   /* R in the low byte  */
    PixelBlueGreenRedReserved8BitPerColor = 1,   /* B in the low byte  */
    PixelBitMask = 2,
    PixelBltOnly = 3
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask, GreenMask, BlueMask, ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32                    Version;
    UINT32                    HorizontalResolution;
    UINT32                    VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK         PixelInformation;
    UINT32                    PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32                                MaxMode;
    UINT32                                Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                 SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    UINTN                                 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef enum {
    EfiBltVideoFill = 0,
    EfiBltVideoToBltBuffer = 1,
    EfiBltBufferToVideo = 2,
    EfiBltVideoToVideo = 3
} EFI_GRAPHICS_OUTPUT_BLT_OPERATION;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                            UINT32 ModeNumber, UINTN *SizeOfInfo,
                            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (*SetMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                          UINT32 ModeNumber);
    EFI_STATUS (*Blt)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                      UINT32 *BltBuffer,               /* BGRA words */
                      EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
                      UINTN SourceX, UINTN SourceY,
                      UINTN DestinationX, UINTN DestinationY,
                      UINTN Width, UINTN Height, UINTN Delta);
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* ---- Simple Pointer (relative mouse) -------------------------------------- */
#define EFI_SIMPLE_POINTER_PROTOCOL_GUID \
    { 0x31878c87, 0x0b75, 0x11d5, { 0x9a, 0x4f, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

typedef struct {
    INT32       RelativeMovementX;
    INT32       RelativeMovementY;
    INT32       RelativeMovementZ;
    EFI_BOOLEAN LeftButton;
    EFI_BOOLEAN RightButton;
} EFI_SIMPLE_POINTER_STATE;

typedef struct {
    UINT64      ResolutionX;        /* counts per mm */
    UINT64      ResolutionY;
    UINT64      ResolutionZ;
    EFI_BOOLEAN LeftButton;
    EFI_BOOLEAN RightButton;
} EFI_SIMPLE_POINTER_MODE;

typedef struct EFI_SIMPLE_POINTER_PROTOCOL EFI_SIMPLE_POINTER_PROTOCOL;
struct EFI_SIMPLE_POINTER_PROTOCOL {
    EFI_STATUS (*Reset)(EFI_SIMPLE_POINTER_PROTOCOL *This,
                        EFI_BOOLEAN ExtendedVerification);
    EFI_STATUS (*GetState)(EFI_SIMPLE_POINTER_PROTOCOL *This,
                           EFI_SIMPLE_POINTER_STATE *State);
    EFI_EVENT WaitForInput;
    EFI_SIMPLE_POINTER_MODE *Mode;
};

/* ---- Absolute Pointer (tablet - what QEMU/OVMF expose for usb-tablet) ----- */
#define EFI_ABSOLUTE_POINTER_PROTOCOL_GUID \
    { 0x8d59d32b, 0xc655, 0x4ae9, { 0x9b, 0x15, 0xf2, 0x59, 0x04, 0x99, 0x2a, 0x43 } }

typedef struct {
    UINT64 AbsoluteMinX, AbsoluteMinY, AbsoluteMinZ;
    UINT64 AbsoluteMaxX, AbsoluteMaxY, AbsoluteMaxZ;
    UINT32 Attributes;
} EFI_ABSOLUTE_POINTER_MODE;

typedef struct {
    UINT64 CurrentX;
    UINT64 CurrentY;
    UINT64 CurrentZ;
    UINT32 ActiveButtons;           /* bit 0 = touch/left */
} EFI_ABSOLUTE_POINTER_STATE;

typedef struct EFI_ABSOLUTE_POINTER_PROTOCOL EFI_ABSOLUTE_POINTER_PROTOCOL;
struct EFI_ABSOLUTE_POINTER_PROTOCOL {
    EFI_STATUS (*Reset)(EFI_ABSOLUTE_POINTER_PROTOCOL *This,
                        EFI_BOOLEAN ExtendedVerification);
    EFI_STATUS (*GetState)(EFI_ABSOLUTE_POINTER_PROTOCOL *This,
                           EFI_ABSOLUTE_POINTER_STATE *State);
    EFI_EVENT WaitForInput;
    EFI_ABSOLUTE_POINTER_MODE *Mode;
};

/* ---- USB I/O (firmware-enumerated USB devices, one handle per interface) --
 * The attached-mode USB transport (usbio.c): drive a USB NIC through the
 * firmware's own stack instead of taking over the xHCI controller - a takeover
 * while boot services are alive would kill the firmware's USB mass storage,
 * i.e. the USB boot stick itself (the F8 failure mode). */
#define EFI_USB_IO_PROTOCOL_GUID \
    { 0x2B2F68D6, 0x0CD2, 0x44cf, { 0x8E, 0x8B, 0xBB, 0xA2, 0x0B, 0x1B, 0x5B, 0x75 } }

typedef enum { EfiUsbDataIn = 0, EfiUsbDataOut = 1, EfiUsbNoData = 2 } EFI_USB_DATA_DIRECTION;

typedef struct {
    UINT8  RequestType;
    UINT8  Request;
    UINT16 Value;
    UINT16 Index;
    UINT16 Length;
} EFI_USB_DEVICE_REQUEST;

typedef struct {
    UINT8  Length, DescriptorType;
    UINT16 BcdUSB;
    UINT8  DeviceClass, DeviceSubClass, DeviceProtocol, MaxPacketSize0;
    UINT16 IdVendor, IdProduct, BcdDevice;
    UINT8  StrManufacturer, StrProduct, StrSerialNumber, NumConfigurations;
} EFI_USB_DEVICE_DESCRIPTOR;

typedef struct {
    UINT8  Length, DescriptorType, InterfaceNumber, AlternateSetting;
    UINT8  NumEndpoints, InterfaceClass, InterfaceSubClass, InterfaceProtocol;
    UINT8  Interface;
} EFI_USB_INTERFACE_DESCRIPTOR;

typedef struct __attribute__((packed)) {
    UINT8  Length, DescriptorType, EndpointAddress, Attributes;
    UINT16 MaxPacketSize;               /* unaligned on the wire -> packed */
    UINT8  Interval;
} EFI_USB_ENDPOINT_DESCRIPTOR;

typedef struct EFI_USB_IO_PROTOCOL EFI_USB_IO_PROTOCOL;
struct EFI_USB_IO_PROTOCOL {
    EFI_STATUS (*UsbControlTransfer)(EFI_USB_IO_PROTOCOL *This,
                                     EFI_USB_DEVICE_REQUEST *Request,
                                     EFI_USB_DATA_DIRECTION Direction,
                                     UINT32 Timeout,           /* ms */
                                     void *Data, UINTN DataLength,
                                     UINT32 *Status);
    EFI_STATUS (*UsbBulkTransfer)(EFI_USB_IO_PROTOCOL *This,
                                  UINT8 DeviceEndpoint,
                                  void *Data, UINTN *DataLength,
                                  UINTN Timeout,               /* ms; 0 = none */
                                  UINT32 *Status);
    void *UsbAsyncInterruptTransfer;
    void *UsbSyncInterruptTransfer;
    void *UsbIsochronousTransfer;
    void *UsbAsyncIsochronousTransfer;
    EFI_STATUS (*UsbGetDeviceDescriptor)(EFI_USB_IO_PROTOCOL *This,
                                         EFI_USB_DEVICE_DESCRIPTOR *Desc);
    void *UsbGetConfigDescriptor;
    EFI_STATUS (*UsbGetInterfaceDescriptor)(EFI_USB_IO_PROTOCOL *This,
                                            EFI_USB_INTERFACE_DESCRIPTOR *Desc);
    EFI_STATUS (*UsbGetEndpointDescriptor)(EFI_USB_IO_PROTOCOL *This,
                                           UINT8 EndpointIndex,
                                           EFI_USB_ENDPOINT_DESCRIPTOR *Desc);
    void *UsbGetStringDescriptor;
    void *UsbGetSupportedLanguages;
    void *UsbPortReset;
};

/* ---- Boot Services --------------------------------------------------------
 * Field-for-field per the spec so the offsets of the entries we call are
 * exact; entries we never touch are void* placeholders. */
typedef struct {
    EFI_TABLE_HEADER Hdr;

    void *RaiseTPL;
    void *RestoreTPL;

    void *AllocatePages;
    void *FreePages;
    void *GetMemoryMap;
    EFI_STATUS (*AllocatePool)(UINTN PoolType, UINTN Size, void **Buffer);
    EFI_STATUS (*FreePool)(void *Buffer);

    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;

    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                                 void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;

    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    void *ExitBootServices;

    void *GetNextMonotonicCount;
    EFI_STATUS (*Stall)(UINTN Microseconds);
    EFI_STATUS (*SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode,
                                   UINTN DataSize, CHAR16 *WatchdogData);

    EFI_STATUS (*ConnectController)(EFI_HANDLE ControllerHandle,
                                    EFI_HANDLE *DriverImageHandle,
                                    void *RemainingDevicePath,
                                    EFI_BOOLEAN Recursive);
    void *DisconnectController;

    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;

    void *ProtocolsPerHandle;
    EFI_STATUS (*LocateHandleBuffer)(UINTN SearchType, EFI_GUID *Protocol,
                                     void *SearchKey, UINTN *NoHandles,
                                     EFI_HANDLE **Buffer);
    EFI_STATUS (*LocateProtocol)(EFI_GUID *Protocol, void *Registration,
                                 void **Interface);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;

    void *CalculateCrc32;

    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

#define EFI_LOCATE_ALL_HANDLES 0
#define EFI_LOCATE_BY_PROTOCOL 2

/* ---- System Table ---------------------------------------------------------- */
typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    UINT32                           FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void                            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                            NumberOfTableEntries;
    void                            *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif /* UNO_UEFI_H */
