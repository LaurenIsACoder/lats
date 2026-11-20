/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Direct KMS dumb-buffer workload for virtio-gpu system-mode testing. */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm.h>
#include <drm_mode.h>

#define DRM_MODE_CONNECTED 1

static double monotonic_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

static void *xcalloc(size_t count, size_t size)
{
    void *ptr = calloc(count, size);

    if (!ptr) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }
    return ptr;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/dri/card0";
    double duration = argc > 2 ? strtod(argv[2], NULL) : 30.0;
    struct drm_mode_card_res res = { 0 };
    struct drm_mode_get_connector conn = { 0 };
    struct drm_mode_get_encoder enc = { 0 };
    struct drm_mode_create_dumb create = { 0 };
    struct drm_mode_map_dumb map = { 0 };
    struct drm_mode_destroy_dumb destroy = { 0 };
    struct drm_mode_fb_cmd fb = { 0 };
    struct drm_mode_crtc crtc = { 0 };
    struct drm_mode_crtc saved_crtc = { 0 };
    struct drm_mode_fb_dirty_cmd dirty = { 0 };
    struct drm_mode_modeinfo *modes = NULL;
    uint32_t *connectors = NULL, *crtcs = NULL, *encoders = NULL;
    uint32_t *conn_encoders = NULL, *props = NULL;
    uint64_t *prop_values = NULL;
    uint32_t connector_id = 0, crtc_id = 0;
    uint8_t *pixels = MAP_FAILED;
    uint64_t frames = 0;
    double start, now;
    int fd = -1, ret = EXIT_FAILURE;
    uint32_t i;

    if (duration <= 0.0) {
        fprintf(stderr, "duration must be positive\n");
        return EXIT_FAILURE;
    }
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return EXIT_FAILURE;
    }

    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        perror("DRM_IOCTL_MODE_GETRESOURCES");
        goto out;
    }
    connectors = xcalloc(res.count_connectors, sizeof(*connectors));
    crtcs = xcalloc(res.count_crtcs, sizeof(*crtcs));
    encoders = xcalloc(res.count_encoders, sizeof(*encoders));
    res.connector_id_ptr = (uintptr_t)connectors;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encoders;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        perror("DRM_IOCTL_MODE_GETRESOURCES arrays");
        goto out;
    }

    for (i = 0; i < res.count_connectors; i++) {
        memset(&conn, 0, sizeof(conn));
        conn.connector_id = connectors[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) != 0) {
            continue;
        }
        fprintf(stderr, "connector=%u status=%u modes=%u encoder=%u\n",
                conn.connector_id, conn.connection, conn.count_modes,
                conn.encoder_id);
        if (conn.connection != DRM_MODE_CONNECTED || conn.count_modes == 0) {
            continue;
        }
        modes = xcalloc(conn.count_modes, sizeof(*modes));
        conn_encoders = xcalloc(conn.count_encoders, sizeof(*conn_encoders));
        props = xcalloc(conn.count_props, sizeof(*props));
        prop_values = xcalloc(conn.count_props, sizeof(*prop_values));
        conn.modes_ptr = (uintptr_t)modes;
        conn.encoders_ptr = (uintptr_t)conn_encoders;
        conn.props_ptr = (uintptr_t)props;
        conn.prop_values_ptr = (uintptr_t)prop_values;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) == 0) {
            connector_id = conn.connector_id;
            break;
        }
        free(modes);
        free(conn_encoders);
        free(props);
        free(prop_values);
        modes = NULL;
        conn_encoders = NULL;
        props = NULL;
        prop_values = NULL;
    }
    if (!connector_id) {
        fprintf(stderr, "no connected DRM connector\n");
        goto out;
    }

    enc.encoder_id = conn.encoder_id;
    if (enc.encoder_id && ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0) {
        crtc_id = enc.crtc_id;
        if (!crtc_id) {
            for (i = 0; i < res.count_crtcs; i++) {
                if (enc.possible_crtcs & (1u << i)) {
                    crtc_id = crtcs[i];
                    break;
                }
            }
        }
    }
    if (!crtc_id && res.count_crtcs) {
        crtc_id = crtcs[0];
    }
    if (!crtc_id) {
        fprintf(stderr, "no usable DRM CRTC\n");
        goto out;
    }
    saved_crtc.crtc_id = crtc_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &saved_crtc) != 0) {
        perror("DRM_IOCTL_MODE_GETCRTC");
        goto out;
    }

    create.width = modes[0].hdisplay;
    create.height = modes[0].vdisplay;
    create.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        goto out;
    }
    fb.width = create.width;
    fb.height = create.height;
    fb.pitch = create.pitch;
    fb.bpp = 32;
    fb.depth = 24;
    fb.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) != 0) {
        perror("DRM_IOCTL_MODE_ADDFB");
        goto out_destroy;
    }
    map.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        goto out_rmfb;
    }
    pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  fd, map.offset);
    if (pixels == MAP_FAILED) {
        perror("mmap dumb buffer");
        goto out_rmfb;
    }

    crtc.crtc_id = crtc_id;
    crtc.fb_id = fb.fb_id;
    crtc.set_connectors_ptr = (uintptr_t)&connector_id;
    crtc.count_connectors = 1;
    crtc.mode = modes[0];
    crtc.mode_valid = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) != 0) {
        perror("DRM_IOCTL_MODE_SETCRTC");
        goto out_unmap;
    }

    dirty.fb_id = fb.fb_id;
    printf("drm=%s connector=%u crtc=%u mode=%ux%u pitch=%u bytes=%llu "
           "duration=%.3f\n", path, connector_id, crtc_id, create.width,
           create.height, create.pitch, (unsigned long long)create.size,
           duration);
    start = monotonic_seconds();
    do {
        uint32_t color = 0x00101010u |
                         ((frames * 3u) & 0xffu) << 16 |
                         ((frames * 5u) & 0xffu) << 8 |
                         ((frames * 7u) & 0xffu);
        uint32_t *dst = (uint32_t *)pixels;
        size_t count = create.size / sizeof(*dst);
        size_t p;

        for (p = 0; p < count; p++) {
            dst[p] = color ^ ((uint32_t)(p >> 8) * 0x00010101u);
        }
        if (ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty) != 0 &&
            errno != ENOSYS && errno != EINVAL) {
            perror("DRM_IOCTL_MODE_DIRTYFB");
            goto out_unmap;
        }
        frames++;
        now = monotonic_seconds();
    } while (now - start < duration);

    printf("frames=%llu elapsed=%.6f fps=%.3f write_mib_s=%.3f\n",
           (unsigned long long)frames, now - start, frames / (now - start),
           frames * (double)create.size / (1024.0 * 1024.0) /
           (now - start));
    ret = EXIT_SUCCESS;

out_unmap:
    saved_crtc.set_connectors_ptr = (uintptr_t)&connector_id;
    saved_crtc.count_connectors = saved_crtc.mode_valid ? 1 : 0;
    ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &saved_crtc);
    munmap(pixels, create.size);
out_rmfb:
    ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb.fb_id);
out_destroy:
    destroy.handle = create.handle;
    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
out:
    free(prop_values);
    free(props);
    free(conn_encoders);
    free(modes);
    free(encoders);
    free(crtcs);
    free(connectors);
    close(fd);
    return ret;
}
