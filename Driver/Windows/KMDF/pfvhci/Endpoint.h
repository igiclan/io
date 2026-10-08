/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * Endpoint.h - UDECXUSBENDPOINT objects, their URB queues and URB translation.
 */
#pragma once

#include "Common.h"

/*
 * Creates a UDECXUSBENDPOINT plus its WDF queue for EvtUsbDeviceDefaultEndpointAdd
 * (Descriptor == NULL: default control endpoint) and EvtUsbDeviceEndpointAdd.
 * EndpointInit is the init structure UdeCx handed to the callback.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS PfEndpointCreate(_In_ UDECXUSBDEVICE Device, _Inout_ _UDECXUSBENDPOINT_INIT** EndpointInit,
                          _In_opt_ const USB_ENDPOINT_DESCRIPTOR* Descriptor, _In_ ULONG DescriptorBufferLength);
