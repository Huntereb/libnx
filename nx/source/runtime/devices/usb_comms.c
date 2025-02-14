#include <string.h>
#include <malloc.h>
#include "types.h"
#include "result.h"
#include "kernel/rwlock.h"
#include "services/fatal.h"
#include "services/usbds.h"
#include "runtime/hosversion.h"
#include "runtime/devices/usb_comms.h"
#include <stdio.h>

#define TOTAL_INTERFACES 4

typedef struct {
    RwLock lock, lock_in, lock_out;
    bool initialized;

    UsbDsInterface* interface;
    UsbDsEndpoint *endpoint_in, *endpoint_out;

    u8 *endpoint_in_buffer, *endpoint_out_buffer;
} usbCommsInterface;

static bool g_usbCommsInitialized = false;

static usbCommsInterface g_usbCommsInterfaces[TOTAL_INTERFACES];

static bool g_usbCommsErrorHandling = 0;

static RwLock g_usbCommsLock;

static Result _usbCommsInterfaceInit1x(u32 intf_ind, const UsbCommsInterfaceInfo *info);
static Result _usbCommsInterfaceInit5x(u32 intf_ind, const UsbCommsInterfaceInfo *info);
static Result _usbCommsInterfaceInit(u32 intf_ind, const UsbCommsInterfaceInfo *info);

static Result _usbCommsWrite(usbCommsInterface *interface, const void* buffer, size_t size, size_t *transferredSize);

static void _usbCommsUpdateInterfaceDescriptor(struct usb_interface_descriptor *desc, const UsbCommsInterfaceInfo *info) {
    if (info != NULL) {
        desc->bInterfaceClass = info->bInterfaceClass;
        desc->bInterfaceSubClass = info->bInterfaceSubClass;
        desc->bInterfaceProtocol = info->bInterfaceProtocol;
    }
}

Result usbCommsInitializeEx(u32 num_interfaces, const UsbCommsInterfaceInfo *infos)
{
    Result rc = 0;
    rwlockWriteLock(&g_usbCommsLock);

    if (g_usbCommsInitialized) {
        rc = MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    } else if (num_interfaces > TOTAL_INTERFACES) {
        rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    } else {
        rc = usbDsInitialize();

        if (R_SUCCEEDED(rc)) {
            if (hosversionAtLeast(5,0,0)) {
                u8 iManufacturer, iProduct, iSerialNumber;
                static const u16 supported_langs[1] = {0x0409};
                // Send language descriptor
                rc = usbDsAddUsbLanguageStringDescriptor(NULL, supported_langs, sizeof(supported_langs)/sizeof(u16));
                // Send manufacturer
                if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iManufacturer, "Nintendo");
                // Send product
                if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iProduct, "Nintendo Switch");
                // Send serial number
                if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iSerialNumber, "SerialNumber");

                // Send device descriptors
                struct usb_device_descriptor device_descriptor = {
                    .bLength = USB_DT_DEVICE_SIZE,
                    .bDescriptorType = USB_DT_DEVICE,
                    .bcdUSB = 0x0110,
                    .bDeviceClass = 0x00,
                    .bDeviceSubClass = 0x00,
                    .bDeviceProtocol = 0x00,
                    .bMaxPacketSize0 = 0x40,
                    .idVendor = 0x057e,
                    .idProduct = 0x3000,
                    .bcdDevice = 0x0100,
                    .iManufacturer = iManufacturer,
                    .iProduct = iProduct,
                    .iSerialNumber = iSerialNumber,
                    .bNumConfigurations = 0x01
                };
                // Full Speed is USB 1.1
                if (R_SUCCEEDED(rc)) rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &device_descriptor);

                // High Speed is USB 2.0
                device_descriptor.bcdUSB = 0x0200;
                if (R_SUCCEEDED(rc)) rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &device_descriptor);

                // Super Speed is USB 3.0
                device_descriptor.bcdUSB = 0x0300;
                // Upgrade packet size to 512
                device_descriptor.bMaxPacketSize0 = 0x09;
                if (R_SUCCEEDED(rc)) rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &device_descriptor);

                // Define Binary Object Store
                u8 bos[0x16] = {
                    0x05, // .bLength
                    USB_DT_BOS, // .bDescriptorType
                    0x16, 0x00, // .wTotalLength
                    0x02, // .bNumDeviceCaps

                    // USB 2.0
                    0x07, // .bLength
                    USB_DT_DEVICE_CAPABILITY, // .bDescriptorType
                    0x02, // .bDevCapabilityType
                    0x02, 0x00, 0x00, 0x00, // dev_capability_data

                    // USB 3.0
                    0x0A, // .bLength
                    USB_DT_DEVICE_CAPABILITY, // .bDescriptorType
                    0x03, // .bDevCapabilityType
                    0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00
                };
                if (R_SUCCEEDED(rc)) rc = usbDsSetBinaryObjectStore(bos, sizeof(bos));
            }

            if (R_SUCCEEDED(rc)) {
                for (u32 i = 0; i < num_interfaces; i++) {
                    usbCommsInterface *intf = &g_usbCommsInterfaces[i];
                    rwlockWriteLock(&intf->lock);
                    rwlockWriteLock(&intf->lock_in);
                    rwlockWriteLock(&intf->lock_out);
                    rc = _usbCommsInterfaceInit(i, infos == NULL ? NULL : infos + i);
                    rwlockWriteUnlock(&intf->lock_out);
                    rwlockWriteUnlock(&intf->lock_in);
                    rwlockWriteUnlock(&intf->lock);
                    if (R_FAILED(rc)) {
                        break;
                    }
                }
            }
        }

        if (R_SUCCEEDED(rc) && hosversionAtLeast(5,0,0)) {
            rc = usbDsEnable();
        }
    }

    if (R_SUCCEEDED(rc)) {
        g_usbCommsInitialized = true;
        g_usbCommsErrorHandling = false;
    }

    rwlockWriteUnlock(&g_usbCommsLock);

    if (R_FAILED(rc)) {
        usbCommsExit();
    }

    return rc;
}

Result usbCommsInitialize(void)
{
    return usbCommsInitializeEx(1, NULL);
}

static void _usbCommsInterfaceFree(usbCommsInterface *interface)
{
    rwlockWriteLock(&interface->lock);
    if (!interface->initialized) {
        rwlockWriteUnlock(&interface->lock);
        return;
    }

    rwlockWriteLock(&interface->lock_in);
    rwlockWriteLock(&interface->lock_out);

    interface->initialized = 0;

    interface->endpoint_in = NULL;
    interface->endpoint_out = NULL;
    interface->interface = NULL;

    free(interface->endpoint_in_buffer);
    free(interface->endpoint_out_buffer);
    interface->endpoint_in_buffer = NULL;
    interface->endpoint_out_buffer = NULL;

    rwlockWriteUnlock(&interface->lock_out);
    rwlockWriteUnlock(&interface->lock_in);

    rwlockWriteUnlock(&interface->lock);
}

void usbCommsExit(void)
{
    u32 i;

    rwlockWriteLock(&g_usbCommsLock);

    usbDsExit();

    g_usbCommsInitialized = false;

    rwlockWriteUnlock(&g_usbCommsLock);

    for (i=0; i<TOTAL_INTERFACES; i++)
    {
        _usbCommsInterfaceFree(&g_usbCommsInterfaces[i]);
    }
}

static Result _usbCommsInterfaceInit(u32 intf_ind, const UsbCommsInterfaceInfo *info)
{
    if (hosversionAtLeast(5,0,0)) {
        return _usbCommsInterfaceInit5x(intf_ind, info);
    } else {
        return _usbCommsInterfaceInit1x(intf_ind, info);
    }
}

static Result _usbCommsInterfaceInit5x(u32 intf_ind, const UsbCommsInterfaceInfo *info)
{
    Result rc = 0;
    usbCommsInterface *interface = &g_usbCommsInterfaces[intf_ind];

    struct usb_interface_descriptor interface_descriptor = {
        .bLength = USB_DT_INTERFACE_SIZE,
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = 4,
        .bNumEndpoints = 2,
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
    };
    _usbCommsUpdateInterfaceDescriptor(&interface_descriptor, info);

    struct usb_endpoint_descriptor endpoint_descriptor_in = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x40,
    };

    struct usb_endpoint_descriptor endpoint_descriptor_out = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_OUT,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x40,
    };

    struct usb_ss_endpoint_companion_descriptor endpoint_companion = {
        .bLength = sizeof(struct usb_ss_endpoint_companion_descriptor),
        .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst = 0x0F,
        .bmAttributes = 0x00,
        .wBytesPerInterval = 0x00,
    };

    interface->initialized = 1;

    //The buffer for PostBufferAsync commands must be 0x1000-byte aligned.
    interface->endpoint_in_buffer = memalign(0x1000, 0x1000);
    if (interface->endpoint_in_buffer==NULL) rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    if (R_SUCCEEDED(rc)) {
        interface->endpoint_out_buffer = memalign(0x1000, 0x1000);
        if (interface->endpoint_out_buffer==NULL) rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    if (R_SUCCEEDED(rc)) {
        memset(interface->endpoint_in_buffer, 0, 0x1000);
        memset(interface->endpoint_out_buffer, 0, 0x1000);
    }

    if (R_FAILED(rc)) return rc;

    rc = usbDsRegisterInterface(&interface->interface);
    if (R_FAILED(rc)) return rc;

    interface_descriptor.bInterfaceNumber = interface->interface->interface_index;
    endpoint_descriptor_in.bEndpointAddress += interface_descriptor.bInterfaceNumber + 1;
    endpoint_descriptor_out.bEndpointAddress += interface_descriptor.bInterfaceNumber + 1;

    // Full Speed Config
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Full, &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Full, &endpoint_descriptor_in, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Full, &endpoint_descriptor_out, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) return rc;

    // High Speed Config
    endpoint_descriptor_in.wMaxPacketSize = 0x200;
    endpoint_descriptor_out.wMaxPacketSize = 0x200;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_High, &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_High, &endpoint_descriptor_in, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_High, &endpoint_descriptor_out, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) return rc;

    // Super Speed Config
    endpoint_descriptor_in.wMaxPacketSize = 0x400;
    endpoint_descriptor_out.wMaxPacketSize = 0x400;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &endpoint_descriptor_in, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &endpoint_companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &endpoint_descriptor_out, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &endpoint_companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_FAILED(rc)) return rc;

    //Setup endpoints.
    rc = usbDsInterface_RegisterEndpoint(interface->interface, &interface->endpoint_in, endpoint_descriptor_in.bEndpointAddress);
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_RegisterEndpoint(interface->interface, &interface->endpoint_out, endpoint_descriptor_out.bEndpointAddress);
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_EnableInterface(interface->interface);
    if (R_FAILED(rc)) return rc;

    return rc;
}


static Result _usbCommsInterfaceInit1x(u32 intf_ind, const UsbCommsInterfaceInfo *info)
{
    Result rc = 0;
    usbCommsInterface *interface = &g_usbCommsInterfaces[intf_ind];

    struct usb_interface_descriptor interface_descriptor = {
        .bLength = USB_DT_INTERFACE_SIZE,
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = intf_ind,
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
    };
    _usbCommsUpdateInterfaceDescriptor(&interface_descriptor, info);

    struct usb_endpoint_descriptor endpoint_descriptor_in = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x200,
    };

    struct usb_endpoint_descriptor endpoint_descriptor_out = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_OUT,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x200,
    };

    interface->initialized = 1;

    //The buffer for PostBufferAsync commands must be 0x1000-byte aligned.
    interface->endpoint_in_buffer = memalign(0x1000, 0x1000);
    if (interface->endpoint_in_buffer==NULL) rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    if (R_SUCCEEDED(rc)) {
        interface->endpoint_out_buffer = memalign(0x1000, 0x1000);
        if (interface->endpoint_out_buffer==NULL) rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    if (R_SUCCEEDED(rc)) {
        memset(interface->endpoint_in_buffer, 0, 0x1000);
        memset(interface->endpoint_out_buffer, 0, 0x1000);
    }

    if (R_FAILED(rc)) return rc;

    //Setup interface.
    rc = usbDsGetDsInterface(&interface->interface, &interface_descriptor, "usb");
    if (R_FAILED(rc)) return rc;

    //Setup endpoints.
    rc = usbDsInterface_GetDsEndpoint(interface->interface, &interface->endpoint_in, &endpoint_descriptor_in);//device->host
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_GetDsEndpoint(interface->interface, &interface->endpoint_out, &endpoint_descriptor_out);//host->device
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_EnableInterface(interface->interface);
    if (R_FAILED(rc)) return rc;

    return rc;
}

void usbCommsSetErrorHandling(bool flag) {
    g_usbCommsErrorHandling = flag;
}

static Result _usbCommsRead(usbCommsInterface *interface, void* buffer, size_t size, size_t *transferredSize)
{
    Result rc=0;
    u32 urbId=0;
    u8 *bufptr = (u8*)buffer;
    u8 *transfer_buffer = NULL;
    u8 transfer_type=0;
    u32 chunksize=0;
    u32 tmp_transferredSize = 0;
    size_t total_transferredSize=0;
    UsbDsReportData reportdata;

    //Makes sure endpoints are ready for data-transfer / wait for init if needed.
    rc = usbDsWaitReady(5000000000);
    if (R_FAILED(rc)) return rc;

    while(size)
    {
        if(((u64)bufptr) & 0xfff)//When bufptr isn't page-aligned copy the data into g_usbComms_endpoint_in_buffer and transfer that, otherwise use the bufptr directly.
        {
            transfer_buffer = interface->endpoint_out_buffer;
            memset(interface->endpoint_out_buffer, 0, 0x1000);

            chunksize = 0x1000;
            chunksize-= ((u64)bufptr) & 0xfff;//After this transfer, bufptr will be page-aligned(if size is large enough for another transfer).
            if (size<chunksize) chunksize = size;

            transfer_type = 0;
        }
        else
        {
            transfer_buffer = bufptr;
            chunksize = size;

            transfer_type = 1;
        }

        //Start a host->device transfer.
        rc = usbDsEndpoint_PostBufferAsync(interface->endpoint_out, transfer_buffer, chunksize, &urbId);
        if (R_FAILED(rc)) return rc;
        //Wait for the transfer to finish.
        if (size < 0x1000) {
            rc = eventWait(&interface->endpoint_out->CompletionEvent, 1000000000);
        }
        else rc = eventWait(&interface->endpoint_out->CompletionEvent, U64_MAX);
        if (R_FAILED(rc))
        {
            usbDsEndpoint_Cancel(interface->endpoint_out);
            eventWait(&interface->endpoint_out->CompletionEvent, U64_MAX);
            eventClear(&interface->endpoint_out->CompletionEvent);
            return rc;
        }
        eventClear(&interface->endpoint_out->CompletionEvent);

        rc = usbDsEndpoint_GetReportData(interface->endpoint_out, &reportdata);
        if (R_FAILED(rc)) return rc;

        rc = usbDsParseReportData(&reportdata, urbId, NULL, &tmp_transferredSize);
        if (R_FAILED(rc)) return rc;

        if (tmp_transferredSize > chunksize) tmp_transferredSize = chunksize;
        total_transferredSize+= (size_t)tmp_transferredSize;

        if (transfer_type==0) memcpy(bufptr, transfer_buffer, tmp_transferredSize);
        bufptr+= tmp_transferredSize;
        size-= tmp_transferredSize;

        if(tmp_transferredSize < chunksize)break;
    }

    if (transferredSize) *transferredSize = total_transferredSize;

    return rc;
}

static Result _usbCommsWrite(usbCommsInterface *interface, const void* buffer, size_t size, size_t *transferredSize)
{
    Result rc=0;
    u32 urbId=0;
    u32 chunksize=0;
    u8 *bufptr = (u8*)buffer;
    u8 *transfer_buffer = NULL;
    u32 tmp_transferredSize = 0;
    size_t total_transferredSize=0;
    UsbDsReportData reportdata;

    //Makes sure endpoints are ready for data-transfer / wait for init if needed.
    rc = usbDsWaitReady(5000000000);
    if (R_FAILED(rc)) return rc;

    while(size)
    {
        if(((u64)bufptr) & 0xfff)//When bufptr isn't page-aligned copy the data into g_usbComms_endpoint_in_buffer and transfer that, otherwise use the bufptr directly.
        {
            transfer_buffer = interface->endpoint_in_buffer;
            memset(interface->endpoint_in_buffer, 0, 0x1000);

            chunksize = 0x1000;
            chunksize-= ((u64)bufptr) & 0xfff;//After this transfer, bufptr will be page-aligned(if size is large enough for another transfer).
            if (size<chunksize) chunksize = size;

            memcpy(interface->endpoint_in_buffer, bufptr, chunksize);
        }
        else
        {
            transfer_buffer = bufptr;
            chunksize = size;
        }

        //Start a device->host transfer.
        rc = usbDsEndpoint_PostBufferAsync(interface->endpoint_in, transfer_buffer, chunksize, &urbId);
        if(R_FAILED(rc))return rc;

        //Wait for the transfer to finish.
        if (size < 0x1000) {
            rc = eventWait(&interface->endpoint_in->CompletionEvent, 1000000000);
        }
        else rc = eventWait(&interface->endpoint_in->CompletionEvent, U64_MAX);
        if (R_FAILED(rc))
        {
            usbDsEndpoint_Cancel(interface->endpoint_in);
            eventWait(&interface->endpoint_in->CompletionEvent, U64_MAX);
            eventClear(&interface->endpoint_in->CompletionEvent);
            return rc;
        }
        eventClear(&interface->endpoint_in->CompletionEvent);

        rc = usbDsEndpoint_GetReportData(interface->endpoint_in, &reportdata);
        if (R_FAILED(rc)) return rc;

        rc = usbDsParseReportData(&reportdata, urbId, NULL, &tmp_transferredSize);
        if (R_FAILED(rc)) return rc;

        if (tmp_transferredSize > chunksize) tmp_transferredSize = chunksize;

        total_transferredSize+= (size_t)tmp_transferredSize;

        bufptr+= tmp_transferredSize;
        size-= tmp_transferredSize;

        if (tmp_transferredSize < chunksize) break;
    }

    if (transferredSize) *transferredSize = total_transferredSize;

    return rc;
}

size_t usbCommsReadEx(void* buffer, size_t size, u32 interface)
{
    size_t transferredSize=0;
    Result rc;
    usbCommsInterface *inter = &g_usbCommsInterfaces[interface];
    bool initialized;

    if (interface>=TOTAL_INTERFACES) return 0;

    rwlockReadLock(&inter->lock);
    initialized = inter->initialized;
    rwlockReadUnlock(&inter->lock);
    if (!initialized) return 0;
    
    rwlockWriteLock(&inter->lock_in);
    rc = _usbCommsRead(&g_usbCommsInterfaces[interface], buffer, size, &transferredSize);
    rwlockWriteUnlock(&inter->lock_in);
    if (R_SUCCEEDED(rc)) return transferredSize;
    else if (R_FAILED(rc)) return 0;
    return 0;
}

size_t usbCommsRead(void* buffer, size_t size)
{
    return usbCommsReadEx(buffer, size, 0);
}

size_t usbCommsWriteEx(const void* buffer, size_t size, u32 interface)
{
    size_t transferredSize=0;
    Result rc;
    usbCommsInterface *inter = &g_usbCommsInterfaces[interface];
    bool initialized;

    if (interface>=TOTAL_INTERFACES) return 0;

    rwlockReadLock(&inter->lock);
    initialized = inter->initialized;
    rwlockReadUnlock(&inter->lock);
    if (!initialized) return 0;

    rwlockWriteLock(&inter->lock_in);
    rc = _usbCommsWrite(&g_usbCommsInterfaces[interface], buffer, size, &transferredSize);
    rwlockWriteUnlock(&inter->lock_in);
    if (R_SUCCEEDED(rc)) return transferredSize;
    else if (R_FAILED(rc)) return 0;
    return 0;
}

size_t usbCommsWrite(const void* buffer, size_t size)
{
    return usbCommsWriteEx(buffer, size, 0);
}

