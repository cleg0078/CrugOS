#pragma once

#include <crug/types.h>

/* Argon2id v1.3 (RFC 9106). Memory cost is in 1 KiB blocks (as in RFC 9106). */
#define ARGON2_VERSION_13 0x13u
#define ARGON2_TYPE_ID 2u

/* Hash into out[outlen] using the kernel page allocator for the workspace.
 * Returns 0 on success, negative on invalid parameters or no memory. */
int argon2id_hash(u8 *out, usize outlen, const u8 *pwd, usize pwdlen, const u8 *salt,
                  usize saltlen, u32 m_cost_kib, u32 t_cost, u32 lanes);

/* Same, but the caller provides the workspace (must be at least
 * m_cost_kib * 1024 bytes). Used by the host unit tests. */
int argon2id_hash_mem(u8 *out, usize outlen, const u8 *pwd, usize pwdlen, const u8 *salt,
                      usize saltlen, u32 m_cost_kib, u32 t_cost, u32 lanes, void *mem,
                      usize mem_bytes);