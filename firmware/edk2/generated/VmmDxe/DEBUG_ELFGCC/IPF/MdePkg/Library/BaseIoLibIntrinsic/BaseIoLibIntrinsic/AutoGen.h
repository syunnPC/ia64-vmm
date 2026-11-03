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

#ifndef _AUTOGENH_926c9cd0_4bb8_479b_9ac4_8a2a23f85307
#define _AUTOGENH_926c9cd0_4bb8_479b_9ac4_8a2a23f85307

#ifdef __cplusplus
extern "C" {
#endif

#include <Base.h>
#include <Library/PcdLib.h>

extern GUID  gEfiCallerIdGuid;
extern CHAR8 *gEfiCallerBaseName;


// Guids
extern GUID gEfiMdePkgTokenSpaceGuid;

// Definition of SkuId Array
extern UINT64 _gPcd_SkuId_Array[];

// PCD definitions
#define _PCD_TOKEN_PcdIoBlockBaseAddressForIpf  16U
extern const UINT64 _gPcd_FixedAtBuild_PcdIoBlockBaseAddressForIpf;
#define _PCD_GET_MODE_64_PcdIoBlockBaseAddressForIpf  _gPcd_FixedAtBuild_PcdIoBlockBaseAddressForIpf
//#define _PCD_SET_MODE_64_PcdIoBlockBaseAddressForIpf  ASSERT(FALSE)  // It is not allowed to set value for a FIXED_AT_BUILD PCD
#define _PCD_VALUE_PcdIoBlockBaseAddressForIpf 0x0003fffffc000000
#define _PCD_SIZE_PcdIoBlockBaseAddressForIpf 8
#define _PCD_GET_MODE_SIZE_PcdIoBlockBaseAddressForIpf _PCD_SIZE_PcdIoBlockBaseAddressForIpf


#ifdef __cplusplus
}
#endif

#endif
