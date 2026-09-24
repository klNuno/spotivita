// Newlib syscall shims the current vitasdk leaves undefined.
#include <psp2/kernel/rng.h>
#include <errno.h>
#include <reent.h>
#include <stddef.h>
#include <stdint.h>

// getentropy() (used by libstdc++'s std::random_device and mbedtls) lands on
// _getentropy_r, which newlib expects the platform to provide. Back it with the
// Vita's hardware RNG, which returns at most 64 bytes per call.
int _getentropy_r(struct _reent *r, void *buf, size_t len) {
    uint8_t *out = (uint8_t *) buf;
    while (len > 0) {
        size_t n = len > 64 ? 64 : len;
        if (sceKernelGetRandomNumber(out, n) < 0) {
            r->_errno = EIO;
            return -1;
        }
        out += n;
        len -= n;
    }
    return 0;
}
