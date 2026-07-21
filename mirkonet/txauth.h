#pragma once
#include <Preferences.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/entropy.h>
#include <mbedtls/private_access.h>
#include "types.h"

class TxAuth {
public:
    static bool begin(Preferences& prefs, NodeID& outId) {
        mbedtls_ecdsa_init(&_ctx);
        mbedtls_entropy_init(&_entropy);
        mbedtls_ctr_drbg_init(&_ctrDrbg);
        
        const char* pers = "mirkonet_txauth";
        if (mbedtls_ctr_drbg_seed(&_ctrDrbg, mbedtls_entropy_func, &_entropy,
                                  (const unsigned char*)pers, strlen(pers)) != 0)
            return false;

        if (mbedtls_ecp_group_load(&_ctx.MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1) != 0)
            return false;

        uint8_t priv[32];
        bool haveKey = prefs.getBytesLength("txpriv") == sizeof(priv);
        if (haveKey) {
            prefs.getBytes("txpriv", priv, sizeof(priv));
            if (mbedtls_mpi_read_binary(&_ctx.MBEDTLS_PRIVATE(d), priv, sizeof(priv)) != 0)
                return false;
            if (mbedtls_ecp_mul(&_ctx.MBEDTLS_PRIVATE(grp), &_ctx.MBEDTLS_PRIVATE(Q),
                                &_ctx.MBEDTLS_PRIVATE(d), &_ctx.MBEDTLS_PRIVATE(grp).G,
                                mbedtls_ctr_drbg_random, &_ctrDrbg) != 0)
                return false;
        } else {
            if (mbedtls_ecdsa_genkey(&_ctx, MBEDTLS_ECP_DP_SECP256R1,
                                     mbedtls_ctr_drbg_random, &_ctrDrbg) != 0)
                return false;
            if (mbedtls_mpi_write_binary(&_ctx.MBEDTLS_PRIVATE(d), priv, sizeof(priv)) != 0)
                return false;
            prefs.putBytes("txpriv", priv, sizeof(priv));
        }

        if (!exportPublic(_pubKey)) return false;
        outId = nodeIdFromPublicKey(_pubKey);
        _ready = true;
        return true;
    }

    static bool sign(Transaction& tx) {
        if (!_ready) return false;
        memcpy(tx.publicKey, _pubKey, sizeof(tx.publicKey));

        Hash256 h = tx.signingHash();
        return signHash(h, tx.publicKey, tx.signature);
    }

    static bool signHash(const Hash256& h, uint8_t publicKey[64],
                         uint8_t signature[64]) {
        if (!_ready) return false;
        memcpy(publicKey, _pubKey, 64);

        mbedtls_mpi r, s;
        mbedtls_mpi_init(&r);
        mbedtls_mpi_init(&s);
        int rc = mbedtls_ecdsa_sign(&_ctx.MBEDTLS_PRIVATE(grp), &r, &s,
                                    &_ctx.MBEDTLS_PRIVATE(d),
                                    h.bytes, HASH_SIZE,
                                    mbedtls_ctr_drbg_random, &_ctrDrbg);
        if (rc == 0) {
            memset(signature, 0, 64);
            rc = mbedtls_mpi_write_binary(&r, signature, 32);
            if (rc == 0)
                rc = mbedtls_mpi_write_binary(&s, signature + 32, 32);
        }
        mbedtls_mpi_free(&r);
        mbedtls_mpi_free(&s);
        return rc == 0;
    }

    static bool verify(const Transaction& tx) {
        if (!tx.hasSignature()) return false;
        Hash256 h = tx.signingHash();
        return verifyHash(h, tx.publicKey, tx.signature, tx.sender);
    }

    static bool verifyHash(const Hash256& h, const uint8_t publicKey[64],
                           const uint8_t signature[64], const NodeID& signer) {
        bool hasSig = false;
        for (int i = 0; i < 64; i++) {
            if (signature[i]) { hasSig = true; break; }
        }
        if (!hasSig) return false;
        if (nodeIdFromPublicKey(publicKey) != signer) return false;

        mbedtls_ecdsa_context ctx;
        mbedtls_ecdsa_init(&ctx);
        bool ok = false;
        if (mbedtls_ecp_group_load(&ctx.MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_mpi_read_binary(&ctx.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X), publicKey, 32) == 0 &&
            mbedtls_mpi_read_binary(&ctx.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y), publicKey + 32, 32) == 0 &&
            mbedtls_mpi_lset(&ctx.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Z), 1) == 0) {
            mbedtls_mpi r, s;
            mbedtls_mpi_init(&r);
            mbedtls_mpi_init(&s);
            if (mbedtls_mpi_read_binary(&r, signature, 32) == 0 &&
                mbedtls_mpi_read_binary(&s, signature + 32, 32) == 0 &&
                mbedtls_ecdsa_verify(&ctx.MBEDTLS_PRIVATE(grp), h.bytes, HASH_SIZE,
                                      &ctx.MBEDTLS_PRIVATE(Q), &r, &s) == 0) {
                ok = true;
            }
            mbedtls_mpi_free(&r);
            mbedtls_mpi_free(&s);
        }
        mbedtls_ecdsa_free(&ctx);
        return ok;
    }

    static NodeID nodeIdFromPublicKey(const uint8_t pubKey[64]) {
        Hash256 h = sha256(pubKey, 64);
        NodeID id;
        memcpy(id.id, h.bytes, 6);
        return id;
    }

private:
    static bool exportPublic(uint8_t out[64]) {
        return mbedtls_mpi_write_binary(&_ctx.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X), out, 32) == 0 &&
               mbedtls_mpi_write_binary(&_ctx.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y), out + 32, 32) == 0;
    }

    static mbedtls_ecdsa_context _ctx;
    static mbedtls_entropy_context _entropy;
    static mbedtls_ctr_drbg_context _ctrDrbg;
    static uint8_t _pubKey[64];
    static bool _ready;
};

mbedtls_ecdsa_context TxAuth::_ctx;
mbedtls_entropy_context TxAuth::_entropy;
mbedtls_ctr_drbg_context TxAuth::_ctrDrbg;
uint8_t TxAuth::_pubKey[64];
bool TxAuth::_ready = false;
