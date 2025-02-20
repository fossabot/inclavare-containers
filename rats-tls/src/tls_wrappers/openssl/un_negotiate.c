/* Copyright (c) 2021 Intel Corporation
 * Copyright (c) 2020-2021 Alibaba Cloud
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include <string.h>
#include <assert.h>
#include <rats-tls/log.h>
#include <rats-tls/err.h>
#include <rats-tls/tls_wrapper.h>
#include <rats-tls/oid.h>
#include <internal/core.h>
#include "sgx_report.h"
#include "sgx_quote_3.h"
#include "per_thread.h"
#include "openssl.h"

static int rtls_memcpy_s(void *dst, uint32_t dst_size, const void *src, uint32_t num_bytes)
{
	int result = 0;

	if (dst == NULL) {
		RTLS_ERR("dst parameter is null pointer!\n");
		goto done;
	}

	if (src == NULL || dst_size < num_bytes) {
		RTLS_ERR("invalid parameters found!\n");
		goto done;
	}

	if ((dst >= src && ((uint8_t *)dst < (uint8_t *)src + num_bytes)) ||
	    (dst < src && ((uint8_t *)dst + dst_size > (uint8_t *)src))) {
		RTLS_ERR("there is overlapping copy here!\n");
		goto done;
	}

	memcpy(dst, src, num_bytes);
	result = 1;

done:
	return result;
}

static crypto_wrapper_err_t sha256_ecc_pubkey(unsigned char hash[SHA256_HASH_SIZE], EC_KEY *key)
{
	int len = i2d_EC_PUBKEY(key, NULL);
	unsigned char buf[len];
	unsigned char *p = buf;

	len = i2d_EC_PUBKEY(key, &p);

	SHA256(buf, len, hash);

	// clang-format off
	RTLS_DEBUG("the sha256 of public key [%d] %02x%02x%02x%02x%02x%02x%02x%02x...%02x%02x%02x%02x\n",
		len, hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7],
		hash[SHA256_HASH_SIZE - 4], hash[SHA256_HASH_SIZE - 3], hash[SHA256_HASH_SIZE - 2],
		hash[SHA256_HASH_SIZE - 1]);
	// clang-format on

	return CRYPTO_WRAPPER_ERR_NONE;
}

static crypto_wrapper_err_t sha256_rsa_pubkey(unsigned char hash[SHA256_HASH_SIZE], RSA *key)
{
	int len = i2d_RSAPublicKey(key, NULL);

	unsigned char buf[len];
	unsigned char *p = buf;
	len = i2d_RSAPublicKey(key, &p);

	SHA256(buf, len, hash);

	// clang-format off
	RTLS_DEBUG("the sha256 of public key [%d] %02x%02x%02x%02x%02x%02x%02x%02x...%02x%02x%02x%02x\n",
		len, hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7],
		hash[SHA256_HASH_SIZE - 4], hash[SHA256_HASH_SIZE - 3], hash[SHA256_HASH_SIZE - 2],
		hash[SHA256_HASH_SIZE - 1]);
	// clang-format on

	return CRYPTO_WRAPPER_ERR_NONE;
}

static crypto_wrapper_err_t calc_pubkey_hash(EVP_PKEY *pkey, rats_tls_cert_algo_t algo,
					     uint8_t *hash)
{
	crypto_wrapper_err_t err;

	if (algo == RATS_TLS_CERT_ALGO_ECC_256_SHA256) {
		EC_KEY *ecc = EVP_PKEY_get1_EC_KEY(pkey);
		err = sha256_ecc_pubkey(hash, ecc);
	} else if (algo == RATS_TLS_CERT_ALGO_RSA_3072_SHA256) {
		RSA *rsa = EVP_PKEY_get1_RSA(pkey);
		err = sha256_rsa_pubkey(hash, rsa);
	} else {
		return -CRYPTO_WRAPPER_ERR_UNSUPPORTED_ALGO;
	}

	if (err != CRYPTO_WRAPPER_ERR_NONE)
		return err;

	EVP_PKEY_free(pkey);

	return CRYPTO_WRAPPER_ERR_NONE;
}

static int find_oid(X509 *crt, const char *oid)
{
	// clang-format off
	const STACK_OF(X509_EXTENSION) *extensions;
	// clang-format on

	/* Set a pointer to the stack of extensions (possibly NULL) */
	if (!(extensions = X509_get0_extensions(crt))) {
		RTLS_DEBUG("there are no extensions in X509 cert\n");
		return 0;
	}

	/* Get the number of extensions (possibly zero) */
	int num_extensions = sk_X509_EXTENSION_num(extensions);

	/* Find the certificate with this OID */
	for (int i = 0; i < num_extensions; ++i) {
		X509_EXTENSION *ext;
		ASN1_OBJECT *obj;
		char oid_buf[128];

		/* Get the i-th extension from the stack */
		if (!(ext = sk_X509_EXTENSION_value(extensions, i))) {
			RTLS_ERR("failed to get X509 extension value\n");
			continue;
		}

		/* Get the OID */
		if (!(obj = X509_EXTENSION_get_object(ext))) {
			RTLS_ERR("failed to get the OID from object\n");
			continue;
		}

		/* Get the string name of the OID */
		if (!OBJ_obj2txt(oid_buf, sizeof(oid_buf), obj, 1)) {
			RTLS_ERR("failed to get string name of the oid\n");
			continue;
		}

		if (!strcmp((const char *)oid_buf, oid))
			return SSL_SUCCESS;
	}

	return 0;
}

static int find_extension_from_cert(X509 *cert, const char *oid, uint8_t *data, uint32_t *size)
{
	int result = SSL_SUCCESS;
	const STACK_OF(X509_EXTENSION) * extensions;

	/* Set a pointer to the stack of extensions (possibly NULL) */
	if (!(extensions = X509_get0_extensions(cert))) {
		RTLS_DEBUG("failed to extensions from X509\n");
		return 0;
	}

	/* Get the number of extensions (possibly zero) */
	int num_extensions = sk_X509_EXTENSION_num(extensions);

	/* Find the certificate with this OID */
	for (int i = 0; i < num_extensions; ++i) {
		X509_EXTENSION *ext;
		ASN1_OBJECT *obj;
		char oid_buf[128];

		/* Get the i-th extension from the stack */
		if (!(ext = sk_X509_EXTENSION_value(extensions, i))) {
			RTLS_ERR("failed to get X509 extension value\n");
			continue;
		}

		/* Get the OID */
		if (!(obj = X509_EXTENSION_get_object(ext))) {
			RTLS_ERR("failed to get the OID from object\n");
			continue;
		}

		/* Get the string name of the OID */
		if (!OBJ_obj2txt(oid_buf, sizeof(oid_buf), obj, 1)) {
			RTLS_ERR("failed to get string name of the oid\n");
			continue;
		}

		/* If found then get the data */
		if (!strcmp((const char *)oid_buf, oid)) {
			ASN1_OCTET_STRING *str;

			/* Get the data from the extension */
			if (!(str = X509_EXTENSION_get_data(ext))) {
				RTLS_ERR("failed to get data from teh extension\n");
				return 0;
			}

			if ((uint32_t)str->length > *size) {
				*size = (uint32_t)str->length;

				if (data)
					RTLS_DEBUG("buffer is too small\n");
			}
			if (data) {
				rtls_memcpy_s(data, *size, str->data, (uint32_t)str->length);
				*size = (uint32_t)str->length;
				result = SSL_SUCCESS;
				goto done;
			}
		}
	}

	result = 0;

done:
	return result;
}

int openssl_extract_x509_extensions(X509 *crt, attestation_evidence_t *evidence)
{
	if (!strcmp(evidence->type, "sgx_epid")) {
		int rc = find_extension_from_cert(crt, ias_response_body_oid,
						  evidence->epid.ias_report,
						  &evidence->epid.ias_report_len);
		if (rc != SSL_SUCCESS)
			return rc;

		rc = find_extension_from_cert(crt, ias_root_cert_oid,
					      evidence->epid.ias_sign_ca_cert,
					      &evidence->epid.ias_sign_ca_cert_len);
		if (rc != SSL_SUCCESS)
			return rc;

		rc = find_extension_from_cert(crt, ias_leaf_cert_oid, evidence->epid.ias_sign_cert,
					      &evidence->epid.ias_sign_cert_len);
		if (rc != SSL_SUCCESS)
			return rc;

		rc = find_extension_from_cert(crt, ias_report_signature_oid,
					      evidence->epid.ias_report_signature,
					      &evidence->epid.ias_report_signature_len);
		return rc;
	} else if (!strcmp(evidence->type, "sgx_ecdsa")) {
		return find_extension_from_cert(crt, ecdsa_quote_oid, evidence->ecdsa.quote,
						&evidence->ecdsa.quote_len);
	} else if (!strcmp(evidence->type, "sgx_la")) {
		return find_extension_from_cert(crt, la_report_oid, evidence->la.report,
						&evidence->la.report_len);
	}

	return SSL_SUCCESS;
}

#ifdef SGX
int verify_certificate(X509_STORE_CTX *ctx)
{
	int preverify = 0;
#else
int verify_certificate(int preverify, X509_STORE_CTX *ctx)
{
#endif
	X509_STORE *cert_store = X509_STORE_CTX_get0_store(ctx);
	int *ex_data = per_thread_getspecific();
	if (!ex_data) {
		RTLS_ERR("failed to get ex_data\n");
		return 0;
	}

	tls_wrapper_ctx_t *tls_ctx = X509_STORE_get_ex_data(cert_store, *ex_data);
	if (!tls_ctx) {
		RTLS_ERR("failed to get tls_wrapper_ctx pointer\n");
		return 0;
	}

	X509 *cert = X509_STORE_CTX_get_current_cert(ctx);
	if (!cert) {
		RTLS_ERR("failed to get cert from x509 context!\n");
		return 0;
	}

	if (preverify == 0) {
		int err = X509_STORE_CTX_get_error(ctx);
		if (err != X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT) {
			RTLS_ERR("This is not a self-signed cert\n");
			return 0;
		}
	}

	EVP_PKEY *publickey = X509_get_pubkey(cert);
	rats_tls_cert_algo_t cert_algo = tls_ctx->rtls_handle->config.cert_algo;
	uint32_t hash_size;

	switch (cert_algo) {
	case RATS_TLS_CERT_ALGO_RSA_3072_SHA256:
	case RATS_TLS_CERT_ALGO_ECC_256_SHA256:
		hash_size = SHA256_HASH_SIZE;
		break;

	default:
		return 0;
	}

	uint8_t hash[hash_size];
	calc_pubkey_hash(publickey, cert_algo, hash);

	/* Extract the Rats TLS certificate extension from the TLS certificate
	 * extension and parse it into evidence
	 */
	attestation_evidence_t evidence;

	if (find_oid(cert, (const char *)ecdsa_quote_oid) == SSL_SUCCESS)
		snprintf(evidence.type, sizeof(evidence.type), "%s", "sgx_ecdsa");
	else if (find_oid(cert, (const char *)la_report_oid) == SSL_SUCCESS)
		snprintf(evidence.type, sizeof(evidence.type), "%s", "sgx_la");
	else
		snprintf(evidence.type, sizeof(evidence.type), "%s", "nullverifier");

	int rc = openssl_extract_x509_extensions(cert, &evidence);
	if (rc != SSL_SUCCESS) {
		RTLS_ERR("failed to extract the extensions from the certificate %d\n", rc);
		return 0;
	}

	tls_wrapper_err_t err =
		tls_wrapper_verify_certificate_extension(tls_ctx, &evidence, hash, hash_size);
	if (err != TLS_WRAPPER_ERR_NONE) {
		RTLS_ERR("failed to verify certificate extension %#x\n", err);
		return 0;
	}

	if (!strncmp(evidence.type, "sgx_ecdsa", sizeof(evidence.type))) {
		rtls_evidence_t ev;
		sgx_quote3_t *quote3 = (sgx_quote3_t *)evidence.ecdsa.quote;

		ev.sgx.mr_enclave = (char *)quote3->report_body.mr_enclave.m;
		ev.sgx.mr_signer = quote3->report_body.mr_signer.m;
		ev.sgx.product_id = quote3->report_body.isv_prod_id;
		ev.sgx.security_version = quote3->report_body.isv_svn;
		ev.sgx.attributes = (char *)&(quote3->report_body.attributes);
		ev.type = SGX_ECDSA;
		ev.quote = (char *)quote3;
		ev.quote_size = sizeof(sgx_quote3_t);

		if (tls_ctx->rtls_handle->user_callback) {
			rc = tls_ctx->rtls_handle->user_callback(&ev);
			if (!rc) {
				RTLS_ERR("failed to verify user callback %d\n", rc);
				return 0;
			}
		}
	}

	return SSL_SUCCESS;
}
