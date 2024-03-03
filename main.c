#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

bool main_loop_active = true;

const char *conn_str(uint32_t conn_type)
{
    switch (conn_type)
    {
    case DRM_MODE_CONNECTOR_Unknown:
        return "Unknown";
    case DRM_MODE_CONNECTOR_VGA:
        return "VGA";
    case DRM_MODE_CONNECTOR_DVII:
        return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID:
        return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA:
        return "DVI-A";
    case DRM_MODE_CONNECTOR_Composite:
        return "Composite";
    case DRM_MODE_CONNECTOR_SVIDEO:
        return "SVIDEO";
    case DRM_MODE_CONNECTOR_LVDS:
        return "LVDS";
    case DRM_MODE_CONNECTOR_Component:
        return "Component";
    case DRM_MODE_CONNECTOR_9PinDIN:
        return "DIN";
    case DRM_MODE_CONNECTOR_DisplayPort:
        return "DP";
    case DRM_MODE_CONNECTOR_HDMIA:
        return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:
        return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV:
        return "TV";
    case DRM_MODE_CONNECTOR_eDP:
        return "eDP";
    case DRM_MODE_CONNECTOR_VIRTUAL:
        return "Virtual";
    case DRM_MODE_CONNECTOR_DSI:
        return "DSI";
    default:
        return "Unknown";
    }
}

struct connector
{
    uint32_t id;
    char name[16];
    bool connected;

    drmModeCrtc *saved;

    uint32_t crtc_id;
    drmModeModeInfo mode;

    uint32_t width;
    uint32_t height;
    uint32_t rate;

    uint32_t drm_fb_id;  
	struct drm_mode_create_dumb drm_fb;
	uint8_t *drm_fb_data;
    uint32_t drm_fb_pixel_format;
};

int refresh_rate(drmModeModeInfo *mode);

static uint32_t find_crtc(int drm_fd, drmModeRes *res, drmModeConnector *conn, uint32_t *taken_crtcs)
{
    for (int i = 0; i < conn->count_encoders; ++i)
    {
        drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoders[i]);
        if (!enc)
            continue;

        for (int i = 0; i < res->count_crtcs; ++i)
        {
            uint32_t bit = 1 << i;
            // Not compatible
            if ((enc->possible_crtcs & bit) == 0)
                continue;

            // Already taken
            if (*taken_crtcs & bit)
                continue;

            drmModeFreeEncoder(enc);
            *taken_crtcs |= bit;
            return res->crtcs[i];
        }

        drmModeFreeEncoder(enc);
    }

    return 0;
}

bool create_fb(int drm_fd, struct connector *conn)
{
    int ret;

    conn->drm_fb.width = conn->width;
    conn->drm_fb.height = conn->height;
    conn->drm_fb.bpp = 24;

    ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &conn->drm_fb);
    if (ret < 0)
    {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        return false;
    }

    printf("Created framebuffer: \n");
    printf("\t height:%d\n", conn->drm_fb.height);
    printf("\t width:%d\n", conn->drm_fb.width);
    printf("\t bpp:%d\n", conn->drm_fb.bpp);
    printf("\t flags:%d\n", conn->drm_fb.flags);
    printf("\t handle:%d\n", conn->drm_fb.handle);
    printf("\t pitch:%d\n", conn->drm_fb.pitch);
    printf("\t size:%d\n", conn->drm_fb.size);

    // ret = drmModeAddFB(drm_fd, 
    //     conn->drm_fb.width, 
    //     conn->drm_fb.height, 
    //     24, 
    //     conn->drm_fb.bpp, 
    //     conn->drm_fb.pitch, 
    //     conn->drm_fb.handle, 
    //     &conn->drm_fb_id);

    uint32_t handles[4] = { conn->drm_fb.handle };
	uint32_t pitches[4] = { conn->drm_fb.pitch };
	uint32_t offsets[4] = { 0 };

	ret = drmModeAddFB2(drm_fd, 
        conn->drm_fb.width, 
        conn->drm_fb.height, 
        DRM_FORMAT_BGR888,
		handles,
        pitches,
        offsets,
        &conn->drm_fb_id, 0);

	if (ret < 0) {
		perror("drmModeAddFB2");
		goto error_dumb;
	}

    if (ret)
    {
        printf("Could not add framebuffer to drm (err=%d)\n", ret);
        goto error_dumb;
    }

    struct drm_mode_map_dumb map = {.handle = conn->drm_fb.handle};

    ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    if (ret < 0)
    {
        perror("Mode map dumb framebuffer failed DRM_IOCTL_MODE_MAP_DUMB");
        goto error_fb;
    }

    conn->drm_fb_data = mmap(0, conn->drm_fb.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, map.offset);
    if (!conn->drm_fb_data)
    {
        perror("mmap failed");
        goto error_fb;
    }

    //Buffer represents white screen
    memset(conn->drm_fb_data, 0xff, conn->drm_fb.size);

    return true;

error_fb:
    drmModeRmFB(drm_fd, conn->drm_fb_id);
error_dumb:;
    struct drm_mode_destroy_dumb destroy = {.handle = conn->drm_fb.handle};

    drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    return false;
}

static void catch_function(int signo) 
{
    printf("Signal received: %s\n", strsignal(signo));
    main_loop_active = false;
}

static void dump_fourcc(uint32_t fourcc)
{
	printf(" %c%c%c%c",
		fourcc,
		fourcc >> 8,
		fourcc >> 16,
		fourcc >> 24);
}

int main(void)
{
    bool error_occured = false;
    if (signal(SIGINT, catch_function) == SIG_ERR)
    {
        fprintf(stderr, "An error occurred while setting a signal handler.\n", stderr);
        return EXIT_FAILURE;
    }

    char device_str[] = "/dev/dri/card0";
    int drm_fd = open(device_str, O_RDWR | O_NONBLOCK);
    if (drm_fd < 0)
    {
        perror(device_str);
        return 1;
    }

    //https://dri.freedesktop.org/docs/drm/gpu/drm-uapi.html
    //If set to 1, the DRM core will expose all planes (overlay, primary, and cursor) to userspace.
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    drmModeRes *res = drmModeGetResources(drm_fd);
    if (!res)
    {
        perror("drmModeGetResources");
        return 1;
    }

    uint32_t taken_crtcs = 0;

    printf("Connector count: %d \n", res->count_connectors);

    printf("Connector list:");
    for (int i = 0; i < res->count_connectors; ++i)
    {
        drmModeConnectorPtr connector = 0;
        drmModeEncoderPtr encoder = 0;

        printf("\nNumber: %d ", res->connectors[i]);
        connector = drmModeGetConnectorCurrent(drm_fd, res->connectors[i]);
        if (!connector)
            continue;

        printf("Name: %s-%u ", conn_str(connector->connector_type), connector->connector_type_id);
        printf("Encoder: %d ", connector->encoder_id);

        encoder = drmModeGetEncoder(drm_fd, connector->encoder_id);
        if (!encoder)
            continue;

        printf("Crtc: %d", encoder->crtc_id);

        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
    }

    printf("\nFramebuffers: ");
    for (int i = 0; i < res->count_fbs; i++)
    {
        printf("%d ", res->fbs[i]);
    }

    printf("\nCRTCs: ");
    for (int i = 0; i < res->count_crtcs; i++)
    {
        printf("%d ", res->crtcs[i]);
    }

    for (int i = 0; i < res->count_crtcs; i++)
    {
        drmModeCrtcPtr drm_crtc = drmModeGetCrtc(drm_fd, res->crtcs[i]);
        if (!drm_crtc)
        {
            printf("\n drmModeGetCrtc failed");
            continue;
        }
        printf("\nbuffer_id: %d, x: %d, y:%d, width: %d, height: %d, mode_valid: %d, gamma_size:%d", 
            drm_crtc->buffer_id,
            drm_crtc->x,
            drm_crtc->y,
            drm_crtc->width,
            drm_crtc->height,
            drm_crtc->mode_valid,
            drm_crtc->gamma_size
        );

        printf("\nmodeinfo clock:%d, vrefresh:%d, flags: %d, type: %d, name:%s",
            drm_crtc->mode.clock,
            drm_crtc->mode.vrefresh,
            drm_crtc->mode.flags,
            drm_crtc->mode.type,
            drm_crtc->mode.name
        );

        printf("\n hdisplay:%d, hsync_start: %d, hsync_end: %d, htotal:%d, hskew:%d",
            
            drm_crtc->mode.hdisplay,
            drm_crtc->mode.hsync_start,
            drm_crtc->mode.hsync_end,
            drm_crtc->mode.htotal,
            drm_crtc->mode.hskew
        );

        printf("\n vdisplay:%d, vsync_start:%d, vsync_end:%d, vtotal:%d, vscan:%d",
            drm_crtc->mode.vdisplay,
            drm_crtc->mode.vsync_start,
            drm_crtc->mode.vsync_end,
            drm_crtc->mode.vtotal,
            drm_crtc->mode.vscan
        );
    }

    printf("\nencoders: ");
    for (int i = 0; i < res->count_encoders; i++)
    {
        printf("%d ", res->encoders[i]);
    }

    drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm_fd);

    if (!plane_res)
    {
        perror("drmModeGetPlaneResources failed");
        return 1;
    }

    printf("\nPlanes(%d):", plane_res->count_planes);
    printf("\nid\tcrtc\tfb\tCRTC x,y\tx,y\tgamma size\tpossible crtcs");
    for (int i = 0; i < plane_res->count_planes; i++) 
    {
        uint32_t *plane_id_ptr = &plane_res->planes[i];
        drmModePlane *drm_plane = drmModeGetPlane(drm_fd , *plane_id_ptr);

		if (!drm_plane)
        {
            printf("\n get plane failed");
            continue;
        }

		printf("\n%d\t%d\t%d\t%d,%d\t\t%d,%d\t%-8d\t0x%08x",
		       drm_plane->plane_id, drm_plane->crtc_id, drm_plane->fb_id,
		       drm_plane->crtc_x, drm_plane->crtc_y, drm_plane->x, drm_plane->y,
		       drm_plane->gamma_size, drm_plane->possible_crtcs);

        if (!drm_plane->count_formats)
        {
            printf("\n plane formats not specified");
            continue;
        }

        printf("\n  formats:");
		for (int j = 0; j < drm_plane->count_formats; j++)
			dump_fourcc(drm_plane->formats[j]);	

        drmModeFreePlane(drm_plane);
    }

    drmModeFreePlaneResources(plane_res);

    struct connector *conn = NULL;
    conn = malloc(sizeof *conn);
    memset(conn, 0, sizeof *conn);

    if (!conn)
    {
        perror("malloc");
        goto exit;
    }

    for (int i = 0; i < res->count_connectors; ++i)
    {
        drmModeConnector *drm_conn = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (!drm_conn)
        {
            perror("drmModeGetConnector");
            continue;
        }

        printf("\nFound connector %s: ", conn_str(drm_conn->connector_type));

        if (drm_conn->connection == DRM_MODE_CONNECTED)
        {
            printf("Status: Connected\n");
        }
        else
        {
            printf("Status:  Disconnected\n");
            goto cleanup;
        }

        conn->id = drm_conn->connector_id;
        snprintf(conn->name, sizeof conn->name,
            "%s-%" PRIu32, conn_str(drm_conn->connector_type),
                 drm_conn->connector_type_id);
        conn->connected = true;

        if (drm_conn->count_modes == 0)
        {
            printf("No valid modes\n");
            conn->connected = false;
            goto cleanup;
        }

        conn->crtc_id = find_crtc(drm_fd, res, drm_conn, &taken_crtcs);
        if (!conn->crtc_id)
        {
            fprintf(stderr, "\033[31mCould not find CRTC for %s\033[0m\n", conn->name);
            conn->connected = false;
            goto cleanup;
        }

        printf("  Using CRTC %" PRIu32 "\n", conn->crtc_id);

        conn->mode = drm_conn->modes[0];

        conn->width = conn->mode.hdisplay;
        conn->height = conn->mode.vdisplay;
        conn->rate = refresh_rate(&conn->mode);

        printf("  Using mode %" PRIu32 "x%" PRIu32 "@%" PRIu32 "mHz\n", conn->width, conn->height, conn->rate);

        int ret = drmSetMaster(drm_fd);
        if (ret)
        {
            printf("\033[31mCould not get master role for DRM.\033[0m\n");
            error_occured = true;
            goto cleanup;
        }

        if (!create_fb(drm_fd, conn))
        {
            error_occured = true;
            goto cleanup;
        }

        printf("  Created frambuffer with ID %" PRIu32 "\n", conn->drm_fb_id);

        conn->saved = drmModeGetCrtc(drm_fd, conn->crtc_id);

        // ret = drmModeSetCrtc(drm_fd, conn->crtc_id, 0, 0, 0, NULL, 0, NULL);
        // if (ret < 0) {
        // 	printf("error null drmModeSetCrtc: %d\n", ret);
        // 	goto cleanup;
        // }

        ret = drmModeSetCrtc(drm_fd, conn->crtc_id, conn->drm_fb_id, 0, 0, &conn->id, 1, &conn->mode);
        if (ret < 0)
        {
            error_occured = true;
            printf("\033[31merror drmModeSetCrtc: %d\033[0m\n", ret);
            if (ret == -EINVAL)
            {
                printf("\t\033[31mcrtc_id is invalid: %d.\033[0m\n", conn->crtc_id);
            }
            else
            {
                perror("wtf");
            }

            goto cleanup;
        }

    cleanup:
        drmModeFreeConnector(drm_conn);
    }

    drmModeFreeResources(res);

    printf("\nSelected connector %s: ", conn->name);
    
    if (conn)
    {
        if (conn->connected)
        {

            for (uint32_t y = 0; y < conn->drm_fb.height; ++y)
            {
                uint8_t *new_line_pixel = conn->drm_fb_data + conn->drm_fb.pitch * y;
                for (uint32_t x = 0; x < conn->drm_fb.width; ++x)
                {
                    // DRM_FORMAT_XRGB8888
                    /// [31:0] x:R:G:B 8:8:8:8 little endian (info from drm_fourcc.h)
		    ///
		    //BGR888 = BG24
                    new_line_pixel[x * 3 + 0] = 0;//R
					new_line_pixel[x * 3 + 1] = 0; //G
					new_line_pixel[x * 3 + 2] = 0xFF; //B
					//new_line_pixel[x * 4 + 3] = 0;
                }
            }

            while (main_loop_active && error_occured == false)
            {

            }

            printf("Restoring connector\n");
            munmap(conn->drm_fb_data, conn->drm_fb.size);
            drmModeRmFB(drm_fd, conn->drm_fb_id);
            
            struct drm_mode_destroy_dumb destroy = {.handle = conn->drm_fb.handle};
            drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);

            drmModeCrtc *crtc = conn->saved;
            if (crtc)
            {
                drmModeSetCrtc(drm_fd, crtc->crtc_id, crtc->buffer_id, crtc->x, crtc->y, &conn->id, 1, &crtc->mode);
                drmModeFreeCrtc(crtc);
            }
            drmDropMaster(drm_fd);
        }

        
    }

    exit:

    free(conn);
    close(drm_fd);
}

int refresh_rate(drmModeModeInfo *mode)
{
    int res = (mode->clock * 1000000LL / mode->htotal + mode->vtotal / 2) / mode->vtotal;

    if (mode->flags & DRM_MODE_FLAG_INTERLACE)
        res *= 2;

    if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
        res /= 2;

    if (mode->vscan > 1)
        res /= mode->vscan;

    return res;
}
