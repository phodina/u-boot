// SPDX-License-Identifier: GPL-2.0+
/*
 * Optional gzip pre-compression for QR code payloads.
 *
 * Tries plain binary encoding first; on failure, gzip-compresses the
 * payload in place with a four-byte "QRGZ" magic header and re-encodes
 * in binary mode.
 */

#include <gzip.h>
#include <linux/string.h>
#include <linux/types.h>
#include <qrcodegen.h>

#define QRGZ_MAGIC_LEN	4
static const uint8_t qrgz_magic[QRGZ_MAGIC_LEN] = { 'Q', 'R', 'G', 'Z' };

bool qrcodegen_encodeBinaryCompressed(uint8_t dataAndTemp[], size_t dataLen,
				      uint8_t qrcode[], enum qrcodegen_Ecc ecl)
{
	size_t buf_max = qrcodegen_BUFFER_LEN_FOR_VERSION(qrcodegen_VERSION_MAX);
	unsigned long out_len;
	uint8_t *scratch;

	if (qrcodegen_encodeBinary(dataAndTemp, dataLen, qrcode, ecl,
				   qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
				   qrcodegen_Mask_AUTO, true))
		return true;

	/*
	 * Compress into qrcode[] (free at this point since the previous
	 * encode failed), then copy the magic + compressed payload back
	 * into dataAndTemp[] for the second encode pass.
	 */
	if (QRGZ_MAGIC_LEN > buf_max)
		return false;

	scratch = qrcode;
	out_len = buf_max;
	if (gzip(scratch, &out_len, dataAndTemp, dataLen) != 0)
		return false;

	if (out_len + QRGZ_MAGIC_LEN > buf_max)
		return false;

	memmove(dataAndTemp + QRGZ_MAGIC_LEN, scratch, out_len);
	memcpy(dataAndTemp, qrgz_magic, QRGZ_MAGIC_LEN);

	return qrcodegen_encodeBinary(dataAndTemp, out_len + QRGZ_MAGIC_LEN,
				      qrcode, ecl, qrcodegen_VERSION_MIN,
				      qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO,
				      true);
}
