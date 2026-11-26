/* SPDX-License-Identifier: MIT */

#include <onekvm/crypto_backend_v1.h>

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/onekvm-crypto-offload"
#define KERNEL_ABI 1u
#define AUTH_TAG 16u
#define MAX_BATCH 128u
#define MAX_INPUT (20u * 1024u)
#define PAYLOAD_ALIGN 16u

struct kernel_key_config {
    uint32_t version;
    uint32_t key_len;
    uint32_t auth_size;
    uint32_t reserved;
    uint8_t key[32];
};

struct kernel_encrypt_request {
    uint32_t version;
    uint32_t flags;
    uint32_t aad_len;
    uint32_t data_len;
    uint64_t aad_ptr;
    uint64_t src_ptr;
    uint64_t dst_ptr;
    uint8_t iv[12];
    uint8_t reserved[4];
};

struct kernel_batch_request {
    uint32_t version;
    uint32_t flags;
    uint32_t count;
    uint32_t completed;
    uint64_t requests_ptr;
};

#if defined(__GLIBC__)
typedef unsigned long ioctl_req_t;
#else
typedef int ioctl_req_t;
#endif

static const ioctl_req_t k_set_key =
    (ioctl_req_t)_IOW(0xb7, 0x01, struct kernel_key_config);
static const ioctl_req_t k_encrypt =
    (ioctl_req_t)_IOWR(0xb7, 0x02, struct kernel_encrypt_request);
static const ioctl_req_t k_encrypt_batch =
    (ioctl_req_t)_IOWR(0xb7, 0x03, struct kernel_batch_request);

struct session {
    int fd;
    uint32_t auth_tag_size;
};

static atomic_bool g_unusable;

static void set_error(char *error, uint32_t capacity, const char *op, int code)
{
    if (error == NULL || capacity == 0) {
        return;
    }
    if (code < 0) {
        code = -code;
    }
    snprintf(error, capacity, "%s: %s", op, strerror(code));
    error[capacity - 1] = '\0';
}

static int32_t fail_errno(char *error, uint32_t capacity, const char *op)
{
    int code = errno == 0 ? EIO : errno;
    set_error(error, capacity, op, code);
    return -code;
}

static int32_t reject_unusable(char *error, uint32_t capacity, const char *op)
{
    if (!atomic_load_explicit(&g_unusable, memory_order_acquire)) {
        return 0;
    }
    set_error(error, capacity, op, ETIMEDOUT);
    return -ETIMEDOUT;
}

static int32_t fail_encrypt(char *error, uint32_t capacity, const char *op)
{
    int code = errno == 0 ? EIO : errno;
    if (code == ETIMEDOUT) {
        atomic_store_explicit(&g_unusable, true, memory_order_release);
    }
    set_error(error, capacity, op, code);
    return -code;
}

static int32_t validate_request(const struct session *session,
                                struct onekvm_crypto_request_v1 *request,
                                char *error, uint32_t error_capacity)
{
    if (session == NULL || request == NULL ||
        request->struct_size < sizeof(*request) || request->flags != 0) {
        set_error(error, error_capacity, "invalid crypto request", EINVAL);
        return -EINVAL;
    }
    if (request->nonce_size != sizeof(request->nonce) ||
        request->src_size > MAX_INPUT || request->aad_size > MAX_INPUT ||
        request->src_size + request->aad_size > MAX_INPUT ||
        request->dst_capacity < request->src_size + session->auth_tag_size ||
        (request->aad_size != 0 && request->aad_ptr == 0) ||
        (request->src_size != 0 && request->src_ptr == 0) ||
        request->dst_ptr == 0) {
        set_error(error, error_capacity, "unsupported crypto request", EINVAL);
        return -EINVAL;
    }
    request->output_size = 0;
    return 0;
}

static struct kernel_encrypt_request kernel_request(
    const struct onekvm_crypto_request_v1 *request)
{
    struct kernel_encrypt_request kernel;

    memset(&kernel, 0, sizeof(kernel));
    kernel.version = KERNEL_ABI;
    kernel.aad_len = request->aad_size;
    kernel.data_len = request->src_size;
    kernel.aad_ptr = request->aad_ptr;
    kernel.src_ptr = request->src_ptr;
    kernel.dst_ptr = request->dst_ptr;
    memcpy(kernel.iv, request->nonce, sizeof(kernel.iv));
    return kernel;
}

static int32_t probe(char *error, uint32_t error_capacity)
{
    int fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);

    if (fd < 0) {
        return fail_errno(error, error_capacity, "open crypto offload device");
    }
    if (close(fd) != 0) {
        return fail_errno(error, error_capacity, "close crypto offload device");
    }
    return 0;
}

static int32_t session_create(const uint8_t *key, uint32_t key_size,
                              uint32_t auth_tag_size, void **result,
                              char *error, uint32_t error_capacity)
{
    struct session *session;
    struct kernel_key_config config;
    int32_t skipped;

    if (result != NULL) {
        *result = NULL;
    }
    skipped = reject_unusable(error, error_capacity, "AES-GCM offload");
    if (skipped != 0) {
        return skipped;
    }
    if (result == NULL || key == NULL ||
        (key_size != 16 && key_size != 32) || auth_tag_size != AUTH_TAG) {
        set_error(error, error_capacity, "invalid AES-GCM session", EINVAL);
        return -EINVAL;
    }
    session = calloc(1, sizeof(*session));
    if (session == NULL) {
        set_error(error, error_capacity, "allocate AES-GCM session", ENOMEM);
        return -ENOMEM;
    }
    session->fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);
    if (session->fd < 0) {
        int32_t code = fail_errno(error, error_capacity, "open crypto offload device");
        free(session);
        return code;
    }
    session->auth_tag_size = auth_tag_size;
    memset(&config, 0, sizeof(config));
    config.version = KERNEL_ABI;
    config.key_len = key_size;
    config.auth_size = auth_tag_size;
    memcpy(config.key, key, key_size);
    if (ioctl(session->fd, k_set_key, &config) != 0) {
        int32_t code = fail_errno(error, error_capacity, "configure crypto offload key");
        memset(config.key, 0, sizeof(config.key));
        close(session->fd);
        free(session);
        return code;
    }
    memset(config.key, 0, sizeof(config.key));
    *result = session;
    return 0;
}

static int32_t session_seal(void *opaque, struct onekvm_crypto_request_v1 *request,
                            char *error, uint32_t error_capacity)
{
    struct session *session = opaque;
    struct kernel_encrypt_request kernel;
    int32_t valid;
    int32_t skipped = reject_unusable(error, error_capacity, "AES-GCM offload");

    if (skipped != 0) {
        return skipped;
    }
    valid = validate_request(session, request, error, error_capacity);
    if (valid != 0) {
        return valid;
    }
    kernel = kernel_request(request);
    if (ioctl(session->fd, k_encrypt, &kernel) != 0) {
        return fail_encrypt(error, error_capacity, "AES-GCM offload");
    }
    request->output_size = request->src_size + session->auth_tag_size;
    return 0;
}

static int32_t session_seal_batch(void *opaque, struct onekvm_crypto_request_v1 *requests,
                                  uint32_t count, uint32_t *completed,
                                  char *error, uint32_t error_capacity)
{
    struct session *session = opaque;
    struct kernel_encrypt_request kernel[MAX_BATCH];
    struct kernel_batch_request batch;
    uint32_t i;
    int32_t skipped;

    if (completed == NULL || requests == NULL || count == 0 || count > MAX_BATCH) {
        set_error(error, error_capacity, "invalid AES-GCM batch", EINVAL);
        return -EINVAL;
    }
    *completed = 0;
    skipped = reject_unusable(error, error_capacity, "AES-GCM batch offload");
    if (skipped != 0) {
        return skipped;
    }
    for (i = 0; i < count; i++) {
        int32_t valid = validate_request(session, &requests[i], error, error_capacity);
        if (valid != 0) {
            return valid;
        }
        kernel[i] = kernel_request(&requests[i]);
    }
    memset(&batch, 0, sizeof(batch));
    batch.version = KERNEL_ABI;
    batch.count = count;
    batch.requests_ptr = (uint64_t)(uintptr_t)kernel;
    if (ioctl(session->fd, k_encrypt_batch, &batch) != 0) {
        *completed = batch.completed;
        return fail_encrypt(error, error_capacity, "AES-GCM batch offload");
    }
    *completed = batch.completed;
    for (i = 0; i < batch.completed && i < count; i++) {
        requests[i].output_size = requests[i].src_size + session->auth_tag_size;
    }
    if (batch.completed != count) {
        set_error(error, error_capacity, "incomplete AES-GCM batch", EIO);
        return -EIO;
    }
    return 0;
}

static void session_destroy(void *opaque)
{
    struct session *session = opaque;

    if (session == NULL) {
        return;
    }
    if (session->fd >= 0) {
        close(session->fd);
    }
    free(session);
}

static const struct onekvm_crypto_backend_v1 k_backend = {
    .struct_size = sizeof(struct onekvm_crypto_backend_v1),
    .abi_version = ONEKVM_CRYPTO_BACKEND_ABI_V1,
    .driver_id = "rk-mpp",
    .backend_name = "picokvm CE v3 AES-GCM",
    .features = ONEKVM_CRYPTO_FEATURE_AES_GCM_TX | ONEKVM_CRYPTO_FEATURE_AES_GCM_TX_BATCH,
    .max_batch = MAX_BATCH,
    .max_input = MAX_INPUT,
    .payload_alignment = PAYLOAD_ALIGN,
    .probe = probe,
    .session_create = session_create,
    .session_seal = session_seal,
    .session_seal_batch = session_seal_batch,
    .session_destroy = session_destroy,
};

ONEKVM_CRYPTO_EXPORT int32_t onekvm_crypto_backend_query(uint32_t requested_abi,
                                                         const void **backend_api)
{
    if (backend_api == NULL) {
        return -EINVAL;
    }
    *backend_api = NULL;
    if (requested_abi != ONEKVM_CRYPTO_BACKEND_ABI_V1) {
        return -EINVAL;
    }
    *backend_api = &k_backend;
    return 0;
}
