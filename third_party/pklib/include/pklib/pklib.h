/*****************************************************************************/
/* pklib.h                                Copyright (c) Ladislav Zezula 2003 */
/*---------------------------------------------------------------------------*/
/* Header file for PKWARE Data Compression Library                           */
/*---------------------------------------------------------------------------*/
/*   Date    Ver   Who  Comment                                              */
/* --------  ----  ---  -------                                              */
/* 31.03.03  1.00  Lad  Created                                              */
/*****************************************************************************/

#ifndef __PKLIB_H__
#define __PKLIB_H__

#pragma once
#include <stddef.h>

#include "./api.h"
#include "./tables.h"

//-----------------------------------------------------------------------------
// Defines

#define CMP_BINARY             0            // Binary compression
#define CMP_ASCII              1            // Ascii compression

enum PklibErrorCode{
	CMP_NO_ERROR = 0,
	CMP_INVALID_DICTSIZE = 1,
	CMP_INVALID_MODE = 2,
	CMP_BAD_DATA = 3,
	CMP_ABORT = 4,
};

enum CommonSizes {
    OUT_BUFF_SIZE = 0x802,
    BUFF_SIZE=0x2204,
};

//-----------------------------------------------------------------------------
// Interface structures

// Sizes needed to allocate buffer for both TCmpStruct and TDcmpStruct and access their fields in a future-proof way
struct CommonSizeConstants{
    size_t own_size, OUT_BUFF_SIZE, BUFF_SIZE;
};

//-----------------------------------------------------------------------------
// Public functions

#ifdef __cplusplus
   extern "C" {
#endif

struct CommonSizeConstants PKEXPORT getCommonSizeConstants();

#ifdef __cplusplus
   }                         // End of 'extern "C"' declaration
#endif

#endif // __PKLIB_H__

