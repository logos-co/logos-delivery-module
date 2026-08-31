// Stub header for logos-delivery's service-discovery plugin ABI - mirrors
// library/logosdelivery_service_discovery.h so the module compiles during unit
// tests without the real library. Keep in sync with the real header when
// bumping the logos-delivery flake input.
//
// The real header pulls in "generated/logosdelivery.h" for the registration
// entry point; the stub set folds the generated surface into liblogosdelivery.h,
// so that is what is included here instead.

#pragma once
#ifndef __logosdelivery_service_discovery__
#define __logosdelivery_service_discovery__

#include <stddef.h>
#include <stdint.h>

#include "liblogosdelivery.h"

#define LD_DISCO_ABI_VERSION 1

#define LD_DISCO_OK 0
#define LD_DISCO_ERROR 1

#ifdef __cplusplus
extern "C"
{
#endif

  typedef int (*LdDiscoStartFn)(void *pluginCtx, char *errBuf, size_t errBufLen);

  typedef int (*LdDiscoStopFn)(void *pluginCtx, char *errBuf, size_t errBufLen);

  typedef int (*LdDiscoLookupFn)(void *pluginCtx,
                                 const char *key,
                                 int64_t limit,
                                 char **outJson,
                                 char *errBuf,
                                 size_t errBufLen);

  typedef int (*LdDiscoRandomLookupFn)(void *pluginCtx,
                                       char **outJson,
                                       char *errBuf,
                                       size_t errBufLen);

  typedef void (*LdDiscoFreeStringFn)(void *pluginCtx, char *s);

  typedef int (*LdDiscoStartAdvertisingFn)(void *pluginCtx,
                                           const char *key,
                                           const uint8_t *data,
                                           size_t dataLen,
                                           const uint8_t *record,
                                           size_t recordLen,
                                           char *errBuf,
                                           size_t errBufLen);

  typedef int (*LdDiscoStopAdvertisingFn)(void *pluginCtx,
                                          const char *key,
                                          char *errBuf,
                                          size_t errBufLen);

  typedef int (*LdDiscoRegisterInterestFn)(void *pluginCtx,
                                           const char *key,
                                           char *errBuf,
                                           size_t errBufLen);

  typedef int (*LdDiscoUnregisterInterestFn)(void *pluginCtx,
                                             const char *key,
                                             char *errBuf,
                                             size_t errBufLen);

  typedef struct
  {
    uint32_t abiVersion;
    void *pluginCtx;
    uint32_t requestTimeoutMs;

    LdDiscoStartFn start;
    LdDiscoStopFn stop;
    LdDiscoLookupFn lookup;
    LdDiscoRandomLookupFn randomLookup;
    LdDiscoFreeStringFn freeString;
    LdDiscoStartAdvertisingFn startAdvertising;
    LdDiscoStopAdvertisingFn stopAdvertising;
    LdDiscoRegisterInterestFn registerInterest;
    LdDiscoUnregisterInterestFn unregisterInterest;
  } LdServiceDiscoveryPlugin;

  /* Generated entry points, declared here because the stub set has no
   * generated/ directory of its own. */
  int logosdelivery_set_service_discovery_plugin(void *ctx,
                                                 LogosDeliveryScalarRawFn callback,
                                                 void *user_data,
                                                 uint64_t pluginPtr);

  int logosdelivery_clear_service_discovery_plugin(void *ctx,
                                                   LogosDeliveryScalarRawFn callback,
                                                   void *user_data);

  static inline int logosdelivery_install_service_discovery_plugin(
      void *ctx,
      const LdServiceDiscoveryPlugin *plugin,
      LogosDeliveryScalarRawFn callback,
      void *user_data)
  {
    return logosdelivery_set_service_discovery_plugin(
        ctx, callback, user_data, (uint64_t)(uintptr_t)plugin);
  }

#ifdef __cplusplus
}
#endif

#endif /* __logosdelivery_service_discovery__ */
