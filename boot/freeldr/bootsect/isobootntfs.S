/* Test-only ISO boot sector: never chain to a disposable disk MBR. */
#define FORCE_BOOT_MEDIUM
#include "isoboot.S"
