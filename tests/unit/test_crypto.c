/* Test vectors for the crypto primitives, host side: SHA-1/256/384/512 and HMAC (FIPS 180-4, RFC 4231), AES-GCM
 * (McGrew and Viega test cases 1-4 and 16), AES-CTR (SP 800-38A F.5.1), ChaCha20 and Poly1305 (RFC 8439 2.4.2, 2.5.2),
 * ChaCha20-Poly1305 (RFC 8439 2.8.2), X25519 (RFC 7748 5.2 and 6.1), Ed25519 (RFC 8032 7.1). The expected values were
 * also computed with an independent implementation (OpenSSL via the Python cryptography package and hashlib) while
 * writing this test. ECDSA and RSA are not on the host (NEMA_ERR_UNSUPPORTED): their tests check that refusal only. */
#include <stdio.h>
#include <string.h>

#include "../../system/src/api_impl.h"
#include "../../system/src/cryptoimpl.h"
#include "../../system/src/crypto_mac.h"
#include "../../system/src/crypto_prim.h"
#include "../../third_party/monocypher/monocypher.h"
#include "nema/nema.h"
#include "test.h"

static size_t unhex(const char *hex, uint8_t *out, size_t cap) {
    size_t n = strlen(hex) / 2;
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

/* True when the bytes of buf equal the hex string. */
static int eq_hex(const uint8_t *buf, size_t n, const char *hex) {
    uint8_t want[256];
    size_t m = unhex(hex, want, sizeof(want));
    return m == n && memcmp(buf, want, n) == 0;
}

static api_ctx_t *NO_CTX = NULL; /* a built-in caller: the kernel's table */

/* ---- digests ---- */

static void digest(int alg, const char *msg, const char *hex) {
    crypto_mac_t m;
    uint8_t out[64];
    CHECK_EQ(crypto_mac_hash(&m, alg), NEMA_OK);
    crypto_mac_update(&m, msg, strlen(msg));
    size_t n = crypto_mac_final(&m, out);
    CHECK(eq_hex(out, n, hex));
}

TEST(sha1_fips_vectors) {
    digest(CI_HASH_SHA1, "abc", "a9993e364706816aba3e25717850c26c9cd0d89d");
    digest(CI_HASH_SHA1, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    digest(CI_HASH_SHA1, "", "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST(sha256_fips_vectors) {
    digest(CI_HASH_SHA256, "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    digest(CI_HASH_SHA256, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(sha384_and_sha512_fips_vectors) {
    digest(CI_HASH_SHA384, "abc", "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
    digest(CI_HASH_SHA384, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
           "3391fdddfc8dc7393707a65b1b4709397cf8b1d162af05abfe8f450de5f36bc6b0455a8520bc4e6f5fe95b1fe3c8452b");
    digest(CI_HASH_SHA512, "abc", "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    digest(CI_HASH_SHA512, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
           "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c33596fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445");
}

/* Feeding the input in pieces of every length gives the same digest as one call (block boundaries are the risk). */
TEST(digests_do_not_depend_on_how_input_is_split) {
    uint8_t data[300];
    for (int i = 0; i < 300; i++) data[i] = (uint8_t)(i * 7 + 1);
    for (int alg = CI_HASH_SHA1; alg <= CI_HASH_SHA512; alg++) {
        uint8_t one[64], many[64];
        crypto_mac_t a, b;
        crypto_mac_hash(&a, alg);
        crypto_mac_update(&a, data, sizeof(data));
        size_t n = crypto_mac_final(&a, one);
        crypto_mac_hash(&b, alg);
        for (size_t off = 0, step = 1; off < sizeof(data); step = step % 129 + 1) {
            size_t take = off + step > sizeof(data) ? sizeof(data) - off : step;
            crypto_mac_update(&b, data + off, take);
            off += take;
        }
        CHECK_EQ(crypto_mac_final(&b, many), n);
        CHECK(memcmp(one, many, n) == 0);
    }
}

TEST(hmac_rfc4231_vectors) {
    crypto_mac_t m;
    uint8_t out[64];
    uint8_t key20[20];
    memset(key20, 0x0b, sizeof(key20));
    CHECK_EQ(crypto_mac_hmac(&m, CI_HASH_SHA256, key20, sizeof(key20)), NEMA_OK);
    crypto_mac_update(&m, "Hi There", 8);
    CHECK(eq_hex(out, crypto_mac_final(&m, out), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    CHECK_EQ(crypto_mac_hmac(&m, CI_HASH_SHA384, key20, sizeof(key20)), NEMA_OK);
    crypto_mac_update(&m, "Hi There", 8);
    CHECK(eq_hex(out, crypto_mac_final(&m, out), "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59cfaea9ea9076ede7f4af152e8b2fa9cb6"));
    CHECK_EQ(crypto_mac_hmac(&m, CI_HASH_SHA512, key20, sizeof(key20)), NEMA_OK);
    crypto_mac_update(&m, "Hi There", 8);
    CHECK(eq_hex(out, crypto_mac_final(&m, out), "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854"));
    CHECK_EQ(crypto_mac_hmac(&m, CI_HASH_SHA256, (const uint8_t *)"Jefe", 4), NEMA_OK);
    crypto_mac_update(&m, "what do ya want for nothing?", 28);
    CHECK(eq_hex(out, crypto_mac_final(&m, out), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    CHECK_EQ(crypto_mac_hmac(&m, CI_HASH_SHA512, (const uint8_t *)"Jefe", 4), NEMA_OK);
    crypto_mac_update(&m, "what do ya want for nothing?", 28);
    CHECK(eq_hex(out, crypto_mac_final(&m, out), "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737"));
    /* RFC 4231 test case 6: a 131-byte key is hashed before use (longer than the 64-byte block). */
    uint8_t key131[131];
    memset(key131, 0xaa, sizeof(key131));
    CHECK_EQ(crypto_mac_hmac(&m, CI_HASH_SHA256, key131, sizeof(key131)), NEMA_OK);
    crypto_mac_update(&m, "Test Using Larger Than Block-Size Key - Hash Key First", 54);
    CHECK(eq_hex(out, crypto_mac_final(&m, out), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

/* ---- AES-GCM and AES-CTR ---- */

static int gcm_seal_check(const char *key, const char *iv, const char *pt, const char *aad, const char *ct, const char *tag) {
    uint8_t k[32], n[12], p[256], a[64], c[256], t[16];
    size_t kl = unhex(key, k, sizeof(k)), pl = unhex(pt, p, sizeof(p)), al = unhex(aad, a, sizeof(a));
    unhex(iv, n, sizeof(n));
    ci_cipher_t cipher;
    if (ci_gcm_init(&cipher, k, kl) != NEMA_OK) return 0;
    memcpy(c, p, pl);
    if (ci_gcm_seal(&cipher, n, a, al, c, pl, t) != NEMA_OK) return 0;
    if (!eq_hex(c, pl, ct) || !eq_hex(t, 16, tag)) return 0;
    /* and the same call opens it back */
    if (ci_gcm_open(&cipher, n, a, al, c, pl, t) != NEMA_OK) return 0;
    return eq_hex(c, pl, pt);
}

TEST(aes128_gcm_mcgrew_viega_test_cases_1_to_4) {
    CHECK(gcm_seal_check("00000000000000000000000000000000", "000000000000000000000000", "", "", "", "58e2fccefa7e3061367f1d57a4e7455a"));
    CHECK(gcm_seal_check("00000000000000000000000000000000", "000000000000000000000000", "00000000000000000000000000000000", "",
                         "0388dace60b6a392f328c2b971b2fe78", "ab6e47d42cec13bdf53a67b21257bddf"));
    const char *p3 = "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255";
    const char *c3 = "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985";
    CHECK(gcm_seal_check("feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888", p3, "", c3, "4d5c2af327cd64a62cf35abd2ba6fab4"));
    /* test case 4: the first 60 bytes of the plaintext of test case 3, with 20 bytes of AAD */
    CHECK(gcm_seal_check("feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888", "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
                         "feedfacedeadbeeffeedfacedeadbeefabaddad2",
                         "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091",
                         "5bc94fbc3221a5db94fae95ae7121a47"));
}

TEST(aes256_gcm_zero_key_and_mcgrew_test_case_16) {
    CHECK(gcm_seal_check("0000000000000000000000000000000000000000000000000000000000000000", "000000000000000000000000", "", "", "",
                         "530f8afbc74536b9a963b4f1c4cb738b"));
    CHECK(gcm_seal_check("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
                         "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
                         "feedfacedeadbeeffeedfacedeadbeefabaddad2",
                         "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
                         "76fc6ece0f4e1768cddf8853bb2d551b"));
}

TEST(gcm_refuses_a_tampered_tag_and_leaves_the_buffer_alone) {
    uint8_t k[16] = {0}, n[12] = {0}, buf[8] = {1, 2, 3, 4, 5, 6, 7, 8}, tag[16] = {0};
    ci_cipher_t c;
    CHECK_EQ(ci_gcm_init(&c, k, 16), NEMA_OK);
    CHECK_EQ(ci_gcm_seal(&c, n, NULL, 0, buf, 8, tag), NEMA_OK);
    uint8_t before[8];
    memcpy(before, buf, 8);
    tag[3] ^= 1;
    CHECK_EQ(ci_gcm_open(&c, n, NULL, 0, buf, 8, tag), NEMA_ERR_AUTH);
    CHECK(memcmp(buf, before, 8) == 0);
    CHECK_EQ(ci_gcm_init(&c, k, 17), NEMA_ERR_INVAL);
}

TEST(aes_ctr_sp800_38a_f51) {
    uint8_t key[16], iv[16], buf[16];
    unhex("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
    unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv, 16);
    unhex("6bc1bee22e409f96e93d7e117393172a", buf, 16);
    ci_cipher_t c;
    CHECK_EQ(ci_ctr_init(&c, key, 16, iv), NEMA_OK);
    ci_ctr_xor(&c, buf, 16);
    CHECK(eq_hex(buf, 16, "874d6191b620e3261bef6864990db6ce"));
    /* the stream continues across calls: 1 byte, then the rest, is the same as one call */
    uint8_t a[20], b[20];
    unhex("00112233445566778899aabbccddeeff00112233", a, 20);
    memcpy(b, a, 20);
    CHECK_EQ(ci_ctr_init(&c, key, 16, iv), NEMA_OK);
    ci_ctr_xor(&c, a, 1);
    ci_ctr_xor(&c, a + 1, 19);
    CHECK_EQ(ci_ctr_init(&c, key, 16, iv), NEMA_OK);
    ci_ctr_xor(&c, b, 20);
    CHECK(memcmp(a, b, 20) == 0);
}

/* ---- ChaCha20, Poly1305, ChaCha20-Poly1305 ---- */

TEST(chacha20_rfc8439_2_4_2_keystream_and_round_trip) {
    uint8_t key[32], nonce[12], pt[114], out[114], back[114];
    unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, 32);
    unhex("000000000000004a00000000", nonce, 12);
    memcpy(pt, "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.", 114);
    CHECK_EQ(prim_chacha20_xor(key, nonce, 12, 1, pt, out, 114), NEMA_OK);
    CHECK(eq_hex(out, 16, "6e2e359a2568f98041ba0728dd0d6981"));
    CHECK_EQ(prim_chacha20_xor(key, nonce, 12, 1, out, back, 114), NEMA_OK);
    CHECK(memcmp(back, pt, 114) == 0);
    CHECK_EQ(prim_chacha20_xor(key, nonce, 11, 1, pt, out, 114), NEMA_ERR_INVAL);
    CHECK_EQ(prim_chacha20_xor(key, nonce, 12, 1ull << 32, pt, out, 114), NEMA_ERR_RANGE);
}

TEST(poly1305_rfc8439_2_5_2) {
    uint8_t key[32], tag[16];
    unhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key, 32);
    prim_poly1305(key, (const uint8_t *)"Cryptographic Forum Research Group", 34, tag);
    CHECK(eq_hex(tag, 16, "a8061dc1305136c6c22b8baf0c0127a9"));
}

TEST(chacha20_poly1305_aead_rfc8439_2_8_2) {
    uint8_t key[32], nonce[12], aad[12], buf[114], tag[16];
    unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7", key, 32);
    unhex("070000004041424344454647", nonce, 12);
    unhex("50515253c0c1c2c3c4c5c6c7", aad, 12);
    memcpy(buf, "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.", 114);
    prim_aead_seal(key, nonce, aad, 12, buf, 114, tag);
    CHECK(eq_hex(buf, 16, "d31a8d34648e60db7b86afbc53ef7ec2"));
    CHECK(eq_hex(tag, 16, "1ae10b594f09e26a7e902ecbd0600691"));
    CHECK_EQ(prim_aead_open(key, nonce, aad, 12, buf, 114, tag), NEMA_OK);
    CHECK(memcmp(buf, "Ladies and", 10) == 0);
    /* a wrong tag: refused, and the buffer stays as it was */
    prim_aead_seal(key, nonce, aad, 12, buf, 114, tag);
    tag[0] ^= 0x80;
    uint8_t before[114];
    memcpy(before, buf, 114);
    CHECK_EQ(prim_aead_open(key, nonce, aad, 12, buf, 114, tag), NEMA_ERR_AUTH);
    CHECK(memcmp(buf, before, 114) == 0);
}

/* ---- X25519 and Ed25519 ---- */

TEST(x25519_rfc7748_5_2_vectors) {
    uint8_t sk[32], pt[32], out[32];
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", sk, 32);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", pt, 32);
    CHECK_EQ(prim_x25519(out, sk, pt), NEMA_OK);
    CHECK(eq_hex(out, 32, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));
    unhex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", sk, 32);
    unhex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", pt, 32);
    CHECK_EQ(prim_x25519(out, sk, pt), NEMA_OK);
    CHECK(eq_hex(out, 32, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"));
}

TEST(x25519_diffie_hellman_rfc7748_6_1) {
    uint8_t a[32], b[32], pa[32], pb[32], sa[32], sb[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, 32);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, 32);
    prim_x25519_base(pa, a);
    prim_x25519_base(pb, b);
    CHECK(eq_hex(pa, 32, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
    CHECK(eq_hex(pb, 32, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
    CHECK_EQ(prim_x25519(sa, a, pb), NEMA_OK);
    CHECK_EQ(prim_x25519(sb, b, pa), NEMA_OK);
    CHECK(memcmp(sa, sb, 32) == 0);
    CHECK(eq_hex(sa, 32, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
    /* the all-zero point is a low-order point: its shared secret is refused */
    uint8_t zero[32] = {0};
    CHECK_EQ(prim_x25519(sa, a, zero), NEMA_ERR_INVAL);
}

TEST(ed25519_rfc8032_7_1_sign_and_verify) {
    uint8_t seed[32], sk[64], pk[32], sig[64];
    unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed, 32);
    prim_ed25519_keypair(sk, pk, seed);
    CHECK(eq_hex(pk, 32, "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"));
    prim_ed25519_sign(sig, sk, (const uint8_t *)"", 0);
    CHECK(eq_hex(sig, 64, "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"));
    CHECK_EQ(prim_ed25519_verify(pk, (const uint8_t *)"", 0, sig), NEMA_OK);
    sig[63] ^= 1;
    CHECK_EQ(prim_ed25519_verify(pk, (const uint8_t *)"", 0, sig), NEMA_ERR_AUTH);
    /* test 2: one-byte message */
    unhex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", seed, 32);
    prim_ed25519_keypair(sk, pk, seed);
    CHECK(eq_hex(pk, 32, "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"));
    uint8_t msg[1] = {0x72};
    prim_ed25519_sign(sig, sk, msg, 1);
    CHECK(eq_hex(sig, 64, "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"));
    CHECK_EQ(prim_ed25519_verify(pk, msg, 1, sig), NEMA_OK);
    CHECK_EQ(prim_ed25519_verify(pk, msg, 0, sig), NEMA_ERR_AUTH);
}

/* ---- the refusals of the host build ---- */

TEST(ecdsa_and_rsa_are_refused_on_the_host) {
    uint8_t pub[64] = {0}, hash[32] = {0}, sig[64] = {0}, n[8] = {1}, e[1] = {3}, priv[32] = {1}, out[64];
    CHECK_EQ(ci_ecdsa_p256_verify(pub, hash, 32, sig), NEMA_ERR_UNSUPPORTED);
    CHECK_EQ(ci_ecdsa_p256_generate(priv, out), NEMA_ERR_UNSUPPORTED);
    CHECK_EQ(ci_rsa_verify_pkcs1(n, 8, e, 1, CI_HASH_SHA256, hash, 32, sig, 8), NEMA_ERR_UNSUPPORTED);
}

/* ---- the API functions: lengths are checked, handles are checked ---- */

TEST(api_checks_lengths_and_handles) {
    uint8_t k[32] = {0}, out[64], pk[32] = {0};
    CHECK_EQ(psys_api_x25519_base(NO_CTX, out, 31, k, 32), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_x25519_base(NO_CTX, out, 32, k, 32), NEMA_OK);
    CHECK_EQ(psys_api_hash_open(NO_CTX, 0), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_hash_open(NO_CTX, 5), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_hash_update(NO_CTX, 99, out, 1), NEMA_ERR_BADF);
    CHECK_EQ(psys_api_aead_open(NO_CTX, CI_AEAD_AES128_GCM, k, 32), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_aead_open(NO_CTX, 9, k, 32), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_ed25519_verify(NO_CTX, pk, 31, out, 0, out, 64), NEMA_ERR_INVAL);
    CHECK_EQ(psys_api_ecdsa_p256_verify(NO_CTX, out, 64, out, 31, out, 64), NEMA_ERR_INVAL);
}

TEST(api_hash_handles_are_limited_to_eight_and_freed_by_final) {
    int32_t h[8];
    for (int i = 0; i < 8; i++) {
        h[i] = psys_api_hash_open(NO_CTX, CI_HASH_SHA256);
        CHECK(h[i] >= 0);
    }
    CHECK_EQ(psys_api_hash_open(NO_CTX, CI_HASH_SHA256), NEMA_ERR_FULL);
    uint8_t out[32];
    CHECK_EQ(psys_api_hash_final(NO_CTX, h[3], out, 31), NEMA_ERR_RANGE); /* too small: nothing is freed */
    CHECK_EQ(psys_api_hash_final(NO_CTX, h[3], out, 32), 32);
    CHECK_EQ(psys_api_hash_final(NO_CTX, h[3], out, 32), NEMA_ERR_BADF);
    CHECK(psys_api_hash_open(NO_CTX, CI_HASH_SHA256) >= 0);
    for (int i = 0; i < 8; i++)
        if (i != 3) CHECK_EQ(psys_api_hash_final(NO_CTX, h[i], out, 32), 32);
}

TEST(api_aead_round_trip_and_tag_refusal) {
    uint8_t key[32], nonce[12] = {0}, aad[4] = {1, 2, 3, 4}, buf[40], tag[16];
    memset(key, 7, sizeof(key));
    memset(buf, 9, sizeof(buf));
    int32_t h = psys_api_aead_open(NO_CTX, CI_AEAD_CHACHA20_POLY1305, key, 32);
    CHECK(h >= 0);
    CHECK_EQ(psys_api_aead_seal(NO_CTX, h, nonce, 12, aad, 4, buf, 40, tag, 16), NEMA_OK);
    uint8_t enc[40];
    memcpy(enc, buf, 40);
    tag[15] ^= 1;
    CHECK_EQ(psys_api_aead_decrypt(NO_CTX, h, nonce, 12, aad, 4, buf, 40, tag, 16), NEMA_ERR_AUTH);
    CHECK(memcmp(buf, enc, 40) == 0);
    tag[15] ^= 1;
    CHECK_EQ(psys_api_aead_decrypt(NO_CTX, h, nonce, 12, aad, 4, buf, 40, tag, 16), NEMA_OK);
    uint8_t want[40];
    memset(want, 9, sizeof(want));
    CHECK(memcmp(buf, want, 40) == 0);
    /* AES-256-GCM through the same handles */
    int32_t g = psys_api_aead_open(NO_CTX, CI_AEAD_AES256_GCM, key, 32);
    CHECK(g >= 0);
    CHECK_EQ(psys_api_aead_seal(NO_CTX, g, nonce, 12, aad, 4, buf, 40, tag, 16), NEMA_OK);
    CHECK_EQ(psys_api_aead_decrypt(NO_CTX, g, nonce, 12, aad, 4, buf, 40, tag, 16), NEMA_OK);
    CHECK(memcmp(buf, want, 40) == 0);
    CHECK_EQ(psys_api_cipher_xor(NO_CTX, h, buf, 4), NEMA_ERR_BADF); /* an AEAD handle is not a stream */
}

int main(void) {
    nema_config_t cfg = {.console_log = false};
    nema_init(&cfg);
    RUN(sha1_fips_vectors);
    RUN(sha256_fips_vectors);
    RUN(sha384_and_sha512_fips_vectors);
    RUN(digests_do_not_depend_on_how_input_is_split);
    RUN(hmac_rfc4231_vectors);
    RUN(aes128_gcm_mcgrew_viega_test_cases_1_to_4);
    RUN(aes256_gcm_zero_key_and_mcgrew_test_case_16);
    RUN(gcm_refuses_a_tampered_tag_and_leaves_the_buffer_alone);
    RUN(aes_ctr_sp800_38a_f51);
    RUN(chacha20_rfc8439_2_4_2_keystream_and_round_trip);
    RUN(poly1305_rfc8439_2_5_2);
    RUN(chacha20_poly1305_aead_rfc8439_2_8_2);
    RUN(x25519_rfc7748_5_2_vectors);
    RUN(x25519_diffie_hellman_rfc7748_6_1);
    RUN(ed25519_rfc8032_7_1_sign_and_verify);
    RUN(ecdsa_and_rsa_are_refused_on_the_host);
    RUN(api_checks_lengths_and_handles);
    RUN(api_hash_handles_are_limited_to_eight_and_freed_by_final);
    RUN(api_aead_round_trip_and_tag_refusal);
    return TEST_DONE();
}
