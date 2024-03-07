// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx logicore test pattern generator driver
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *
 * Author: Anatoliy Klymenko <anatoliy.klymenko@amd.com>
 *
 * This driver introduces support for the test CRTC based on AMD/Xilinx
 * Test Pattern Generator IP. The main goal of the driver is to enable
 * simplistic FPGA design that could be used to test FPGA CRTC to external
 * encoder IP connectivity.
 * Reference: https://docs.xilinx.com/r/en-US/pg103-v-tpg
 */

#include "xlnx_vtc.h"

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <linux/media-bus-format.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>
#include <linux/component.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <video/videomode.h>

#define DRIVER_NAME	"xlnx-tpg"
#define DRIVER_DESC	"Xilinx TPG DRM KMS Driver"
#define DRIVER_DATE	"20240307"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

#define XLNX_TPG_CONTROL		0x0000
#define XLNX_TPG_GLOBAL_IRQ_EN		0x0004
#define XLNX_TPG_IP_IRQ_EN		0x0008
#define XLNX_TPG_IP_IRQ_STATUS		0x000C
#define XLNX_TPG_ACTIVE_HEIGHT		0x0010
#define XLNX_TPG_ACTIVE_WIDTH		0x0018
#define XLNX_TPG_PATTERN_ID		0x0020
#define XLNX_TPG_COLOR_FORMAT		0x0040

#define XLNX_TPG_IP_IRQ_AP_DONE		BIT(0)

#define XLNX_TPG_START			BIT(0)
#define XLNX_TPG_AUTO_RESTART		BIT(7)

enum xlnx_tpg_pattern {
	XTPG_PAT_HORIZONTAL_RAMP = 0x1,
	XTPG_PAT_VERTICAL_RAMP,
	XTPG_PAT_TEMPORAL_RAMP,
	XTPG_PAT_SOLID_RED,
	XTPG_PAT_SOLID_GREEN,
	XTPG_PAT_SOLID_BLUE,
	XTPG_PAT_SOLID_BLACK,
	XTPG_PAT_SOLID_WHITE,
	XTPG_PAT_COLOR_BARS,
	XTPG_PAT_ZONE_PLATE,
	XTPG_PAT_TARTAN_COLOR_BARS,
	XTPG_PAT_CROSS_HATCH,
	XTPG_PAT_COLOR_SWEEP,
	XTPG_PAT_COMBO_RAMP,
	XTPG_PAT_CHECKER_BOARD,
	XTPG_PAT_DP_COLOR_RAMP,
	XTPG_PAT_DP_VERTICAL_LINES,
	XTPG_PAT_DP_COLOR_SQUARE,
};

static const struct drm_prop_enum_list xtpg_pattern_list[] = {
	{ XTPG_PAT_HORIZONTAL_RAMP, "horizontal-ramp" },
	{ XTPG_PAT_VERTICAL_RAMP, "vertical-ramp" },
	{ XTPG_PAT_TEMPORAL_RAMP, "temporal-ramp" },
	{ XTPG_PAT_SOLID_RED, "red" },
	{ XTPG_PAT_SOLID_GREEN, "green" },
	{ XTPG_PAT_SOLID_BLUE, "blue" },
	{ XTPG_PAT_SOLID_BLACK, "black" },
	{ XTPG_PAT_SOLID_WHITE, "white" },
	{ XTPG_PAT_COLOR_BARS, "color-bars" },
	{ XTPG_PAT_ZONE_PLATE, "zone-plate" },
	{ XTPG_PAT_TARTAN_COLOR_BARS, "tartan-color-bars" },
	{ XTPG_PAT_CROSS_HATCH, "cross-hatch" },
	{ XTPG_PAT_COLOR_SWEEP, "color-sweep" },
	{ XTPG_PAT_COMBO_RAMP, "combo-ramp" },
	{ XTPG_PAT_CHECKER_BOARD, "checker-board" },
	{ XTPG_PAT_DP_COLOR_RAMP, "dp-color-ramp" },
	{ XTPG_PAT_DP_VERTICAL_LINES, "dp-vertical-lines" },
	{ XTPG_PAT_DP_COLOR_SQUARE, "dp-color-square" },
};

enum xlnx_tpg_format {
	XTPG_FMT_RGB = 0x0,
	XTPG_FMT_YUV_444,
	XTPG_FMT_YUV_422,
	XTPG_FMT_YUV_420,
	XTPG_FMT_INVALID,
};

struct xlnx_tpg;

/**
 * struct xlnx_tpg_drm - TPG CRTC DRM/KMS data
 * @tpg: Back pointer to parent TPG
 * @dev: DRM device
 * @crtc: DRM CRTC
 * @plane: DRM primary plane
 * @encoder: DRM encoder
 * @connector: DRM connector
 * @pattern_prop: DRM property representing TPG video pattern
 * @event: Pending DRM VBLANK event
 */
struct xlnx_tpg_drm {
	struct xlnx_tpg *tpg;
	struct drm_device dev;
	struct drm_crtc crtc;
	struct drm_plane plane;
	struct drm_encoder encoder;
	struct drm_connector *connector;
	struct drm_property *pattern_prop;
	struct drm_pending_vblank_event *event;
};

/**
 * struct xlnx_tpg_drm - Test Pattern Generator data
 * @pdev: Platform device
 * @drm: TPG DRM data
 * @vtc: Video timing controller interface
 * @disp_bridge: DRM display bridge
 * @regs: Mapped TPG IP register space
 * @irq: TPG IRQ
 * @output_bus_format: Chosen TPG output bus format
 * @color_format: TPG color format
 */
struct xlnx_tpg {
	struct platform_device *pdev;
	struct xlnx_tpg_drm *drm;
	struct xlnx_vtc_iface *vtc;
	struct drm_bridge *disp_bridge;
	void __iomem *regs;
	int irq;
	u32 output_bus_format;
	enum xlnx_tpg_format color_format;
};

static inline struct xlnx_tpg *crtc_to_tpg(struct drm_crtc *crtc)
{
	return container_of(crtc, struct xlnx_tpg_drm, crtc)->tpg;
}

static inline struct xlnx_tpg *plane_to_tpg(struct drm_plane *plane)
{
	return container_of(plane, struct xlnx_tpg_drm, plane)->tpg;
}

static inline struct xlnx_tpg *encoder_to_tpg(struct drm_encoder *encoder)
{
	return container_of(encoder, struct xlnx_tpg_drm, encoder)->tpg;
}

struct xlnx_tpg_format_map {
	u32 bus_format;
	enum xlnx_tpg_format color_format;
};

/**
 * xlnx_tpg_bus_to_color_format - Map media bus format to TPG color format
 * @bus_format: Media bus format
 *
 * Return: TPG color format that matches @bus_format or XTPG_FMT_INVALID if
 * input media bus format is not supported
 */
static enum xlnx_tpg_format xlnx_tpg_bus_to_color_format(u32 bus_format)
{
	static const struct xlnx_tpg_format_map format_map[] = {
		{
			.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
			.color_format = XTPG_FMT_RGB,
		}, {
			.bus_format = MEDIA_BUS_FMT_RBG888_1X24,
			.color_format = XTPG_FMT_RGB,
		}, {
			.bus_format = MEDIA_BUS_FMT_UYVY8_1X16,
			.color_format = XTPG_FMT_YUV_422,
		}, {
			.bus_format = MEDIA_BUS_FMT_VUY8_1X24,
			.color_format = XTPG_FMT_YUV_444,
		}, {
			.bus_format = MEDIA_BUS_FMT_UYVY10_1X20,
			.color_format = XTPG_FMT_YUV_422,
		},
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(format_map); ++i)
		if (format_map[i].bus_format == bus_format)
			return format_map[i].color_format;

	return XTPG_FMT_INVALID;
}

/* -----------------------------------------------------------------------------
 * TPG IP ops
 */

static void xlnx_tpg_write(struct xlnx_tpg *tpg, int offset, u32 val)
{
	writel(val, tpg->regs + offset);
}

static u32 xlnx_tpg_read(struct xlnx_tpg *tpg, int offset)
{
	return readl(tpg->regs + offset);
}

/**
 * xlnx_tpg_set_dimensions - Set TPG output signal dimensions
 * @tpg: The TPG
 * @w: Output video frame width
 * @h: Output video frame height
 */
static void xlnx_tpg_set_dimensions(struct xlnx_tpg *tpg, u16 w, u16 h)
{
	xlnx_tpg_write(tpg, XLNX_TPG_ACTIVE_WIDTH, w);
	xlnx_tpg_write(tpg, XLNX_TPG_ACTIVE_HEIGHT, h);
}

/**
 * xlnx_tpg_set_pattern - Set TPG output video pattern
 * @tpg: The TPG
 * @pattern: The pattern
 */
static void xlnx_tpg_set_pattern(struct xlnx_tpg *tpg,  enum xlnx_tpg_pattern pattern)
{
	xlnx_tpg_write(tpg, XLNX_TPG_PATTERN_ID, pattern);
}

/**
 * xlnx_tpg_get_pattern - Get programmed TPG output video pattern
 * @tpg: The TPG
 *
 * Return: Video signal pattern programmed
 */
static enum xlnx_tpg_pattern xlnx_tpg_get_pattern(struct xlnx_tpg *tpg)
{
	return xlnx_tpg_read(tpg, XLNX_TPG_PATTERN_ID);
}

/**
 * xlnx_tpg_set_format - Set TPG output video color format
 * @tpg: The TPG
 * @format: Color format to program
 */
static void xlnx_tpg_set_format(struct xlnx_tpg *tpg,  enum xlnx_tpg_format format)
{
	xlnx_tpg_write(tpg, XLNX_TPG_COLOR_FORMAT, format);
}

/**
 * xlnx_tpg_start - Start generation of the video signal
 * @tpg: The TPG
 */
static void xlnx_tpg_start(struct xlnx_tpg *tpg)
{
	xlnx_tpg_write(tpg, XLNX_TPG_CONTROL, XLNX_TPG_START | XLNX_TPG_AUTO_RESTART);
}

/**
 * xlnx_tpg_enable_irq - Enable generation of the frame done interrupts
 * @tpg: The TPG
 */
static void xlnx_tpg_enable_irq(struct xlnx_tpg *tpg)
{
	xlnx_tpg_write(tpg, XLNX_TPG_GLOBAL_IRQ_EN, 1);
	xlnx_tpg_write(tpg, XLNX_TPG_IP_IRQ_EN, 1);
}

/**
 * xlnx_tpg_disable_irq - Disable generation of the frame done interrupts
 * @tpg: The TPG
 */
static void xlnx_tpg_disable_irq(struct xlnx_tpg *tpg)
{
	xlnx_tpg_write(tpg, XLNX_TPG_GLOBAL_IRQ_EN, 0);
	xlnx_tpg_write(tpg, XLNX_TPG_IP_IRQ_EN, 0);
}

static irqreturn_t xlnx_tpg_irq_handler(int irq, void *data)
{
	struct xlnx_tpg *tpg = data;
	struct drm_crtc *crtc = &tpg->drm->crtc;
	struct drm_pending_vblank_event *event;
	unsigned long flags;
	u32 status = xlnx_tpg_read(tpg, XLNX_TPG_IP_IRQ_STATUS);

	xlnx_tpg_write(tpg, XLNX_TPG_IP_IRQ_STATUS, status);

	status &= XLNX_TPG_IP_IRQ_AP_DONE;
	if (!status)
		return IRQ_NONE;

	drm_crtc_handle_vblank(crtc);

	/* Finish page flip */
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	event = tpg->drm->event;
	tpg->drm->event = NULL;
	if (event) {
		drm_crtc_send_vblank_event(crtc, event);
		drm_crtc_vblank_put(crtc);
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);

	return IRQ_HANDLED;
}

/**
 * xlnx_tpg_setup_irq - Setup TPG interrupt
 * @tpg: The TPG
 *
 * Return: 0 on success or error code
 */
static int xlnx_tpg_setup_irq(struct xlnx_tpg *tpg)
{
	struct device_node *node = tpg->pdev->dev.of_node;
	int ret;

	tpg->irq = irq_of_parse_and_map(node, 0);
	if (!tpg->irq) {
		dev_err(&tpg->pdev->dev, "failed to parse irq\n");
		return -EINVAL;
	}

	ret = devm_request_irq(&tpg->pdev->dev, tpg->irq, xlnx_tpg_irq_handler,
			       IRQF_SHARED, "xlnx-tpg", tpg);
	if (ret < 0) {
		dev_err(&tpg->pdev->dev, "failed to request irq\n");
		return ret;
	}

	xlnx_tpg_enable_irq(tpg);

	return 0;
}

/**
 * xlnx_tpg_map_resources - Map TPG register space
 * @tpg: The TPG
 *
 * Return: 0 on success or error code
 */
static int xlnx_tpg_map_resources(struct xlnx_tpg *tpg)
{
	struct device_node *node = tpg->pdev->dev.of_node;
	struct resource	res;
	int ret;

	ret = of_address_to_resource(node, 0, &res);
	if (ret < 0) {
		dev_err(&tpg->pdev->dev, "failed to parse resource\n");
		return ret;
	}

	tpg->regs = devm_ioremap_resource(&tpg->pdev->dev, &res);
	if (IS_ERR(tpg->regs)) {
		ret = PTR_ERR(tpg->regs);
		dev_err(&tpg->pdev->dev, "failed to map register space\n");
		return ret;
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * DRM plane
 */

static void xlnx_tpg_plane_atomic_update(struct drm_plane *plane,
					 struct drm_atomic_state *state)
{
	struct xlnx_tpg *tpg = plane_to_tpg(plane);
	struct drm_crtc *crtc = &tpg->drm->crtc;

	drm_crtc_vblank_on(crtc);
	if (crtc->state->event) {
		/* Consume the flip_done event from atomic helper */
		crtc->state->event->pipe = drm_crtc_index(crtc);
		drm_crtc_vblank_get(crtc);
		tpg->drm->event = crtc->state->event;
		crtc->state->event = NULL;
	}
}

static int xlnx_tpg_plane_atomic_check(struct drm_plane *plane,
				       struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct xlnx_tpg *tpg = plane_to_tpg(plane);
	struct drm_crtc_state *crtc_state;

	crtc_state = drm_atomic_get_new_crtc_state(state, &tpg->drm->crtc);

	return drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, false);
}

static const struct drm_plane_helper_funcs xlnx_tpg_plane_helper_funcs = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	.atomic_check = xlnx_tpg_plane_atomic_check,
	.atomic_update = xlnx_tpg_plane_atomic_update,
};

static bool xlnx_tpg_format_mod_supported(struct drm_plane *plane,
					  uint32_t format,
					  uint64_t modifier)
{
	return modifier == DRM_FORMAT_MOD_LINEAR;
}

static int xlnx_tpg_plane_set_property(struct drm_plane *plane,
				       struct drm_plane_state *state,
				       struct drm_property *property,
				       u64 val)
{
	struct xlnx_tpg *tpg = plane_to_tpg(plane);

	if (property == tpg->drm->pattern_prop)
		xlnx_tpg_set_pattern(tpg, val);
	else
		return -EINVAL;
	return 0;
}

static int xlnx_tpg_plane_get_property(struct drm_plane *plane,
				       const struct drm_plane_state *state,
				       struct drm_property *property,
				       uint64_t *val)
{
	struct xlnx_tpg *tpg = plane_to_tpg(plane);

	if (property == tpg->drm->pattern_prop)
		*val = xlnx_tpg_get_pattern(tpg);
	else
		return -EINVAL;
	return 0;
}

static const struct drm_plane_funcs xlnx_tpg_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
	.format_mod_supported   = xlnx_tpg_format_mod_supported,
	.atomic_set_property	= xlnx_tpg_plane_set_property,
	.atomic_get_property	= xlnx_tpg_plane_get_property,
};

/**
 * xlnx_tpg_create_properties - Create TPG DRM properties
 * @tpg: The TPG
 */
static void xlnx_tpg_create_properties(struct xlnx_tpg *tpg)
{
	struct drm_device *drm = &tpg->drm->dev;
	struct drm_mode_object *obj = &tpg->drm->plane.base;

	tpg->drm->pattern_prop = drm_property_create_enum(drm, 0, "pattern", xtpg_pattern_list,
							  ARRAY_SIZE(xtpg_pattern_list));
	drm_object_attach_property(obj, tpg->drm->pattern_prop, XTPG_PAT_COLOR_BARS);
	xlnx_tpg_set_pattern(tpg, XTPG_PAT_COLOR_BARS);
}

/* -----------------------------------------------------------------------------
 * DRM CRTC
 */

static enum drm_mode_status xlnx_tpg_crtc_mode_valid(struct drm_crtc *crtc,
						     const struct drm_display_mode *mode)
{
	return MODE_OK;
}

static int xlnx_tpg_crtc_check(struct drm_crtc *crtc,
			       struct drm_atomic_state *state)
{
	struct xlnx_tpg *tpg = crtc_to_tpg(crtc);
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	int ret;

	if (!crtc_state->enable)
		goto out;

	ret = drm_atomic_helper_check_crtc_primary_plane(crtc_state);
	if (ret)
		return ret;

	if (tpg->output_bus_format != crtc_state->output_bus_format)
		return -EINVAL;

out:
	return drm_atomic_add_affected_planes(state, crtc);
}

static void xlnx_tpg_crtc_enable(struct drm_crtc *crtc,
				 struct drm_atomic_state *state)
{
	struct videomode vm;
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	struct xlnx_tpg *tpg = crtc_to_tpg(crtc);

	if (tpg->vtc) {
		drm_display_mode_to_videomode(mode, &vm);
		xlnx_vtc_iface_set_timing(tpg->vtc, &vm);
		xlnx_vtc_iface_enable(tpg->vtc);
	}

	xlnx_tpg_set_dimensions(tpg, mode->hdisplay, mode->vdisplay);

	xlnx_tpg_set_format(tpg, tpg->color_format);
	xlnx_tpg_start(tpg);
}

static void xlnx_tpg_crtc_disable(struct drm_crtc *crtc,
				  struct drm_atomic_state *state)
{
	struct xlnx_tpg *tpg = crtc_to_tpg(crtc);

	if (tpg->vtc)
		xlnx_vtc_iface_disable(tpg->vtc);
	if (crtc->state->event) {
		complete_all(crtc->state->event->base.completion);
		crtc->state->event = NULL;
	}
	drm_crtc_vblank_off(crtc);
}

static u32 xlnx_tpg_crtc_select_output_bus_format(struct drm_crtc *crtc,
						  struct drm_crtc_state *crtc_state,
						  const u32 *in_bus_fmts,
						  unsigned int num_in_bus_fmts)
{
	struct xlnx_tpg *tpg = crtc_to_tpg(crtc);
	unsigned int i;

	for (i = 0; i < num_in_bus_fmts; ++i)
		if (in_bus_fmts[i] == tpg->output_bus_format)
			return tpg->output_bus_format;

	return 0;
}

static const struct drm_crtc_helper_funcs xlnx_tpg_crtc_helper_funcs = {
	.mode_valid = xlnx_tpg_crtc_mode_valid,
	.atomic_check = xlnx_tpg_crtc_check,
	.atomic_enable = xlnx_tpg_crtc_enable,
	.atomic_disable = xlnx_tpg_crtc_disable,
	.select_output_bus_format = xlnx_tpg_crtc_select_output_bus_format,
};

static int xlnx_tpg_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct xlnx_tpg *tpg = crtc_to_tpg(crtc);

	xlnx_tpg_enable_irq(tpg);

	return 0;
}

static void xlnx_tpg_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct xlnx_tpg *tpg = crtc_to_tpg(crtc);

	xlnx_tpg_disable_irq(tpg);
}

static const struct drm_crtc_funcs xlnx_tpg_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = xlnx_tpg_crtc_enable_vblank,
	.disable_vblank = xlnx_tpg_crtc_disable_vblank,
};

/* -----------------------------------------------------------------------------
 * Setup & Init
 */

/**
 * xlnx_tpg_pipeline_init - Initialize DRM pipeline
 * @drm: DRM device
 *
 * Create and link CRTC, plane, and encoder. Attach external DRM bridge.
 *
 * Return: 0 on success, or a negative error code otherwise
 */
static int xlnx_tpg_pipeline_init(struct drm_device *drm)
{
	static const uint32_t xlnx_tpg_formats[] = {
		DRM_FORMAT_XRGB8888,
	};
	static const uint64_t xlnx_tpg_modifiers[] = {
		DRM_FORMAT_MOD_LINEAR,
		DRM_FORMAT_MOD_INVALID,
	};

	struct xlnx_tpg *tpg = dev_get_drvdata(drm->dev);
	struct drm_connector *connector;
	struct drm_encoder *encoder = &tpg->drm->encoder;
	struct drm_plane *plane = &tpg->drm->plane;
	struct drm_crtc *crtc = &tpg->drm->crtc;
	int ret;

	ret = xlnx_tpg_map_resources(tpg);
	if (ret < 0)
		return ret;

	ret = xlnx_tpg_setup_irq(tpg);
	if (ret < 0)
		return ret;

	drm_plane_helper_add(plane, &xlnx_tpg_plane_helper_funcs);
	ret = drm_universal_plane_init(drm, plane, 0,
				       &xlnx_tpg_plane_funcs,
				       xlnx_tpg_formats,
				       ARRAY_SIZE(xlnx_tpg_formats),
				       xlnx_tpg_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		dev_err(drm->dev, "failed to init plane: %d\n", ret);
		return ret;
	}

	drm_crtc_helper_add(crtc, &xlnx_tpg_crtc_helper_funcs);
	ret = drm_crtc_init_with_planes(drm, crtc, plane, NULL,
					&xlnx_tpg_crtc_funcs, NULL);
	if (ret) {
		dev_err(drm->dev, "failed to init crtc: %d\n", ret);
		return ret;
	}

	encoder->possible_crtcs = drm_crtc_mask(crtc);
	ret = drm_simple_encoder_init(drm, encoder, DRM_MODE_ENCODER_NONE);
	if (ret) {
		dev_err(drm->dev, "failed to init encoder: %d\n", ret);
		return ret;
	}

	ret = drm_bridge_attach(encoder, tpg->disp_bridge, NULL,
				DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret < 0) {
		dev_err(drm->dev, "failed to attach bridge to encoder: %d\n", ret);
		return ret;
	}

	connector = drm_bridge_connector_init(drm, encoder);
	if (IS_ERR(connector)) {
		ret = PTR_ERR(connector);
		dev_err(drm->dev, "failed to init connector: %d\n", ret);
		return ret;
	}

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret < 0) {
		dev_err(drm->dev, "failed to attach encoder: %d\n", ret);
		return ret;
	}

	xlnx_tpg_create_properties(tpg);

	return 0;
}

static const struct drm_mode_config_funcs xlnx_tpg_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_DMA_FOPS(xlnx_tpg_gem_dma_fops);
static struct drm_driver xlnx_tpg_drm_driver = {
	.driver_features		= DRIVER_MODESET | DRIVER_GEM |
					  DRIVER_ATOMIC,
	.fops				= &xlnx_tpg_gem_dma_fops,
	.name				= DRIVER_NAME,
	.desc				= DRIVER_DESC,
	.date				= DRIVER_DATE,
	.major				= DRIVER_MAJOR,
	.minor				= DRIVER_MINOR,
	DRM_GEM_DMA_DRIVER_OPS,
};

/**
 * xlnx_tpg_drm_init - Initialize DRM device
 * @dev: The device
 *
 * Allocate and initialize DRM device. Configure mode config and initialize
 * TPG DRM pipeline.
 *
 * Return: 0 on success, or a negative error code otherwise
 */
static int xlnx_tpg_drm_init(struct device *dev)
{
	struct xlnx_tpg *tpg = dev_get_drvdata(dev);
	struct drm_device *drm;
	int ret;

	tpg->drm = devm_drm_dev_alloc(dev, &xlnx_tpg_drm_driver,
				      struct xlnx_tpg_drm, dev);
	if (IS_ERR(tpg->drm))
		return PTR_ERR(tpg->drm);
	tpg->drm->tpg = tpg;
	drm = &tpg->drm->dev;

	ret = drm_mode_config_init(drm);
	if (ret < 0)
		return ret;

	tpg->drm->dev.mode_config.funcs = &xlnx_tpg_mode_config_funcs;
	tpg->drm->dev.mode_config.min_width = 0;
	tpg->drm->dev.mode_config.min_height = 0;
	tpg->drm->dev.mode_config.max_width = 4096;
	tpg->drm->dev.mode_config.max_height = 4096;

	ret = drm_vblank_init(drm, 1);
	if (ret < 0)
		return ret;

	drm_kms_helper_poll_init(drm);

	ret = xlnx_tpg_pipeline_init(drm);
	if (ret < 0)
		goto err_poll_fini;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret < 0)
		goto err_poll_fini;

	return ret;

err_poll_fini:
	drm_kms_helper_poll_fini(drm);

	return ret;
}

/**
 * xlnx_tpg_drm_fini - Finilize DRM device
 * @dev: The device
 */
static void xlnx_tpg_drm_fini(struct device *dev)
{
	struct xlnx_tpg *tpg = dev_get_drvdata(dev);

	drm_kms_helper_poll_fini(&tpg->drm->dev);
}

static int xlnx_tpg_probe(struct platform_device *pdev)
{
	struct xlnx_tpg *tpg;
	struct device_node *node, *vtc_node;
	int ret;

	tpg = devm_kzalloc(&pdev->dev, sizeof(*tpg), GFP_KERNEL);
	if (!tpg)
		return -ENOMEM;

	tpg->pdev = pdev;
	platform_set_drvdata(pdev, tpg);
	node = pdev->dev.of_node;

	tpg->disp_bridge = devm_drm_of_get_bridge(&pdev->dev, node, 0, 0);
	if (IS_ERR(tpg->disp_bridge)) {
		ret = PTR_ERR(tpg->disp_bridge);
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "failed to discover display bridge\n");
		return ret;
	}

	if (of_property_read_u32(node, "bus-format", &tpg->output_bus_format)) {
		dev_err(&pdev->dev, "required bus-format property undefined\n");
		return -EINVAL;
	}
	tpg->color_format = xlnx_tpg_bus_to_color_format(tpg->output_bus_format);

	vtc_node = of_parse_phandle(node, "xlnx,bridge", 0);
	if (!vtc_node) {
		dev_err(&pdev->dev, "required vtc node is missing\n");
		return -EINVAL;
	}
	ret = xlnx_of_find_vtc(vtc_node, &tpg->vtc);
	if (ret < 0)
		return ret;

	ret = xlnx_tpg_drm_init(&pdev->dev);
	if (ret < 0)
		return ret;

	dev_info(&pdev->dev, "xlnx-tpg driver probed\n");

	return 0;
}

static int xlnx_tpg_remove(struct platform_device *pdev)
{
	xlnx_tpg_drm_fini(&pdev->dev);

	return 0;
}

static const struct of_device_id xlnx_tpg_of_match[] = {
	{ .compatible = "xlnx,v-tpg-8.2", },
	{ .compatible = "xlnx,v-tpg-8.0", },
	{ /* end of table */ },
};
MODULE_DEVICE_TABLE(of, xlnx_tpg_of_match);

static struct platform_driver xlnx_tpg_driver = {
	.probe			= xlnx_tpg_probe,
	.remove			= xlnx_tpg_remove,
	.driver			= {
		.name		= "xlnx-tpg",
		.of_match_table	= xlnx_tpg_of_match,
	},
};

module_platform_driver(xlnx_tpg_driver);

MODULE_AUTHOR("Anatoliy Klymenko");
MODULE_DESCRIPTION("Xilinx TPG CRTC Driver");
MODULE_LICENSE("GPL");
