#ifndef KERN_NFC_NCI_INTERNAL_H
#define KERN_NFC_NCI_INTERNAL_H

/*
 * Test-visible NCI helpers.
 *
 * These are not part of the public nfc.h API.  They exist so the host-side
 * framing test can feed synthetic packets through the same code the driver
 * uses, without a chip attached.  nfc_nci.c keeps them non-static, but the
 * only intended caller outside that file is components/nfc/test/.
 */

#include <stddef.h>
#include <stdint.h>

/*
 * Build one framed packet: HCIT type 0x10, MT|GID, OID, length, payload.
 * Returns the packet length, or 0 if the payload does not fit in one NCI
 * packet (length is one byte) or the output buffer is too small.
 */
size_t kern_nfc_nci_build_packet(uint8_t mt_gid, uint8_t oid,
                                 const uint8_t *payload, size_t payload_len,
                                 uint8_t *out, size_t cap);

/* 0 if the buffer is one whole, well-formed NCI packet; -1 otherwise. */
int kern_nfc_nci_validate_packet(const uint8_t *pkt, size_t len);

/* The nfc.py IsoDepTransport._is_tag() test. */
int kern_nfc_nci_is_tag(const uint8_t *pkt, size_t len);

/*
 * Derive the RF_DISCOVER_SELECT parameters from a discovery notification.
 * nfc.py falls back to SELECT_PARAMS when the notification does not carry a
 * discovery id/protocol pair.  out must hold 3 bytes.  Returns 0, or -1 if
 * out is NULL.
 */
int kern_nfc_nci_select_params(const uint8_t *ntf, size_t len, uint8_t out[3]);

/*
 * Split an MT_DATA packet into (data, data_len, sw1, sw2).  Returns 0 on
 * success, -1 for a non-data/malformed packet or a body shorter than 2.
 */
int kern_nfc_nci_parse_data(const uint8_t *pkt, size_t len,
                            const uint8_t **data, size_t *data_len,
                            uint8_t *sw1, uint8_t *sw2);

/*
 * The NCI status byte (payload[0]) of a command response.  Returns 0 and
 * stores it, or -1 if the packet is malformed or has no payload.
 */
int kern_nfc_nci_response_status(const uint8_t *pkt, size_t len,
                                 uint8_t *status);

/*
 * True for the notifications that nfc.py _data() treats as fatal to the
 * ISO-DEP link: CORE_INTERFACE_ERROR, CORE_GENERIC_ERROR, RF_DEACTIVATE.
 */
int kern_nfc_nci_is_terminal_error(const uint8_t *pkt, size_t len);

#endif
