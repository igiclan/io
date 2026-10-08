/*
 * ParsFlash USB Redirector - pfvhci.sys
 * https://parsflash.com
 *
 * Version.h - single source of the driver version.
 *
 * The build number can be injected by CI: add PFVHCI_VERSION_BUILD=<n> to the
 * preprocessor definitions of both the C++ compiler and the resource compiler,
 * and pass the same 1.0.0.<n> to stampinf (vcxproj property PfvhciVersion) so that
 * DriverVer in the INF and the file version resource agree.
 */
#pragma once

#define PFVHCI_VERSION_MAJOR 1
#define PFVHCI_VERSION_MINOR 0
#define PFVHCI_VERSION_PATCH 0
#ifndef PFVHCI_VERSION_BUILD
#define PFVHCI_VERSION_BUILD 0
#endif

#define PFVHCI_STRINGIZE2(x) #x
#define PFVHCI_STRINGIZE(x) PFVHCI_STRINGIZE2(x)

#define PFVHCI_VERSION_STRING                    \
    PFVHCI_STRINGIZE(PFVHCI_VERSION_MAJOR) "."   \
    PFVHCI_STRINGIZE(PFVHCI_VERSION_MINOR) "."   \
    PFVHCI_STRINGIZE(PFVHCI_VERSION_PATCH) "."   \
    PFVHCI_STRINGIZE(PFVHCI_VERSION_BUILD)

#define PFVHCI_COMPANY_NAME     "ParsFlash"
#define PFVHCI_COMPANY_URL      "https://parsflash.com"
#define PFVHCI_PRODUCT_NAME     "ParsFlash USB Redirector"
#define PFVHCI_FILE_DESCRIPTION "ParsFlash Virtual USB Host Controller (UdeCx)"
#define PFVHCI_COPYRIGHT        "Copyright (C) 2026 ParsFlash. https://parsflash.com"
#define PFVHCI_INTERNAL_NAME    "pfvhci"
#define PFVHCI_ORIGINAL_NAME    "pfvhci.sys"
