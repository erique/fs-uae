/*
 * UAE - The Un*x Amiga Emulator
 *
 * uaeusb.device — Poseidon usbhardware.device-class host controller
 * that services USB transfers via host libusb on worker threads.
 */

#include "sysconfig.h"
#include "sysdeps.h"

#ifdef WITH_LIBUSB

#include "threaddep/thread.h"
#include "options.h"
#include "uae/memory.h"
#include "custom.h"
#include "events.h"
#include "newcpu.h"
#include "autoconf.h"
#include "traps.h"
#include "execlib.h"
#include "native2amiga.h"
#include "uae.h"
#include "execio.h"
#include "uaeusb.h"

#include <libusb-1.0/libusb.h>

// Device identity
#define UAEUSB_DEVICE_NAME      _T("uaeusb.device")
#define UAEUSB_ID_STRING        _T("UAE usb.device 1.0")
#define UAEUSB_VERSION          1
#define UAEUSB_REVISION         0
#define UAEUSB_ROMTAG_PRI       5
#define UAEUSB_DEVICE_BASE_SIZE 0x100
#define UAEUSB_UNIT             0

// UHCMD_QUERYDEVICE identity strings (emitted into rtarea, Amiga-readable)
#define UAEUSB_MANUFACTURER     _T("UAE")
#define UAEUSB_PRODUCTNAME      _T("UAE USB Controller")
#define UAEUSB_DESCRIPTION      _T("USB host controller via host libusb")
#define UAEUSB_COPYRIGHT        _T("(C) 2026 UAE")

/*
 * exec struct Resident (exec/resident.h) field offsets and constants.
 */
#define RT_MATCHWORD            0x00    // rt_MatchWord
#define RT_MATCHTAG             0x02    // rt_MatchTag
#define RT_ENDSKIP              0x06    // rt_EndSkip
#define RT_FLAGS                0x0A    // rt_Flags byte + rt_Version byte
#define RT_TYPE                 0x0C    // rt_Type byte + rt_Pri byte
#define RT_NAME                 0x0E    // rt_Name
#define RT_IDSTRING             0x12    // rt_IdString
#define RT_INIT                 0x16    // rt_Init
#define RT_SIZE                 0x1A    // sizeof(struct Resident)
#define RTC_MATCHWORD           0x4AFC
#define RTF_AUTOINIT            0x80
#define RTF_COLDSTART           0x01

// exec constants not provided by execlib.h
#define NT_REPLYMSG             7       // mn_Node.ln_Type of a replied message
#define LIB_OPENCNT             32      // struct Library lib_OpenCnt offset

/*
 * exec IORequest field offsets (exec/io.h).
 */
#define IO_LN_TYPE              8       // io_Message.mn_Node.ln_Type
#define IO_MN_LENGTH            18      // io_Message.mn_Length
#define IO_DEVICE               20      // io_Device
#define IO_UNIT                 24      // io_Unit
#define IO_COMMAND              28      // io_Command
#define IO_FLAGS                30      // io_Flags
#define IO_ERROR                31      // io_Error

/*
 * IOUsbHWReq field offsets, derived from Poseidon devices/usbhardware.h 2.1
 * via a314/Software/usbhardware/device.h (#pragma pack(2), no padding).
 */
#define IOUH_FLAGS              32      // iouh_Flags (UWORD)
#define IOUH_STATE              34      // iouh_State (UWORD, UHSF_*)
#define IOUH_DIR                36      // iouh_Dir (UWORD, UHDIR_*)
#define IOUH_DEVADDR            38      // iouh_DevAddr (UWORD)
#define IOUH_ENDPOINT           40      // iouh_Endpoint (UWORD)
#define IOUH_MAXPKTSIZE         42      // iouh_MaxPktSize (UWORD)
#define IOUH_ACTUAL             44      // iouh_Actual (ULONG)
#define IOUH_LENGTH             48      // iouh_Length (ULONG)
#define IOUH_DATA               52      // iouh_Data (APTR into Amiga RAM)
#define IOUH_INTERVAL           56      // iouh_Interval (UWORD)
#define IOUH_NAKTIMEOUT         58      // iouh_NakTimeout (ULONG, ms)
#define IOUH_SETUP_BMREQTYPE    62      // iouh_SetupData.bmRequestType (UBYTE)
#define IOUH_SETUP_BREQUEST     63      // iouh_SetupData.bRequest (UBYTE)
#define IOUH_SETUP_WVALUE       64      // iouh_SetupData.wValue (UWORD, little-endian)
#define IOUH_SETUP_WINDEX       66      // iouh_SetupData.wIndex (UWORD, little-endian)
#define IOUH_SETUP_WLENGTH      68      // iouh_SetupData.wLength (UWORD, little-endian)
#define IOUH_USERDATA           70      // iouh_UserData (APTR)
#define IOUH_EXTERROR           74      // iouh_ExtError (UWORD)
#define IOUSBHWREQ_SIZE         76      // sizeof(struct IOUsbHWReq)

/*
 * Poseidon usbhardware.device commands (devices/usbhardware.h 2.1).
 */
#define UHCMD_QUERYDEVICE       (CMD_NONSTD + 0)    // UHCMD_QUERYDEVICE
#define UHCMD_USBRESET          (CMD_NONSTD + 1)    // UHCMD_USBRESET
#define UHCMD_USBRESUME         (CMD_NONSTD + 2)    // UHCMD_USBRESUME
#define UHCMD_USBSUSPEND        CMD_STOP            // UHCMD_USBSUSPEND
#define UHCMD_USBOPER           CMD_START           // UHCMD_USBOPER
#define UHCMD_CONTROLXFER       (CMD_NONSTD + 3)    // UHCMD_CONTROLXFER
#define UHCMD_ISOXFER           (CMD_NONSTD + 4)    // UHCMD_ISOXFER
#define UHCMD_INTXFER           (CMD_NONSTD + 5)    // UHCMD_INTXFER
#define UHCMD_BULKXFER          (CMD_NONSTD + 6)    // UHCMD_BULKXFER

// iouh_Dir values
#define UHDIR_SETUP             0                   // UHDIR_SETUP
#define UHDIR_OUT               1                   // UHDIR_OUT
#define UHDIR_IN                2                   // UHDIR_IN

// iouh_Flags bits
#define UHFB_NAKTIMEOUT         3                   // UHFB_NAKTIMEOUT
#define UHFF_NAKTIMEOUT         (1 << UHFB_NAKTIMEOUT)

// io_Error codes
#define UHIOERR_NO_ERROR        0                   // UHIOERR_NO_ERROR
#define UHIOERR_USBOFFLINE      1                   // UHIOERR_USBOFFLINE
#define UHIOERR_NAK             2                   // UHIOERR_NAK
#define UHIOERR_HOSTERROR       3                   // UHIOERR_HOSTERROR
#define UHIOERR_STALL           4                   // UHIOERR_STALL
#define UHIOERR_TIMEOUT         6                   // UHIOERR_TIMEOUT
#define UHIOERR_NAKTIMEOUT      10                  // UHIOERR_NAKTIMEOUT
#define UHIOERR_BADPARAMS       11                  // UHIOERR_BADPARAMS
#define UHIOERR_OUTOFMEMORY     12                  // UHIOERR_OUTOFMEMORY

// iouh_State bits
#define UHSB_OPERATIONAL        0                   // UHSB_OPERATIONAL
#define UHSB_RESUMING           1                   // UHSB_RESUMING
#define UHSB_SUSPENDED          2                   // UHSB_SUSPENDED
#define UHSB_RESET              3                   // UHSB_RESET
#define UHSF_OPERATIONAL        (1 << UHSB_OPERATIONAL)
#define UHSF_RESUMING           (1 << UHSB_RESUMING)
#define UHSF_SUSPENDED          (1 << UHSB_SUSPENDED)
#define UHSF_RESET              (1 << UHSB_RESET)

// UHCMD_QUERYDEVICE tags
#define UHA_DUMMY               ((uae_u32)TAG_USER + 0x4711) // UHA_Dummy
#define UHA_STATE               (UHA_DUMMY + 0x01)  // UHA_State
#define UHA_MANUFACTURER        (UHA_DUMMY + 0x10)  // UHA_Manufacturer
#define UHA_PRODUCTNAME         (UHA_DUMMY + 0x11)  // UHA_ProductName
#define UHA_VERSION             (UHA_DUMMY + 0x12)  // UHA_Version
#define UHA_REVISION            (UHA_DUMMY + 0x13)  // UHA_Revision
#define UHA_DESCRIPTION         (UHA_DUMMY + 0x14)  // UHA_Description
#define UHA_COPYRIGHT           (UHA_DUMMY + 0x15)  // UHA_Copyright
#define UHA_DRIVERVERSION       (UHA_DUMMY + 0x20)  // UHA_DriverVersion

// utility/tagitem.h TagItem layout
#define TAG_ITEM_SIZE           8       // sizeof(struct TagItem)
#define TAG_ITEM_DATA           4       // ti_Data offset
#define MAX_TAG_ITEMS           256     // runaway guard when walking user taglists

/*
 * USB standard request codes and descriptor types (USB 1.1 chapter 9).
 */
#define USB_REQ_GET_STATUS      0x00
#define USB_REQ_CLEAR_FEATURE   0x01
#define USB_REQ_SET_FEATURE     0x03
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_CONFIGURATION 0x09

#define USB_DIR_IN              0x80    // bmRequestType/endpoint direction bit

// bmRequestType type field (bits 6-5)
#define USB_TYPE_STANDARD       0
#define USB_TYPE_CLASS          1

// bmRequestType recipient field (bits 4-0)
#define USB_RECIP_DEVICE        0
#define USB_RECIP_OTHER         3       // hub port

#define USB_DT_DEVICE           0x01
#define USB_DT_CONFIG           0x02
#define USB_DT_STRING           0x03
#define USB_DT_HUB              0x29

// Hub port features (USB 1.1 table 11-12)
#define USB_PORT_FEAT_CONNECTION        0
#define USB_PORT_FEAT_ENABLE            1
#define USB_PORT_FEAT_SUSPEND           2
#define USB_PORT_FEAT_OVER_CURRENT      3
#define USB_PORT_FEAT_RESET             4
#define USB_PORT_FEAT_POWER             8
#define USB_PORT_FEAT_LOWSPEED          9
#define USB_PORT_FEAT_C_CONNECTION      16
#define USB_PORT_FEAT_C_ENABLE          17
#define USB_PORT_FEAT_C_SUSPEND         18
#define USB_PORT_FEAT_C_OVER_CURRENT    19
#define USB_PORT_FEAT_C_RESET           20

// wPortStatus bits (a314 usb_protocol.h)
#define USB_PORT_CONNECTION     0x0001
#define USB_PORT_ENABLE         0x0002
#define USB_PORT_SUSPEND        0x0004
#define USB_PORT_OVER_CURRENT   0x0008
#define USB_PORT_RESET          0x0010
#define USB_PORT_POWER          0x0100
#define USB_PORT_LOW_SPEED      0x0200
#define USB_PORT_HIGH_SPEED     0x0400

// wPortChange bits (a314 usb_protocol.h)
#define USB_PORT_C_CONNECTION   0x0001
#define USB_PORT_C_ENABLE       0x0002
#define USB_PORT_C_SUSPEND      0x0004
#define USB_PORT_C_OVER_CURRENT 0x0008
#define USB_PORT_C_RESET        0x0010

// Interrupt-poll NakTimeout sentinel: poll until data, error or abort
#define USB_NAK_TIMEOUT_INFINITE 0xFFFFFFFF

// Real-device transfer tuning
#define USB_MAX_DEV_ADDR        127     // USB 7-bit address space
#define USB_MAX_INTERFACES      32      // width of the claim/detach interface bitmasks
#define USB_CTRL_TIMEOUT_MS     5000    // default control-transfer deadline
#define USB_OUT_TIMEOUT_MS      5000    // default OUT bulk/interrupt deadline
#define USB_BULK_TIMEOUT_MS     5000    // default bulk IN deadline when UHFF_NAKTIMEOUT is clear
#define USB_XFER_SLICE_MS       100     // IN park-loop slice; bounds abort latency
#define USB_INT_FALLBACK_PKT    64      // interrupt read size when iouh_MaxPktSize is 0

// Virtual root hub
#define ROOTHUB_PORT_COUNT      4       // a314 default port count
#define ROOTHUB_ADDR_NONE       (-1)    // no address assigned yet (bus default state)
#define ROOTHUB_INT_ENDPOINT    1       // status-change interrupt IN endpoint number
#define ROOTHUB_STRING_LANGID   0       // string descriptor index: LANGID table
#define ROOTHUB_STRING_MANUFACTURER 1   // string descriptor index: iManufacturer
#define ROOTHUB_STRING_PRODUCT  2       // string descriptor index: iProduct
#define HUB_CTRL_BUF_SIZE       64      // covers every synthesized descriptor
#define HUB_INT_POLL_SLICE_MS   10      // park-loop granularity for hub interrupt polls

// Host-side libusb integration
#define USB_HOTPLUG_POLL_MS     1000    // fallback enumeration-diff interval
#define USB_EVENT_TIMEOUT_MS    250     // hotplug event loop wakeup granularity

// Async request slot table
#define MAX_ASYNC_REQUESTS      64
#define ASYNC_REQUEST_NONE      0
#define ASYNC_REQUEST_TEMP      1       // request is queued to the worker
#define ASYNC_REQUEST_ACTIVE    2       // worker is processing the request

static int logUaeusb = 1;

/*
 * Per-VID:PID quirk switches (usbbridge.py hidpp_boot_mouse and
 * force_boot_protocol). Default OFF with no config surface yet, so the
 * quirk branches never run; the flags mark where the conversions plug in
 * without touching the generic hot path.
 */
static int quirkHidppBootMouse = 0;
static int quirkForceBootProtocol = 0;

/*
 * Single global HCD context: the driver exposes one USB bus, unit 0 is
 * the only unit and there is no per-unit state (a314 device.c model).
 */
struct HcdState
{
    int opencnt;
    volatile int stopping;              // parked workers must unpark and bail out
    volatile int activeWorkers;         // request threads not yet finished
    uae_u16 busState;                   // UHSF_* bus state
    volatile uaecptr asyncRequest[MAX_ASYNC_REQUESTS];
    volatile int asyncType[MAX_ASYNC_REQUESTS];
    volatile int asyncAborted[MAX_ASYNC_REQUESTS];
};

/*
 * Virtual root hub: a self-contained 4-port hub synthesized entirely in
 * host code (a314 usbbridge.py RootHub model). Ports are 1-based;
 * index 0 of the status/change arrays is unused.
 */
struct RootHubState
{
    int devAddr;                        // address Poseidon assigned via SET_ADDRESS
    uae_u8 configuration;               // SET_CONFIGURATION value
    uae_u16 portStatus[ROOTHUB_PORT_COUNT + 1];
    uae_u16 portChange[ROOTHUB_PORT_COUNT + 1];
};

/*
 * One virtual root-hub port mapped to a real host USB device. The device
 * is keyed for its whole attachment lifetime by (bus number, device
 * address) — stable per attachment, unlike VID:PID which breaks with
 * duplicate models and replug (usbbridge.py check_device_changes model).
 */
struct UsbPort
{
    libusb_device* dev;                 // ref held while attached, NULL = free port
    libusb_device_handle* handle;       // opened lazily; NULL until first use
    int busNumber;
    int deviceAddress;
    int lowSpeed;                       // LIBUSB_SPEED_LOW at attach time
    int openFailed;                     // libusb_open failed (permissions); don't retry
    volatile int pendingRemove;         // flagged by the hotplug callback, detached by the event thread
    volatile int inUse;                 // worker transfers in flight on the handle; defers close
    uae_u32 claimedMask;                // interfaces claimed after SET_CONFIGURATION
    uae_u32 detachedMask;               // interfaces whose kernel driver this session detached
};

/*
 * Host libusb context plus the event thread that services hotplug.
 * Ports are 1-based to match the root hub arrays; index 0 unused.
 */
struct UsbHostState
{
    libusb_context* ctx;
    volatile int eventThreadRunning;
    volatile int eventThreadStop;
    int hotplugSupported;
    libusb_hotplug_callback_handle hotplugHandle;
    struct UsbPort ports[ROOTHUB_PORT_COUNT + 1];
    int devAddrMap[USB_MAX_DEV_ADDR + 1]; // Poseidon-assigned USB address -> port, 0 = unmapped
    int enumeratingPort;                // port reset started enumeration at address 0; 0 = none
    uae_sem_t lock;                     // protects ports[], address map and root hub port bits
    uae_sem_t eventSyncSem;             // event thread start/stop handshake
};

static struct HcdState hcd;
static struct RootHubState rootHub;
static struct UsbHostState usbHost;
static uae_sem_t asyncSem;              // serializes worker vs abort/reset
static int semsInitialized;

static uaecptr ROM_uaeusb_resname;
static uaecptr ROM_uaeusb_resid;
static uaecptr ROM_uaeusb_init;

// UHCMD_QUERYDEVICE answer strings, emitted into rtarea so the taglist
// responses can point at Amiga-addressable memory
static uaecptr ROM_uaeusb_manufacturer;
static uaecptr ROM_uaeusb_productname;
static uaecptr ROM_uaeusb_description;
static uaecptr ROM_uaeusb_copyright;

/*
 * Root hub descriptors (a314 usbbridge.py RootHub).
 */
static const uae_u8 hubDeviceDescriptor[] =
{
    0x12,                       // bLength
    USB_DT_DEVICE,              // bDescriptorType
    0x10, 0x01,                 // bcdUSB 1.10
    0x09,                       // bDeviceClass = Hub
    0x00, 0x00,                 // bDeviceSubClass, bDeviceProtocol
    0x08,                       // bMaxPacketSize0
    0x27, 0x06,                 // idVendor 0x0627
    0x00, 0x00,                 // idProduct
    0x00, 0x01,                 // bcdDevice
    ROOTHUB_STRING_MANUFACTURER,// iManufacturer
    ROOTHUB_STRING_PRODUCT,     // iProduct
    0x00,                       // iSerialNumber
    0x01,                       // bNumConfigurations
};

static const uae_u8 hubConfigDescriptor[] =
{
    // Configuration descriptor
    0x09, USB_DT_CONFIG,
    0x19, 0x00,                 // wTotalLength = 25
    0x01,                       // bNumInterfaces
    0x01,                       // bConfigurationValue
    0x00,                       // iConfiguration
    0xC0,                       // bmAttributes (self-powered)
    0x00,                       // bMaxPower
    // Interface descriptor
    0x09, 0x04,                 // bLength, INTERFACE
    0x00, 0x00,                 // bInterfaceNumber, bAlternateSetting
    0x01,                       // bNumEndpoints
    0x09,                       // bInterfaceClass = Hub
    0x00, 0x00,                 // bInterfaceSubClass, bInterfaceProtocol
    0x00,                       // iInterface
    // Endpoint descriptor: EP1 IN, interrupt, status change
    0x07, 0x05,                 // bLength, ENDPOINT
    USB_DIR_IN | ROOTHUB_INT_ENDPOINT,
    0x03,                       // bmAttributes = Interrupt
    0x01, 0x00,                 // wMaxPacketSize = 1
    0xFF,                       // bInterval
};

static const uae_u8 hubClassDescriptor[] =
{
    0x09, USB_DT_HUB,
    ROOTHUB_PORT_COUNT,         // bNbrPorts
    0x00, 0x00,                 // wHubCharacteristics
    0x32,                       // bPwrOn2PwrGood (100 ms)
    0x00,                       // bHubContrCurrent
    0x00, 0xFF,                 // DeviceRemovable, PortPwrCtrlMask
};

static void ioLog (const TCHAR* msg, uaecptr request)
{
    if (logUaeusb)
        write_log (_T("%s: req=%08X cmd=%d len=%u actual=%u io_error=%d\n"),
            msg, request, get_word (request + IO_COMMAND),
            get_long (request + IOUH_LENGTH), get_long (request + IOUH_ACTUAL),
            (uae_s8)get_byte (request + IO_ERROR));
}

// Poseidon stores setup-packet words little-endian (USB wire order); get_word
// reads big-endian, so the value is recovered by swapping
static uae_u16 swapLe16 (uae_u16 v)
{
    return (uae_u16)((v >> 8) | (v << 8));
}

static void resetRootHubLocked (void)
{
    rootHub.devAddr = ROOTHUB_ADDR_NONE;
    rootHub.configuration = 0;
    // A bus reset returns every device to the default (unaddressed) state
    for (int addr = 0; addr <= USB_MAX_DEV_ADDR; addr++)
        usbHost.devAddrMap[addr] = 0;
    usbHost.enumeratingPort = 0;
    for (int port = 1; port <= ROOTHUB_PORT_COUNT; port++)
    {
        // Ports come up powered; a mapped real device reports its connection
        // (and speed) with the connect-change bit latched so the hub driver
        // re-enumerates it after the bus reset
        uae_u16 status = USB_PORT_POWER;
        uae_u16 change = 0;
        if (usbHost.ports[port].dev)
        {
            status |= USB_PORT_CONNECTION;
            if (usbHost.ports[port].lowSpeed)
                status |= USB_PORT_LOW_SPEED;
            change |= USB_PORT_C_CONNECTION;
        }
        rootHub.portStatus[port] = status;
        rootHub.portChange[port] = change;
    }
}

static void resetRootHub (void)
{
    if (semsInitialized)
        uae_sem_wait (&usbHost.lock);
    resetRootHubLocked ();
    if (semsInitialized)
        uae_sem_post (&usbHost.lock);
}

/*
 * ---- Host libusb layer: enumeration, hotplug, port mapping ----
 */

// Called with usbHost.lock held
static int usbFindPortByKey (int busNumber, int deviceAddress)
{
    for (int port = 1; port <= ROOTHUB_PORT_COUNT; port++)
    {
        if (usbHost.ports[port].dev &&
            usbHost.ports[port].busNumber == busNumber &&
            usbHost.ports[port].deviceAddress == deviceAddress)
            return port;
    }
    return 0;
}

// Called with usbHost.lock held
static int usbFindFreePort (void)
{
    for (int port = 1; port <= ROOTHUB_PORT_COUNT; port++)
    {
        if (!usbHost.ports[port].dev)
            return port;
    }
    return 0;
}

/*
 * Return a real device to the host: release every interface this session
 * claimed and reattach the kernel drivers it detached (usbbridge.py
 * release_device model). Called with usbHost.lock held; each libusb call
 * fails harmlessly (LIBUSB_ERROR_NO_DEVICE) when the device is already
 * physically gone.
 */
static void usbPortReleaseInterfacesLocked (struct UsbPort* p)
{
    if (p->handle)
    {
        for (int intf = 0; intf < USB_MAX_INTERFACES; intf++)
        {
            if (p->claimedMask & (1u << intf))
                libusb_release_interface (p->handle, intf);
            if (p->detachedMask & (1u << intf))
                libusb_attach_kernel_driver (p->handle, intf);
        }
    }
    p->claimedMask = 0;
    p->detachedMask = 0;
}

// Called with usbHost.lock held; never called from hotplug-callback context
// (libusb_close is not callback-safe). Releases the device to the host,
// closes the handle, drops the device ref and reports the disconnect on
// the virtual port.
static void usbDetachPortLocked (int port)
{
    struct UsbPort* p = &usbHost.ports[port];
    if (!p->dev)
        return;
    if (p->inUse)
    {
        // A worker transfer holds the handle: the transfer fails on its own
        // (LIBUSB_ERROR_NO_DEVICE) and the event loop retries the detach
        p->pendingRemove = 1;
        return;
    }
    if (logUaeusb)
        write_log (_T("uaeusb.device: detached port=%d bus=%d addr=%d\n"),
            port, p->busNumber, p->deviceAddress);
    usbPortReleaseInterfacesLocked (p);
    if (p->handle)
    {
        libusb_close (p->handle);
        p->handle = NULL;
    }
    libusb_unref_device (p->dev);
    p->dev = NULL;
    p->openFailed = 0;
    p->pendingRemove = 0;
    for (int addr = 0; addr <= USB_MAX_DEV_ADDR; addr++)
    {
        if (usbHost.devAddrMap[addr] == port)
            usbHost.devAddrMap[addr] = 0;
    }
    if (usbHost.enumeratingPort == port)
        usbHost.enumeratingPort = 0;
    rootHub.portStatus[port] &= ~(USB_PORT_CONNECTION | USB_PORT_ENABLE |
        USB_PORT_SUSPEND | USB_PORT_LOW_SPEED);
    rootHub.portChange[port] |= USB_PORT_C_CONNECTION;
}

/*
 * Map a real host device to a free virtual root-hub port. Hubs are skipped:
 * the Amiga sees only the virtual root hub with real devices as its direct
 * children. Safe to call from the hotplug callback (only cached-descriptor
 * libusb calls are made). The low-speed port bit is derived from the host
 * bus speed; unknown speed leaves it clear (full-speed assumption).
 */
static void usbDeviceAttached (libusb_device* dev)
{
    struct libusb_device_descriptor desc;
    if (libusb_get_device_descriptor (dev, &desc) != 0)
        return;
    if (desc.bDeviceClass == LIBUSB_CLASS_HUB)
        return;

    int busNumber = libusb_get_bus_number (dev);
    int deviceAddress = libusb_get_device_address (dev);
    int lowSpeed = libusb_get_device_speed (dev) == LIBUSB_SPEED_LOW;

    uae_sem_wait (&usbHost.lock);
    if (usbFindPortByKey (busNumber, deviceAddress))
    {
        // Already mapped (initial scan vs hotplug race)
        uae_sem_post (&usbHost.lock);
        return;
    }
    int port = usbFindFreePort ();
    if (!port)
    {
        uae_sem_post (&usbHost.lock);
        write_log (_T("uaeusb.device: no free port for %04x:%04x (bus=%d addr=%d)\n"),
            desc.idVendor, desc.idProduct, busNumber, deviceAddress);
        return;
    }
    struct UsbPort* p = &usbHost.ports[port];
    libusb_ref_device (dev);
    p->dev = dev;
    p->handle = NULL;
    p->busNumber = busNumber;
    p->deviceAddress = deviceAddress;
    p->lowSpeed = lowSpeed;
    p->openFailed = 0;
    p->pendingRemove = 0;
    rootHub.portStatus[port] |= USB_PORT_CONNECTION;
    if (lowSpeed)
        rootHub.portStatus[port] |= USB_PORT_LOW_SPEED;
    else
        rootHub.portStatus[port] &= ~USB_PORT_LOW_SPEED;
    rootHub.portChange[port] |= USB_PORT_C_CONNECTION;
    uae_sem_post (&usbHost.lock);
    write_log (_T("uaeusb.device: attached port=%d bus=%d addr=%d vid=%04x pid=%04x%s\n"),
        port, busNumber, deviceAddress, desc.idVendor, desc.idProduct,
        lowSpeed ? _T(" (low-speed)") : _T(""));
}

// Enumerate the host bus and map every eligible new device
static void usbScanDevices (void)
{
    libusb_device** list;
    ssize_t count = libusb_get_device_list (usbHost.ctx, &list);
    if (count < 0)
    {
        write_log (_T("uaeusb.device: libusb_get_device_list failed: %s\n"),
            libusb_error_name ((int)count));
        return;
    }
    for (ssize_t i = 0; i < count; i++)
        usbDeviceAttached (list[i]);
    libusb_free_device_list (list, 1);
}

/*
 * Fallback hotplug detection when the runtime lacks hotplug support:
 * enumeration diff keyed by (bus, address). That pair is stable for the
 * lifetime of an attachment, so two identical models coexist and a replug
 * (which yields a new address) presents as a clean removal followed by an
 * insertion (usbbridge.py check_device_changes model).
 */
static void usbCheckDeviceChanges (void)
{
    libusb_device** list;
    ssize_t count = libusb_get_device_list (usbHost.ctx, &list);
    if (count < 0)
        return;

    int hadRemoval = 0;
    uae_sem_wait (&usbHost.lock);
    for (int port = 1; port <= ROOTHUB_PORT_COUNT; port++)
    {
        if (!usbHost.ports[port].dev)
            continue;
        int present = 0;
        for (ssize_t i = 0; i < count; i++)
        {
            if (libusb_get_bus_number (list[i]) == usbHost.ports[port].busNumber &&
                libusb_get_device_address (list[i]) == usbHost.ports[port].deviceAddress)
            {
                present = 1;
                break;
            }
        }
        if (!present)
        {
            usbDetachPortLocked (port);
            hadRemoval = 1;
        }
    }
    uae_sem_post (&usbHost.lock);

    // Let the hub driver see a disconnect before inserting new devices
    if (!hadRemoval)
    {
        for (ssize_t i = 0; i < count; i++)
            usbDeviceAttached (list[i]);
    }
    libusb_free_device_list (list, 1);
}

// Finish detaches flagged by the hotplug callback (event thread context,
// outside the callback, where libusb_close is safe)
static void usbProcessPendingRemovals (void)
{
    uae_sem_wait (&usbHost.lock);
    for (int port = 1; port <= ROOTHUB_PORT_COUNT; port++)
    {
        if (usbHost.ports[port].dev && usbHost.ports[port].pendingRemove)
            usbDetachPortLocked (port);
    }
    uae_sem_post (&usbHost.lock);
}

static int LIBUSB_CALL usbHotplugCallback (libusb_context* ctx, libusb_device* dev,
    libusb_hotplug_event event, void* userData)
{
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
    {
        usbDeviceAttached (dev);
    }
    else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT)
    {
        // libusb_close must not run in callback context: flag the port and
        // let the event thread finish the detach after handle_events returns
        uae_sem_wait (&usbHost.lock);
        int port = usbFindPortByKey (libusb_get_bus_number (dev),
            libusb_get_device_address (dev));
        if (port)
            usbHost.ports[port].pendingRemove = 1;
        uae_sem_post (&usbHost.lock);
    }
    return 0;
}

/*
 * Host event thread: services libusb hotplug events, or falls back to
 * periodic enumeration diffing when the runtime has no hotplug support.
 * All blocking/polling happens here, never on the emulation thread.
 */
static void* usbEventThread (void* arg)
{
    usbHost.eventThreadRunning = 1;
    uae_sem_post (&usbHost.eventSyncSem);
    while (!usbHost.eventThreadStop)
    {
        if (usbHost.hotplugSupported)
        {
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = USB_EVENT_TIMEOUT_MS * 1000;
            libusb_handle_events_timeout_completed (usbHost.ctx, &tv, NULL);
            usbProcessPendingRemovals ();
        }
        else
        {
            usbCheckDeviceChanges ();
            usbProcessPendingRemovals ();
            for (int waited = 0; waited < USB_HOTPLUG_POLL_MS && !usbHost.eventThreadStop;
                 waited += HUB_INT_POLL_SLICE_MS)
                sleep_millis (HUB_INT_POLL_SLICE_MS);
        }
    }
    usbHost.eventThreadRunning = 0;
    uae_sem_post (&usbHost.eventSyncSem);
    return 0;
}

// Bring up the host side: libusb context, initial bus scan, hotplug.
// Failure is graceful: the device still opens, the bus just has no ports
// with anything connected.
static int usbHostInit (void)
{
    if (usbHost.ctx)
        return 1;
    int rc = libusb_init (&usbHost.ctx);
    if (rc != 0)
    {
        write_log (_T("uaeusb.device: libusb_init failed: %s\n"), libusb_error_name (rc));
        usbHost.ctx = NULL;
        return 0;
    }
    usbHost.hotplugSupported = libusb_has_capability (LIBUSB_CAP_HAS_HOTPLUG) != 0;
    usbScanDevices ();
    if (usbHost.hotplugSupported)
    {
        rc = libusb_hotplug_register_callback (usbHost.ctx,
            (libusb_hotplug_event)(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
            (libusb_hotplug_flag)0,
            LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
            usbHotplugCallback, NULL, &usbHost.hotplugHandle);
        if (rc != LIBUSB_SUCCESS)
        {
            write_log (_T("uaeusb.device: hotplug registration failed (%s), polling instead\n"),
                libusb_error_name (rc));
            usbHost.hotplugSupported = 0;
        }
    }
    if (logUaeusb)
        write_log (_T("uaeusb.device: libusb up, hotplug=%s\n"),
            usbHost.hotplugSupported ? _T("event") : _T("poll"));
    usbHost.eventThreadStop = 0;
    uae_sem_init (&usbHost.eventSyncSem, 0, 0);
    uae_start_thread (_T("uaeusb_ev"), usbEventThread, NULL, NULL);
    uae_sem_wait (&usbHost.eventSyncSem);
    return 1;
}

// Tear down the host side: stop the event thread, release every mapped
// device, exit libusb
static void usbHostExit (void)
{
    if (!usbHost.ctx)
        return;
    usbHost.eventThreadStop = 1;
    if (usbHost.hotplugSupported)
        libusb_hotplug_deregister_callback (usbHost.ctx, usbHost.hotplugHandle);
    uae_sem_wait (&usbHost.eventSyncSem);
    uae_sem_wait (&usbHost.lock);
    for (int port = 1; port <= ROOTHUB_PORT_COUNT; port++)
        usbDetachPortLocked (port);
    uae_sem_post (&usbHost.lock);
    libusb_exit (usbHost.ctx);
    usbHost.ctx = NULL;
    usbHost.hotplugSupported = 0;
}

/*
 * Lazily open the libusb handle for a mapped port (first transfer, port
 * power, port reset). Open failure — typically missing permissions — is
 * remembered so the device stays visible on the hub while its transfers
 * fail cleanly instead of retrying libusb_open on every request. The open
 * runs with the port lock released: the device ref keeps it valid, and a
 * concurrent detach is reconciled afterwards.
 */
static int usbPortOpenHandle (int port)
{
    if (port < 1 || port > ROOTHUB_PORT_COUNT)
        return 0;
    uae_sem_wait (&usbHost.lock);
    struct UsbPort* p = &usbHost.ports[port];
    libusb_device* dev = p->dev;
    if (!dev || p->handle || p->openFailed)
    {
        int ok = dev && p->handle != NULL;
        uae_sem_post (&usbHost.lock);
        return ok;
    }
    libusb_ref_device (dev);
    uae_sem_post (&usbHost.lock);

    libusb_device_handle* handle = NULL;
    int rc = libusb_open (dev, &handle);

    uae_sem_wait (&usbHost.lock);
    int ok = 0;
    if (p->dev == dev)
    {
        if (rc == 0)
        {
            p->handle = handle;
            handle = NULL;
            ok = 1;
        }
        else
        {
            p->openFailed = 1;
            write_log (_T("uaeusb.device: libusb_open failed for port %d (bus=%d addr=%d): %s (check device permissions)\n"),
                port, p->busNumber, p->deviceAddress, libusb_error_name (rc));
        }
    }
    uae_sem_post (&usbHost.lock);
    if (handle)
        libusb_close (handle);
    libusb_unref_device (dev);
    return ok;
}

/*
 * Pin the port handle for one worker transfer. While inUse is nonzero the
 * event thread defers libusb_close on a detach, so a blocking synchronous
 * transfer never races the handle being closed under it; the transfer
 * itself fails with LIBUSB_ERROR_NO_DEVICE when the device disappears.
 */
static libusb_device_handle* usbPortAcquireHandle (int port)
{
    if (port < 1 || port > ROOTHUB_PORT_COUNT)
        return NULL;
    usbPortOpenHandle (port);
    uae_sem_wait (&usbHost.lock);
    struct UsbPort* p = &usbHost.ports[port];
    libusb_device_handle* handle = p->dev && !p->pendingRemove ? p->handle : NULL;
    if (handle)
        p->inUse++;
    uae_sem_post (&usbHost.lock);
    return handle;
}

static void usbPortReleaseHandle (int port)
{
    uae_sem_wait (&usbHost.lock);
    usbHost.ports[port].inUse--;
    uae_sem_post (&usbHost.lock);
}

// libusb error -> Poseidon io_Error. A STALL (LIBUSB_ERROR_PIPE) is a normal
// response class drivers probe optional requests with, never a host error.
static int mapLibusbError (int rc)
{
    switch (rc)
    {
    case LIBUSB_ERROR_PIPE:
        return UHIOERR_STALL;
    case LIBUSB_ERROR_TIMEOUT:
        return UHIOERR_NAKTIMEOUT;
    case LIBUSB_ERROR_NO_DEVICE:
    case LIBUSB_ERROR_NOT_FOUND:
        return UHIOERR_USBOFFLINE;
    default:
        return UHIOERR_HOSTERROR;
    }
}

// Resolve a Poseidon-assigned USB address to the mapped virtual port
static int portForDevAddr (int devAddr)
{
    if (devAddr < 1 || devAddr > USB_MAX_DEV_ADDR)
        return 0;
    uae_sem_wait (&usbHost.lock);
    int port = usbHost.devAddrMap[devAddr];
    if (port && !usbHost.ports[port].dev)
        port = 0;
    uae_sem_post (&usbHost.lock);
    return port;
}

/*
 * NakTimeout policy shared by the transfer handlers: UHFF_NAKTIMEOUT clear
 * or the 0xFFFFFFFF sentinel means no deadline; otherwise iouh_NakTimeout
 * is a deadline in milliseconds.
 */
static int nakTimeoutInfinite (uaecptr request)
{
    return !(get_word (request + IOUH_FLAGS) & UHFF_NAKTIMEOUT) ||
        get_long (request + IOUH_NAKTIMEOUT) == USB_NAK_TIMEOUT_INFINITE;
}

// Deadline for single-shot (OUT/control) transfers where an unbounded
// blocking call would wedge abort handling: the sentinel and the unset
// flag both fall back to defaultMs
static unsigned int boundedTimeout (uaecptr request, unsigned int defaultMs)
{
    if (nakTimeoutInfinite (request))
        return defaultMs;
    uae_u32 nakTimeout = get_long (request + IOUH_NAKTIMEOUT);
    return nakTimeout ? nakTimeout : defaultMs;
}

/*
 * SET_CONFIGURATION addressed to a real device: configure it on the host
 * and take ownership of the config's interfaces. Kernel drivers are
 * detached first (Linux needs this for set_configuration and claiming to
 * succeed) and every detach is recorded so the driver is reattached when
 * the device is released back to the host (usbbridge.py model).
 * libusb_set_auto_detach_kernel_driver is enabled as well so platforms
 * that support it also self-heal on libusb_close.
 */
static int devSetConfiguration (uaecptr request, int port, int configValue)
{
    libusb_device_handle* handle = usbPortAcquireHandle (port);
    if (!handle)
        return UHIOERR_USBOFFLINE;
    struct UsbPort* p = &usbHost.ports[port];

    libusb_set_auto_detach_kernel_driver (handle, 1);

    // A previously configured session releases its claims first so the new
    // configuration starts from a clean baseline
    uae_sem_wait (&usbHost.lock);
    usbPortReleaseInterfacesLocked (p);
    uae_sem_post (&usbHost.lock);

    struct libusb_config_descriptor* cfg = NULL;
    if (libusb_get_config_descriptor_by_value (libusb_get_device (handle),
        (uae_u8)configValue, &cfg) != 0)
        cfg = NULL;

    uae_u32 detached = 0;
    if (cfg)
    {
        for (int i = 0; i < cfg->bNumInterfaces; i++)
        {
            int intfNum = cfg->interface[i].altsetting[0].bInterfaceNumber;
            if (intfNum >= USB_MAX_INTERFACES)
                continue;
            if (libusb_kernel_driver_active (handle, intfNum) == 1 &&
                libusb_detach_kernel_driver (handle, intfNum) == 0)
                detached |= 1u << intfNum;
        }
    }

    int ioError = UHIOERR_NO_ERROR;
    uae_u32 claimed = 0;
    int rc = libusb_set_configuration (handle, configValue);
    if (rc != 0)
    {
        write_log (_T("uaeusb.device: set_configuration %d failed on port %d: %s\n"),
            configValue, port, libusb_error_name (rc));
        ioError = mapLibusbError (rc);
    }
    else if (cfg)
    {
        for (int i = 0; i < cfg->bNumInterfaces; i++)
        {
            int intfNum = cfg->interface[i].altsetting[0].bInterfaceNumber;
            if (intfNum >= USB_MAX_INTERFACES)
                continue;
            rc = libusb_claim_interface (handle, intfNum);
            if (rc == 0)
            {
                claimed |= 1u << intfNum;
                if (logUaeusb)
                    write_log (_T("uaeusb.device: claimed interface %d on port %d\n"),
                        intfNum, port);
                if (quirkForceBootProtocol)
                {
                    // Forced HID boot protocol (SET_PROTOCOL) plugs in here;
                    // deliberately unimplemented while the quirk has no
                    // config surface
                }
            }
            else
            {
                write_log (_T("uaeusb.device: claim interface %d failed on port %d: %s\n"),
                    intfNum, port, libusb_error_name (rc));
                ioError = UHIOERR_HOSTERROR;
            }
        }
    }

    uae_sem_wait (&usbHost.lock);
    p->claimedMask = claimed;
    p->detachedMask = detached;
    uae_sem_post (&usbHost.lock);
    if (cfg)
        libusb_free_config_descriptor (cfg);
    usbPortReleaseHandle (port);
    put_long (request + IOUH_ACTUAL, 0);
    return ioError;
}

/*
 * Host-side action for a virtual port reset. When a prior session on this
 * emulator run left the device configured with claimed interfaces, the
 * claims are released and libusb_reset_device returns the device to the
 * Default state so Poseidon's re-enumeration starts from a clean slate,
 * mirroring a genuine bus reset. A freshly mapped device is left alone:
 * it is already in the state the host kernel put it in and a gratuitous
 * reset is slow and can re-trigger kernel driver binding.
 * LIBUSB_ERROR_NOT_FOUND from the reset means the device re-enumerated on
 * the host bus: the mapping is dead, so the port is flagged for detach and
 * hotplug re-maps the device.
 */
static void usbPortHostReset (int port)
{
    libusb_device_handle* handle = usbPortAcquireHandle (port);
    if (!handle)
        return;
    struct UsbPort* p = &usbHost.ports[port];
    uae_sem_wait (&usbHost.lock);
    int wasConfigured = p->claimedMask != 0 || p->detachedMask != 0;
    if (wasConfigured)
        usbPortReleaseInterfacesLocked (p);
    // The restarted enumeration invalidates addresses previously bound here
    for (int addr = 0; addr <= USB_MAX_DEV_ADDR; addr++)
    {
        if (usbHost.devAddrMap[addr] == port)
            usbHost.devAddrMap[addr] = 0;
    }
    uae_sem_post (&usbHost.lock);
    if (wasConfigured)
    {
        int rc = libusb_reset_device (handle);
        if (logUaeusb)
            write_log (_T("uaeusb.device: host reset port=%d: %s\n"),
                port, rc == 0 ? _T("ok") : libusb_error_name (rc));
        if (rc == LIBUSB_ERROR_NOT_FOUND)
        {
            uae_sem_wait (&usbHost.lock);
            p->pendingRemove = 1;
            uae_sem_post (&usbHost.lock);
        }
    }
    usbPortReleaseHandle (port);
}

/*
 * Control transfer to a real device (worker context). The setup packet
 * words are stored little-endian by Poseidon (USB wire order) and swapped
 * to host order here; the data stage lives in Amiga RAM at iouh_Data and
 * is bounced through a host buffer.
 */
static int devControlTransfer (uaecptr request, int port)
{
    uae_u8 bmRT = get_byte (request + IOUH_SETUP_BMREQTYPE);
    uae_u8 bReq = get_byte (request + IOUH_SETUP_BREQUEST);
    uae_u16 wValue = swapLe16 (get_word (request + IOUH_SETUP_WVALUE));
    uae_u16 wIndex = swapLe16 (get_word (request + IOUH_SETUP_WINDEX));
    uae_u16 wLength = swapLe16 (get_word (request + IOUH_SETUP_WLENGTH));

    // Configuring a device implies claiming it on the host
    if (bmRT == 0 && bReq == USB_REQ_SET_CONFIGURATION)
        return devSetConfiguration (request, port, wValue);

    libusb_device_handle* handle = usbPortAcquireHandle (port);
    if (!handle)
        return UHIOERR_USBOFFLINE;

    uae_u8* buf = NULL;
    if (wLength)
    {
        buf = xmalloc (uae_u8, wLength);
        if (!buf)
        {
            usbPortReleaseHandle (port);
            return UHIOERR_OUTOFMEMORY;
        }
        if (!(bmRT & USB_DIR_IN))
            memcpyah_safe (buf, get_long (request + IOUH_DATA), wLength);
    }

    int rc = libusb_control_transfer (handle, bmRT, bReq, wValue, wIndex,
        buf, wLength, boundedTimeout (request, USB_CTRL_TIMEOUT_MS));

    int ioError;
    if (rc >= 0)
    {
        if ((bmRT & USB_DIR_IN) && rc > 0)
            memcpyha_safe (get_long (request + IOUH_DATA), buf, rc);
        put_long (request + IOUH_ACTUAL, (uae_u32)rc);
        ioError = UHIOERR_NO_ERROR;
    }
    else
    {
        put_long (request + IOUH_ACTUAL, 0);
        ioError = mapLibusbError (rc);
        // A control STALL is an expected probe result, not log-worthy noise
        if (rc != LIBUSB_ERROR_PIPE)
            write_log (_T("uaeusb.device: control xfer failed port=%d bmRT=%02X bReq=%02X: %s\n"),
                port, bmRT, bReq, libusb_error_name (rc));
    }
    xfree (buf);
    usbPortReleaseHandle (port);
    return ioError;
}

/*
 * IN park loop shared by real-device interrupt and bulk reads: block in
 * ≤USB_XFER_SLICE_MS slices until data arrives, the NAK deadline passes,
 * the request is aborted, the device errors out, or the driver shuts
 * down. No data is never reported as a 0-byte success — that causes a
 * re-poll storm in the Poseidon classes.
 */
static int devInTransferLoop (uaecptr request, int slot, libusb_device_handle* handle,
    uae_u8 endpoint, uae_u8* buf, uae_u32 readLen, uae_u32 copyLen, int interrupt,
    unsigned int fallbackTimeoutMs)
{
    int infinite = nakTimeoutInfinite (request);
    uae_u32 deadline = infinite ? fallbackTimeoutMs : get_long (request + IOUH_NAKTIMEOUT);
    int deadlineActive = !infinite || fallbackTimeoutMs != 0;
    uae_u32 elapsed = 0;

    for (;;)
    {
        int transferred = 0;
        int rc = interrupt ?
            libusb_interrupt_transfer (handle, endpoint, buf, (int)readLen,
                &transferred, USB_XFER_SLICE_MS) :
            libusb_bulk_transfer (handle, endpoint, buf, (int)readLen,
                &transferred, USB_XFER_SLICE_MS);
        if (rc == 0 || (rc == LIBUSB_ERROR_TIMEOUT && transferred > 0))
        {
            if (quirkHidppBootMouse)
            {
                // Logitech HID++ 7-byte to 3-byte boot-mouse report
                // conversion plugs in here; deliberately unimplemented
                // while the quirk has no config surface
            }
            uae_u32 actual = (uae_u32)transferred;
            if (actual > copyLen)
                actual = copyLen;
            if (actual)
                memcpyha_safe (get_long (request + IOUH_DATA), buf, actual);
            put_long (request + IOUH_ACTUAL, actual);
            return UHIOERR_NO_ERROR;
        }
        if (rc != LIBUSB_ERROR_TIMEOUT)
        {
            put_long (request + IOUH_ACTUAL, 0);
            write_log (_T("uaeusb.device: %s IN failed ep=%02X: %s\n"),
                interrupt ? _T("interrupt") : _T("bulk"), endpoint, libusb_error_name (rc));
            return mapLibusbError (rc);
        }
        if (slot >= 0 && hcd.asyncAborted[slot])
        {
            put_long (request + IOUH_ACTUAL, 0);
            return IOERR_ABORTED;
        }
        if (hcd.stopping)
        {
            put_long (request + IOUH_ACTUAL, 0);
            return IOERR_ABORTED;
        }
        elapsed += USB_XFER_SLICE_MS;
        if (deadlineActive && elapsed >= deadline)
        {
            put_long (request + IOUH_ACTUAL, 0);
            return UHIOERR_NAKTIMEOUT;
        }
    }
}

/*
 * Interrupt or bulk transfer to a real device (worker context).
 * Interrupt IN reads are sized at least the endpoint max packet size so a
 * full-size report never overflows the buffer, and never below the full
 * iouh_Length; the copy back to iouh_Data is capped at iouh_Length. An
 * interrupt IN with no deadline parks until data, error or abort; a bulk
 * IN with no deadline falls back to a bounded default so mass-storage
 * requests cannot wedge forever.
 */
static int devBulkOrIntTransfer (uaecptr request, int slot, int interrupt)
{
    int port = portForDevAddr (get_word (request + IOUH_DEVADDR));
    if (!port)
    {
        put_long (request + IOUH_ACTUAL, 0);
        return UHIOERR_HOSTERROR;
    }
    libusb_device_handle* handle = usbPortAcquireHandle (port);
    if (!handle)
    {
        put_long (request + IOUH_ACTUAL, 0);
        return UHIOERR_USBOFFLINE;
    }

    int dirIn = get_word (request + IOUH_DIR) == UHDIR_IN;
    uae_u8 endpoint = (uae_u8)((get_word (request + IOUH_ENDPOINT) & 0x0F) |
        (dirIn ? USB_DIR_IN : 0));
    uae_u32 length = get_long (request + IOUH_LENGTH);
    int ioError;

    if (dirIn)
    {
        uae_u32 readLen = length;
        if (interrupt)
        {
            uae_u16 maxPkt = get_word (request + IOUH_MAXPKTSIZE);
            uae_u32 minRead = maxPkt ? maxPkt : USB_INT_FALLBACK_PKT;
            if (readLen < minRead)
                readLen = minRead;
        }
        uae_u8* buf = xmalloc (uae_u8, readLen ? readLen : 1);
        if (!buf)
        {
            usbPortReleaseHandle (port);
            put_long (request + IOUH_ACTUAL, 0);
            return UHIOERR_OUTOFMEMORY;
        }
        ioError = devInTransferLoop (request, slot, handle, endpoint, buf,
            readLen, length, interrupt, interrupt ? 0 : USB_BULK_TIMEOUT_MS);
        xfree (buf);
    }
    else
    {
        uae_u8* buf = NULL;
        if (length)
        {
            buf = xmalloc (uae_u8, length);
            if (!buf)
            {
                usbPortReleaseHandle (port);
                put_long (request + IOUH_ACTUAL, 0);
                return UHIOERR_OUTOFMEMORY;
            }
            memcpyah_safe (buf, get_long (request + IOUH_DATA), length);
        }
        int transferred = 0;
        unsigned int timeout = boundedTimeout (request, USB_OUT_TIMEOUT_MS);
        int rc = interrupt ?
            libusb_interrupt_transfer (handle, endpoint, buf, (int)length,
                &transferred, timeout) :
            libusb_bulk_transfer (handle, endpoint, buf, (int)length,
                &transferred, timeout);
        if (rc == 0)
        {
            put_long (request + IOUH_ACTUAL, (uae_u32)transferred);
            ioError = UHIOERR_NO_ERROR;
        }
        else
        {
            put_long (request + IOUH_ACTUAL, 0);
            write_log (_T("uaeusb.device: %s OUT failed ep=%02X: %s\n"),
                interrupt ? _T("interrupt") : _T("bulk"), endpoint, libusb_error_name (rc));
            ioError = mapLibusbError (rc);
        }
        xfree (buf);
    }
    usbPortReleaseHandle (port);
    return ioError;
}

/*
 * Control-transfer routing, hub vs real device: address 0 belongs to the
 * root hub until a downstream device enumeration claims it (a later phase
 * tracks the enumerating device, a314 usbbridge.py model); the address
 * Poseidon assigned via SET_ADDRESS is recorded, never assumed to be 1.
 * Anything else is a real device and is dispatched to libusb later.
 */
static int isHubAddress (int devAddr)
{
    if (devAddr == 0)
        return 1;
    return rootHub.devAddr != ROOTHUB_ADDR_NONE && devAddr == rootHub.devAddr;
}

static int findAsyncSlot (uaecptr request)
{
    for (int i = 0; i < MAX_ASYNC_REQUESTS; i++)
    {
        if (hcd.asyncRequest[i] == request)
            return i;
    }
    return -1;
}

static int addAsyncRequest (uaecptr request, int type)
{
    int slot = findAsyncSlot (request);
    if (slot >= 0)
    {
        hcd.asyncType[slot] = type;
        return slot;
    }
    for (int i = 0; i < MAX_ASYNC_REQUESTS; i++)
    {
        if (hcd.asyncRequest[i] == 0)
        {
            hcd.asyncRequest[i] = request;
            hcd.asyncType[i] = type;
            hcd.asyncAborted[i] = 0;
            return i;
        }
    }
    return -1;
}

static void releaseAsyncRequest (uaecptr request)
{
    int slot = findAsyncSlot (request);
    if (slot >= 0)
    {
        hcd.asyncRequest[slot] = 0;
        hcd.asyncType[slot] = ASYNC_REQUEST_NONE;
        hcd.asyncAborted[slot] = 0;
    }
}

/*
 * Flag the request aborted and wait for the worker to complete and release
 * it. A queued request is completed IOERR_ABORTED when dequeued; a parked
 * hub interrupt poll notices the flag each poll slice and unparks.
 */
static void abortAsync (uaecptr request)
{
    for (;;)
    {
        // Hold asyncSem across the find + flag set so the slot cannot be
        // released (worker) and recycled (addAsyncRequest) between the two:
        // otherwise a stale store could land on a slot already reassigned to
        // an unrelated request and abort it spuriously. Sleep outside the
        // lock so parked workers can still take asyncSem to complete.
        uae_sem_wait (&asyncSem);
        int slot = findAsyncSlot (request);
        if (slot >= 0)
            hcd.asyncAborted[slot] = 1;
        uae_sem_post (&asyncSem);
        if (slot < 0)
            return;
        sleep_millis (HUB_INT_POLL_SLICE_MS);
    }
}

/*
 * String descriptors: indices 0 (LANGID), 1 and 2 are known; any other
 * index returns -1 so the caller STALLs instead of reporting a bogus
 * short success (a314 usbbridge.py get_string_descriptor model).
 */
static int buildHubStringDescriptor (int index, uae_u8* buf)
{
    if (index == ROOTHUB_STRING_LANGID)
    {
        buf[0] = 4;
        buf[1] = USB_DT_STRING;
        buf[2] = 0x09;                  // LANGID 0x0409 = English (US), LE
        buf[3] = 0x04;
        return 4;
    }
    const char* s;
    if (index == ROOTHUB_STRING_MANUFACTURER)
        s = "UAE";
    else if (index == ROOTHUB_STRING_PRODUCT)
        s = "UAE Virtual Hub";
    else
        return -1;
    int len = (int)strlen (s);
    buf[0] = (uae_u8)(2 + len * 2);
    buf[1] = USB_DT_STRING;
    for (int i = 0; i < len; i++)
    {
        // ASCII to UTF-16LE
        buf[2 + i * 2] = (uae_u8)s[i];
        buf[3 + i * 2] = 0;
    }
    return buf[0];
}

/*
 * Control request addressed to the virtual root hub. Synthesizes standard
 * device requests, hub class requests and per-port class requests entirely
 * in host code; anything unhandled STALLs (a314 RootHub model).
 */
static int hubControlTransfer (uaecptr request)
{
    uae_u8 bmRT = get_byte (request + IOUH_SETUP_BMREQTYPE);
    uae_u8 bReq = get_byte (request + IOUH_SETUP_BREQUEST);
    uae_u16 wValue = swapLe16 (get_word (request + IOUH_SETUP_WVALUE));
    uae_u16 wIndex = swapLe16 (get_word (request + IOUH_SETUP_WINDEX));
    uae_u16 wLength = swapLe16 (get_word (request + IOUH_SETUP_WLENGTH));
    int reqType = (bmRT >> 5) & 3;
    int recipient = bmRT & 0x1F;
    uae_u8 buf[HUB_CTRL_BUF_SIZE];
    int dataLen = 0;
    int stall = 1;
    int lazyOpenPort = 0;
    int hostResetPort = 0;

    if (reqType == USB_TYPE_STANDARD && recipient == USB_RECIP_DEVICE)
    {
        switch (bReq)
        {
        case USB_REQ_GET_DESCRIPTOR:
        {
            int descType = (wValue >> 8) & 0xFF;
            int descIndex = wValue & 0xFF;
            if (descType == USB_DT_DEVICE)
            {
                memcpy (buf, hubDeviceDescriptor, sizeof hubDeviceDescriptor);
                dataLen = sizeof hubDeviceDescriptor;
                stall = 0;
            }
            else if (descType == USB_DT_CONFIG)
            {
                memcpy (buf, hubConfigDescriptor, sizeof hubConfigDescriptor);
                dataLen = sizeof hubConfigDescriptor;
                stall = 0;
            }
            else if (descType == USB_DT_STRING)
            {
                dataLen = buildHubStringDescriptor (descIndex, buf);
                if (dataLen >= 0)
                    stall = 0;
            }
            break;
        }
        case USB_REQ_SET_ADDRESS:
            // Record whatever address Poseidon assigns; never assume 1
            rootHub.devAddr = wValue & 0x7F;
            if (logUaeusb)
                write_log (_T("uaeusb.device: root hub assigned address %d\n"), rootHub.devAddr);
            stall = 0;
            break;
        case USB_REQ_SET_CONFIGURATION:
            rootHub.configuration = (uae_u8)wValue;
            stall = 0;
            break;
        case USB_REQ_GET_STATUS:
            buf[0] = 0x01;              // self-powered, LE
            buf[1] = 0x00;
            dataLen = 2;
            stall = 0;
            break;
        }
    }
    else if (reqType == USB_TYPE_CLASS && recipient == USB_RECIP_DEVICE)
    {
        if (bReq == USB_REQ_GET_DESCRIPTOR && ((wValue >> 8) & 0xFF) == USB_DT_HUB)
        {
            memcpy (buf, hubClassDescriptor, sizeof hubClassDescriptor);
            dataLen = sizeof hubClassDescriptor;
            stall = 0;
        }
        else if (bReq == USB_REQ_GET_STATUS)
        {
            // wHubStatus + wHubChange: no local-power/over-current conditions
            memset (buf, 0, 4);
            dataLen = 4;
            stall = 0;
        }
    }
    else if (reqType == USB_TYPE_CLASS && recipient == USB_RECIP_OTHER)
    {
        int port = wIndex & 0xFF;
        if (port >= 1 && port <= ROOTHUB_PORT_COUNT)
        {
            uae_sem_wait (&usbHost.lock);
            if (bReq == USB_REQ_GET_STATUS)
            {
                // wPortStatus + wPortChange, both little-endian
                buf[0] = rootHub.portStatus[port] & 0xFF;
                buf[1] = (rootHub.portStatus[port] >> 8) & 0xFF;
                buf[2] = rootHub.portChange[port] & 0xFF;
                buf[3] = (rootHub.portChange[port] >> 8) & 0xFF;
                dataLen = 4;
                stall = 0;
            }
            else if (bReq == USB_REQ_SET_FEATURE)
            {
                int feature = wValue & 0xFF;
                if (feature == USB_PORT_FEAT_RESET)
                {
                    // The reset completes instantly: latch the change bit,
                    // leave the port enabled and re-assert the attach-time
                    // speed (a314 RootHub model)
                    rootHub.portChange[port] |= USB_PORT_C_RESET;
                    rootHub.portStatus[port] &= ~USB_PORT_RESET;
                    rootHub.portStatus[port] |= USB_PORT_ENABLE;
                    if (usbHost.ports[port].dev && usbHost.ports[port].lowSpeed)
                        rootHub.portStatus[port] |= USB_PORT_LOW_SPEED;
                    // A port reset restarts enumeration at address 0. A stale
                    // in-flight slot (overlapping reset, or an enumeration
                    // that never reached SET_ADDRESS) is superseded
                    // deterministically, with a warning when it belonged to
                    // a different port (usbbridge.py enumerating_device model)
                    if (usbHost.enumeratingPort && usbHost.enumeratingPort != port)
                        write_log (_T("uaeusb.device: port %d reset supersedes in-flight enumeration of port %d\n"),
                            port, usbHost.enumeratingPort);
                    usbHost.enumeratingPort = usbHost.ports[port].dev ? port : 0;
                    lazyOpenPort = port;
                    hostResetPort = port;
                    stall = 0;
                }
                else if (feature == USB_PORT_FEAT_POWER)
                {
                    rootHub.portStatus[port] |= USB_PORT_POWER;
                    lazyOpenPort = port;
                    stall = 0;
                }
                else if (feature == USB_PORT_FEAT_ENABLE)
                {
                    rootHub.portStatus[port] |= USB_PORT_ENABLE;
                    stall = 0;
                }
                else if (feature == USB_PORT_FEAT_SUSPEND)
                {
                    rootHub.portStatus[port] |= USB_PORT_SUSPEND;
                    stall = 0;
                }
            }
            else if (bReq == USB_REQ_CLEAR_FEATURE)
            {
                int feature = wValue & 0xFF;
                switch (feature)
                {
                case USB_PORT_FEAT_C_CONNECTION:
                    rootHub.portChange[port] &= ~USB_PORT_C_CONNECTION;
                    break;
                case USB_PORT_FEAT_C_RESET:
                    rootHub.portChange[port] &= ~USB_PORT_C_RESET;
                    break;
                case USB_PORT_FEAT_C_ENABLE:
                    rootHub.portChange[port] &= ~USB_PORT_C_ENABLE;
                    break;
                case USB_PORT_FEAT_C_SUSPEND:
                    rootHub.portChange[port] &= ~USB_PORT_C_SUSPEND;
                    break;
                case USB_PORT_FEAT_C_OVER_CURRENT:
                    rootHub.portChange[port] &= ~USB_PORT_C_OVER_CURRENT;
                    break;
                case USB_PORT_FEAT_ENABLE:
                    rootHub.portStatus[port] &= ~USB_PORT_ENABLE;
                    break;
                case USB_PORT_FEAT_SUSPEND:
                    rootHub.portStatus[port] &= ~USB_PORT_SUSPEND;
                    break;
                case USB_PORT_FEAT_POWER:
                    rootHub.portStatus[port] &= ~USB_PORT_POWER;
                    break;
                default:
                    // Succeed silently on other clear-feature requests
                    break;
                }
                stall = 0;
            }
            uae_sem_post (&usbHost.lock);
        }
    }

    // Port power/reset touches the real device for the first time: open the
    // libusb handle now, outside the port lock (failure leaves the port
    // visible; transfers to it fail cleanly)
    if (lazyOpenPort)
        usbPortOpenHandle (lazyOpenPort);

    // A port reset also re-syncs the host device outside the lock: release
    // stale claims and, when a prior session configured it, a real
    // libusb_reset_device (see usbPortHostReset)
    if (hostResetPort)
        usbPortHostReset (hostResetPort);

    if (stall)
    {
        if (logUaeusb)
            write_log (_T("uaeusb.device: hub STALL bmRT=%02X bReq=%02X wValue=%04X wIndex=%04X\n"),
                bmRT, bReq, wValue, wIndex);
        put_long (request + IOUH_ACTUAL, 0);
        return UHIOERR_STALL;
    }

    uae_u32 actual = 0;
    if ((bmRT & USB_DIR_IN) && dataLen > 0)
    {
        actual = dataLen < wLength ? (uae_u32)dataLen : (uae_u32)wLength;
        if (actual)
            memcpyha_safe (get_long (request + IOUH_DATA), buf, actual);
    }
    put_long (request + IOUH_ACTUAL, actual);
    return UHIOERR_NO_ERROR;
}

/*
 * UHCMD_CONTROLXFER routing (worker context). Address 0 belongs to the
 * device a port reset put into the default state, falling back to the
 * root hub before the hub has its own address; SET_ADDRESS at address 0
 * binds the enumerating device to the address Poseidon assigned (the host
 * device keeps its host-side address — libusb routes by handle). The hub
 * address goes to the synthesized hub; any other address resolves through
 * the enumeration-built map to a real device.
 */
static int usbControlTransfer (uaecptr request)
{
    int devAddr = get_word (request + IOUH_DEVADDR);
    uae_u8 bmRT = get_byte (request + IOUH_SETUP_BMREQTYPE);
    uae_u8 bReq = get_byte (request + IOUH_SETUP_BREQUEST);

    if (devAddr == 0)
    {
        if (bmRT == 0 && bReq == USB_REQ_SET_ADDRESS)
        {
            int newAddr = swapLe16 (get_word (request + IOUH_SETUP_WVALUE)) & USB_MAX_DEV_ADDR;
            uae_sem_wait (&usbHost.lock);
            int port = usbHost.enumeratingPort;
            if (port && newAddr)
            {
                usbHost.devAddrMap[newAddr] = port;
                usbHost.enumeratingPort = 0;
                uae_sem_post (&usbHost.lock);
                if (logUaeusb)
                    write_log (_T("uaeusb.device: SET_ADDRESS port %d -> addr %d\n"),
                        port, newAddr);
                put_long (request + IOUH_ACTUAL, 0);
                return UHIOERR_NO_ERROR;
            }
            uae_sem_post (&usbHost.lock);
            // No device enumerating: this SET_ADDRESS is the root hub being
            // given its own address
            return hubControlTransfer (request);
        }
        uae_sem_wait (&usbHost.lock);
        int port = usbHost.enumeratingPort;
        uae_sem_post (&usbHost.lock);
        if (port)
            return devControlTransfer (request, port);
        return hubControlTransfer (request);
    }

    if (isHubAddress (devAddr))
        return hubControlTransfer (request);

    int port = portForDevAddr (devAddr);
    if (!port)
    {
        write_log (_T("uaeusb.device: control xfer to unknown address %d\n"), devAddr);
        put_long (request + IOUH_ACTUAL, 0);
        return UHIOERR_HOSTERROR;
    }
    return devControlTransfer (request, port);
}

// Bitmap for the hub's status-change interrupt endpoint: bit N set when
// port N has a pending change, 0 when nothing changed
static int hubStatusChangeBitmap (void)
{
    int bitmap = 0;
    uae_sem_wait (&usbHost.lock);
    for (int port = 1; port <= ROOTHUB_PORT_COUNT; port++)
    {
        if (rootHub.portChange[port])
            bitmap |= 1 << port;
    }
    uae_sem_post (&usbHost.lock);
    return bitmap;
}

/*
 * Hub interrupt-IN poll (worker context, may park). Returns the port-change
 * bitmap when a change is pending; otherwise holds the request honoring
 * iouh_NakTimeout — never a fake 0-byte success, which would cause a
 * re-poll storm. UHFF_NAKTIMEOUT clear or the 0xFFFFFFFF sentinel means
 * park until data, abort or shutdown.
 */
static int hubInterruptTransfer (uaecptr request, int slot)
{
    if (get_word (request + IOUH_DIR) != UHDIR_IN)
    {
        put_long (request + IOUH_ACTUAL, 0);
        return UHIOERR_STALL;
    }

    uae_u32 length = get_long (request + IOUH_LENGTH);
    uae_u32 nakTimeout = get_long (request + IOUH_NAKTIMEOUT);
    int infinite = !(get_word (request + IOUH_FLAGS) & UHFF_NAKTIMEOUT) ||
        nakTimeout == USB_NAK_TIMEOUT_INFINITE;
    uae_u32 elapsed = 0;

    for (;;)
    {
        int bitmap = hubStatusChangeBitmap ();
        if (bitmap)
        {
            uae_u8 b = (uae_u8)bitmap;
            uae_u32 actual = length >= 1 ? 1 : 0;
            if (actual)
                memcpyha_safe (get_long (request + IOUH_DATA), &b, actual);
            put_long (request + IOUH_ACTUAL, actual);
            return UHIOERR_NO_ERROR;
        }
        if (slot >= 0 && hcd.asyncAborted[slot])
        {
            put_long (request + IOUH_ACTUAL, 0);
            return IOERR_ABORTED;
        }
        if (hcd.stopping)
        {
            put_long (request + IOUH_ACTUAL, 0);
            return IOERR_ABORTED;
        }
        if (!infinite && elapsed >= nakTimeout)
        {
            put_long (request + IOUH_ACTUAL, 0);
            return UHIOERR_NAKTIMEOUT;
        }
        sleep_millis (HUB_INT_POLL_SLICE_MS);
        elapsed += HUB_INT_POLL_SLICE_MS;
    }
}

/*
 * UHCMD_QUERYDEVICE: walk the caller's TagItem list directly in Amiga
 * memory with NextTagItem semantics (TAG_DONE/TAG_IGNORE/TAG_MORE/TAG_SKIP)
 * and answer the UHA_* capability tags (a314 usbhw.c cmdQueryDevice model).
 * Each ti_Data is a pointer to a ULONG/STRPTR slot the answer is stored to.
 */
static int usbQueryDevice (uaecptr request)
{
    uaecptr tags = get_long (request + IOUH_DATA);
    if (!tags)
        return UHIOERR_BADPARAMS;

    for (int guard = 0; guard < MAX_TAG_ITEMS && tags; guard++)
    {
        uae_u32 tag = get_long (tags);
        if (tag == TAG_DONE)
            break;
        if (tag == TAG_IGNORE)
        {
            tags += TAG_ITEM_SIZE;
            continue;
        }
        if (tag == TAG_MORE)
        {
            tags = get_long (tags + TAG_ITEM_DATA);
            continue;
        }
        if (tag == TAG_SKIP)
        {
            tags += TAG_ITEM_SIZE * (1 + get_long (tags + TAG_ITEM_DATA));
            continue;
        }
        uaecptr data = get_long (tags + TAG_ITEM_DATA);
        switch (tag)
        {
        case UHA_MANUFACTURER:
            put_long (data, ROM_uaeusb_manufacturer);
            break;
        case UHA_PRODUCTNAME:
            put_long (data, ROM_uaeusb_productname);
            break;
        case UHA_DESCRIPTION:
            put_long (data, ROM_uaeusb_description);
            break;
        case UHA_COPYRIGHT:
            put_long (data, ROM_uaeusb_copyright);
            break;
        case UHA_VERSION:
            put_long (data, UAEUSB_VERSION);
            break;
        case UHA_REVISION:
            put_long (data, UAEUSB_REVISION);
            break;
        case UHA_DRIVERVERSION:
            put_long (data, (UAEUSB_VERSION << 8) | UAEUSB_REVISION);
            break;
        case UHA_STATE:
            put_long (data, hcd.busState);
            break;
        default:
            break;
        }
        tags += TAG_ITEM_SIZE;
    }
    return UHIOERR_NO_ERROR;
}

/*
 * CMD_FLUSH: flag every outstanding request aborted, without waiting.
 * A request still queued to the worker completes IOERR_ABORTED when
 * dequeued; an in-flight parked IN transfer notices the flag within one
 * poll slice and unparks. The worker is the only completer, so exactly
 * one reply per request is preserved.
 */
static void usbFlushQueued (void)
{
    for (int i = 0; i < MAX_ASYNC_REQUESTS; i++)
    {
        if (hcd.asyncRequest[i])
            hcd.asyncAborted[i] = 1;
    }
}

/*
 * Command dispatch, shared by the inline (quick) path and the worker.
 * slot is the async slot index for worker-executed requests, -1 inline.
 */
static int usbDoCommand (uaecptr request, int slot)
{
    uae_u32 command = get_word (request + IO_COMMAND);
    int ioError = UHIOERR_NO_ERROR;

    put_long (request + IOUH_ACTUAL, 0);

    switch (command)
    {
    case CMD_RESET:
        hcd.busState = UHSF_OPERATIONAL;
        put_word (request + IOUH_STATE, hcd.busState);
        break;
    case CMD_FLUSH:
        usbFlushQueued ();
        break;
    case UHCMD_QUERYDEVICE:
        ioError = usbQueryDevice (request);
        put_word (request + IOUH_STATE, hcd.busState);
        break;
    case UHCMD_USBRESET:
        // Bus reset returns every device to the default (unaddressed) state
        hcd.busState = UHSF_OPERATIONAL;
        resetRootHub ();
        put_word (request + IOUH_STATE, hcd.busState);
        break;
    case UHCMD_USBRESUME:
        hcd.busState = UHSF_OPERATIONAL;
        put_word (request + IOUH_STATE, hcd.busState);
        break;
    case UHCMD_USBSUSPEND:
        hcd.busState = UHSF_SUSPENDED;
        put_word (request + IOUH_STATE, hcd.busState);
        break;
    case UHCMD_USBOPER:
        hcd.busState = UHSF_OPERATIONAL;
        put_word (request + IOUH_STATE, hcd.busState);
        break;
    case UHCMD_CONTROLXFER:
        ioError = usbControlTransfer (request);
        break;
    case UHCMD_INTXFER:
        if (isHubAddress (get_word (request + IOUH_DEVADDR)))
        {
            if (get_word (request + IOUH_ENDPOINT) == ROOTHUB_INT_ENDPOINT)
                ioError = hubInterruptTransfer (request, slot);
            else
                ioError = UHIOERR_STALL;    // the hub has no other endpoints
        }
        else
        {
            ioError = devBulkOrIntTransfer (request, slot, 1);
        }
        break;
    case UHCMD_BULKXFER:
        if (isHubAddress (get_word (request + IOUH_DEVADDR)))
        {
            // The hub has no bulk endpoints
            ioError = UHIOERR_STALL;
        }
        else
        {
            ioError = devBulkOrIntTransfer (request, slot, 0);
        }
        break;
    default:
        ioError = IOERR_NOCMD;
        break;
    }

    put_byte (request + IO_ERROR, ioError);
    ioLog (_T("uaeusb_io"), request);
    return ioError;
}

/*
 * Commands with no blocking work are completed inline from BeginIO;
 * everything else defers to a request thread. Every transfer command
 * defers — even root-hub control transfers, because a port reset performs
 * blocking libusb work (handle open, device reset) that must never run on
 * the emulation thread.
 */
static int usbCanQuick (uaecptr request)
{
    uae_u32 command = get_word (request + IO_COMMAND);
    switch (command)
    {
    case CMD_RESET:
    case CMD_FLUSH:
    case UHCMD_QUERYDEVICE:
    case UHCMD_USBRESET:
    case UHCMD_USBRESUME:
    case UHCMD_USBSUSPEND:
    case UHCMD_USBOPER:
        return 1;
    }
    return 0;
}

/*
 * One detached thread per deferred request (usbbridge.py spawn_worker
 * model). Poseidon keeps requests pending on several pipes at once — the
 * hub status-change poll and per-device interrupt polls park until data —
 * so a shared worker queue would let one parked poll starve all other
 * traffic. Per-pipe ordering is preserved by Poseidon itself: it never has
 * more than one request outstanding on the same pipe. asyncSem guards the
 * slot table and makes flag-check-then-complete atomic against AbortIO, so
 * each request is replied to exactly once.
 */
static void* usbRequestThread (void* arg)
{
    uaecptr request = (uaecptr)(uintptr_t)arg;

    uae_sem_wait (&asyncSem);
    int slot = findAsyncSlot (request);
    int aborted = slot >= 0 && hcd.asyncAborted[slot];
    if (slot >= 0 && !aborted)
        hcd.asyncType[slot] = ASYNC_REQUEST_ACTIVE;
    uae_sem_post (&asyncSem);

    if (aborted || hcd.stopping)
    {
        // Aborted (AbortIO or CMD_FLUSH) before the transfer started
        put_long (request + IOUH_ACTUAL, 0);
        put_byte (request + IO_ERROR, IOERR_ABORTED);
    }
    else
    {
        usbDoCommand (request, slot);
    }

    uae_sem_wait (&asyncSem);
    put_byte (request + IO_FLAGS, get_byte (request + IO_FLAGS) & ~IOF_QUICK);
    releaseAsyncRequest (request);
    // A shutdown drain discards the reply: the Amiga side is resetting and
    // the ioreq no longer has a waiter
    if (!hcd.stopping)
        uae_ReplyMsg (request);
    hcd.activeWorkers--;
    uae_sem_post (&asyncSem);
    return 0;
}

static void startHost (void)
{
    hcd.stopping = 0;
    if (!usbHostInit ())
        write_log (_T("uaeusb.device: libusb unavailable, bus reports no devices\n"));
}

// Unpark every held IN poll and wait until all request threads finished
static void drainWorkers (void)
{
    hcd.stopping = 1;
    for (;;)
    {
        uae_sem_wait (&asyncSem);
        int busy = hcd.activeWorkers;
        uae_sem_post (&asyncSem);
        if (!busy)
            break;
        sleep_millis (HUB_INT_POLL_SLICE_MS);
    }
    hcd.stopping = 0;
}

static uae_u32 openFail (uaecptr ioreq, int error)
{
    put_long (ioreq + IO_DEVICE, -1);
    put_byte (ioreq + IO_ERROR, error);
    return (uae_u32)-1;
}

static uae_u32 REGPARAM2 usbOpen (TrapContext* context)
{
    uaecptr ioreq = m68k_areg (regs, 1);
    uae_u32 unit = m68k_dreg (regs, 0);

    if (logUaeusb)
        write_log (_T("uaeusb.device: open unit=%d ioreq=%08X\n"), unit, ioreq);
    uae_u32 mnLength = get_word (ioreq + IO_MN_LENGTH);
    if (mnLength < IOSTDREQ_SIZE && mnLength > 0)
        return openFail (ioreq, IOERR_BADLENGTH);
    if (unit != UAEUSB_UNIT)
        return openFail (ioreq, IOERR_BadUnitNum);
    if (!hcd.opencnt)
        startHost ();
    hcd.opencnt++;
    // io_Unit is deliberately left NULL: unit 0 is the only unit
    put_word (m68k_areg (regs, 6) + LIB_OPENCNT, get_word (m68k_areg (regs, 6) + LIB_OPENCNT) + 1);
    put_byte (ioreq + IO_ERROR, 0);
    put_byte (ioreq + IO_LN_TYPE, NT_REPLYMSG);
    return 0;
}

static uae_u32 REGPARAM2 usbClose (TrapContext* context)
{
    uaecptr ioreq = m68k_areg (regs, 1);

    if (logUaeusb)
        write_log (_T("uaeusb.device: close ioreq=%08X\n"), ioreq);
    if (hcd.opencnt)
    {
        hcd.opencnt--;
        if (!hcd.opencnt)
        {
            drainWorkers ();
            usbHostExit ();
        }
    }
    put_long (ioreq + IO_DEVICE, 0);
    put_word (m68k_areg (regs, 6) + LIB_OPENCNT, get_word (m68k_areg (regs, 6) + LIB_OPENCNT) - 1);
    return 0;
}

static uae_u32 REGPARAM2 usbExpunge (TrapContext* context)
{
    return 0;
}

static uae_u32 REGPARAM2 usbInit (TrapContext* context)
{
    uae_u32 base = m68k_dreg (regs, 0);
    if (logUaeusb)
        write_log (_T("uaeusb.device: init base=%08X\n"), base);
    hcd.busState = UHSF_OPERATIONAL;
    resetRootHub ();
    return base;
}

static uae_u32 REGPARAM2 usbBeginio (TrapContext* context)
{
    uae_u32 request = m68k_areg (regs, 1);
    uae_u8 flags = get_byte (request + IO_FLAGS);
    uae_u32 command = get_word (request + IO_COMMAND);

    put_byte (request + IO_LN_TYPE, NT_MESSAGE);
    put_byte (request + IO_ERROR, 0);
    put_word (request + IOUH_STATE, hcd.busState);

    // A suspended bus must not accept new transfers
    if ((command == UHCMD_CONTROLXFER || command == UHCMD_INTXFER ||
         command == UHCMD_BULKXFER || command == UHCMD_ISOXFER) &&
        (hcd.busState & UHSF_SUSPENDED))
    {
        put_long (request + IOUH_ACTUAL, 0);
        put_byte (request + IO_ERROR, UHIOERR_USBOFFLINE);
        if (!(flags & IOF_QUICK))
            uae_ReplyMsg (request);
        return UHIOERR_USBOFFLINE;
    }

    if (usbCanQuick (request))
    {
        usbDoCommand (request, -1);
        if (!(flags & IOF_QUICK))
            uae_ReplyMsg (request);
        return (uae_s8)get_byte (request + IO_ERROR);
    }
    else
    {
        put_byte (request + IO_FLAGS, get_byte (request + IO_FLAGS) & ~IOF_QUICK);
        uae_sem_wait (&asyncSem);
        addAsyncRequest (request, ASYNC_REQUEST_TEMP);
        hcd.activeWorkers++;
        uae_sem_post (&asyncSem);
        if (!uae_start_thread (_T("uaeusb_req"), usbRequestThread,
            (void*)(uintptr_t)request, NULL))
        {
            // No thread, no completer: fail the request here
            uae_sem_wait (&asyncSem);
            releaseAsyncRequest (request);
            hcd.activeWorkers--;
            uae_sem_post (&asyncSem);
            put_long (request + IOUH_ACTUAL, 0);
            put_byte (request + IO_ERROR, UHIOERR_HOSTERROR);
            uae_ReplyMsg (request);
            return UHIOERR_HOSTERROR;
        }
        return 0;
    }
}

static uae_u32 REGPARAM2 usbAbortio (TrapContext* context)
{
    uae_u32 request = m68k_areg (regs, 1);

    if (logUaeusb)
        write_log (_T("uaeusb.device: abortio request=%08X\n"), request);
    put_byte (request + IO_ERROR, IOERR_ABORTED);
    abortAsync (request);
    return 0;
}

uaecptr UaeusbStartup (uaecptr resaddr)
{
    if (logUaeusb)
        write_log (_T("UaeusbStartup(0x%x)\n"), resaddr);
    // Build a struct Resident that sets up and initializes uaeusb.device
    put_word (resaddr + RT_MATCHWORD, RTC_MATCHWORD);
    put_long (resaddr + RT_MATCHTAG, resaddr);
    put_long (resaddr + RT_ENDSKIP, resaddr + RT_SIZE);
    put_word (resaddr + RT_FLAGS, ((RTF_AUTOINIT | RTF_COLDSTART) << 8) | UAEUSB_VERSION);
    put_word (resaddr + RT_TYPE, (NT_DEVICE << 8) | UAEUSB_ROMTAG_PRI);
    put_long (resaddr + RT_NAME, ROM_uaeusb_resname);
    put_long (resaddr + RT_IDSTRING, ROM_uaeusb_resid);
    put_long (resaddr + RT_INIT, ROM_uaeusb_init);
    resaddr += RT_SIZE;
    return resaddr;
}

void UaeusbInstall (void)
{
    if (logUaeusb)
        write_log (_T("UaeusbInstall(): 0x%x\n"), here ());

    if (!semsInitialized)
    {
        uae_sem_init (&asyncSem, 0, 1);
        uae_sem_init (&usbHost.lock, 0, 1);
        semsInitialized = 1;
    }

    ROM_uaeusb_resname = ds (UAEUSB_DEVICE_NAME);
    ROM_uaeusb_resid = ds (UAEUSB_ID_STRING);

    // UHCMD_QUERYDEVICE tag answers point at these rtarea strings
    ROM_uaeusb_manufacturer = ds (UAEUSB_MANUFACTURER);
    ROM_uaeusb_productname = ds (UAEUSB_PRODUCTNAME);
    ROM_uaeusb_description = ds (UAEUSB_DESCRIPTION);
    ROM_uaeusb_copyright = ds (UAEUSB_COPYRIGHT);

    // initcode
    uae_u32 initcode = here ();
    calltrap (deftrap (usbInit)); dw (RTS);

    // Open
    uae_u32 openfunc = here ();
    calltrap (deftrap (usbOpen)); dw (RTS);

    // Close
    uae_u32 closefunc = here ();
    calltrap (deftrap (usbClose)); dw (RTS);

    // Expunge
    uae_u32 expungefunc = here ();
    calltrap (deftrap (usbExpunge)); dw (RTS);

    // BeginIO
    uae_u32 beginiofunc = here ();
    calltrap (deftrap (usbBeginio)); dw (RTS);

    // AbortIO
    uae_u32 abortiofunc = here ();
    calltrap (deftrap (usbAbortio)); dw (RTS);

    // FuncTable
    uae_u32 functable = here ();
    dl (openfunc);              // Open
    dl (closefunc);             // Close
    dl (expungefunc);           // Expunge
    dl (EXPANSION_nullfunc);    // Null (reserved, never called)
    dl (beginiofunc);           // BeginIO
    dl (abortiofunc);           // AbortIO
    dl (0xFFFFFFFFul);          // end of table

    // DataTable
    uae_u32 datatable = makedatatable (ROM_uaeusb_resid, ROM_uaeusb_resname,
        NT_DEVICE, UAEUSB_ROMTAG_PRI, UAEUSB_VERSION, UAEUSB_REVISION);

    // RTF_AUTOINIT init tuple consumed by exec MakeLibrary
    ROM_uaeusb_init = here ();
    dl (UAEUSB_DEVICE_BASE_SIZE);
    dl (functable);
    dl (datatable);
    dl (initcode);
}

void UaeusbReset (void)
{
    if (!semsInitialized)
        return;
    drainWorkers ();
    usbHostExit ();
    for (int i = 0; i < MAX_ASYNC_REQUESTS; i++)
    {
        hcd.asyncRequest[i] = 0;
        hcd.asyncType[i] = ASYNC_REQUEST_NONE;
        hcd.asyncAborted[i] = 0;
    }
    hcd.opencnt = 0;
    hcd.busState = UHSF_OPERATIONAL;
    resetRootHub ();
}

void UaeusbFree (void)
{
    UaeusbReset ();
}

#endif /* WITH_LIBUSB */
