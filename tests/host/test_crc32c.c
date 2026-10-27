/* Host tests for the CRC32C primitive used by ext4 and JBD2 checksums. */
#include "test.h"
#include <crug/crc32c.h>
#include <crug/string.h>

void test_crc32c_functions(void)
{
	static const char digits[] = "123456789";
	static const u8 zeros[4] = { 0, 0, 0, 0 };
	static const char text[] = "ext4 metadata checksum";
	const usize text_len = sizeof(text) - 1;
	u32 whole = crc32c(CRC32C_SEED_ONES, text, text_len);
	u32 part;

	/* The published CRC-32C check value 0xE3069283 uses an all-ones seed
	 * and a final xor of all ones; this raw primitive omits the final xor.
	 */
	CHECK(crc32c(CRC32C_SEED_ONES, digits, 9) == 0x1CF96D7Cu);
	CHECK(crc32c(CRC32C_SEED_ZERO, digits, 9) == 0x58E3FA20u);
	CHECK(crc32c(CRC32C_SEED_ZERO, digits, 0) == 0u);
	CHECK(crc32c(CRC32C_SEED_ONES, digits, 0) == 0xFFFFFFFFu);
	CHECK(crc32c(CRC32C_SEED_ONES, zeros, sizeof(zeros)) == 0xB798B438u);

	/* Chaining is associative over concatenation. */
	part = crc32c(CRC32C_SEED_ONES, text, 5);
	part = crc32c(part, text + 5, text_len - 5);
	CHECK(part == whole);

	/* Feeding bytes one at a time matches the bulk call. */
	part = CRC32C_SEED_ONES;
	for (usize i = 0; i < text_len; i++)
		part = crc32c(part, text + i, 1);
	CHECK(part == whole);

	/* A zero-length range leaves the seed untouched. */
	CHECK(crc32c(whole, text, 0) == whole);
}
