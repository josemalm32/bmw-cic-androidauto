Required Notice: Copyright (c) 2026 josemalm32 (https://github.com/josemalm32)

AAPlugin is licensed under the PolyForm Noncommercial License 1.0.0 (LICENSE.md):
free to use, study, modify and share for any noncommercial purpose. Selling it,
selling devices or installation services that include it, or any other commercial
use requires prior written permission from josemalm32.

Third-party components
----------------------
* mbed TLS 3.6.7 (https://github.com/Mbed-TLS/mbedtls), Apache License 2.0,
  included as a git submodule in third_party/mbedtls (unmodified). At build time
  patches/mbedtls-3.6.7-x509-utc-offset.patch is applied to a copy of
  library/x509.c (accepts UTCTime with a numeric timezone, needed by some
  head-unit certificates). The result is compiled into libcicaa.so; binary
  distributions must include the Apache-2.0 license text (the release zip does).

Not included and not covered by this license: BMW firmware (HmiMain, DSP image,
system libraries; only symbol names and code signatures are referenced) and any
Android Auto head-unit certificate.
