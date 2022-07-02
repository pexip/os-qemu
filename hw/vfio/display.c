/*
 * display support for mdev based vgpu devices
 *
 * Copyright Red Hat, Inc. 2017
 *
 * Authors:
 *    Gerd Hoffmann
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include <linux/vfio.h>
#include <sys/ioctl.h>

#include "sysemu/sysemu.h"
#include "ui/console.h"
#include "qapi/error.h"
#include "pci.h"

#ifndef DRM_PLANE_TYPE_PRIMARY
# define DRM_PLANE_TYPE_PRIMARY 1
# define DRM_PLANE_TYPE_CURSOR  2
#endif

static void vfio_display_update_cursor(VFIODMABuf *dmabuf,
                                       struct vfio_device_gfx_plane_info *plane)
{
    if (dmabuf->pos_x != plane->x_pos || dmabuf->pos_y != plane->y_pos) {
        dmabuf->pos_x      = plane->x_pos;
        dmabuf->pos_y      = plane->y_pos;
        dmabuf->pos_updates++;
    }
    if (dmabuf->hot_x != plane->x_hot || dmabuf->hot_y != plane->y_hot) {
        dmabuf->hot_x      = plane->x_hot;
        dmabuf->hot_y      = plane->y_hot;
        dmabuf->hot_updates++;
    }
}

static VFIODMABuf *vfio_display_get_dmabuf(VFIOPCIDevice *vdev,
                                           uint32_t plane_type)
{
    VFIODisplay *dpy = vdev->dpy;
    struct vfio_device_gfx_plane_info plane;
    VFIODMABuf *dmabuf;
    int fd, ret;

    memset(&plane, 0, sizeof(plane));
    plane.argsz = sizeof(plane);
    plane.flags = VFIO_GFX_PLANE_TYPE_DMABUF;
    plane.drm_plane_type = plane_type;
    ret = ioctl(vdev->vbasedev.fd, VFIO_DEVICE_QUERY_GFX_PLANE, &plane);
    if (ret < 0) {
        return NULL;
    }
    if (!plane.drm_format || !plane.size) {
        return NULL;
    }

    QTAILQ_FOREACH(dmabuf, &dpy->dmabuf.bufs, next) {
        if (dmabuf->dmabuf_id == plane.dmabuf_id) {
            /* found in list, move to head, return it */
            QTAILQ_REMOVE(&dpy->dmabuf.bufs, dmabuf, next);
            QTAILQ_INSERT_HEAD(&dpy->dmabuf.bufs, dmabuf, next);
            if (plane_type == DRM_PLANE_TYPE_CURSOR) {
                vfio_display_update_cursor(dmabuf, &plane);
            }
            return dmabuf;
        }
    }

    fd = ioctl(vdev->vbasedev.fd, VFIO_DEVICE_GET_GFX_DMABUF, &plane.dmabuf_id);
    if (fd < 0) {
        return NULL;
    }

    dmabuf = g_new0(VFIODMABuf, 1);
    dmabuf->dmabuf_id  = plane.dmabuf_id;
    dmabuf->buf.width  = plane.width;
    dmabuf->buf.height = plane.height;
    dmabuf->buf.stride = plane.stride;
    dmabuf->buf.fourcc = plane.drm_format;
    dmabuf->buf.fd     = fd;
    if (plane_type == DRM_PLANE_TYPE_CURSOR) {
        vfio_display_update_cursor(dmabuf, &plane);
    }

    QTAILQ_INSERT_HEAD(&dpy->dmabuf.bufs, dmabuf, next);
    return dmabuf;
}

static void vfio_display_free_one_dmabuf(VFIODisplay *dpy, VFIODMABuf *dmabuf)
{
    QTAILQ_REMOVE(&dpy->dmabuf.bufs, dmabuf, next);
    dpy_gl_release_dmabuf(dpy->con, &dmabuf->buf);
    close(dmabuf->buf.fd);
    g_free(dmabuf);
}

static void vfio_display_free_dmabufs(VFIOPCIDevice *vdev)
{
    VFIODisplay *dpy = vdev->dpy;
    VFIODMABuf *dmabuf, *tmp;
    uint32_t keep = 5;

    QTAILQ_FOREACH_SAFE(dmabuf, &dpy->dmabuf.bufs, next, tmp) {
        if (keep > 0) {
            keep--;
            continue;
        }
        assert(dmabuf != dpy->dmabuf.primary);
        vfio_display_free_one_dmabuf(dpy, dmabuf);
    }
}

static void vfio_display_dmabuf_update(void *opaque)
{
    VFIOPCIDevice *vdev = opaque;
    VFIODisplay *dpy = vdev->dpy;
    VFIODMABuf *primary, *cursor;
    bool free_bufs = false, new_cursor = false;;

    primary = vfio_display_get_dmabuf(vdev, DRM_PLANE_TYPE_PRIMARY);
    if (primary == NULL) {
        if (dpy->ramfb) {
            ramfb_display_update(dpy->con, dpy->ramfb);
        }
        return;
    }

    if (dpy->dmabuf.primary != primary) {
        dpy->dmabuf.primary = primary;
        qemu_console_resize(dpy->con,
                            primary->buf.width, primary->buf.height);
        dpy_gl_scanout_dmabuf(dpy->con, &primary->buf);
        free_bufs = true;
    }

    cursor = vfio_display_get_dmabuf(vdev, DRM_PLANE_TYPE_CURSOR);
    if (dpy->dmabuf.cursor != cursor) {
        dpy->dmabuf.cursor = cursor;
        new_cursor = true;
        free_bufs = true;
    }

    if (cursor && (new_cursor || cursor->hot_updates)) {
        bool have_hot = (cursor->hot_x != 0xffffffff &&
                         cursor->hot_y != 0xffffffff);
        dpy_gl_cursor_dmabuf(dpy->con, &cursor->buf, have_hot,
                             cursor->hot_x, cursor->hot_y);
        cursor->hot_updates = 0;
    } else if (!cursor && new_cursor) {
        dpy_gl_cursor_dmabuf(dpy->con, NULL, false, 0, 0);
    }

    if (cursor && cursor->pos_updates) {
        dpy_gl_cursor_position(dpy->con,
                               cursor->pos_x,
                               cursor->pos_y);
        cursor->pos_updates = 0;
    }

    dpy_gl_update(dpy->con, 0, 0, primary->buf.width, primary->buf.height);

    if (free_bufs) {
        vfio_display_free_dmabufs(vdev);
    }
}

static const GraphicHwOps vfio_display_dmabuf_ops = {
    .gfx_update = vfio_display_dmabuf_update,
};

static int vfio_display_dmabuf_init(VFIOPCIDevice *vdev, Error **errp)
{
    if (!display_opengl) {
        error_setg(errp, "vfio-display-dmabuf: opengl not available");
        return -1;
    }

    vdev->dpy = g_new0(VFIODisplay, 1);
    vdev->dpy->con = graphic_console_init(DEVICE(vdev), 0,
                                          &vfio_display_dmabuf_ops,
                                          vdev);
    if (vdev->enable_ramfb) {
        vdev->dpy->ramfb = ramfb_setup(errp);
    }
    return 0;
}

static void vfio_display_dmabuf_exit(VFIODisplay *dpy)
{
    VFIODMABuf *dmabuf;

    if (QTAILQ_EMPTY(&dpy->dmabuf.bufs)) {
        return;
    }

    while ((dmabuf = QTAILQ_FIRST(&dpy->dmabuf.bufs)) != NULL) {
        vfio_display_free_one_dmabuf(dpy, dmabuf);
    }
}

/* ---------------------------------------------------------------------- */
void vfio_display_reset(VFIOPCIDevice *vdev)
{
    if (!vdev || !vdev->dpy || !vdev->dpy->con ||
        !vdev->dpy->dmabuf.primary) {
        return;
    }

    dpy_gl_scanout_disable(vdev->dpy->con);
    vfio_display_dmabuf_exit(vdev->dpy);
    dpy_gfx_update_full(vdev->dpy->con);
}

static void vfio_display_region_update(void *opaque)
{
    VFIOPCIDevice *vdev = opaque;
    VFIODisplay *dpy = vdev->dpy;
    struct vfio_device_gfx_plane_info plane = {
        .argsz = sizeof(plane),
        .flags = VFIO_GFX_PLANE_TYPE_REGION
    };
    pixman_format_code_t format;
    int ret;

    ret = ioctl(vdev->vbasedev.fd, VFIO_DEVICE_QUERY_GFX_PLANE, &plane);
    if (ret < 0) {
        error_report("ioctl VFIO_DEVICE_QUERY_GFX_PLANE: %s",
                     strerror(errno));
        return;
    }
    if (!plane.drm_format || !plane.size) {
        if (dpy->ramfb) {
            ramfb_display_update(dpy->con, dpy->ramfb);
        }
        return;
    }
    format = qemu_drm_format_to_pixman(plane.drm_format);
    if (!format) {
        return;
    }

    if (dpy->region.buffer.size &&
        dpy->region.buffer.nr != plane.region_index) {
        /* region changed */
        vfio_region_exit(&dpy->region.buffer);
        vfio_region_finalize(&dpy->region.buffer);
        dpy->region.surface = NULL;
    }

    if (dpy->region.surface &&
        (surface_width(dpy->region.surface) != plane.width ||
         surface_height(dpy->region.surface) != plane.height ||
         surface_format(dpy->region.surface) != format)) {
        /* size changed */
        dpy->region.surface = NULL;
    }

    if (!dpy->region.buffer.size) {
        /* mmap region */
        ret = vfio_region_setup(OBJECT(vdev), &vdev->vbasedev,
                                &dpy->region.buffer,
                                plane.region_index,
                                "display");
        if (ret != 0) {
            error_report("%s: vfio_region_setup(%d): %s",
                         __func__, plane.region_index, strerror(-ret));
            goto err;
        }
        ret = vfio_region_mmap(&dpy->region.buffer);
        if (ret != 0) {
            error_report("%s: vfio_region_mmap(%d): %s", __func__,
                         plane.region_index, strerror(-ret));
            goto err;
        }
        assert(dpy->region.buffer.mmaps[0].mmap != NULL);
    }

    if (dpy->region.surface == NULL) {
        /* create surface */
        dpy->region.surface = qemu_create_displaysurface_from
            (plane.width, plane.height, format,
             plane.stride, dpy->region.buffer.mmaps[0].mmap);
        dpy_gfx_replace_surface(dpy->con, dpy->region.surface);
    }

    /* full screen update */
    dpy_gfx_update(dpy->con, 0, 0,
                   surface_width(dpy->region.surface),
                   surface_height(dpy->region.surface));
    return;

err:
    vfio_region_exit(&dpy->region.buffer);
    vfio_region_finalize(&dpy->region.buffer);
}

static const GraphicHwOps vfio_display_region_ops = {
    .gfx_update = vfio_display_region_update,
};

static int vfio_display_region_init(VFIOPCIDevice *vdev, Error **errp)
{
    vdev->dpy = g_new0(VFIODisplay, 1);
    vdev->dpy->con = graphic_console_init(DEVICE(vdev), 0,
                                          &vfio_display_region_ops,
                                          vdev);
    if (vdev->enable_ramfb) {
        vdev->dpy->ramfb = ramfb_setup(errp);
    }
    return 0;
}

static void vfio_display_region_exit(VFIODisplay *dpy)
{
    if (!dpy->region.buffer.size) {
        return;
    }

    vfio_region_exit(&dpy->region.buffer);
    vfio_region_finalize(&dpy->region.buffer);
}

/* ---------------------------------------------------------------------- */

int vfio_display_probe(VFIOPCIDevice *vdev, Error **errp)
{
    struct vfio_device_gfx_plane_info probe;
    int ret;

    memset(&probe, 0, sizeof(probe));
    probe.argsz = sizeof(probe);
    probe.flags = VFIO_GFX_PLANE_TYPE_PROBE | VFIO_GFX_PLANE_TYPE_DMABUF;
    ret = ioctl(vdev->vbasedev.fd, VFIO_DEVICE_QUERY_GFX_PLANE, &probe);
    if (ret == 0) {
        return vfio_display_dmabuf_init(vdev, errp);
    }

    memset(&probe, 0, sizeof(probe));
    probe.argsz = sizeof(probe);
    probe.flags = VFIO_GFX_PLANE_TYPE_PROBE | VFIO_GFX_PLANE_TYPE_REGION;
    ret = ioctl(vdev->vbasedev.fd, VFIO_DEVICE_QUERY_GFX_PLANE, &probe);
    if (ret == 0) {
        return vfio_display_region_init(vdev, errp);
    }

    if (vdev->display == ON_OFF_AUTO_AUTO) {
        /* not an error in automatic mode */
        return 0;
    }

    error_setg(errp, "vfio: device doesn't support any (known) display method");
    return -1;
}

void vfio_display_finalize(VFIOPCIDevice *vdev)
{
    if (!vdev->dpy) {
        return;
    }

    graphic_console_close(vdev->dpy->con);
    vfio_display_dmabuf_exit(vdev->dpy);
    vfio_display_region_exit(vdev->dpy);
    g_free(vdev->dpy);
}
