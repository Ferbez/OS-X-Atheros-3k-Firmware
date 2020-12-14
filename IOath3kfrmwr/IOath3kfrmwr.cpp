/* Disclaimer:
 This code is loosely based on the template of the class 
 in AnchorUSB Driver example from IOUSBFamily, 
 Open Source by Apple http://www.opensource.apple.com
 
 For information on driver matching for USB devices, see: 
 http://developer.apple.com/qa/qa2001/qa1076.html

 */
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/usb/IOUSBInterface.h>

#include "IOath3kfrmwr.h"

#ifndef IOATH3KNULL
#include "ath3k-1fw.h"
#endif

//REVIEW: avoids problem with Xcode 5.1.0 where -dead_strip eliminates these required symbols
#include <libkern/OSKextLib.h>
void* _org_rehabman_dontstrip_[] =
{
    (void*)&OSKextGetCurrentIdentifier,
    (void*)&OSKextGetCurrentLoadTag,
    (void*)&OSKextGetCurrentVersionString,
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - //

#define USB_REQ_DFU_DNLOAD	1

//rehabman:
// Note: mac4mat's original had this BULK_SIZE at 4096.  Turns out sending
// the firmware 4k at a time doesn't quite work in SL. And it seems
// sending it 1k at a time works in SL, Lion, and ML.

#define BULK_SIZE	1024

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - //

OSDefineMetaClassAndStructors(org_rehabman_IOath3kfrmwr, IOService)

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - //

#ifdef DEBUG
bool local_IOath3kfrmwr::init(OSDictionary *propTable)
{
#ifdef DEBUG
    IOLog("org_rehabman_IOath3kfrmwr(%p): init (https://github.com/RehabMan/OS-X-Atheros-3k-Firmware.git)\n", this);
#else
    IOLog("IOath3kfrmwr: init (https://github.com/RehabMan/OS-X-Atheros-3k-Firmware.git)\n");
#endif
    return (super::init(propTable));
}

IOService* local_IOath3kfrmwr::probe(IOService *provider, SInt32 *score)
{
    DEBUG_LOG("%s(%p)::probe\n", getName(), this);
    return super::probe(provider, score);			// this returns this
}

bool local_IOath3kfrmwr::attach(IOService *provider)
{
    // be careful when performing initialization in this method. It can be and
    // usually will be called mutliple 
    // times per instantiation
    DEBUG_LOG("%s(%p)::attach\n", getName(), this);
    return super::attach(provider);
}

void local_IOath3kfrmwr::detach(IOService *provider)
{
    // Like attach, this method may be called multiple times
    DEBUG_LOG("%s(%p)::detach\n", getName(), this);
    return super::detach(provider);
}
#endif // DEBUG

//
// start
// when this method is called, I have been selected as the driver for this device.
// I can still return false to allow a different driver to load
//
bool local_IOath3kfrmwr::start(IOService *provider)
{
#ifdef DEBUG
    IOLog("%s(%p)::start - Version 1.2.1 starting\n", getName(), this);
#else
    IOLog("IOath3kfrmwr: Version 1.2.1 starting\n");
#endif
    
    IOReturn 				err;
    const IOUSBConfigurationDescriptor *cd;
    
    // Do all the work here, on an IOKit matching thread.
    
    // 0.1 Get my USB Device
    DEBUG_LOG("%s(%p)::start!\n", getName(), this);
    pUsbDev = OSDynamicCast(IOUSBDevice, provider);
    if(!pUsbDev) 
    {
        IOLog("%s(%p)::start - Provider isn't a USB device!!!\n", getName(), this);
        return false;
    }

    // 0.2 Reset the device
    err = pUsbDev->ResetDevice();
    if (err)
    {
        IOLog("%s(%p)::start - failed to reset the device\n", getName(), this);
        //return false;
    }
    else
        DEBUG_LOG("%s(%p)::start: device reset\n", getName(), this);
    
    // 0.3 Find the first config/interface
    int numconf = 0;
    if ((numconf = pUsbDev->GetNumConfigurations()) < 1)
    {
        IOLog("%s(%p)::start - no composite configurations\n", getName(), this);
        return false;
    }
    DEBUG_LOG("%s(%p)::start: num configurations %d\n", getName(), this, numconf);
        
    // 0.4 Get first config descriptor
    cd = pUsbDev->GetFullConfigurationDescriptor(0);
    if (!cd)
    {
        IOLog("%s(%p)::start - no config descriptor\n", getName(), this);
        return false;
    }
	
    // 1.0 Open the USB device
    if (!pUsbDev->open(this))
    {
        IOLog("%s(%p)::start - unable to open device for configuration\n", getName(), this);
        return false;
    }
    
    // 1.1 Set the configuration to the first config
    err = pUsbDev->SetConfiguration(this, cd->bConfigurationValue, true);
    if (err)
    {
        IOLog("%s(%p)::start - unable to set the configuration\n", getName(), this);
        pUsbDev->close(this);
        return false;
    }
    
    // 1.2 Get the status of the USB device (optional, for diag.)
    USBStatus status;
    err = pUsbDev->GetDeviceStatus(&status);
    if (err)
    {
        IOLog("%s(%p)::start - unable to get device status\n", getName(), this);
        pUsbDev->close(this);
        return false;
    }
    DEBUG_LOG("%s(%p)::start: device status %d\n", getName(), this, (int)status);

// rehabman:
// IOATH3KNULL can be used to create an IOath3kfrmwr.kext that effectively
// disables the device, so a 3rd party device can be used instead.
// To make this really work, there is probably additional device IDs that must be
// entered in the Info.plist
//
// Credit to mac4mat for this solution too...
    
#ifndef IOATH3KNULL
    // 2.0 Find the interface for bulk endpoint transfers
    IOUSBFindInterfaceRequest request;
    request.bInterfaceClass = kIOUSBFindInterfaceDontCare;
    request.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    request.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    request.bAlternateSetting = kIOUSBFindInterfaceDontCare;
    
    IOUSBInterface * intf = pUsbDev->FindNextInterface(NULL, &request);
    if (!intf) {
        IOLog("%s(%p)::start - unable to find interface\n", getName(), this);
        pUsbDev->close(this);
        return false;
    }

    // 2.1 Open the interface
    if (!intf->open(this))
    {
        IOLog("%s(%p)::start - unable to open interface\n", getName(), this);
        pUsbDev->close(this);
        return false;
    }

    // 2.2 Get info on endpoints (optional, for diag.)
    DEBUG_LOG("%s(%p)::start: interface has %d endpoints\n", getName(), this, intf->GetNumEndpoints());
    
    OSArray* check = OSDynamicCast(OSArray, getProperty("CheckEndpoints"));
    if (check) {
        int count = check->getCount();
        for (int i = 0; i < count; i++) {
            OSDictionary* ep = OSDynamicCast(OSDictionary, check->getObject(i));
            if (!ep)
                continue;
            OSNumber* nEndpoint = OSDynamicCast(OSNumber, ep->getObject("EndpointNumber"));
            // TransferType: kUsbIn=1, kUsbOut=0
            OSNumber* nTransType = OSDynamicCast(OSNumber, ep->getObject("TransferType"));
            if (!nEndpoint || !nTransType)
                continue;
            UInt8 transferType = 0;
            UInt16 maxPacketSize = 0;
            UInt8 interval = 0;
            err = intf->GetEndpointProperties(0, nEndpoint->unsigned8BitValue(), nTransType->unsigned8BitValue(), &transferType, &maxPacketSize, &interval);
            if (err) {
                IOLog("%s(%p)::start - failed to get endpoint %d properties\n", getName(), this, i);
                intf->close(this);
                pUsbDev->close(this);
                return false;
            }
            DEBUG_LOG("%s(%p)::start: EP%d %d %d %d\n", getName(), this, nEndpoint->unsigned8BitValue(), transferType, maxPacketSize, interval);
        }
    }
    
    // 2.3 Get the pipe for bulk endpoint 2 Out
    OSNumber* nPipe = OSDynamicCast(OSNumber, getProperty("PipeNumber"));
    if (!nPipe) {
        DEBUG_LOG("%s(%p)::start - PipeNumber not specified\n", getName(), this);
        intf->close(this);
        pUsbDev->close(this);
        return false;
    }
    IOUSBPipe * pipe = intf->GetPipeObj(nPipe->unsigned8BitValue());
    if (!pipe) {
        IOLog("%s(%p)::start - failed to find bulk out pipe %d\n", getName(), this, nPipe->unsigned8BitValue());
        intf->close(this);
        pUsbDev->close(this);
        return false;
    }
   
    /*  // TODO: Test the alternative way to do it:
     IOUSBFindEndpointRequest pipereq;
     pipereq.type = kUSBBulk;
     pipereq.direction = kUSBOut;
     pipereq.maxPacketSize = BULK_SIZE;
     pipereq.interval = 0;
     IOUSBPipe *pipe = intf->FindNextPipe(NULL, &pipereq);
     pipe = intf->FindNextPipe(pipe, &pipereq);
     if (!pipe) {
     DEBUG_LOG("%s(%p)::start - failed to find bulk out pipe 2\n", getName(), this);
     intf->close(this);
     pUsbDev->close(this);
     return false;    
     }
     */

    // 3.0 Send request to Control Endpoint to initiate the firmware transfer
    IOUSBDevRequest ctlreq;
    ctlreq.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBVendor, kUSBDevice);
    ctlreq.bRequest = USB_REQ_DFU_DNLOAD;
    ctlreq.wValue = 0;
    ctlreq.wIndex = 0;
    ctlreq.wLength = 20;
    ctlreq.pData = firmware_buf;

#if 0  // Trying to troubleshoot the problem after Restart (with OSBundleRequired Root)
    for (int irep = 0; irep < 5; irep++) { // retry on error
        err = pUsbDev->DeviceRequest(&ctlreq); // (synchronous, will block)
        if (err)
            IOLog("%s(%p)::start - failed to initiate firmware transfer (%d), retrying (%d)\n", getName(), this, err, irep+1);
        else
            break;
    }
#else
    err = pUsbDev->DeviceRequest(&ctlreq); // (synchronous, will block)
#endif
    if (err) {
        IOLog("%s(%p)::start - failed to initiate firmware transfer (%d)\n", getName(), this, err);
        intf->close(this);
        pUsbDev->close(this);
        return false;
    }

    // 3.1 Create IOMemoryDescriptor for bulk transfers
    char buftmp[BULK_SIZE];
    IOMemoryDescriptor * membuf = IOMemoryDescriptor::withAddress(&buftmp, BULK_SIZE, kIODirectionOut);
    if (!membuf) {
        IOLog("%s(%p)::start - failed to map memory descriptor\n", getName(), this);
        intf->close(this);
        pUsbDev->close(this);
        return false; 
    }
    err = membuf->prepare(kIODirectionOut);
    if (err) {
        IOLog("%s(%p)::start - failed to prepare memory descriptor\n", getName(), this);
        intf->close(this);
        pUsbDev->close(this);
        return false; 
    }
    
    // 3.2 Send the rest of firmware to the bulk pipe
    char * buf = firmware_buf;
    int size = sizeof(firmware_buf); 
    buf += 20;
    size -= 20;
    int ii = 1;
    while (size) {
        int to_send = size < BULK_SIZE ? size : BULK_SIZE; 
        
        memcpy(buftmp, buf, to_send);
        err = pipe->Write(membuf, 0, 0, membuf->getLength(), NULL);
        if (err) {
            IOLog("%s(%p)::start - failed to write firmware to bulk pipe (err:%d, block:%d, to_send:%d)\n", getName(), this, err, ii, to_send);
            intf->close(this);
            pUsbDev->close(this);
            return false; 
        }
        buf += to_send;
        size -= to_send;
        ii++;
    }
    // It may be better to remove to_send, but for now I'll leave it here
    
#ifdef DEBUG
    IOLog("%s(%p)::start: firmware was sent to bulk pipe\n", getName(), this);
#else
    IOLog("IOath3kfrmwr: firmware loaded successfully!\n");
#endif
    
    err = membuf->complete(kIODirectionOut);
    if (err) {
        IOLog("%s(%p)::start - failed to complete memory descriptor\n", getName(), this);
        intf->close(this);
        pUsbDev->close(this);
        return false; 
    }

    /*  // TODO: Test the alternative way to do it:
     IOMemoryDescriptor * membuf = IOMemoryDescriptor::withAddress(&firmware_buf[20], 246804-20, kIODirectionNone); // sizeof(firmware_buf)
     if (!membuf) {
     IOLog("%s(%p)::start - failed to map memory descriptor\n", getName(), this);
     intf->close(this);
     pUsbDev->close(this);
     return false; 
     }
     err = membuf->prepare();
     if (err) {
     IOLog("%s(%p)::start - failed to prepare memory descriptor\n", getName(), this);
     intf->close(this);
     pUsbDev->close(this);
     return false; 
     }
     
     //err = pipe->Write(membuf);
     err = pipe->Write(membuf, 10000, 10000, 246804-20, NULL);
     if (err) {
     IOLog("%s(%p)::start - failed to write firmware to bulk pipe\n", getName(), this);
     intf->close(this);
     pUsbDev->close(this);
     return false; 
     }
     IOLog("%s(%p)::start: firmware was sent to bulk pipe\n", getName(), this);
     */
    
    // 4.0 Get device status (it fails, but somehow is important for operational device)
    err = pUsbDev->GetDeviceStatus(&status);
    if (err)
    {
        // this is the normal case...
        DEBUG_LOG("%s(%p)::start - unable to get device status\n", getName(), this);
    }
    else
    {
        // this is more of an error case... after firmware load
        // device status shouldn't work, as the devices has changed IDs
        IOLog("%s(%p)::start: device status %d\n", getName(), this, (int)status);
    }

    // Close the interface
    intf->close(this);

    // Close the USB device
    pUsbDev->close(this);
    return false;  // return false to allow a different driver to load
#else   // !IOATH3KNULL
    // Do not load the firmware, leave the controller non-operational
    
    // Do not close the USB device
    //pUsbDev->close(this);
    return true;  // return true to retain exclusive access to USB device
#endif  // !IOATH3KNULL
}

#ifdef DEBUG

void local_IOath3kfrmwr::stop(IOService *provider)
{
    DEBUG_LOG("%s(%p)::stop\n", getName(), this);
    super::stop(provider);
}

bool local_IOath3kfrmwr::handleOpen(IOService *forClient, IOOptionBits options, void *arg )
{
    DEBUG_LOG("%s(%p)::handleOpen\n", getName(), this);
    return super::handleOpen(forClient, options, arg);
}

void local_IOath3kfrmwr::handleClose(IOService *forClient, IOOptionBits options )
{
    DEBUG_LOG("%s(%p)::handleClose\n", getName(), this);
    super::handleClose(forClient, options);
}

IOReturn local_IOath3kfrmwr::message(UInt32 type, IOService *provider, void *argument)
{
    DEBUG_LOG("%s(%p)::message\n", getName(), this);
    switch ( type )
    {
        case kIOMessageServiceIsTerminated:
            if (pUsbDev->isOpen(this))
            {
                IOLog("%s(%p)::message - service is terminated - closing device\n", getName(), this);
//                pUsbDev->close(this);
            }
            break;
            
        case kIOMessageServiceIsSuspended:
        case kIOMessageServiceIsResumed:
        case kIOMessageServiceIsRequestingClose:
        case kIOMessageServiceWasClosed: 
        case kIOMessageServiceBusyStateChange:
        default:
            break;
    }
    
    return super::message(type, provider, argument);
}

bool local_IOath3kfrmwr::terminate(IOOptionBits options)
{
    DEBUG_LOG("%s(%p)::terminate\n", getName(), this);
    return super::terminate(options);
}

bool local_IOath3kfrmwr::finalize(IOOptionBits options)
{
    DEBUG_LOG("%s(%p)::finalize\n", getName(), this);
    return super::finalize(options);
}

#endif
