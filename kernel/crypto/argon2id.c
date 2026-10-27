/* Argon2id v1.3 (RFC 9106), single-threaded lane fill. */
#include <crug/argon2.h>
#include <crug/blake2b.h>
#include <crug/mm.h>
#include <crug/string.h>

#define ARGON2_BLOCK_WORDS 128u
#define ARGON2_BLOCK_BYTES 1024u
#define ARGON2_SYNC_POINTS 4u
#define ARGON2_ADDRESSES_IN_BLOCK 128u
#define ARGON2_PREHASH_DIGEST 64u
#define ARGON2_PREHASH_SEED 72u
#define ARGON2_MAX_LANES 64u
#define ARGON2_MAX_OUT 1024u

typedef struct {
    u64 v[ARGON2_BLOCK_WORDS];
} argon2_block;

static void store32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void store64(u8 *p, u64 v)
{
    int i;

    for (i = 0; i < 8; i++) {
        p[i] = (u8)(v >> (8 * i));
    }
}

static u64 load64(const u8 *p)
{
    u64 v = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static u64 rotr64(u64 x, unsigned n)
{
    return (x >> n) | (x << (64 - n));
}

/* Lyra PHC multiplication: x + y + 2 * lo32(x) * lo32(y). */
static u64 fblamka(u64 x, u64 y)
{
    u64 lo = (x & 0xFFFFFFFFull) * (y & 0xFFFFFFFFull);

    return x + y + 2 * lo;
}

static void g(u64 *a, u64 *b, u64 *c, u64 *d)
{
    *a = fblamka(*a, *b);
    *d = rotr64(*d ^ *a, 32);
    *c = fblamka(*c, *d);
    *b = rotr64(*b ^ *c, 24);
    *a = fblamka(*a, *b);
    *d = rotr64(*d ^ *a, 16);
    *c = fblamka(*c, *d);
    *b = rotr64(*b ^ *c, 63);
}

static void round16(u64 *v)
{
    g(&v[0], &v[4], &v[8], &v[12]);
    g(&v[1], &v[5], &v[9], &v[13]);
    g(&v[2], &v[6], &v[10], &v[14]);
    g(&v[3], &v[7], &v[11], &v[15]);
    g(&v[0], &v[5], &v[10], &v[15]);
    g(&v[1], &v[6], &v[11], &v[12]);
    g(&v[2], &v[7], &v[8], &v[13]);
    g(&v[3], &v[4], &v[9], &v[14]);
}

static void round_mixed(u64 *v, const unsigned *idx)
{
    u64 w[16];
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = v[idx[i]];
    }
    round16(w);
    for (i = 0; i < 16; i++) {
        v[idx[i]] = w[i];
    }
}

static void fill_block(const argon2_block *prev, const argon2_block *ref,
                       argon2_block *next, int with_xor)
{
    argon2_block r;
    argon2_block tmp;
    int i;

    for (i = 0; i < (int)ARGON2_BLOCK_WORDS; i++) {
        r.v[i] = ref->v[i] ^ prev->v[i];
    }
    tmp = r;
    if (with_xor) {
        for (i = 0; i < (int)ARGON2_BLOCK_WORDS; i++) {
            tmp.v[i] ^= next->v[i];
        }
    }
    for (i = 0; i < 8; i++) {
        round16(&r.v[16 * i]);
    }
    for (i = 0; i < 8; i++) {
        unsigned idx[16];
        int j;

        for (j = 0; j < 8; j++) {
            idx[2 * j] = (unsigned)(2 * i + 16 * j);
            idx[2 * j + 1] = (unsigned)(2 * i + 16 * j + 1);
        }
        round_mixed(r.v, idx);
    }
    for (i = 0; i < (int)ARGON2_BLOCK_WORDS; i++) {
        next->v[i] = tmp.v[i] ^ r.v[i];
    }
}

static void next_addresses(argon2_block *address, argon2_block *input,
                           const argon2_block *zero)
{
    input->v[6]++;
    fill_block(zero, input, address, 0);
    fill_block(zero, address, address, 0);
}

typedef struct {
    u32 pass;
    u32 lane;
    u32 slice;
    u32 index;
} argon2_position;

typedef struct {
    argon2_block *memory;
    u32 memory_blocks;
    u32 passes;
    u32 lanes;
    u32 segment_length;
    u32 lane_length;
} argon2_instance;

static u32 index_alpha(const argon2_instance *in, const argon2_position *pos,
                       u32 pseudo_rand, int same_lane)
{
    u32 reference_area_size;
    u64 relative_position;
    u32 start_position;
    u32 absolute_position;

    if (pos->pass == 0) {
        if (pos->slice == 0) {
            reference_area_size = pos->index - 1;
        } else if (same_lane) {
            reference_area_size = pos->slice * in->segment_length + pos->index - 1;
        } else {
            reference_area_size = pos->slice * in->segment_length +
                                  ((pos->index == 0) ? 0xFFFFFFFFu : 0u);
        }
    } else if (same_lane) {
        reference_area_size = in->lane_length - in->segment_length + pos->index - 1;
    } else {
        reference_area_size = in->lane_length - in->segment_length +
                              ((pos->index == 0) ? 0xFFFFFFFFu : 0u);
    }

    relative_position = pseudo_rand;
    relative_position = (relative_position * relative_position) >> 32;
    relative_position = reference_area_size - 1 -
                        (((u64)reference_area_size * relative_position) >> 32);
    start_position = 0;
    if (pos->pass != 0) {
        start_position = (pos->slice == ARGON2_SYNC_POINTS - 1)
                             ? 0
                             : (pos->slice + 1) * in->segment_length;
    }
    absolute_position = (u32)((start_position + relative_position) % in->lane_length);
    return absolute_position;
}

static void fill_segment(const argon2_instance *in, argon2_position pos)
{
    argon2_block address;
    argon2_block input;
    argon2_block zero;
    u32 starting_index = 0;
    u32 curr_offset;
    u32 prev_offset;
    u32 i;
    int data_independent;

    data_independent = (pos.pass == 0) && (pos.slice < ARGON2_SYNC_POINTS / 2);
    memset(&zero, 0, sizeof(zero));
    memset(&input, 0, sizeof(input));
    memset(&address, 0, sizeof(address));

    if (data_independent) {
        input.v[0] = pos.pass;
        input.v[1] = pos.lane;
        input.v[2] = pos.slice;
        input.v[3] = in->memory_blocks;
        input.v[4] = in->passes;
        input.v[5] = ARGON2_TYPE_ID;
    }
    if (pos.pass == 0 && pos.slice == 0) {
        starting_index = 2;
        if (data_independent) {
            next_addresses(&address, &input, &zero);
        }
    }

    curr_offset = pos.lane * in->lane_length + pos.slice * in->segment_length + starting_index;
    if (curr_offset % in->lane_length == 0) {
        prev_offset = curr_offset + in->lane_length - 1;
    } else {
        prev_offset = curr_offset - 1;
    }

    for (i = starting_index; i < in->segment_length; i++, curr_offset++, prev_offset++) {
        u64 pseudo_rand;
        u32 ref_lane;
        u32 ref_index;
        argon2_block *ref_block;
        argon2_block *curr_block;

        if (curr_offset % in->lane_length == 1) {
            prev_offset = curr_offset - 1;
        }
        if (data_independent) {
            if (i % ARGON2_ADDRESSES_IN_BLOCK == 0) {
                next_addresses(&address, &input, &zero);
            }
            pseudo_rand = address.v[i % ARGON2_ADDRESSES_IN_BLOCK];
        } else {
            pseudo_rand = in->memory[prev_offset].v[0];
        }
        ref_lane = (u32)((pseudo_rand >> 32) % in->lanes);
        if (pos.pass == 0 && pos.slice == 0) {
            ref_lane = pos.lane;
        }
        pos.index = i;
        ref_index = index_alpha(in, &pos, (u32)(pseudo_rand & 0xFFFFFFFFu),
                                ref_lane == pos.lane);
        ref_block = &in->memory[(u64)in->lane_length * ref_lane + ref_index];
        curr_block = &in->memory[curr_offset];
        if (pos.pass == 0) {
            fill_block(&in->memory[prev_offset], ref_block, curr_block, 0);
        } else {
            fill_block(&in->memory[prev_offset], ref_block, curr_block, 1);
        }
    }
}

static int blake2b_long(u8 *out, usize outlen, const void *in, usize inlen)
{
    u8 outlen_bytes[4];
    u8 buffer[64];

    if (outlen == 0 || outlen > 0xFFFFFFFFull) {
        return -1;
    }
    store32(outlen_bytes, (u32)outlen);
    if (outlen <= 64) {
        struct blake2b_ctx ctx;

        blake2b_init(&ctx, outlen);
        blake2b_update(&ctx, outlen_bytes, sizeof(outlen_bytes));
        blake2b_update(&ctx, in, inlen);
        blake2b_final(&ctx, out);
        return 0;
    }
    {
        struct blake2b_ctx ctx;
        usize produced = 32;
        usize off = 32;

        blake2b_init(&ctx, 64);
        blake2b_update(&ctx, outlen_bytes, sizeof(outlen_bytes));
        blake2b_update(&ctx, in, inlen);
        blake2b_final(&ctx, buffer);
        memcpy(out, buffer, 32);
        while (outlen - produced > 64) {
            u8 next[64];

            blake2b_hash(next, 64, 0, 0, buffer, 64);
            memcpy(buffer, next, 64);
            memcpy(out + off, buffer, 32);
            off += 32;
            produced += 32;
        }
        {
            u8 last[64];
            usize remain = outlen - produced;

            blake2b_hash(last, remain, 0, 0, buffer, 64);
            memcpy(out + off, last, remain);
        }
    }
    return 0;
}

static int argon2id_core(u8 *out, usize outlen, const u8 *pwd, usize pwdlen,
                         const u8 *salt, usize saltlen, u32 m_cost_kib, u32 t_cost,
                         u32 lanes, argon2_block *memory, u32 memory_blocks)
{
    u8 h0[ARGON2_PREHASH_DIGEST];
    u8 seed[ARGON2_PREHASH_SEED];
    u8 block_bytes[ARGON2_BLOCK_BYTES];
    argon2_block blockhash;
    struct blake2b_ctx ctx;
    u32 segment_length;
    u32 lane_length;
    u32 l;
    u32 p;
    int i;

    segment_length = memory_blocks / (lanes * ARGON2_SYNC_POINTS);
    if (segment_length == 0) {
        return -2;
    }
    memory_blocks = segment_length * lanes * ARGON2_SYNC_POINTS;
    lane_length = segment_length * ARGON2_SYNC_POINTS;

    blake2b_init(&ctx, ARGON2_PREHASH_DIGEST);
    {
        u8 v[4];

        store32(v, lanes); blake2b_update(&ctx, v, 4);
        store32(v, (u32)outlen); blake2b_update(&ctx, v, 4);
        store32(v, m_cost_kib); blake2b_update(&ctx, v, 4);
        store32(v, t_cost); blake2b_update(&ctx, v, 4);
        store32(v, ARGON2_VERSION_13); blake2b_update(&ctx, v, 4);
        store32(v, ARGON2_TYPE_ID); blake2b_update(&ctx, v, 4);
        store32(v, (u32)pwdlen); blake2b_update(&ctx, v, 4);
    }
    blake2b_update(&ctx, pwd, pwdlen);
    {
        u8 v[4];

        store32(v, (u32)saltlen); blake2b_update(&ctx, v, 4);
    }
    blake2b_update(&ctx, salt, saltlen);
    {
        u8 zero4[4];

        memset(zero4, 0, sizeof(zero4));
        blake2b_update(&ctx, zero4, 4);
        blake2b_update(&ctx, zero4, 4);
    }
    blake2b_final(&ctx, h0);

    memcpy(seed, h0, sizeof(h0));
    for (l = 0; l < lanes; l++) {
        int j;

        for (j = 0; j < 2; j++) {
            store32(seed + 64, (u32)j);
            store32(seed + 68, l);
            blake2b_long(block_bytes, ARGON2_BLOCK_BYTES, seed, ARGON2_PREHASH_SEED);
            for (i = 0; i < (int)ARGON2_BLOCK_WORDS; i++) {
                memory[(u64)l * lane_length + j].v[i] = load64(block_bytes + 8 * i);
            }
        }
    }

    for (p = 0; p < t_cost; p++) {
        u32 s;

        for (s = 0; s < ARGON2_SYNC_POINTS; s++) {
            argon2_instance inst;

            inst.memory = memory;
            inst.memory_blocks = memory_blocks;
            inst.passes = t_cost;
            inst.lanes = lanes;
            inst.segment_length = segment_length;
            inst.lane_length = lane_length;
            for (l = 0; l < lanes; l++) {
                argon2_position pos;

                pos.pass = p;
                pos.lane = l;
                pos.slice = s;
                pos.index = 0;
                fill_segment(&inst, pos);
            }
        }
    }

    memcpy(&blockhash, &memory[lane_length - 1], sizeof(blockhash));
    for (l = 1; l < lanes; l++) {
        const argon2_block *last = &memory[(u64)l * lane_length + lane_length - 1];

        for (i = 0; i < (int)ARGON2_BLOCK_WORDS; i++) {
            blockhash.v[i] ^= last->v[i];
        }
    }
    for (i = 0; i < (int)ARGON2_BLOCK_WORDS; i++) {
        store64(block_bytes + 8 * i, blockhash.v[i]);
    }
    blake2b_long(out, outlen, block_bytes, sizeof(block_bytes));
    memset(&blockhash, 0, sizeof(blockhash));
    memset(block_bytes, 0, sizeof(block_bytes));
    memset(h0, 0, sizeof(h0));
    memset(seed, 0, sizeof(seed));
    return 0;
}

int argon2id_hash_mem(u8 *out, usize outlen, const u8 *pwd, usize pwdlen, const u8 *salt,
                      usize saltlen, u32 m_cost_kib, u32 t_cost, u32 lanes, void *mem,
                      usize mem_bytes)
{
    u32 memory_blocks;
    u32 segment_length;
    usize need;

    if (!out || !mem || outlen == 0 || outlen > ARGON2_MAX_OUT || lanes == 0 ||
        lanes > ARGON2_MAX_LANES || t_cost == 0 || m_cost_kib < 2 * ARGON2_SYNC_POINTS * lanes) {
        return -1;
    }
    if (pwdlen && !pwd) {
        return -1;
    }
    if (saltlen && !salt) {
        return -1;
    }
    memory_blocks = m_cost_kib;
    segment_length = memory_blocks / (lanes * ARGON2_SYNC_POINTS);
    memory_blocks = segment_length * lanes * ARGON2_SYNC_POINTS;
    need = (usize)memory_blocks * sizeof(argon2_block);
    if (mem_bytes < need) {
        return -1;
    }
    memset(mem, 0, need);
    return argon2id_core(out, outlen, pwd, pwdlen, salt, saltlen, m_cost_kib, t_cost, lanes,
                         (argon2_block *)mem, memory_blocks);
}

int argon2id_hash(u8 *out, usize outlen, const u8 *pwd, usize pwdlen, const u8 *salt,
                  usize saltlen, u32 m_cost_kib, u32 t_cost, u32 lanes)
{
    u32 segment_length;
    u32 memory_blocks;
    u64 pages;
    u64 pa;
    int rc;

    if (lanes == 0 || lanes > ARGON2_MAX_LANES || t_cost == 0) {
        return -1;
    }
    segment_length = m_cost_kib / (lanes * ARGON2_SYNC_POINTS);
    memory_blocks = segment_length * lanes * ARGON2_SYNC_POINTS;
    pages = ((u64)memory_blocks * ARGON2_BLOCK_BYTES + CRUG_PAGE_SIZE - 1) / CRUG_PAGE_SIZE;
    pa = pmm_alloc_pages(pages);
    if (pa == 0) {
        return -3;
    }
    rc = argon2id_hash_mem(out, outlen, pwd, pwdlen, salt, saltlen, m_cost_kib, t_cost, lanes,
                           pmm_phys_to_virt(pa), (usize)(pages * CRUG_PAGE_SIZE));
    pmm_free_pages(pa, pages);
    return rc;
}