/**
 * Copyright (C) UT-Battelle, LLC. 2015. ALL RIGHTS RESERVED.
 * Copyright (C) Mellanox Technologies Ltd. 2001-2019.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#include <knem_io.h>

#include "knem_ep.h"
#include "knem_md.h"
#include <uct/base/uct_iov.inl>
#include <uct/sm/base/sm_iface.h>
#include <ucs/debug/log.h>

static UCS_CLASS_INIT_FUNC(uct_knem_ep_t, const uct_ep_params_t *params)
{
    UCS_CLASS_CALL_SUPER_INIT(uct_scopy_ep_t, params);
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_knem_ep_t)
{
    /* No op */
}

UCS_CLASS_DEFINE(uct_knem_ep_t, uct_scopy_ep_t)
UCS_CLASS_DEFINE_NEW_FUNC(uct_knem_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_knem_ep_t, uct_ep_t);


#define uct_knem_trace_data(_remote_addr, _rkey, _fmt, ...) \
    ucs_trace_data(_fmt " to %"PRIx64"(%+ld)", ## __VA_ARGS__, (_remote_addr), \
                   (_rkey))

#define UCT_KNEM_ZERO_LENGTH_POST(len)              \
    if (0 == len) {                                     \
        ucs_trace_data("Zero length request: skip it"); \
        return UCS_OK;                                  \
    }


static UCS_F_ALWAYS_INLINE
void uct_knem_iovec_set_length(struct knem_cmd_param_iovec *iov, size_t length)
{
    iov->len = length;
}

static UCS_F_ALWAYS_INLINE
void uct_knem_iovec_set_buffer(struct knem_cmd_param_iovec *iov, void *buffer)
{
    iov->base = (uintptr_t)buffer;
}

static inline ucs_status_t uct_knem_rma(uct_ep_h tl_ep, const uct_iov_t *iov,
                                        size_t iovcnt, uint64_t remote_addr,
                                        uct_knem_key_t *key, int write)
{
    uct_knem_iface_t *knem_iface = ucs_derived_of(tl_ep->iface, uct_knem_iface_t);
    int knem_fd                  = knem_iface->knem_md->knem_fd;
    size_t knem_iov_cnt          = UCT_SM_MAX_IOV;
    struct knem_cmd_inline_copy icopy;
    struct knem_cmd_param_iovec knem_iov[UCT_SM_MAX_IOV];
    ucs_iov_iter_t uct_iov_iter;
    int rc;

    UCT_CHECK_IOV_SIZE(iovcnt, knem_iface->super.config.max_iov,
                       write ? "uct_knem_ep_put_zcopy" : "uct_knem_ep_get_zcopy");

    ucs_iov_iter_init(&uct_iov_iter);
    ucs_iov_converter(knem_iov, &knem_iov_cnt,
                      uct_knem_iovec_set_buffer, uct_knem_iovec_set_length,
                      iov, iovcnt,
                      uct_iov_get_buffer, uct_iov_get_length,
                      SIZE_MAX, &uct_iov_iter);

    UCT_KNEM_ZERO_LENGTH_POST(knem_iov_cnt);

    icopy.local_iovec_array = (uintptr_t)knem_iov;
    icopy.local_iovec_nr    = knem_iov_cnt;
    icopy.remote_cookie     = key->cookie;
    ucs_assert(remote_addr >= key->address);
    icopy.current_status    = 0;
    icopy.remote_offset     = remote_addr - key->address;
    /* if 0 then, READ from the remote region into my local segments
     * if 1 then, WRITE to the remote region from my local segment */
    icopy.write             = write;
    /* TBD: add check and support for KNEM_FLAG_DMA */
    icopy.flags             = 0;

    ucs_assert(knem_fd > -1);
    rc = ioctl(knem_fd, KNEM_CMD_INLINE_COPY, &icopy);
    if (ucs_unlikely((rc < 0) || (icopy.current_status != KNEM_STATUS_SUCCESS))) {
        ucs_error("KNEM inline copy failed, ioctl() return value - %d, "
                  "copy status - %d: %m", rc, icopy.current_status);
        return UCS_ERR_IO_ERROR;
    }

    uct_knem_trace_data(remote_addr, (uintptr_t)key, "%s [length %zu]",
                        write?"PUT_ZCOPY":"GET_ZCOPY",
                        uct_iov_total_length(iov, iovcnt));
    return UCS_OK;
}

ucs_status_t uct_knem_ep_put_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov, size_t iovcnt,
                                   uint64_t remote_addr, uct_rkey_t rkey,
                                   uct_completion_t *comp)
{
    uct_knem_key_t *key = (uct_knem_key_t *)rkey;
    ucs_status_t status;

    status = uct_knem_rma(tl_ep, iov, iovcnt, remote_addr, key, 1);
    UCT_TL_EP_STAT_OP_IF_SUCCESS(status, ucs_derived_of(tl_ep, uct_base_ep_t),
                                 PUT, ZCOPY, uct_iov_total_length(iov, iovcnt));
    return status;
}

ucs_status_t uct_knem_ep_get_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov, size_t iovcnt,
                                   uint64_t remote_addr, uct_rkey_t rkey,
                                   uct_completion_t *comp)
{
    uct_knem_key_t *key = (uct_knem_key_t *)rkey;
    ucs_status_t status;

    status = uct_knem_rma(tl_ep, iov, iovcnt, remote_addr, key, 0);
    UCT_TL_EP_STAT_OP_IF_SUCCESS(status, ucs_derived_of(tl_ep, uct_base_ep_t),
                                 GET, ZCOPY, uct_iov_total_length(iov, iovcnt));
    return status;
}
