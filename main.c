#include <drm_fourcc.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <errno.h>


const char *conn_str(uint32_t conn_type)
{
	switch (conn_type) {
	case DRM_MODE_CONNECTOR_Unknown:     return "Unknown";
	case DRM_MODE_CONNECTOR_VGA:         return "VGA";
	case DRM_MODE_CONNECTOR_DVII:        return "DVI-I";
	case DRM_MODE_CONNECTOR_DVID:        return "DVI-D";
	case DRM_MODE_CONNECTOR_DVIA:        return "DVI-A";
	case DRM_MODE_CONNECTOR_Composite:   return "Composite";
	case DRM_MODE_CONNECTOR_SVIDEO:      return "SVIDEO";
	case DRM_MODE_CONNECTOR_LVDS:        return "LVDS";
	case DRM_MODE_CONNECTOR_Component:   return "Component";
	case DRM_MODE_CONNECTOR_9PinDIN:     return "DIN";
	case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
	case DRM_MODE_CONNECTOR_HDMIA:       return "HDMI-A";
	case DRM_MODE_CONNECTOR_HDMIB:       return "HDMI-B";
	case DRM_MODE_CONNECTOR_TV:          return "TV";
	case DRM_MODE_CONNECTOR_eDP:         return "eDP";
	case DRM_MODE_CONNECTOR_VIRTUAL:     return "Virtual";
	case DRM_MODE_CONNECTOR_DSI:         return "DSI";
	default:                             return "Unknown";
	}
}

struct dumb_framebuffer 
{
	uint32_t id; 
	uint32_t width;
	uint32_t height;
	uint32_t handle;  
	uint64_t size;   

	uint8_t *data;    
};

struct connector {
	uint32_t id;
	char name[16];
	bool connected;

	drmModeCrtc *saved;

	uint32_t crtc_id;
	drmModeModeInfo mode;

	uint32_t width;
	uint32_t height;
	uint32_t rate;

	struct dumb_framebuffer fb;
};

int refresh_rate(drmModeModeInfo *mode);

static uint32_t find_crtc(int drm_fd, drmModeRes *res, drmModeConnector *conn,
		uint32_t *taken_crtcs)
{
	for (int i = 0; i < conn->count_encoders; ++i) {
		drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoders[i]);
		if (!enc)
			continue;

		for (int i = 0; i < res->count_crtcs; ++i) {
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

bool create_fb(int drm_fd, uint32_t width, uint32_t height, struct dumb_framebuffer *fb)
{
	int ret;

	struct drm_mode_create_dumb created_buffer = {
		.width = width,
		.height = height,
		.bpp = 32,
	};

	ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &created_buffer);
	if (ret < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return false;
	}

	fb->height = created_buffer.height;
	fb->width = created_buffer.width;
	fb->handle = created_buffer.handle;
	fb->size = created_buffer.size;

	ret = drmModeAddFB(drm_fd, width, height, 24, 32,
		created_buffer.pitch,  created_buffer.handle, &fb->id);
    if (ret) {
        printf("Could not add framebuffer to drm (err=%d)\n", ret);
        goto error_dumb;
    }

	struct drm_mode_map_dumb map = { .handle = fb->handle };
	ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	if (ret < 0) {
		perror("Mode map dumb framebuffer failed DRM_IOCTL_MODE_MAP_DUMB");
		goto error_fb;
	}

	fb->data = mmap(0, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		drm_fd, map.offset);
	if (!fb->data) {
		perror("mmap failed");
		goto error_fb;
	}

	memset(fb->data, 0xff, fb->size);

	return true;

error_fb:
	drmModeRmFB(drm_fd, fb->id);
error_dumb:
	;
	struct drm_mode_destroy_dumb destroy = { .handle = fb->handle };
	drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	return false;
}

int main(void)
{
	int drm_fd = open("/dev/dri/card1", O_RDWR | O_NONBLOCK);
	if (drm_fd < 0) 
	{
		perror("/dev/dri/card0");
		return 1;
	}

	drmModeRes *res = drmModeGetResources(drm_fd);
	if (!res) 
	{
		perror("drmModeGetResources");
		return 1;
	}

	struct connector *conn_list = NULL;
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

    printf("\nencoders: ");
    for (int i = 0; i < res->count_encoders; i++) 
	{
        printf("%d ", res->encoders[i]);
    }

	for (int i = 0; i < res->count_connectors; ++i) 
	{
		drmModeConnector *drm_conn = drmModeGetConnector(drm_fd, res->connectors[i]);
		if (!drm_conn) {
			perror("drmModeGetConnector");
			continue;
		}

		struct connector *conn = malloc(sizeof *conn);
		if (!conn) 
		{
			perror("malloc");
			goto cleanup;
		}

		conn->id = drm_conn->connector_id;
		snprintf(conn->name, sizeof conn->name, "%s-%"PRIu32,
			conn_str(drm_conn->connector_type),
			drm_conn->connector_type_id);
		conn->connected = drm_conn->connection == DRM_MODE_CONNECTED;

		printf("\nFound connector %s: ", conn->name);

		if (!conn->connected) {
			printf("Status:  Disconnected\n");
			goto cleanup;
		}
		else
		{
			printf("Status: Connected\n");
		}

		if (drm_conn->count_modes == 0) {
			printf("No valid modes\n");
			conn->connected = false;
			goto cleanup;
		}

		conn->crtc_id = find_crtc(drm_fd, res, drm_conn, &taken_crtcs);
		if (!conn->crtc_id) {
			fprintf(stderr, "\033[31mCould not find CRTC for %s\033[0m\n", conn->name);
			conn->connected = false;
			goto cleanup;
		}

		printf("  Using CRTC %"PRIu32"\n", conn->crtc_id);

		conn->mode = drm_conn->modes[0];

		conn->width = conn->mode.hdisplay;
		conn->height = conn->mode.vdisplay;
		conn->rate = refresh_rate(&conn->mode);

		printf("  Using mode %"PRIu32"x%"PRIu32"@%"PRIu32"mHz\n",
			conn->width, conn->height, conn->rate);

		int ret = drmSetMaster(drm_fd);
		if(ret)
		{
			printf("\033[31mCould not get master role for DRM.\033[0m\n");
			goto cleanup;
		}

		if (!create_fb(drm_fd, conn->width, conn->height, &conn->fb)) {
			conn->connected = false;
			goto cleanup;
		}

		// drmDropMaster(drm_fd);

		printf("  Created frambuffer with ID %"PRIu32"\n", conn->fb.id);

		conn->saved = drmModeGetCrtc(drm_fd, conn->crtc_id);

		// ret = drmModeSetCrtc(drm_fd, conn->crtc_id, 0, 0, 0, NULL, 0, NULL);
		// if (ret < 0) {
		// 	printf("error null drmModeSetCrtc: %d\n", ret);
		// 	goto cleanup;
		// }
		ret = drmModeSetCrtc(drm_fd, conn->crtc_id, conn->fb.id, 0, 0,
			&conn->id, 1, &conn->mode);
		if (ret < 0) {
			printf("\033[31merror drmModeSetCrtc: %d\033[0m\n", ret);
			if(ret == -EINVAL)
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
