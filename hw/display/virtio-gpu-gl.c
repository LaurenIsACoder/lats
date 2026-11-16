/*
 * Virtio GPU Device
 *
 * Copyright Red Hat, Inc. 2013-2014
 *
 * Authors:
 *     Dave Airlie <airlied@redhat.com>
 *     Gerd Hoffmann <kraxel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu/mmap-alloc.h"
#include "sysemu/cpus.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-bswap.h"
#include "hw/virtio/virtio-gpu-pixman.h"
#include "hw/qdev-properties.h"

#include <virglrenderer.h>

static bool virtio_gpu_gl_preserve_on_s3(VirtIOGPUGL *gl)
{
    BusState *bus = qdev_get_parent_bus(DEVICE(gl));
    DeviceState *transport = bus ? bus->parent : NULL;
    VirtIOGPU *g = VIRTIO_GPU(gl);

    return !virtio_gpu_blob_enabled(g->parent_obj.conf) &&
           !virtio_gpu_venus_enabled(g->parent_obj.conf) &&
           transport &&
           object_dynamic_cast(OBJECT(transport), TYPE_VIRTIO_PCI) &&
           (VIRTIO_PCI(transport)->flags &
            VIRTIO_PCI_FLAG_PM_NO_SOFT_RESET);
}

void virtio_gpu_gl_resume_complete(VirtIOGPUGL *gl)
{
    if (!gl->renderer_preserved) {
        return;
    }

    timer_del(gl->resume_timer);
    gl->renderer_preserved = false;
    gl->renderer_reset_s3_candidate = false;
    gl->wakeup_reset_expected = false;
}

static void virtio_gpu_gl_preserve_scanouts(VirtIOGPUGL *gl)
{
    int i;

    for (i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        gl->scanout_state[i].restore_pending |=
            gl->scanout_state[i].restore_candidate;
        gl->scanout_state[i].restore_candidate = false;
    }
}

static void virtio_gpu_gl_suspend(Notifier *notifier, void *data)
{
    VirtIOGPUGL *gl = container_of(notifier, VirtIOGPUGL,
                                    suspend_notifier);
    VirtIOGPU *g = VIRTIO_GPU(gl);
    bool reset_can_preserve = gl->renderer_state == RS_RESET ?
        gl->renderer_reset_s3_candidate : VIRTIO_DEVICE(gl)->reset_pending;

    if ((gl->renderer_state != RS_INITED &&
         gl->renderer_state != RS_RESET) || g->inflight ||
        g->parent_obj.renderer_blocked ||
        (!gl->renderer_preserved && !reset_can_preserve)) {
        return;
    }
    timer_del(gl->resume_timer);

    virtio_gpu_virgl_detach_backings(g);
    virtio_gpu_gl_preserve_scanouts(gl);
    gl->renderer_state = RS_INITED;
    gl->renderer_reset_s3_candidate = false;
    if (!gl->renderer_preserved && ++gl->renderer_generation == 0) {
        gl->renderer_generation = 1;
    }
    gl->renderer_preserved = true;
    gl->wakeup_reset_expected = true;
}

static void virtio_gpu_gl_resume_timer(void *opaque)
{
    VirtIOGPUGL *gl = opaque;

    if (VIRTIO_DEVICE(gl)->reset_pending || !gl->renderer_preserved) {
        return;
    }

    if (virtio_gpu_virgl_restore_scanouts(VIRTIO_GPU(gl), true)) {
        timer_mod(gl->resume_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
    }
}

static void virtio_gpu_gl_wakeup(Notifier *notifier, void *data)
{
    VirtIOGPUGL *gl = container_of(notifier, VirtIOGPUGL,
                                    wakeup_notifier);

    gl->wakeup_reset_expected = false;
    if (!gl->renderer_preserved) {
        return;
    }

    timer_mod(gl->resume_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
}

static void virtio_gpu_gl_update_cursor_data(VirtIOGPU *g,
                                             struct virtio_gpu_scanout *s,
                                             uint32_t resource_id)
{
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
    uint32_t width, height;
    uint32_t pixels, *data;

    if (gl->renderer_state != RS_INITED) {
        return;
    }

    data = virgl_renderer_get_cursor_data(resource_id, &width, &height);
    if (!data) {
        return;
    }

    if (width != s->current_cursor->width ||
        height != s->current_cursor->height) {
        free(data);
        return;
    }

    pixels = s->current_cursor->width * s->current_cursor->height;
    memcpy(s->current_cursor->data, data, pixels * sizeof(uint32_t));
    free(data);
}

static void virtio_gpu_gl_flushed(VirtIOGPUBase *b)
{
    VirtIOGPU *g = VIRTIO_GPU(b);

    if (VIRTIO_DEVICE(g)->reset_pending) {
        return;
    }

    virtio_gpu_process_cmdq(g);
    virtio_gpu_virgl_restore_scanouts(g, false);
}

static void virtio_gpu_gl_handle_ctrl(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    struct virtio_gpu_ctrl_command *cmd;

    if (!virtio_queue_ready(vq)) {
        return;
    }

    if (!virtio_gpu_virgl_update_render_state(g)) {
        return;
    }

    cmd = virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command));
    while (cmd) {
        cmd->vq = vq;
        cmd->error = 0;
        cmd->finished = false;
        QTAILQ_INSERT_TAIL(&g->cmdq, cmd, next);
        cmd = virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command));
    }

    virtio_gpu_process_cmdq(g);
    virtio_gpu_virgl_fence_poll(g);
    virtio_gpu_virgl_restore_scanouts(g, false);
}

static void virtio_gpu_gl_reset_scanout(VirtIOGPU *g)
{
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

    if (gl->renderer_state == RS_INITED) {
        virtio_gpu_virgl_reset_scanout(g);
    }
}

static void virtio_gpu_gl_reset(VirtIODevice *vdev)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(vdev);
    bool from_guest = qemu_in_vcpu_thread() || vdev->reset_pending;
    bool renderer_idle = g->inflight == 0 &&
                         !g->parent_obj.renderer_blocked;
    bool wakeup_reset = gl->wakeup_reset_expected &&
                        runstate_is_running() && !from_guest;
    bool preserve_renderer = gl->renderer_preserved && renderer_idle &&
                             (from_guest || wakeup_reset);
    bool s3_candidate = gl->power_notifiers_registered &&
                        from_guest && renderer_idle;
    bool keep_resources;
    int i;

    for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
        if (gl->scanout_state[i].valid &&
            g->parent_obj.scanout[i].resource_id ==
                gl->scanout_state[i].resource_id) {
            gl->scanout_state[i].restore_candidate = true;
        }
    }

    if (gl->renderer_state == RS_RESET) {
        keep_resources = gl->renderer_reset_s3_candidate && s3_candidate;
    } else {
        keep_resources = gl->renderer_state == RS_INITED &&
                         (s3_candidate || preserve_renderer);
    }

    g->reset_preserve_resources = keep_resources;
    virtio_gpu_reset(vdev);
    g->reset_preserve_resources = false;

    if (wakeup_reset) {
        gl->wakeup_reset_expected = false;
    }
    if (gl->renderer_state == RS_START ||
        gl->renderer_state == RS_INIT_FAILED) {
        return;
    }
    if (gl->renderer_state == RS_RESET) {
        gl->renderer_reset_s3_candidate &= s3_candidate;
        return;
    }
    if (preserve_renderer) {
        virtio_gpu_gl_preserve_scanouts(gl);
        timer_mod(gl->resume_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
        return;
    }

    timer_del(gl->resume_timer);
    gl->renderer_preserved = false;
    gl->wakeup_reset_expected = false;
    gl->renderer_state = RS_RESET;
    gl->renderer_reset_s3_candidate = s3_candidate;
}

static void virtio_gpu_gl_device_realize(DeviceState *qdev, Error **errp)
{
    ERRP_GUARD();
    VirtIOGPU *g = VIRTIO_GPU(qdev);
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(qdev);
#if defined(HAVE_VIRGL_RESOURCE_MAP_FIXED) && !defined(CONFIG_WIN32)
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(qdev);
    void *map;
#endif

#if HOST_BIG_ENDIAN
    error_setg(errp, "virgl is not supported on bigendian platforms");
    return;
#endif

    if (!object_resolve_path_type("", TYPE_VIRTIO_GPU_GL, NULL)) {
        error_setg(errp, "at most one %s device is permitted", TYPE_VIRTIO_GPU_GL);
        return;
    }

    if (!display_opengl) {
        error_setg(errp,
                   "The display backend does not have OpenGL support enabled");
        error_append_hint(errp,
                          "It can be enabled with '-display BACKEND,gl=on' "
                          "where BACKEND is the name of the display backend "
                          "to use.\n");
        return;
    }

    g->parent_obj.conf.flags |= (1 << VIRTIO_GPU_FLAG_VIRGL_ENABLED);
    g->capset_ids = virtio_gpu_virgl_get_capsets(g);
    VIRTIO_GPU_BASE(g)->virtio_config.num_capsets = g->capset_ids->len;

#if VIRGL_VERSION_MAJOR >= 1
    g->parent_obj.conf.flags |= 1 << VIRTIO_GPU_FLAG_CONTEXT_INIT_ENABLED;
#endif

    if (gl->external_blob &&
        !virtio_gpu_hostmem_enabled(g->parent_obj.conf)) {
        error_setg(errp, "external-blob requires blob hostmem");
        return;
    }

#if defined(HAVE_VIRGL_RESOURCE_MAP_FIXED) && !defined(CONFIG_WIN32)
    if (virtio_gpu_hostmem_enabled(b->conf)) {
        map = qemu_ram_mmap(-1, b->conf.hostmem, qemu_real_host_page_size(),
                            0, 0);
        if (map == MAP_FAILED) {
            error_setg_errno(errp, errno,
                             "virgl hostmem region could not be initialized");
            return;
        }

        gl->hostmem_mmap = map;
        memory_region_init_ram_ptr(&gl->hostmem_background, NULL,
                                   "hostmem-background", b->conf.hostmem,
                                   gl->hostmem_mmap);
        memory_region_add_subregion(&b->hostmem, 0, &gl->hostmem_background);
    }
#else
    if (gl->external_blob) {
        error_setg(errp, "external-blob requires virglrenderer map_fixed");
        return;
    }
#endif

    virtio_gpu_device_realize(qdev, errp);
    if (*errp) {
        return;
    }

    gl->renderer_contexts =
        g_hash_table_new(g_direct_hash, g_direct_equal);
    gl->renderer_generation = 1;
    gl->resume_timer =
        timer_new_ms(QEMU_CLOCK_VIRTUAL, virtio_gpu_gl_resume_timer,
                     gl);
    if (virtio_gpu_gl_preserve_on_s3(gl)) {
        gl->suspend_notifier.notify = virtio_gpu_gl_suspend;
        gl->wakeup_notifier.notify = virtio_gpu_gl_wakeup;
        qemu_register_suspend_notifier(&gl->suspend_notifier);
        qemu_register_wakeup_notifier(&gl->wakeup_notifier);
        gl->power_notifiers_registered = true;
    }
}

static Property virtio_gpu_gl_properties[] = {
    DEFINE_PROP_BIT("stats", VirtIOGPU, parent_obj.conf.flags,
                    VIRTIO_GPU_FLAG_STATS_ENABLED, false),
    DEFINE_PROP_BIT("venus", VirtIOGPU, parent_obj.conf.flags,
                    VIRTIO_GPU_FLAG_VENUS_ENABLED, false),
    DEFINE_PROP_BOOL("external-blob", VirtIOGPUGL, external_blob, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_gpu_gl_device_unrealize(DeviceState *qdev)
{
    VirtIOGPU *g = VIRTIO_GPU(qdev);
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(qdev);

    if (gl->power_notifiers_registered) {
        notifier_remove(&gl->wakeup_notifier);
        notifier_remove(&gl->suspend_notifier);
        gl->power_notifiers_registered = false;
    }
    g_clear_pointer(&gl->resume_timer, timer_free);

    if (gl->renderer_state == RS_INITED ||
        gl->renderer_state == RS_RESET) {
        if (!virtio_gpu_blob_enabled(g->parent_obj.conf)) {
            virtio_gpu_virgl_detach_backings(g);
            virtio_gpu_reset_resources(g);
        }
#if VIRGL_VERSION_MAJOR >= 1
        g_clear_pointer(&gl->cmdq_resume_bh, qemu_bh_delete);
#endif
        g_clear_pointer(&gl->print_stats, timer_free);
        g_clear_pointer(&gl->fence_poll, timer_free);
        virgl_renderer_cleanup(NULL);
    }

    gl->renderer_state = RS_START;
    g_clear_pointer(&gl->renderer_contexts, g_hash_table_destroy);

    g_array_unref(g->capset_ids);
    virtio_gpu_device_unrealize(qdev);
}

static void virtio_gpu_gl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    VirtIOGPUBaseClass *vbc = VIRTIO_GPU_BASE_CLASS(klass);
    VirtIOGPUClass *vgc = VIRTIO_GPU_CLASS(klass);

    vbc->gl_flushed = virtio_gpu_gl_flushed;
    vgc->handle_ctrl = virtio_gpu_gl_handle_ctrl;
    vgc->process_cmd = virtio_gpu_virgl_process_cmd;
    vgc->reset_scanout = virtio_gpu_gl_reset_scanout;
    vgc->update_cursor_data = virtio_gpu_gl_update_cursor_data;
    vgc->resource_destroy = virtio_gpu_virgl_resource_destroy;

    vdc->realize = virtio_gpu_gl_device_realize;
    vdc->unrealize = virtio_gpu_gl_device_unrealize;
    vdc->reset = virtio_gpu_gl_reset;
    vdc->defer_modern_guest_reset = true;
    device_class_set_props(dc, virtio_gpu_gl_properties);
}

static const TypeInfo virtio_gpu_gl_info = {
    .name = TYPE_VIRTIO_GPU_GL,
    .parent = TYPE_VIRTIO_GPU,
    .instance_size = sizeof(VirtIOGPUGL),
    .class_init = virtio_gpu_gl_class_init,
};
module_obj(TYPE_VIRTIO_GPU_GL);
module_kconfig(VIRTIO_GPU);

static void virtio_register_types(void)
{
    type_register_static(&virtio_gpu_gl_info);
}

type_init(virtio_register_types)

module_dep("hw-display-virtio-gpu");
module_dep("ui-opengl");
