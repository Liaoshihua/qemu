/*
 * vhost-vdpa
 *
 *  Copyright(c) 2017-2018 Intel Corporation.
 *  Copyright(c) 2020 Red Hat, Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include <linux/vhost.h>
#include <linux/vfio.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include "hw/virtio/vhost.h"
#include "hw/virtio/vhost-backend.h"
#include "hw/virtio/virtio-net.h"
#include "hw/virtio/vhost-shadow-virtqueue.h"
#include "hw/virtio/vhost-vdpa.h"
#include "exec/address-spaces.h"
#include "qemu/main-loop.h"
#include "cpu.h"
#include "trace.h"
#include "qemu-common.h"
#include "qapi/error.h"

/*
 * Return one past the end of the end of section. Be careful with uint64_t
 * conversions!
 */
static Int128 vhost_vdpa_section_end(const MemoryRegionSection *section)
{
    Int128 llend = int128_make64(section->offset_within_address_space);
    llend = int128_add(llend, section->size);
    llend = int128_and(llend, int128_exts64(TARGET_PAGE_MASK));

    return llend;
}

static bool vhost_vdpa_listener_skipped_section(MemoryRegionSection *section,
                                                uint64_t iova_min,
                                                uint64_t iova_max)
{
    Int128 llend;

    if ((!memory_region_is_ram(section->mr) &&
         !memory_region_is_iommu(section->mr)) ||
        memory_region_is_protected(section->mr) ||
        /* vhost-vDPA doesn't allow MMIO to be mapped  */
        memory_region_is_ram_device(section->mr)) {
        return true;
    }

    if (section->offset_within_address_space < iova_min) {
        error_report("RAM section out of device range (min=0x%" PRIx64
                     ", addr=0x%" HWADDR_PRIx ")",
                     iova_min, section->offset_within_address_space);
        return true;
    }

    llend = vhost_vdpa_section_end(section);
    if (int128_gt(llend, int128_make64(iova_max))) {
        error_report("RAM section out of device range (max=0x%" PRIx64
                     ", end addr=0x%" PRIx64 ")",
                     iova_max, int128_get64(llend));
        return true;
    }

    return false;
}

static int vhost_vdpa_dma_map(struct vhost_vdpa *v, hwaddr iova, hwaddr size,
                              void *vaddr, bool readonly)
{
    struct vhost_msg_v2 msg = {};
    int fd = v->device_fd;
    int ret = 0;

    msg.type = v->msg_type;
    msg.iotlb.iova = iova;
    msg.iotlb.size = size;
    msg.iotlb.uaddr = (uint64_t)(uintptr_t)vaddr;
    msg.iotlb.perm = readonly ? VHOST_ACCESS_RO : VHOST_ACCESS_RW;
    msg.iotlb.type = VHOST_IOTLB_UPDATE;

   trace_vhost_vdpa_dma_map(v, fd, msg.type, msg.iotlb.iova, msg.iotlb.size,
                            msg.iotlb.uaddr, msg.iotlb.perm, msg.iotlb.type);

    if (write(fd, &msg, sizeof(msg)) != sizeof(msg)) {
        error_report("failed to write, fd=%d, errno=%d (%s)",
            fd, errno, strerror(errno));
        return -EIO ;
    }

    return ret;
}

static int vhost_vdpa_dma_unmap(struct vhost_vdpa *v, hwaddr iova,
                                hwaddr size)
{
    struct vhost_msg_v2 msg = {};
    int fd = v->device_fd;
    int ret = 0;

    msg.type = v->msg_type;
    msg.iotlb.iova = iova;
    msg.iotlb.size = size;
    msg.iotlb.type = VHOST_IOTLB_INVALIDATE;

    trace_vhost_vdpa_dma_unmap(v, fd, msg.type, msg.iotlb.iova,
                               msg.iotlb.size, msg.iotlb.type);

    if (write(fd, &msg, sizeof(msg)) != sizeof(msg)) {
        error_report("failed to write, fd=%d, errno=%d (%s)",
            fd, errno, strerror(errno));
        return -EIO ;
    }

    return ret;
}

static void vhost_vdpa_listener_begin_batch(struct vhost_vdpa *v)
{
    int fd = v->device_fd;
    struct vhost_msg_v2 msg = {
        .type = v->msg_type,
        .iotlb.type = VHOST_IOTLB_BATCH_BEGIN,
    };

    if (write(fd, &msg, sizeof(msg)) != sizeof(msg)) {
        error_report("failed to write, fd=%d, errno=%d (%s)",
                     fd, errno, strerror(errno));
    }
}

static void vhost_vdpa_iotlb_batch_begin_once(struct vhost_vdpa *v)
{
    if (v->dev->backend_cap & (0x1ULL << VHOST_BACKEND_F_IOTLB_BATCH) &&
        !v->iotlb_batch_begin_sent) {
        vhost_vdpa_listener_begin_batch(v);
    }

    v->iotlb_batch_begin_sent = true;
}

static void vhost_vdpa_listener_commit(MemoryListener *listener)
{
    struct vhost_vdpa *v = container_of(listener, struct vhost_vdpa, listener);
    struct vhost_dev *dev = v->dev;
    struct vhost_msg_v2 msg = {};
    int fd = v->device_fd;

    if (!(dev->backend_cap & (0x1ULL << VHOST_BACKEND_F_IOTLB_BATCH))) {
        return;
    }

    if (!v->iotlb_batch_begin_sent) {
        return;
    }

    msg.type = v->msg_type;
    msg.iotlb.type = VHOST_IOTLB_BATCH_END;

    if (write(fd, &msg, sizeof(msg)) != sizeof(msg)) {
        error_report("failed to write, fd=%d, errno=%d (%s)",
                     fd, errno, strerror(errno));
    }

    v->iotlb_batch_begin_sent = false;
}

static void vhost_vdpa_listener_region_add(MemoryListener *listener,
                                           MemoryRegionSection *section)
{
    struct vhost_vdpa *v = container_of(listener, struct vhost_vdpa, listener);
    hwaddr iova;
    Int128 llend, llsize;
    void *vaddr;
    int ret;

    if (vhost_vdpa_listener_skipped_section(section, v->iova_range.first,
                                            v->iova_range.last)) {
        return;
    }

    if (unlikely((section->offset_within_address_space & ~TARGET_PAGE_MASK) !=
                 (section->offset_within_region & ~TARGET_PAGE_MASK))) {
        error_report("%s received unaligned region", __func__);
        return;
    }

    iova = TARGET_PAGE_ALIGN(section->offset_within_address_space);
    llend = vhost_vdpa_section_end(section);
    if (int128_ge(int128_make64(iova), llend)) {
        return;
    }

    memory_region_ref(section->mr);

    /* Here we assume that memory_region_is_ram(section->mr)==true */

    vaddr = memory_region_get_ram_ptr(section->mr) +
            section->offset_within_region +
            (iova - section->offset_within_address_space);

    trace_vhost_vdpa_listener_region_add(v, iova, int128_get64(llend),
                                         vaddr, section->readonly);

    llsize = int128_sub(llend, int128_make64(iova));

    vhost_vdpa_iotlb_batch_begin_once(v);
    ret = vhost_vdpa_dma_map(v, iova, int128_get64(llsize),
                             vaddr, section->readonly);
    if (ret) {
        error_report("vhost vdpa map fail!");
        goto fail;
    }

    return;

fail:
    /*
     * On the initfn path, store the first error in the container so we
     * can gracefully fail.  Runtime, there's not much we can do other
     * than throw a hardware error.
     */
    error_report("vhost-vdpa: DMA mapping failed, unable to continue");
    return;

}

static void vhost_vdpa_listener_region_del(MemoryListener *listener,
                                           MemoryRegionSection *section)
{
    struct vhost_vdpa *v = container_of(listener, struct vhost_vdpa, listener);
    hwaddr iova;
    Int128 llend, llsize;
    int ret;

    if (vhost_vdpa_listener_skipped_section(section, v->iova_range.first,
                                            v->iova_range.last)) {
        return;
    }

    if (unlikely((section->offset_within_address_space & ~TARGET_PAGE_MASK) !=
                 (section->offset_within_region & ~TARGET_PAGE_MASK))) {
        error_report("%s received unaligned region", __func__);
        return;
    }

    iova = TARGET_PAGE_ALIGN(section->offset_within_address_space);
    llend = vhost_vdpa_section_end(section);

    trace_vhost_vdpa_listener_region_del(v, iova, int128_get64(llend));

    if (int128_ge(int128_make64(iova), llend)) {
        return;
    }

    llsize = int128_sub(llend, int128_make64(iova));

    vhost_vdpa_iotlb_batch_begin_once(v);
    ret = vhost_vdpa_dma_unmap(v, iova, int128_get64(llsize));
    if (ret) {
        error_report("vhost_vdpa dma unmap error!");
    }

    memory_region_unref(section->mr);
}
/*
 * IOTLB API is used by vhost-vpda which requires incremental updating
 * of the mapping. So we can not use generic vhost memory listener which
 * depends on the addnop().
 */
static const MemoryListener vhost_vdpa_memory_listener = {
    .name = "vhost-vdpa",
    .commit = vhost_vdpa_listener_commit,
    .region_add = vhost_vdpa_listener_region_add,
    .region_del = vhost_vdpa_listener_region_del,
};

static int vhost_vdpa_call(struct vhost_dev *dev, unsigned long int request,
                             void *arg)
{
    struct vhost_vdpa *v = dev->opaque;
    int fd = v->device_fd;
    int ret;

    assert(dev->vhost_ops->backend_type == VHOST_BACKEND_TYPE_VDPA);

    ret = ioctl(fd, request, arg);
    return ret < 0 ? -errno : ret;
}

static int vhost_vdpa_add_status(struct vhost_dev *dev, uint8_t status)
{
    uint8_t s;
    int ret;

    trace_vhost_vdpa_add_status(dev, status);
    ret = vhost_vdpa_call(dev, VHOST_VDPA_GET_STATUS, &s);
    if (ret < 0) {
        return ret;
    }

    s |= status;

    ret = vhost_vdpa_call(dev, VHOST_VDPA_SET_STATUS, &s);
    if (ret < 0) {
        return ret;
    }

    ret = vhost_vdpa_call(dev, VHOST_VDPA_GET_STATUS, &s);
    if (ret < 0) {
        return ret;
    }

    if (!(s & status)) {
        return -EIO;
    }

    return 0;
}

static void vhost_vdpa_get_iova_range(struct vhost_vdpa *v)
{
    int ret = vhost_vdpa_call(v->dev, VHOST_VDPA_GET_IOVA_RANGE,
                              &v->iova_range);
    if (ret != 0) {
        v->iova_range.first = 0;
        v->iova_range.last = UINT64_MAX;
    }

    trace_vhost_vdpa_get_iova_range(v->dev, v->iova_range.first,
                                    v->iova_range.last);
}

static bool vhost_vdpa_one_time_request(struct vhost_dev *dev)
{
    struct vhost_vdpa *v = dev->opaque;

    return v->index != 0;
}

static int vhost_vdpa_init_svq(struct vhost_dev *hdev, struct vhost_vdpa *v,
                               Error **errp)
{
    g_autoptr(GPtrArray) shadow_vqs = NULL;
    uint64_t dev_features, svq_features;
    int r;
    bool ok;

    if (!v->shadow_vqs_enabled) {
        return 0;
    }

    r = hdev->vhost_ops->vhost_get_features(hdev, &dev_features);
    if (r != 0) {
        error_setg_errno(errp, -r, "Can't get vdpa device features");
        return r;
    }

    svq_features = dev_features;
    ok = vhost_svq_valid_features(svq_features, errp);
    if (unlikely(!ok)) {
        return -1;
    }

    shadow_vqs = g_ptr_array_new_full(hdev->nvqs, vhost_svq_free);
    for (unsigned n = 0; n < hdev->nvqs; ++n) {
        g_autoptr(VhostShadowVirtqueue) svq = vhost_svq_new();

        if (unlikely(!svq)) {
            error_setg(errp, "Cannot create svq %u", n);
            return -1;
        }
        g_ptr_array_add(shadow_vqs, g_steal_pointer(&svq));
    }

    v->shadow_vqs = g_steal_pointer(&shadow_vqs);
    return 0;
}

static int vhost_vdpa_init(struct vhost_dev *dev, void *opaque, Error **errp)
{
    struct vhost_vdpa *v;
    assert(dev->vhost_ops->backend_type == VHOST_BACKEND_TYPE_VDPA);
    trace_vhost_vdpa_init(dev, opaque);
    int ret;

    /*
     * Similar to VFIO, we end up pinning all guest memory and have to
     * disable discarding of RAM.
     */
    ret = ram_block_discard_disable(true);
    if (ret) {
        error_report("Cannot set discarding of RAM broken");
        return ret;
    }

    v = opaque;
    v->dev = dev;
    dev->opaque =  opaque ;
    v->listener = vhost_vdpa_memory_listener;
    v->msg_type = VHOST_IOTLB_MSG_V2;
    ret = vhost_vdpa_init_svq(dev, v, errp);
    if (ret) {
        goto err;
    }

    vhost_vdpa_get_iova_range(v);

    if (vhost_vdpa_one_time_request(dev)) {
        return 0;
    }

    vhost_vdpa_add_status(dev, VIRTIO_CONFIG_S_ACKNOWLEDGE |
                               VIRTIO_CONFIG_S_DRIVER);

    return 0;

err:
    ram_block_discard_disable(false);
    return ret;
}

static void vhost_vdpa_host_notifier_uninit(struct vhost_dev *dev,
                                            int queue_index)
{
    size_t page_size = qemu_real_host_page_size;
    struct vhost_vdpa *v = dev->opaque;
    VirtIODevice *vdev = dev->vdev;
    VhostVDPAHostNotifier *n;

    n = &v->notifier[queue_index];

    if (n->addr) {
        virtio_queue_set_host_notifier_mr(vdev, queue_index, &n->mr, false);
        object_unparent(OBJECT(&n->mr));
        munmap(n->addr, page_size);
        n->addr = NULL;
    }
}

static int vhost_vdpa_host_notifier_init(struct vhost_dev *dev, int queue_index)
{
    size_t page_size = qemu_real_host_page_size;
    struct vhost_vdpa *v = dev->opaque;
    VirtIODevice *vdev = dev->vdev;
    VhostVDPAHostNotifier *n;
    int fd = v->device_fd;
    void *addr;
    char *name;

    vhost_vdpa_host_notifier_uninit(dev, queue_index);

    n = &v->notifier[queue_index];

    addr = mmap(NULL, page_size, PROT_WRITE, MAP_SHARED, fd,
                queue_index * page_size);
    if (addr == MAP_FAILED) {
        goto err;
    }

    name = g_strdup_printf("vhost-vdpa/host-notifier@%p mmaps[%d]",
                           v, queue_index);
    memory_region_init_ram_device_ptr(&n->mr, OBJECT(vdev), name,
                                      page_size, addr);
    g_free(name);

    if (virtio_queue_set_host_notifier_mr(vdev, queue_index, &n->mr, true)) {
        object_unparent(OBJECT(&n->mr));
        munmap(addr, page_size);
        goto err;
    }
    n->addr = addr;

    return 0;

err:
    return -1;
}

static void vhost_vdpa_host_notifiers_uninit(struct vhost_dev *dev, int n)
{
    int i;

    for (i = dev->vq_index; i < dev->vq_index + n; i++) {
        vhost_vdpa_host_notifier_uninit(dev, i);
    }
}

static void vhost_vdpa_host_notifiers_init(struct vhost_dev *dev)
{
    struct vhost_vdpa *v = dev->opaque;
    int i;

    if (v->shadow_vqs_enabled) {
        /* FIXME SVQ is not compatible with host notifiers mr */
        return;
    }

    for (i = dev->vq_index; i < dev->vq_index + dev->nvqs; i++) {
        if (vhost_vdpa_host_notifier_init(dev, i)) {
            goto err;
        }
    }

    return;

err:
    vhost_vdpa_host_notifiers_uninit(dev, i - dev->vq_index);
    return;
}

static void vhost_vdpa_svq_cleanup(struct vhost_dev *dev)
{
    struct vhost_vdpa *v = dev->opaque;
    size_t idx;

    if (!v->shadow_vqs) {
        return;
    }

    for (idx = 0; idx < v->shadow_vqs->len; ++idx) {
        vhost_svq_stop(g_ptr_array_index(v->shadow_vqs, idx));
    }
    g_ptr_array_free(v->shadow_vqs, true);
}

static int vhost_vdpa_cleanup(struct vhost_dev *dev)
{
    struct vhost_vdpa *v;
    assert(dev->vhost_ops->backend_type == VHOST_BACKEND_TYPE_VDPA);
    v = dev->opaque;
    trace_vhost_vdpa_cleanup(dev, v);
    vhost_vdpa_host_notifiers_uninit(dev, dev->nvqs);
    memory_listener_unregister(&v->listener);
    vhost_vdpa_svq_cleanup(dev);

    dev->opaque = NULL;
    ram_block_discard_disable(false);

    return 0;
}

static int vhost_vdpa_memslots_limit(struct vhost_dev *dev)
{
    trace_vhost_vdpa_memslots_limit(dev, INT_MAX);
    return INT_MAX;
}

static int vhost_vdpa_set_mem_table(struct vhost_dev *dev,
                                    struct vhost_memory *mem)
{
    if (vhost_vdpa_one_time_request(dev)) {
        return 0;
    }

    trace_vhost_vdpa_set_mem_table(dev, mem->nregions, mem->padding);
    if (trace_event_get_state_backends(TRACE_VHOST_VDPA_SET_MEM_TABLE) &&
        trace_event_get_state_backends(TRACE_VHOST_VDPA_DUMP_REGIONS)) {
        int i;
        for (i = 0; i < mem->nregions; i++) {
            trace_vhost_vdpa_dump_regions(dev, i,
                                          mem->regions[i].guest_phys_addr,
                                          mem->regions[i].memory_size,
                                          mem->regions[i].userspace_addr,
                                          mem->regions[i].flags_padding);
        }
    }
    if (mem->padding) {
        return -EINVAL;
    }

    return 0;
}

static int vhost_vdpa_set_features(struct vhost_dev *dev,
                                   uint64_t features)
{
    int ret;

    if (vhost_vdpa_one_time_request(dev)) {
        return 0;
    }

    trace_vhost_vdpa_set_features(dev, features);
    ret = vhost_vdpa_call(dev, VHOST_SET_FEATURES, &features);
    if (ret) {
        return ret;
    }

    return vhost_vdpa_add_status(dev, VIRTIO_CONFIG_S_FEATURES_OK);
}

static int vhost_vdpa_set_backend_cap(struct vhost_dev *dev)
{
    uint64_t features;
    uint64_t f = 0x1ULL << VHOST_BACKEND_F_IOTLB_MSG_V2 |
        0x1ULL << VHOST_BACKEND_F_IOTLB_BATCH;
    int r;

    if (vhost_vdpa_call(dev, VHOST_GET_BACKEND_FEATURES, &features)) {
        return -EFAULT;
    }

    features &= f;

    if (vhost_vdpa_one_time_request(dev)) {
        r = vhost_vdpa_call(dev, VHOST_SET_BACKEND_FEATURES, &features);
        if (r) {
            return -EFAULT;
        }
    }

    dev->backend_cap = features;

    return 0;
}

static int vhost_vdpa_get_device_id(struct vhost_dev *dev,
                                    uint32_t *device_id)
{
    int ret;
    ret = vhost_vdpa_call(dev, VHOST_VDPA_GET_DEVICE_ID, device_id);
    trace_vhost_vdpa_get_device_id(dev, *device_id);
    return ret;
}

static void vhost_vdpa_reset_svq(struct vhost_vdpa *v)
{
    if (!v->shadow_vqs_enabled) {
        return;
    }

    for (unsigned i = 0; i < v->shadow_vqs->len; ++i) {
        VhostShadowVirtqueue *svq = g_ptr_array_index(v->shadow_vqs, i);
        vhost_svq_stop(svq);
    }
}

static int vhost_vdpa_reset_device(struct vhost_dev *dev)
{
    struct vhost_vdpa *v = dev->opaque;
    int ret;
    uint8_t status = 0;

    vhost_vdpa_reset_svq(v);

    ret = vhost_vdpa_call(dev, VHOST_VDPA_SET_STATUS, &status);
    trace_vhost_vdpa_reset_device(dev, status);
    return ret;
}

static int vhost_vdpa_get_vq_index(struct vhost_dev *dev, int idx)
{
    assert(idx >= dev->vq_index && idx < dev->vq_index + dev->nvqs);

    trace_vhost_vdpa_get_vq_index(dev, idx, idx);
    return idx;
}

static int vhost_vdpa_set_vring_ready(struct vhost_dev *dev)
{
    int i;
    trace_vhost_vdpa_set_vring_ready(dev);
    for (i = 0; i < dev->nvqs; ++i) {
        struct vhost_vring_state state = {
            .index = dev->vq_index + i,
            .num = 1,
        };
        vhost_vdpa_call(dev, VHOST_VDPA_SET_VRING_ENABLE, &state);
    }
    return 0;
}

static void vhost_vdpa_dump_config(struct vhost_dev *dev, const uint8_t *config,
                                   uint32_t config_len)
{
    int b, len;
    char line[QEMU_HEXDUMP_LINE_LEN];

    for (b = 0; b < config_len; b += 16) {
        len = config_len - b;
        qemu_hexdump_line(line, b, config, len, false);
        trace_vhost_vdpa_dump_config(dev, line);
    }
}

static int vhost_vdpa_set_config(struct vhost_dev *dev, const uint8_t *data,
                                   uint32_t offset, uint32_t size,
                                   uint32_t flags)
{
    struct vhost_vdpa_config *config;
    int ret;
    unsigned long config_size = offsetof(struct vhost_vdpa_config, buf);

    trace_vhost_vdpa_set_config(dev, offset, size, flags);
    config = g_malloc(size + config_size);
    config->off = offset;
    config->len = size;
    memcpy(config->buf, data, size);
    if (trace_event_get_state_backends(TRACE_VHOST_VDPA_SET_CONFIG) &&
        trace_event_get_state_backends(TRACE_VHOST_VDPA_DUMP_CONFIG)) {
        vhost_vdpa_dump_config(dev, data, size);
    }
    ret = vhost_vdpa_call(dev, VHOST_VDPA_SET_CONFIG, config);
    g_free(config);
    return ret;
}

static int vhost_vdpa_get_config(struct vhost_dev *dev, uint8_t *config,
                                   uint32_t config_len, Error **errp)
{
    struct vhost_vdpa_config *v_config;
    unsigned long config_size = offsetof(struct vhost_vdpa_config, buf);
    int ret;

    trace_vhost_vdpa_get_config(dev, config, config_len);
    v_config = g_malloc(config_len + config_size);
    v_config->len = config_len;
    v_config->off = 0;
    ret = vhost_vdpa_call(dev, VHOST_VDPA_GET_CONFIG, v_config);
    memcpy(config, v_config->buf, config_len);
    g_free(v_config);
    if (trace_event_get_state_backends(TRACE_VHOST_VDPA_GET_CONFIG) &&
        trace_event_get_state_backends(TRACE_VHOST_VDPA_DUMP_CONFIG)) {
        vhost_vdpa_dump_config(dev, config, config_len);
    }
    return ret;
 }

static int vhost_vdpa_set_dev_vring_base(struct vhost_dev *dev,
                                         struct vhost_vring_state *ring)
{
    trace_vhost_vdpa_set_vring_base(dev, ring->index, ring->num);
    return vhost_vdpa_call(dev, VHOST_SET_VRING_BASE, ring);
}

static int vhost_vdpa_set_vring_dev_kick(struct vhost_dev *dev,
                                         struct vhost_vring_file *file)
{
    trace_vhost_vdpa_set_vring_kick(dev, file->index, file->fd);
    return vhost_vdpa_call(dev, VHOST_SET_VRING_KICK, file);
}

static int vhost_vdpa_set_vring_dev_call(struct vhost_dev *dev,
                                         struct vhost_vring_file *file)
{
    trace_vhost_vdpa_set_vring_call(dev, file->index, file->fd);
    return vhost_vdpa_call(dev, VHOST_SET_VRING_CALL, file);
}

static int vhost_vdpa_set_vring_dev_addr(struct vhost_dev *dev,
                                         struct vhost_vring_addr *addr)
{
    trace_vhost_vdpa_set_vring_addr(dev, addr->index, addr->flags,
                                addr->desc_user_addr, addr->used_user_addr,
                                addr->avail_user_addr,
                                addr->log_guest_addr);

    return vhost_vdpa_call(dev, VHOST_SET_VRING_ADDR, addr);

}

/**
 * Set the shadow virtqueue descriptors to the device
 *
 * @dev: The vhost device model
 * @svq: The shadow virtqueue
 * @idx: The index of the virtqueue in the vhost device
 * @errp: Error
 *
 * Note that this function does not rewind kick file descriptor if cannot set
 * call one.
 */
static bool vhost_vdpa_svq_setup(struct vhost_dev *dev,
                                 VhostShadowVirtqueue *svq, unsigned idx,
                                 Error **errp)
{
    struct vhost_vring_file file = {
        .index = dev->vq_index + idx,
    };
    const EventNotifier *event_notifier = &svq->hdev_kick;
    int r;

    file.fd = event_notifier_get_fd(event_notifier);
    r = vhost_vdpa_set_vring_dev_kick(dev, &file);
    if (unlikely(r != 0)) {
        error_setg_errno(errp, -r, "Can't set device kick fd");
        return false;
    }

    event_notifier = &svq->hdev_call;
    file.fd = event_notifier_get_fd(event_notifier);
    r = vhost_vdpa_set_vring_dev_call(dev, &file);
    if (unlikely(r != 0)) {
        error_setg_errno(errp, -r, "Can't set device call fd");
    }

    return r == 0;
}

static bool vhost_vdpa_svqs_start(struct vhost_dev *dev)
{
    struct vhost_vdpa *v = dev->opaque;
    Error *err = NULL;
    unsigned i;

    if (!v->shadow_vqs) {
        return true;
    }

    for (i = 0; i < v->shadow_vqs->len; ++i) {
        VhostShadowVirtqueue *svq = g_ptr_array_index(v->shadow_vqs, i);
        bool ok = vhost_vdpa_svq_setup(dev, svq, i, &err);
        if (unlikely(!ok)) {
            error_reportf_err(err, "Cannot setup SVQ %u: ", i);
            return false;
        }
    }

    return true;
}

static int vhost_vdpa_dev_start(struct vhost_dev *dev, bool started)
{
    struct vhost_vdpa *v = dev->opaque;
    bool ok;
    trace_vhost_vdpa_dev_start(dev, started);

    if (started) {
        vhost_vdpa_host_notifiers_init(dev);
        ok = vhost_vdpa_svqs_start(dev);
        if (unlikely(!ok)) {
            return -1;
        }
        vhost_vdpa_set_vring_ready(dev);
    } else {
        vhost_vdpa_host_notifiers_uninit(dev, dev->nvqs);
    }

    if (dev->vq_index + dev->nvqs != dev->vq_index_end) {
        return 0;
    }

    if (started) {
        memory_listener_register(&v->listener, &address_space_memory);
        return vhost_vdpa_add_status(dev, VIRTIO_CONFIG_S_DRIVER_OK);
    } else {
        vhost_vdpa_reset_device(dev);
        vhost_vdpa_add_status(dev, VIRTIO_CONFIG_S_ACKNOWLEDGE |
                                   VIRTIO_CONFIG_S_DRIVER);
        memory_listener_unregister(&v->listener);

        return 0;
    }
}

static int vhost_vdpa_set_log_base(struct vhost_dev *dev, uint64_t base,
                                     struct vhost_log *log)
{
    if (vhost_vdpa_one_time_request(dev)) {
        return 0;
    }

    trace_vhost_vdpa_set_log_base(dev, base, log->size, log->refcnt, log->fd,
                                  log->log);
    return vhost_vdpa_call(dev, VHOST_SET_LOG_BASE, &base);
}

static int vhost_vdpa_set_vring_addr(struct vhost_dev *dev,
                                       struct vhost_vring_addr *addr)
{
    struct vhost_vdpa *v = dev->opaque;

    if (v->shadow_vqs_enabled) {
        /*
         * Device vring addr was set at device start. SVQ base is handled by
         * VirtQueue code.
         */
        return 0;
    }

    return vhost_vdpa_set_vring_dev_addr(dev, addr);
}

static int vhost_vdpa_set_vring_num(struct vhost_dev *dev,
                                      struct vhost_vring_state *ring)
{
    trace_vhost_vdpa_set_vring_num(dev, ring->index, ring->num);
    return vhost_vdpa_call(dev, VHOST_SET_VRING_NUM, ring);
}

static int vhost_vdpa_set_vring_base(struct vhost_dev *dev,
                                       struct vhost_vring_state *ring)
{
    struct vhost_vdpa *v = dev->opaque;

    if (v->shadow_vqs_enabled) {
        /*
         * Device vring base was set at device start. SVQ base is handled by
         * VirtQueue code.
         */
        return 0;
    }

    return vhost_vdpa_set_dev_vring_base(dev, ring);
}

static int vhost_vdpa_get_vring_base(struct vhost_dev *dev,
                                       struct vhost_vring_state *ring)
{
    int ret;

    ret = vhost_vdpa_call(dev, VHOST_GET_VRING_BASE, ring);
    trace_vhost_vdpa_get_vring_base(dev, ring->index, ring->num);
    return ret;
}

static int vhost_vdpa_set_vring_kick(struct vhost_dev *dev,
                                       struct vhost_vring_file *file)
{
    struct vhost_vdpa *v = dev->opaque;
    int vdpa_idx = file->index - dev->vq_index;

    if (v->shadow_vqs_enabled) {
        VhostShadowVirtqueue *svq = g_ptr_array_index(v->shadow_vqs, vdpa_idx);
        vhost_svq_set_svq_kick_fd(svq, file->fd);
        return 0;
    } else {
        return vhost_vdpa_set_vring_dev_kick(dev, file);
    }
}

static int vhost_vdpa_set_vring_call(struct vhost_dev *dev,
                                       struct vhost_vring_file *file)
{
    struct vhost_vdpa *v = dev->opaque;

    if (v->shadow_vqs_enabled) {
        int vdpa_idx = file->index - dev->vq_index;
        VhostShadowVirtqueue *svq = g_ptr_array_index(v->shadow_vqs, vdpa_idx);

        vhost_svq_set_svq_call_fd(svq, file->fd);
        return 0;
    } else {
        return vhost_vdpa_set_vring_dev_call(dev, file);
    }
}

static int vhost_vdpa_get_features(struct vhost_dev *dev,
                                     uint64_t *features)
{
    int ret;

    ret = vhost_vdpa_call(dev, VHOST_GET_FEATURES, features);
    trace_vhost_vdpa_get_features(dev, *features);
    return ret;
}

static int vhost_vdpa_set_owner(struct vhost_dev *dev)
{
    if (vhost_vdpa_one_time_request(dev)) {
        return 0;
    }

    trace_vhost_vdpa_set_owner(dev);
    return vhost_vdpa_call(dev, VHOST_SET_OWNER, NULL);
}

static int vhost_vdpa_vq_get_addr(struct vhost_dev *dev,
                    struct vhost_vring_addr *addr, struct vhost_virtqueue *vq)
{
    assert(dev->vhost_ops->backend_type == VHOST_BACKEND_TYPE_VDPA);
    addr->desc_user_addr = (uint64_t)(unsigned long)vq->desc_phys;
    addr->avail_user_addr = (uint64_t)(unsigned long)vq->avail_phys;
    addr->used_user_addr = (uint64_t)(unsigned long)vq->used_phys;
    trace_vhost_vdpa_vq_get_addr(dev, vq, addr->desc_user_addr,
                                 addr->avail_user_addr, addr->used_user_addr);
    return 0;
}

static bool  vhost_vdpa_force_iommu(struct vhost_dev *dev)
{
    return true;
}

const VhostOps vdpa_ops = {
        .backend_type = VHOST_BACKEND_TYPE_VDPA,
        .vhost_backend_init = vhost_vdpa_init,
        .vhost_backend_cleanup = vhost_vdpa_cleanup,
        .vhost_set_log_base = vhost_vdpa_set_log_base,
        .vhost_set_vring_addr = vhost_vdpa_set_vring_addr,
        .vhost_set_vring_num = vhost_vdpa_set_vring_num,
        .vhost_set_vring_base = vhost_vdpa_set_vring_base,
        .vhost_get_vring_base = vhost_vdpa_get_vring_base,
        .vhost_set_vring_kick = vhost_vdpa_set_vring_kick,
        .vhost_set_vring_call = vhost_vdpa_set_vring_call,
        .vhost_get_features = vhost_vdpa_get_features,
        .vhost_set_backend_cap = vhost_vdpa_set_backend_cap,
        .vhost_set_owner = vhost_vdpa_set_owner,
        .vhost_set_vring_endian = NULL,
        .vhost_backend_memslots_limit = vhost_vdpa_memslots_limit,
        .vhost_set_mem_table = vhost_vdpa_set_mem_table,
        .vhost_set_features = vhost_vdpa_set_features,
        .vhost_reset_device = vhost_vdpa_reset_device,
        .vhost_get_vq_index = vhost_vdpa_get_vq_index,
        .vhost_get_config  = vhost_vdpa_get_config,
        .vhost_set_config = vhost_vdpa_set_config,
        .vhost_requires_shm_log = NULL,
        .vhost_migration_done = NULL,
        .vhost_backend_can_merge = NULL,
        .vhost_net_set_mtu = NULL,
        .vhost_set_iotlb_callback = NULL,
        .vhost_send_device_iotlb_msg = NULL,
        .vhost_dev_start = vhost_vdpa_dev_start,
        .vhost_get_device_id = vhost_vdpa_get_device_id,
        .vhost_vq_get_addr = vhost_vdpa_vq_get_addr,
        .vhost_force_iommu = vhost_vdpa_force_iommu,
};
