/*
 * GnuTLS-based implementation of the schannel (SSL/TLS) provider.
 *
 * Copyright 2005 Juan Lang
 * Copyright 2008 Henri Verbeet
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <stdarg.h>
#ifdef SONAME_LIBGNUTLS
#include <gnutls/gnutls.h>
#endif

#include "windef.h"
#include "winbase.h"
#include "sspi.h"
#include "schannel.h"
#include "secur32_priv.h"
#include "wine/debug.h"
#include "wine/library.h"
#include "winreg.h"

WINE_DEFAULT_DEBUG_CHANNEL(secur32);

#if defined(SONAME_LIBGNUTLS) && !defined(HAVE_SECURITY_SECURITY_H)

static void *libgnutls_handle;
#define MAKE_FUNCPTR(f) static typeof(f) * p##f
MAKE_FUNCPTR(gnutls_alert_get);
MAKE_FUNCPTR(gnutls_alert_get_name);
MAKE_FUNCPTR(gnutls_certificate_allocate_credentials);
MAKE_FUNCPTR(gnutls_certificate_free_credentials);
MAKE_FUNCPTR(gnutls_certificate_get_peers);
MAKE_FUNCPTR(gnutls_cipher_get);
MAKE_FUNCPTR(gnutls_cipher_get_key_size);
MAKE_FUNCPTR(gnutls_credentials_set);
MAKE_FUNCPTR(gnutls_deinit);
MAKE_FUNCPTR(gnutls_global_deinit);
MAKE_FUNCPTR(gnutls_global_init);
MAKE_FUNCPTR(gnutls_global_set_log_function);
MAKE_FUNCPTR(gnutls_global_set_log_level);
MAKE_FUNCPTR(gnutls_handshake);
MAKE_FUNCPTR(gnutls_init);
MAKE_FUNCPTR(gnutls_kx_get);
MAKE_FUNCPTR(gnutls_mac_get);
MAKE_FUNCPTR(gnutls_mac_get_key_size);
MAKE_FUNCPTR(gnutls_perror);
MAKE_FUNCPTR(gnutls_priority_init);
MAKE_FUNCPTR(gnutls_priority_set);
MAKE_FUNCPTR(gnutls_protocol_get_version);
MAKE_FUNCPTR(gnutls_set_default_priority);
MAKE_FUNCPTR(gnutls_record_get_max_size);
MAKE_FUNCPTR(gnutls_record_recv);
MAKE_FUNCPTR(gnutls_record_send);
MAKE_FUNCPTR(gnutls_transport_get_ptr);
MAKE_FUNCPTR(gnutls_transport_set_errno);
MAKE_FUNCPTR(gnutls_transport_set_ptr);
MAKE_FUNCPTR(gnutls_transport_set_pull_function);
MAKE_FUNCPTR(gnutls_transport_set_push_function);
#undef MAKE_FUNCPTR



static ssize_t schan_pull_adapter(gnutls_transport_ptr_t transport,
                                      void *buff, size_t buff_len)
{
    struct schan_transport *t = (struct schan_transport*)transport;
    gnutls_session_t s = (gnutls_session_t)schan_session_for_transport(t);

    int ret = schan_pull(transport, buff, &buff_len);
    if (ret)
    {
        pgnutls_transport_set_errno(s, ret);
        return -1;
    }

    return buff_len;
}

static ssize_t schan_push_adapter(gnutls_transport_ptr_t transport,
                                      const void *buff, size_t buff_len)
{
    struct schan_transport *t = (struct schan_transport*)transport;
    gnutls_session_t s = (gnutls_session_t)schan_session_for_transport(t);

    int ret = schan_push(transport, buff, &buff_len);
    if (ret)
    {
        pgnutls_transport_set_errno(s, ret);
        return -1;
    }

    return buff_len;
}

#ifndef SSL_OP_NO_TLSv1_2
#define SSL_OP_NO_TLSv1_2				0x08000000L
#endif
#ifndef SSL_OP_NO_TLSv1_1
#define SSL_OP_NO_TLSv1_1				0x10000000L
#endif
#ifndef SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION
#define SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION	0x00040000L
#endif

static long schan_get_tls_option(void) { 
    long tls_option=0;
    DWORD type, val, size;
    HKEY hkey,protocols,tls12_client,tls11_client;
    LONG res;
    const WCHAR Schannel[] = { /* SYSTEM\\CurrentControlSet\\Control\\SecurityProviders\\SCANNEL */
              'S','Y','S','T','E','M','\\',
              'C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t','\\',
              'C','o','n','t','r','o','l','\\',
              'S','e','c','u','r','i','t','y','P','r','o','v','i','d','e','r','s','\\',
              'S','C','H','A','N','N','E','L', 0};
    const WCHAR Protocols[] = {'P','r','o','t','o','c','o','l','s',0};
    const WCHAR AllowInsecureRenegoClients[] = {
              'A','l','l','o','w','I','n','s','e','c','u','r','e','R','e','n','e','g','o',
              'C','l','i','e','n','t','s', 0};
#if 0
    const WCHAR AllowInsecureRenegoServers[] = {
              'A','l','l','o','w','I','n','s','e','c','u','r','e','R','e','n','e','g','o',
              'S','e','r','v','e','r','s', 0};
#endif
    const WCHAR TLS12_Client[] = {'T','L','S',' ','1','.','2','\\','C','l','i','e','n','t',0};
    const WCHAR TLS11_Client[] = {'T','L','S',' ','1','.','1','\\','C','l','i','e','n','t',0};
    const WCHAR DisabledByDefault[] = {'D','i','s','a','b','l','e','d','B','y','D','e','f','a','u','l','t',0};

    res = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
          Schannel,
          0, KEY_READ, &hkey);
    if (res != ERROR_SUCCESS) {
        tls_option |= SSL_OP_NO_TLSv1_2;
        tls_option |= SSL_OP_NO_TLSv1_1;
        goto end;
    }

    size = sizeof(DWORD);
    if (RegQueryValueExW(hkey, AllowInsecureRenegoClients, NULL, &type,  (LPBYTE) &val, &size) == ERROR_SUCCESS 
            && type == REG_DWORD) {
        tls_option |= val?SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION:0;
    } 
    if (RegOpenKeyExW(hkey, Protocols, 0, KEY_READ, &protocols) == ERROR_SUCCESS) {
        if (RegOpenKeyExW(protocols, TLS12_Client, 0, KEY_READ, &tls12_client) == ERROR_SUCCESS) {
            size = sizeof(DWORD);
            if (RegQueryValueExW(tls12_client, DisabledByDefault, NULL, &type,  (LPBYTE) &val, &size) || type != REG_DWORD) {
                tls_option |= SSL_OP_NO_TLSv1_2;
            } else {
                tls_option |= val?SSL_OP_NO_TLSv1_2:0;
            }
            RegCloseKey(tls12_client);
        } else {
            tls_option |= SSL_OP_NO_TLSv1_2;
        }
        if (RegOpenKeyExW(protocols, TLS11_Client, 0, KEY_READ, &tls11_client) == ERROR_SUCCESS) {
            size = sizeof(DWORD);
            if (RegQueryValueExW(tls11_client, DisabledByDefault, NULL, &type,  (LPBYTE) &val, &size) || type != REG_DWORD) {
                tls_option |= SSL_OP_NO_TLSv1_1;
            } else {
                tls_option |= val?SSL_OP_NO_TLSv1_1:0;
            }
            RegCloseKey(tls11_client);
        } else {
            tls_option |= SSL_OP_NO_TLSv1_1;
        }
        RegCloseKey(protocols);
    }

    RegCloseKey(hkey);
end:
    return tls_option;
}

static gnutls_priority_t gnutls_priorities[2][2][2];
    /* fields: TLS1.1:TLS1.2:Unsafe renagotiation */

static void schannel_gnutls_init_priorities (void)
{
    /* FIXME "NORMAL:%COMPAT" or "SECURE256:%COMPAT"? */

    pgnutls_priority_init (&gnutls_priorities[FALSE][FALSE][FALSE],
        "NORMAL:%COMPAT:!VERS-TLS1.2:!VERS-TLS1.1", NULL);
    pgnutls_priority_init (&gnutls_priorities[TRUE][FALSE][FALSE],
        "NORMAL:%COMPAT:!VERS-TLS1.2", NULL);
    pgnutls_priority_init (&gnutls_priorities[FALSE][TRUE][FALSE],
        "NORMAL:%COMPAT:!VERS-TLS1.1", NULL);
    pgnutls_priority_init (&gnutls_priorities[TRUE][TRUE][FALSE],
        "NORMAL:%COMPAT", NULL);

    pgnutls_priority_init (&gnutls_priorities[FALSE][FALSE][TRUE],
        "NORMAL:%COMPAT:!VERS-TLS1.2:!VERS-TLS1.1:%UNSAFE_RENEGOTIATION", NULL);
    pgnutls_priority_init (&gnutls_priorities[TRUE][FALSE][TRUE],
        "NORMAL:%COMPAT:!VERS-TLS1.2:%UNSAFE_RENEGOTIATION", NULL);
    pgnutls_priority_init (&gnutls_priorities[FALSE][TRUE][TRUE],
        "NORMAL:%COMPAT:!VERS-TLS1.1:%UNSAFE_RENEGOTIATION", NULL);
    pgnutls_priority_init (&gnutls_priorities[TRUE][TRUE][TRUE],
        "NORMAL:%COMPAT:%UNSAFE_RENEGOTIATION", NULL);
}

BOOL schan_imp_create_session(schan_imp_session *session, BOOL is_server,
                              schan_imp_certificate_credentials cred)
{
    gnutls_session_t *s = (gnutls_session_t*)session;
    long tls_option;
    int enable_tls11, enable_tls12, unsafe_rehandshake;

    int err = pgnutls_init(s, is_server ? GNUTLS_SERVER : GNUTLS_CLIENT);
    if (err != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror(err);
        return FALSE;
    }

    /* FIXME registory DisabledByDefault value does not take precedence
       over the grbitEnabledProtocols of credential,
       but 'Enabled' value should take precedence over it.(for ssl3.0) */
    tls_option = schan_get_tls_option();
    if ((int)cred & SP_PROT_TLS1_1_CLIENT) {
        enable_tls11 = TRUE;
    } else {
        enable_tls11 = (tls_option & SSL_OP_NO_TLSv1_1)?FALSE:TRUE;
    }
    if ((int)cred & SP_PROT_TLS1_2_CLIENT) {
        enable_tls12 = TRUE;
    } else {
        enable_tls12 = (tls_option & SSL_OP_NO_TLSv1_2)?FALSE:TRUE;
    }
    unsafe_rehandshake = (tls_option & SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION)?TRUE:FALSE;

    /* FIXME: We should be using the information from the credentials here. */
    FIXME("Using hardcoded priority with: TLS1.1:%s, TLS1.2:%s, UnsafeRenego:%s\n", 
                                 enable_tls11?"enabled":"disabled",
                                 enable_tls12?"enabled":"disabled",
                                 unsafe_rehandshake?"enabled":"disabled"
                                 );
    err = pgnutls_priority_set (*s, gnutls_priorities[enable_tls11][enable_tls12][unsafe_rehandshake]);

    if (err != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror(err);
        pgnutls_deinit(*s);
        return FALSE;
    }

    err = pgnutls_credentials_set(*s, GNUTLS_CRD_CERTIFICATE,
                                  (gnutls_certificate_credentials)cred);
    if (err != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror(err);
        pgnutls_deinit(*s);
        return FALSE;
    }

    pgnutls_transport_set_pull_function(*s, schan_pull_adapter);
    pgnutls_transport_set_push_function(*s, schan_push_adapter);

    return TRUE;
}

void schan_imp_dispose_session(schan_imp_session session)
{
    gnutls_session_t s = (gnutls_session_t)session;
    pgnutls_deinit(s);
}

void schan_imp_set_session_transport(schan_imp_session session,
                                     struct schan_transport *t)
{
    gnutls_session_t s = (gnutls_session_t)session;
    pgnutls_transport_set_ptr(s, (gnutls_transport_ptr_t)t);
}

SECURITY_STATUS schan_imp_handshake(schan_imp_session session)
{
    gnutls_session_t s = (gnutls_session_t)session;
    int err = pgnutls_handshake(s);
    switch(err)
    {
        case GNUTLS_E_SUCCESS:
            TRACE("Handshake completed\n");
            return SEC_E_OK;

        case GNUTLS_E_AGAIN:
            TRACE("Continue...\n");
            return SEC_I_CONTINUE_NEEDED;

        case GNUTLS_E_WARNING_ALERT_RECEIVED:
        case GNUTLS_E_FATAL_ALERT_RECEIVED:
        {
            gnutls_alert_description_t alert = pgnutls_alert_get(s);
            const char *alert_name = pgnutls_alert_get_name(alert);
            WARN("ALERT: %d %s\n", alert, alert_name);
            return SEC_E_INTERNAL_ERROR;
        }

        default:
            pgnutls_perror(err);
            return SEC_E_INTERNAL_ERROR;
    }

    /* Never reached */
    return SEC_E_OK;
}

static unsigned int schannel_get_cipher_block_size(gnutls_cipher_algorithm_t cipher)
{
    const struct
    {
        gnutls_cipher_algorithm_t cipher;
        unsigned int block_size;
    }
    algorithms[] =
    {
        {GNUTLS_CIPHER_3DES_CBC, 8},
        {GNUTLS_CIPHER_AES_128_CBC, 16},
        {GNUTLS_CIPHER_AES_256_CBC, 16},
        {GNUTLS_CIPHER_ARCFOUR_128, 1},
        {GNUTLS_CIPHER_ARCFOUR_40, 1},
        {GNUTLS_CIPHER_DES_CBC, 8},
        {GNUTLS_CIPHER_NULL, 1},
        {GNUTLS_CIPHER_RC2_40_CBC, 8},
    };
    unsigned int i;

    for (i = 0; i < sizeof(algorithms) / sizeof(*algorithms); ++i)
    {
        if (algorithms[i].cipher == cipher)
            return algorithms[i].block_size;
    }

    FIXME("Unknown cipher %#x, returning 1\n", cipher);

    return 1;
}

static DWORD schannel_get_protocol(gnutls_protocol_t proto)
{
    /* FIXME: currently schannel only implements client connections, but
     * there's no reason it couldn't be used for servers as well.  The
     * context doesn't tell us which it is, so assume client for now.
     */
    switch (proto)
    {
    case GNUTLS_SSL3: return SP_PROT_SSL3_CLIENT;
    case GNUTLS_TLS1_0: return SP_PROT_TLS1_0_CLIENT;
    case GNUTLS_TLS1_1: return SP_PROT_TLS1_1_CLIENT;
    case GNUTLS_TLS1_2: return SP_PROT_TLS1_2_CLIENT;
    default:
        FIXME("unknown protocol %d\n", proto);
        return 0;
    }
}

static ALG_ID schannel_get_cipher_algid(gnutls_cipher_algorithm_t cipher)
{
    switch (cipher)
    {
    case GNUTLS_CIPHER_UNKNOWN:
    case GNUTLS_CIPHER_NULL: return 0;
    case GNUTLS_CIPHER_ARCFOUR_40:
    case GNUTLS_CIPHER_ARCFOUR_128: return CALG_RC4;
    case GNUTLS_CIPHER_DES_CBC:
    case GNUTLS_CIPHER_3DES_CBC: return CALG_DES;
    case GNUTLS_CIPHER_AES_128_CBC:
    case GNUTLS_CIPHER_AES_256_CBC: return CALG_AES;
    case GNUTLS_CIPHER_RC2_40_CBC: return CALG_RC2;
    default:
        FIXME("unknown algorithm %d\n", cipher);
        return 0;
    }
}

static ALG_ID schannel_get_mac_algid(gnutls_mac_algorithm_t mac)
{
    switch (mac)
    {
    case GNUTLS_MAC_UNKNOWN:
    case GNUTLS_MAC_NULL: return 0;
    case GNUTLS_MAC_MD5: return CALG_MD5;
    case GNUTLS_MAC_SHA1:
    case GNUTLS_MAC_SHA256:
    case GNUTLS_MAC_SHA384:
    case GNUTLS_MAC_SHA512: return CALG_SHA;
    default:
        FIXME("unknown algorithm %d\n", mac);
        return 0;
    }
}

static ALG_ID schannel_get_kx_algid(gnutls_kx_algorithm_t kx)
{
    switch (kx)
    {
        case GNUTLS_KX_RSA: return CALG_RSA_KEYX;
        case GNUTLS_KX_DHE_DSS:
        case GNUTLS_KX_DHE_RSA: return CALG_DH_EPHEM;
    default:
        FIXME("unknown algorithm %d\n", kx);
        return 0;
    }
}

unsigned int schan_imp_get_session_cipher_block_size(schan_imp_session session)
{
    gnutls_session_t s = (gnutls_session_t)session;
    gnutls_cipher_algorithm_t cipher = pgnutls_cipher_get(s);
    return schannel_get_cipher_block_size(cipher);
}

unsigned int schan_imp_get_max_message_size(schan_imp_session session)
{
    return pgnutls_record_get_max_size((gnutls_session_t)session);
}

SECURITY_STATUS schan_imp_get_connection_info(schan_imp_session session,
                                              SecPkgContext_ConnectionInfo *info)
{
    gnutls_session_t s = (gnutls_session_t)session;
    gnutls_protocol_t proto = pgnutls_protocol_get_version(s);
    gnutls_cipher_algorithm_t alg = pgnutls_cipher_get(s);
    gnutls_mac_algorithm_t mac = pgnutls_mac_get(s);
    gnutls_kx_algorithm_t kx = pgnutls_kx_get(s);

    info->dwProtocol = schannel_get_protocol(proto);
    info->aiCipher = schannel_get_cipher_algid(alg);
    info->dwCipherStrength = pgnutls_cipher_get_key_size(alg);
    info->aiHash = schannel_get_mac_algid(mac);
    info->dwHashStrength = pgnutls_mac_get_key_size(mac);
    info->aiExch = schannel_get_kx_algid(kx);
    /* FIXME: info->dwExchStrength? */
    info->dwExchStrength = 0;
    return SEC_E_OK;
}

SECURITY_STATUS schan_imp_get_session_peer_certificate(schan_imp_session session,
                                                       PCCERT_CONTEXT *cert)
{
    gnutls_session_t s = (gnutls_session_t)session;
    unsigned int list_size;
    const gnutls_datum_t *datum;

    datum = pgnutls_certificate_get_peers(s, &list_size);
    if (datum)
    {
        *cert = CertCreateCertificateContext(X509_ASN_ENCODING, datum->data,
                                             datum->size);
        if (!*cert)
            return GetLastError();
        else
            return SEC_E_OK;
    }
    else
        return SEC_E_INTERNAL_ERROR;
}

SECURITY_STATUS schan_imp_send(schan_imp_session session, const void *buffer,
                               SIZE_T *length)
{
    gnutls_session_t s = (gnutls_session_t)session;
    ssize_t ret;

again:
    ret = pgnutls_record_send(s, buffer, *length);

    if (ret >= 0)
        *length = ret;
    else if (ret == GNUTLS_E_AGAIN)
    {
        struct schan_transport *t = (struct schan_transport *)pgnutls_transport_get_ptr(s);
        SIZE_T count = 0;

        if (schan_get_buffer(t, &t->out, &count))
            goto again;

        return SEC_I_CONTINUE_NEEDED;
    }
    else
    {
        pgnutls_perror(ret);
        return SEC_E_INTERNAL_ERROR;
    }

    return SEC_E_OK;
}

SECURITY_STATUS schan_imp_recv(schan_imp_session session, void *buffer,
                               SIZE_T *length)
{
    gnutls_session_t s = (gnutls_session_t)session;
    ssize_t ret;

again:
    ret = pgnutls_record_recv(s, buffer, *length);

    if (ret >= 0)
        *length = ret;
    else if (ret == GNUTLS_E_AGAIN)
    {
        struct schan_transport *t = (struct schan_transport *)pgnutls_transport_get_ptr(s);
        SIZE_T count = 0;

        if (schan_get_buffer(t, &t->in, &count))
            goto again;

        return SEC_I_CONTINUE_NEEDED;
    }
    else
    {
        pgnutls_perror(ret);
        return SEC_E_INTERNAL_ERROR;
    }

    return SEC_E_OK;
}

BOOL schan_imp_allocate_certificate_credentials(schan_imp_certificate_credentials *c)
{
    int ret = pgnutls_certificate_allocate_credentials((gnutls_certificate_credentials*)c);
    if (ret != GNUTLS_E_SUCCESS)
        pgnutls_perror(ret);
    return (ret == GNUTLS_E_SUCCESS);
}

void schan_imp_free_certificate_credentials(schan_imp_certificate_credentials c)
{
    pgnutls_certificate_free_credentials((gnutls_certificate_credentials)c);
}

static void schan_gnutls_log(int level, const char *msg)
{
    TRACE("<%d> %s", level, msg);
}

BOOL schan_imp_init(void)
{
    int ret;

    libgnutls_handle = wine_dlopen(SONAME_LIBGNUTLS, RTLD_NOW, NULL, 0);
    if (!libgnutls_handle)
    {
        WARN("Failed to load libgnutls.\n");
        return FALSE;
    }

#define LOAD_FUNCPTR(f) \
    if (!(p##f = wine_dlsym(libgnutls_handle, #f, NULL, 0))) \
    { \
        ERR("Failed to load %s\n", #f); \
        goto fail; \
    }

    LOAD_FUNCPTR(gnutls_alert_get)
    LOAD_FUNCPTR(gnutls_alert_get_name)
    LOAD_FUNCPTR(gnutls_certificate_allocate_credentials)
    LOAD_FUNCPTR(gnutls_certificate_free_credentials)
    LOAD_FUNCPTR(gnutls_certificate_get_peers)
    LOAD_FUNCPTR(gnutls_cipher_get)
    LOAD_FUNCPTR(gnutls_cipher_get_key_size)
    LOAD_FUNCPTR(gnutls_credentials_set)
    LOAD_FUNCPTR(gnutls_deinit)
    LOAD_FUNCPTR(gnutls_global_deinit)
    LOAD_FUNCPTR(gnutls_global_init)
    LOAD_FUNCPTR(gnutls_global_set_log_function)
    LOAD_FUNCPTR(gnutls_global_set_log_level)
    LOAD_FUNCPTR(gnutls_handshake)
    LOAD_FUNCPTR(gnutls_init)
    LOAD_FUNCPTR(gnutls_kx_get)
    LOAD_FUNCPTR(gnutls_mac_get)
    LOAD_FUNCPTR(gnutls_mac_get_key_size)
    LOAD_FUNCPTR(gnutls_perror)
    LOAD_FUNCPTR(gnutls_priority_init);
    LOAD_FUNCPTR(gnutls_priority_set);
    LOAD_FUNCPTR(gnutls_protocol_get_version)
    LOAD_FUNCPTR(gnutls_set_default_priority)
    LOAD_FUNCPTR(gnutls_record_get_max_size);
    LOAD_FUNCPTR(gnutls_record_recv);
    LOAD_FUNCPTR(gnutls_record_send);
    LOAD_FUNCPTR(gnutls_transport_get_ptr)
    LOAD_FUNCPTR(gnutls_transport_set_errno)
    LOAD_FUNCPTR(gnutls_transport_set_ptr)
    LOAD_FUNCPTR(gnutls_transport_set_pull_function)
    LOAD_FUNCPTR(gnutls_transport_set_push_function)
#undef LOAD_FUNCPTR

    ret = pgnutls_global_init();
    if (ret != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror(ret);
        goto fail;
    }
    schannel_gnutls_init_priorities();

    if (TRACE_ON(secur32))
    {
        pgnutls_global_set_log_level(4);
        pgnutls_global_set_log_function(schan_gnutls_log);
    }

    return TRUE;

fail:
    wine_dlclose(libgnutls_handle, NULL, 0);
    libgnutls_handle = NULL;
    return FALSE;
}

void schan_imp_deinit(void)
{
    pgnutls_global_deinit();
    wine_dlclose(libgnutls_handle, NULL, 0);
    libgnutls_handle = NULL;
}

#endif /* SONAME_LIBGNUTLS && !HAVE_SECURITY_SECURITY_H */
