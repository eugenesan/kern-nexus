/*
 * BCM2079x NFC transport for the Nexus 5 (hammerhead).
 *
 * The chip is on I2C bus 0 at 0x77, but the vendor driver owns that
 * address.  Userspace reaches it through /dev/bcm2079x and seven ioctls;
 * the driver delivers one whole framed packet per read().
 *
 * Protocol (NCI bring-up, ISO-DEP, APDUs) is layered in nfc_nci.c.  The
 * reference is krux-nexus/krux/src/nfc.py and docs/nfc-transport.md.
 */
#include "nfc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BCMNFC_MAGIC 0xFA
#define BCMNFC_POWER_CTL 0xFA01
#define BCMNFC_CHANGE_ADDR 0xFA02
#define BCMNFC_READ_FULL_PACKET 0xFA03
#define BCMNFC_SET_WAKE_ACTIVE_STATE 0xFA04
#define BCMNFC_WAKE_CTL 0xFA05
#define BCMNFC_READ_MULTI_PACKETS 0xFA06
#define BCMNFC_SET_CLIENT_ADDR 0xFA07

int kern_nfc_open(kern_nfc_t *nfc) {
  if (!nfc)
    return -1;
  nfc->fd = open(KERN_NFC_DEV, O_RDWR | O_NOCTTY);
  return nfc->fd < 0 ? -1 : 0;
}

void kern_nfc_close(kern_nfc_t *nfc) {
  if (nfc && nfc->fd >= 0) {
    close(nfc->fd);
    nfc->fd = -1;
  }
}

int kern_nfc_power(kern_nfc_t *nfc, int on) {
  return ioctl(nfc->fd, BCMNFC_POWER_CTL, on);
}

int kern_nfc_wake(kern_nfc_t *nfc, int state) {
  return ioctl(nfc->fd, BCMNFC_WAKE_CTL, state);
}

int kern_nfc_write(kern_nfc_t *nfc, const uint8_t *data, size_t len) {
  ssize_t n = write(nfc->fd, data, len);
  return n < 0 ? -1 : (int)n;
}

int kern_nfc_read(kern_nfc_t *nfc, uint8_t *buf, size_t cap, int timeout_ms) {
  struct pollfd pfd = {.fd = nfc->fd, .events = POLLIN};
  int ready = poll(&pfd, 1, timeout_ms);
  if (ready < 0)
    return errno == EINTR ? 0 : -1;
  if (ready == 0)
    return 0;
  ssize_t n = read(nfc->fd, buf, cap);
  if (n < 0)
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
  return (int)n;
}
