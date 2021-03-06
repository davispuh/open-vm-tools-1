/*********************************************************
 * Copyright (C) 2011-2015 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

#ifndef _CERTVERIFY_H_
#define _CERTVERIFY_H_

/*
 * @file certverify.h
 *
 * Certificate verification support.
 */

#include <glib.h>
#include "VGAuthAuthentication.h"

/*
 * XXX Do we still need this?  What other algorithms do SAML tokens use?
 */
typedef int VGAuthHashAlg;
enum {
      /** SHA-2 with a 256-bit output size (also known as SHA-256). */
      VGAUTH_HASH_ALG_SHA256,
};

void CertVerify_Init(void);

gboolean CertVerify_IsWellFormedPEMCert(const char *pemCert);

VGAuthError CertVerify_CertChain(const char *pemLeafCert,
                                  int numUntrustedCerts,
                                  const char **pemUntrustedCertChain,
                                  int numTrustedCerts,
                                  const char **pemTrustedCertChain);

VGAuthError CertVerify_CheckSignatureUsingCert(VGAuthHashAlg hash,
                                               const char *pemCert,
                                               size_t dataLen,
                                               const unsigned char *data,
                                               size_t signatureLen,
                                               const unsigned char *signature);

gchar * CertVerify_StripPEMCert(const gchar *pemCert);

#endif // _CERTVERIFY_H_
