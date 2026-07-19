/*
   GNSS Receiver Communication SDK for C++ and Qt
   For connecting to and communicating with a Septentrio GNSS receiver, using C++/Qt

   \par Copyright:
     (c) 2012-2015 Copyright Septentrio NV/SA. All rights reserved.
*/


/**
   \file ssnrx_export.h

   Preprocessor magic to allow export of library symbols.

   This is strictly internal.

   \note You should not include this header directly from an
   application. You should just use <tt> \#include "ssnrx.h" </tt> instead.

*/

#ifndef SSNRX_EXPORT_H__
#define SSNRX_EXPORT_H__

#include <QtGlobal>

#ifdef Q_OS_WIN
# ifndef SSNRX_STATIC
#  ifdef SSNRX_MAKEDLL
#   define SSNRX_EXPORT Q_DECL_EXPORT
#  else
#   define SSNRX_EXPORT Q_DECL_IMPORT
#  endif
# endif
#endif

#ifndef SSNRX_EXPORT
# define SSNRX_EXPORT
#endif

#endif // SSNRX_EXPORT_H__
