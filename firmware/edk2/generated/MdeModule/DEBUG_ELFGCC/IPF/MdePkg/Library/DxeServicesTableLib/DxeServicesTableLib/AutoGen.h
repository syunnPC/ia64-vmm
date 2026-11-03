/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2007 - 2017, Intel Corporation. All rights reserved.
 * EDK2 BaseTools/Source/Python/AutoGen templates; terms in third_party/edk2-ipf/License.txt.
 */
/**
  DO NOT EDIT
  FILE auto-generated
  Module name:
    AutoGen.h
  Abstract:       Auto-generated AutoGen.h for building module or library.
**/

#ifndef _AUTOGENH_baa1baa3_0a8d_402c_8042_985115fae953
#define _AUTOGENH_baa1baa3_0a8d_402c_8042_985115fae953

#ifdef __cplusplus
extern "C" {
#endif

#include <Base.h>
#include <PiDxe.h>

extern GUID  gEfiCallerIdGuid;
extern CHAR8 *gEfiCallerBaseName;


// Guids
extern EFI_GUID gEfiDxeServicesTableGuid;

// Definition of SkuId Array
extern UINT64 _gPcd_SkuId_Array[];

EFI_STATUS
EFIAPI
DxeServicesTableLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );


#ifdef __cplusplus
}
#endif

#endif
