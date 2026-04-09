// SPDX-License-Identifier: GPL-2.0
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/ratelimit.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_prime.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_vma_manager.h>

#include "ascend_kernel_hal.h"
#include "ascend_vdp_drm_hdmi.h"
#include "gfbg_def.h"
#include "gfbg_graphic_hal.h"
#include "gfbg_graphics_drv.h"
#include "gfbg_vou_graphics.h"
#include "ot_hdmi_mod_init.h"
#include "soc_adp_hdmi_calllback.h"
#include "securec.h"

extern volatile vdp_regs_type *g_gfbg_reg;

#define ASCEND_VDP_DRM_NAME "ascend_vdp_drm"
#define ASCEND_VDP_DRM_DESC "atlas-200a2 DRM driver"
#define ASCEND_VDP_DRM_COMPATIBLE "atlas-200a2,plat-drm"
#define ASCEND_VDP_DRM_PLATFORM "atlas-200a2-drm"
#define ASCEND_VDP_QOS_NAME "VDP_QOS"
#define ASCEND_VDP_DEFAULT_WIDTH 1920
#define ASCEND_VDP_DEFAULT_HEIGHT 1080
#define ASCEND_VDP_MAX_MODE 3840
#define ASCEND_VDP_PLANE_FORMATS 7
#define ASCEND_VDP_HDMI_EDID_LEN 512
#define ASCEND_VDP_LAYER GRAPHICS_LAYER_G4
#define ASCEND_VDP_HW_LAYER HAL_DISP_LAYER_GFX4
#define ASCEND_VDP_HDMI_EVENT_HOTPLUG 0x10
#define ASCEND_VDP_MEDIA_SUBCTRL_REG 0x400130000ULL
#define ASCEND_VDP_MEDIA_SUBCTRL_SIZE 0x10000U
#define ASCEND_VDP_VDP_WRAP_REG 0x401020000ULL
#define ASCEND_VDP_VDP_WRAP_SIZE 0x10000U
#define ASCEND_VDP_VDP_REG 0x401000000ULL
#define ASCEND_VDP_VDP_SIZE 0x20000U
#define ASCEND_VDP_GPU_WRAP_REG 0x4010A0000ULL
#define ASCEND_VDP_GPU_WRAP_SIZE 0x100U
#define ASCEND_VDP_MEDIA_SMMU_REG 0x400000000ULL
#define ASCEND_VDP_MEDIA_SMMU_SIZE 0x100000U
#define ASCEND_VDP_CROSS_STATION_REG 0xC1010000ULL
#define ASCEND_VDP_CROSS_STATION_SIZE 0x10000U
#define ASCEND_VDP_AA_MEDIA_REG 0xC1110000ULL
#define ASCEND_VDP_AA_MEDIA_SIZE 0x10000U
#define ASCEND_VDP_SC_PLL_LOCK_STATUS 0xC100U

struct ascend_vdp_connector_state {
	struct drm_connector_state base;
	u32 brightness;
	u32 contrast;
	u32 saturation;
	u32 hue;
};

struct ascend_vdp_connector {
	struct drm_connector base;
	struct drm_property *brightness_prop;
	struct drm_property *contrast_prop;
	struct drm_property *saturation_prop;
	struct drm_property *hue_prop;
	enum drm_connector_status cached_status;
};

struct ascend_vdp_plane {
	struct drm_plane base;
	u32 layer_id;
	u32 layer_opened;
	u32 alpha_en;
	u32 premul_en;
	u32 global_alpha;
	u32 update_cnt;
	u32 src_x;
	u32 src_y;
	u32 src_w;
	u32 src_h;
};

struct ascend_vdp_crtc {
	struct drm_crtc base;
	u32 enabled;
	u8 vblank_enabled;
};

struct ascend_vdp_drm_private {
	struct device *dev;
	u32 plane_count;
	struct ascend_vdp_plane plane;
	u32 crtc_count;
	struct ascend_vdp_crtc crtc;
	u32 encoder_count;
	struct drm_encoder encoder;
	u32 connector_count;
	struct ascend_vdp_connector connector;
	struct qos_master_node qos_master;
	hal_disp_syncinfo sync_info;
	int irq;
	u32 init_done;
	int connector_registered;
	void __iomem *hdmi0_phy;
	void __iomem *hdmi0_reg;
	void __iomem *hdmi1_phy;
	void __iomem *hdmi1_reg;
};

struct ascend_vdp_drm_dev_ctx {
	u32 init_irq;
	u8 reserved[428];
};

static struct ascend_drm_hdmi_property g_hdmi_property;
static struct ascend_drm_hdmi_infoframe g_hdmi_infoframe;
static u32 g_hdmi_default_format;
static u32 g_vo_sync;
struct drm_device *g_drm;
u8 g_drm_edid_raw_inner[ASCEND_VDP_HDMI_EDID_LEN];
u32 g_hdmi0_status;
static void __iomem *g_media_subctrl_base;
static void __iomem *g_vdp_wrap_base;
static void __iomem *g_vdp_base;
static void __iomem *g_gpu_wrap_base;
static void __iomem *g_media_smmu_base;
static void __iomem *g_cross_station_base;
static void __iomem *g_aa_media_base;
static u32 g_qos_default_reg;
static u32 g_qos_smmu_reg0;
static u32 g_qos_aa_media_reg;
static u32 g_qos_smmu_reg1;
static u32 g_qos_cross_station_reg;
static u32 g_mpamid;
static u32 g_qos;
static u32 g_pmg;
static int g_drm_irq;
static struct ascend_vdp_drm_dev_ctx g_drm_dev[1];

static struct ratelimit_state g_low_bandwidth_rs = RATELIMIT_STATE_INIT("drm_drv_low_bandwidth", HZ, 1);
static struct ratelimit_state g_bus_err_rs = RATELIMIT_STATE_INIT("drm_drv_bus_err", HZ, 1);
static u32 g_low_bandwidth_cnt;
static u32 g_bus_err_cnt;
static DEFINE_SPINLOCK(g_drm_lock);

static int drm_set_gfx_galpha(u32 layer_id, u32 alpha_en, u8 global_alpha);
static int drm_hal_set_layer_galpha(u32 layer_id, u8 alpha);

#define ASCEND_VDP_INT_MASK_DHD0 0x06
#define ASCEND_VDP_INT_MASK_DHD1 0x60
#define ASCEND_VDP_INT_MASK_DSD0 0x600

u64 drm_sched_clock(void);
static void drm_irq_init(int irq);
static int drm_register_irq(struct drm_crtc *crtc);
static void drm_unregister_irq(struct drm_crtc *crtc);
static void drm_dev_int_enable(u32 dev_id, int enable);
static void drm_init_default_setting(void);
static void drm_set_vtth(u32 dev_id);
static void drm_drv_open(u32 dev_id);
static void drm_hal_clear_int_status(u32 int_status);
static irqreturn_t drm_interrupt_route(int irq, void *data);

static const u32 ext_graphics_formats[ASCEND_VDP_PLANE_FORMATS] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_ARGB4444,
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_ARGB1555,
};

struct ascend_vdp_pll_cfg {
	u64 cfg0;
	u64 cfg1;
	u32 div;
};

struct ascend_vdp_sync_mode {
	u32 sync;
	u32 hact;
	u32 vact;
	u32 refresh;
};

struct ascend_vdp_pll_mode {
	u32 sync;
	struct ascend_vdp_pll_cfg cfg;
};

#define ASCEND_VDP_SYNC_MODE(_sync, _hact, _vact, _refresh) \
	{ .sync = (_sync), .hact = (_hact), .vact = (_vact), .refresh = (_refresh) }

#define ASCEND_VDP_PLL_MODE(_sync, _cfg0_lo, _cfg0_hi, _cfg1_lo, _cfg1_hi, _div) \
	{ \
		.sync = (_sync), \
		.cfg = { \
			.cfg0 = (((u64)(_cfg0_hi)) << 32) | (u32)(_cfg0_lo), \
			.cfg1 = (((u64)(_cfg1_hi)) << 32) | (u32)(_cfg1_lo), \
			.div = (_div), \
		}, \
	}

#define ASCEND_VDP_SYNC_TIMING(_synm, _iop, _intfb, _vact, _vbb, _vfb, _hact, _hbb, _hfb, _hmid, \
				 _bvact, _bvbb, _bvfb, _hpw, _vpw, _idv, _ihs, _ivs) \
	{ \
		.synm = (_synm), \
		.iop = (_iop), \
		.intfb = (_intfb), \
		.vact = (_vact), \
		.vbb = (_vbb), \
		.vfb = (_vfb), \
		.hact = (_hact), \
		.hbb = (_hbb), \
		.hfb = (_hfb), \
		.hmid = (_hmid), \
		.bvact = (_bvact), \
		.bvbb = (_bvbb), \
		.bvfb = (_bvfb), \
		.hpw = (_hpw), \
		.vpw = (_vpw), \
		.idv = (_idv), \
		.ihs = (_ihs), \
		.ivs = (_ivs), \
	}

static const struct ascend_vdp_sync_mode g_drm_vo_intf_sync_info[] = {
	ASCEND_VDP_SYNC_MODE(0, 720, 576, 25),
	ASCEND_VDP_SYNC_MODE(1, 720, 480, 30),
	ASCEND_VDP_SYNC_MODE(2, 960, 576, 25),
	ASCEND_VDP_SYNC_MODE(3, 960, 480, 30),
	ASCEND_VDP_SYNC_MODE(4, 640, 480, 60),
	ASCEND_VDP_SYNC_MODE(5, 720, 480, 60),
	ASCEND_VDP_SYNC_MODE(6, 720, 576, 50),
	ASCEND_VDP_SYNC_MODE(7, 800, 600, 60),
	ASCEND_VDP_SYNC_MODE(8, 1024, 768, 60),
	ASCEND_VDP_SYNC_MODE(9, 1280, 720, 50),
	ASCEND_VDP_SYNC_MODE(10, 1280, 720, 60),
	ASCEND_VDP_SYNC_MODE(11, 1280, 800, 60),
	ASCEND_VDP_SYNC_MODE(12, 1280, 1024, 60),
	ASCEND_VDP_SYNC_MODE(13, 1366, 768, 60),
	ASCEND_VDP_SYNC_MODE(14, 1400, 1050, 60),
	ASCEND_VDP_SYNC_MODE(15, 1440, 900, 60),
	ASCEND_VDP_SYNC_MODE(16, 1680, 1050, 60),
	ASCEND_VDP_SYNC_MODE(17, 1920, 1080, 24),
	ASCEND_VDP_SYNC_MODE(18, 1920, 1080, 25),
	ASCEND_VDP_SYNC_MODE(19, 1920, 1080, 30),
	ASCEND_VDP_SYNC_MODE(20, 1920, 1080, 25),
	ASCEND_VDP_SYNC_MODE(21, 1920, 1080, 30),
	ASCEND_VDP_SYNC_MODE(22, 1920, 1080, 50),
	ASCEND_VDP_SYNC_MODE(23, 1920, 1080, 60),
	ASCEND_VDP_SYNC_MODE(24, 1600, 1200, 60),
	ASCEND_VDP_SYNC_MODE(25, 1920, 1200, 60),
	ASCEND_VDP_SYNC_MODE(26, 1920, 2160, 30),
	ASCEND_VDP_SYNC_MODE(27, 2560, 1440, 30),
	ASCEND_VDP_SYNC_MODE(28, 2560, 1440, 60),
	ASCEND_VDP_SYNC_MODE(29, 2560, 1600, 60),
	ASCEND_VDP_SYNC_MODE(30, 3840, 2160, 24),
	ASCEND_VDP_SYNC_MODE(31, 3840, 2160, 25),
	ASCEND_VDP_SYNC_MODE(32, 3840, 2160, 30),
	ASCEND_VDP_SYNC_MODE(33, 3840, 2160, 50),
	ASCEND_VDP_SYNC_MODE(34, 3840, 2160, 60),
	ASCEND_VDP_SYNC_MODE(35, 4096, 2160, 24),
	ASCEND_VDP_SYNC_MODE(36, 4096, 2160, 25),
	ASCEND_VDP_SYNC_MODE(37, 4096, 2160, 30),
	ASCEND_VDP_SYNC_MODE(38, 4096, 2160, 50),
	ASCEND_VDP_SYNC_MODE(39, 4096, 2160, 60),
	ASCEND_VDP_SYNC_MODE(40, 7680, 4320, 30),
	ASCEND_VDP_SYNC_MODE(41, 240, 320, 50),
	ASCEND_VDP_SYNC_MODE(42, 320, 240, 50),
	ASCEND_VDP_SYNC_MODE(43, 240, 320, 60),
	ASCEND_VDP_SYNC_MODE(44, 320, 240, 60),
	ASCEND_VDP_SYNC_MODE(45, 800, 600, 50),
	ASCEND_VDP_SYNC_MODE(46, 720, 1280, 60),
	ASCEND_VDP_SYNC_MODE(47, 1080, 1920, 60),
	ASCEND_VDP_SYNC_MODE(48, 800, 480, 60),
	ASCEND_VDP_SYNC_MODE(49, 1080, 1920, 60),
};

static const hal_disp_syncinfo g_drm_sync_timing[] = {
	ASCEND_VDP_SYNC_TIMING(0, 0, 0, 22, 2, 720, 132, 12, 1, 288, 23, 2, 126, 3, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 0, 0, 18, 4, 720, 119, 19, 1, 240, 19, 4, 124, 3, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 0, 0, 22, 2, 960, 176, 16, 1, 288, 23, 2, 168, 3, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 0, 0, 18, 4, 960, 163, 21, 1, 240, 19, 4, 168, 3, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(1, 1, 2, 35, 10, 640, 144, 16, 1, 1, 1, 1, 96, 2, 0, 0, 1, 1),
	ASCEND_VDP_SYNC_TIMING(1, 1, 1, 36, 9, 720, 122, 16, 1, 1, 1, 1, 62, 6, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(1, 1, 1, 44, 5, 720, 132, 12, 1, 1, 1, 1, 64, 5, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(1, 1, 2, 27, 1, 800, 216, 40, 1, 1, 1, 1, 128, 4, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(1, 1, 2, 35, 3, 1024, 296, 24, 1, 1, 1, 1, 136, 6, 0, 0, 1, 1),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 25, 5, 1280, 260, 440, 1, 1, 1, 1, 40, 5, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 25, 5, 1280, 260, 110, 1, 1, 1, 1, 40, 5, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(1, 1, 2, 28, 3, 1280, 328, 72, 1, 1, 1, 1, 128, 6, 0, 0, 1, 0),
	ASCEND_VDP_SYNC_TIMING(1, 1, 2, 41, 1, 1280, 360, 48, 1, 1, 1, 1, 112, 3, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(1, 1, 2, 27, 3, 1366, 356, 70, 1, 1, 1, 1, 143, 3, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(1, 1, 2, 36, 3, 1400, 376, 88, 1, 1, 1, 1, 144, 4, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(1, 1, 2, 31, 3, 1440, 384, 80, 1, 1, 1, 1, 152, 6, 0, 0, 1, 0),
	ASCEND_VDP_SYNC_TIMING(1, 1, 2, 36, 3, 1680, 456, 104, 1, 1, 1, 1, 176, 6, 0, 0, 1, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 41, 4, 1920, 192, 638, 1, 1, 1, 1, 44, 5, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 41, 4, 1920, 192, 528, 1, 1, 1, 1, 44, 5, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 41, 4, 1920, 192, 88, 1, 1, 1, 1, 44, 5, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 0, 1, 20, 2, 1920, 192, 528, 1128, 540, 21, 2, 44, 5, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 0, 1, 20, 2, 1920, 192, 88, 908, 540, 21, 2, 44, 5, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 41, 4, 1920, 192, 528, 1, 1, 1, 1, 44, 5, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 41, 4, 1920, 192, 88, 1, 1, 1, 1, 44, 5, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(1, 1, 2, 49, 1, 1600, 496, 64, 1, 1, 1, 1, 192, 3, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(1, 1, 2, 32, 3, 1920, 112, 48, 1, 1, 1, 1, 32, 6, 0, 0, 0, 1),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 72, 8, 1920, 192, 88, 1, 1, 1, 1, 44, 5, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(1, 1, 2, 39, 2, 2560, 112, 48, 1, 1, 1, 1, 32, 5, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(1, 1, 2, 39, 2, 2560, 112, 48, 1, 1, 1, 1, 32, 5, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 2, 43, 3, 2560, 112, 48, 1, 1, 1, 1, 32, 6, 0, 0, 0, 1),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 82, 8, 3840, 384, 1276, 1, 1, 1, 1, 88, 10, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 82, 8, 3840, 384, 1056, 1, 1, 1, 1, 88, 10, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 82, 8, 3840, 384, 176, 1, 1, 1, 1, 88, 10, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 82, 8, 3840, 384, 1056, 1, 1, 1, 1, 88, 10, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 82, 8, 3840, 384, 176, 1, 1, 1, 1, 88, 10, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 82, 8, 4096, 384, 1020, 1, 1, 1, 1, 88, 10, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 82, 8, 4096, 216, 968, 1, 1, 1, 1, 88, 10, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 82, 8, 4096, 216, 88, 1, 1, 1, 1, 88, 10, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 82, 8, 4096, 216, 968, 1, 1, 1, 1, 88, 10, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 82, 8, 4096, 216, 88, 1, 1, 1, 1, 88, 10, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 64, 16, 7680, 768, 552, 1, 1, 1, 1, 176, 20, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 10, 4, 240, 30, 10, 1, 1, 1, 1, 10, 2, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 2, 2, 320, 5, 10, 1, 1, 1, 1, 10, 1, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 4, 8, 240, 20, 10, 1, 1, 1, 1, 2, 2, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 15, 9, 320, 65, 7, 1, 240, 14, 9, 1, 1, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 23, 12, 800, 210, 46, 1, 1, 1, 1, 2, 1, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 24, 8, 720, 123, 99, 1, 1, 1, 1, 24, 4, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 1, 1, 36, 16, 1080, 28, 130, 1, 1, 1, 1, 8, 10, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
	ASCEND_VDP_SYNC_TIMING(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
};

static const struct ascend_vdp_pll_mode g_drm_vo_pll_param_no_div[] = {
	ASCEND_VDP_PLL_MODE(0, 40, 8388608, 1, 6, 6),
	ASCEND_VDP_PLL_MODE(1, 40, 8388608, 1, 6, 6),
	ASCEND_VDP_PLL_MODE(2, 36, 0, 1, 6, 4),
	ASCEND_VDP_PLL_MODE(3, 36, 0, 1, 6, 4),
	ASCEND_VDP_PLL_MODE(4, 18, 6291456, 1, 7, 5),
	ASCEND_VDP_PLL_MODE(5, 20, 4194304, 1, 6, 6),
	ASCEND_VDP_PLL_MODE(6, 20, 4194304, 1, 6, 6),
	ASCEND_VDP_PLL_MODE(7, 20, 0, 1, 6, 4),
	ASCEND_VDP_PLL_MODE(8, 32, 8388608, 1, 6, 4),
	ASCEND_VDP_PLL_MODE(9, 24, 12582912, 1, 4, 4),
	ASCEND_VDP_PLL_MODE(10, 24, 12582912, 1, 4, 4),
	ASCEND_VDP_PLL_MODE(11, 41, 12582912, 1, 6, 4),
	ASCEND_VDP_PLL_MODE(12, 54, 0, 1, 6, 4),
	ASCEND_VDP_PLL_MODE(13, 42, 12582912, 1, 6, 4),
	ASCEND_VDP_PLL_MODE(14, 60, 14680064, 1, 6, 4),
	ASCEND_VDP_PLL_MODE(15, 53, 4194304, 1, 6, 4),
	ASCEND_VDP_PLL_MODE(16, 48, 12582912, 1, 4, 4),
	ASCEND_VDP_PLL_MODE(17, 24, 12582912, 1, 4, 4),
	ASCEND_VDP_PLL_MODE(18, 24, 12582912, 1, 4, 4),
	ASCEND_VDP_PLL_MODE(19, 24, 12582912, 1, 4, 4),
	ASCEND_VDP_PLL_MODE(20, 24, 12582912, 1, 4, 4),
	ASCEND_VDP_PLL_MODE(21, 24, 12582912, 1, 4, 4),
	ASCEND_VDP_PLL_MODE(22, 49, 8388608, 1, 4, 4),
	ASCEND_VDP_PLL_MODE(23, 49, 8388608, 1, 4, 4),
	ASCEND_VDP_PLL_MODE(24, 40, 8388608, 1, 6, 2),
	ASCEND_VDP_PLL_MODE(25, 38, 8388608, 1, 6, 2),
	ASCEND_VDP_PLL_MODE(26, 49, 8388608, 1, 4, 4),
	ASCEND_VDP_PLL_MODE(27, 25, 3145728, 1, 5, 2),
	ASCEND_VDP_PLL_MODE(28, 40, 4194304, 1, 4, 2),
	ASCEND_VDP_PLL_MODE(29, 44, 12582912, 1, 4, 2),
	ASCEND_VDP_PLL_MODE(30, 49, 8388608, 1, 4, 2),
	ASCEND_VDP_PLL_MODE(31, 49, 8388608, 1, 4, 2),
	ASCEND_VDP_PLL_MODE(32, 49, 8388608, 1, 4, 2),
	ASCEND_VDP_PLL_MODE(33, 49, 8388608, 1, 2, 2),
	ASCEND_VDP_PLL_MODE(34, 49, 8388608, 1, 2, 2),
	ASCEND_VDP_PLL_MODE(35, 99, 0, 1, 4, 2),
	ASCEND_VDP_PLL_MODE(36, 99, 0, 1, 4, 2),
	ASCEND_VDP_PLL_MODE(37, 99, 0, 1, 4, 2),
	ASCEND_VDP_PLL_MODE(38, 99, 0, 1, 2, 2),
	ASCEND_VDP_PLL_MODE(39, 99, 0, 1, 2, 2),
	ASCEND_VDP_PLL_MODE(40, 99, 0, 1, 2, 1),
	ASCEND_VDP_PLL_MODE(41, 36, 1207960, 1, 4, 2),
	ASCEND_VDP_PLL_MODE(42, 36, 13136560, 1, 3, 3),
	ASCEND_VDP_PLL_MODE(43, 36, 12348031, 1, 5, 4),
	ASCEND_VDP_PLL_MODE(44, 41, 6630356, 1, 5, 4),
	ASCEND_VDP_PLL_MODE(45, 33, 8858370, 1, 3, 1),
	ASCEND_VDP_PLL_MODE(46, 24, 12582912, 1, 4, 4),
	ASCEND_VDP_PLL_MODE(47, 37, 12582912, 1, 6, 2),
	ASCEND_VDP_PLL_MODE(48, 28, 367001, 1, 7, 7),
	ASCEND_VDP_PLL_MODE(49, 37, 12582912, 1, 6, 2),
};

static const struct ascend_vdp_pll_cfg g_drm_default_pll_cfg = {
	.cfg0 = 0x80000000000031ULL,
	.cfg1 = 0x400000001ULL,
	.div = 4,
};

static void local_drm_gem_dma_free(struct drm_gem_dma_object *dma_obj)
{
	struct drm_gem_object *gem_obj = &dma_obj->base;
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(dma_obj->vaddr);

	if (drm_gem_is_imported(gem_obj)) {
		if (dma_obj->vaddr)
			dma_buf_vunmap_unlocked(gem_obj->import_attach->dmabuf, &map);
		drm_prime_gem_destroy(gem_obj, dma_obj->sgt);
	} else if (dma_obj->vaddr) {
		if (dma_obj->map_noncoherent) {
			dma_free_noncoherent(gem_obj->dev->dev, dma_obj->base.size,
					     dma_obj->vaddr, dma_obj->dma_addr,
					     DMA_TO_DEVICE);
		} else {
			dma_free_wc(gem_obj->dev->dev, dma_obj->base.size,
				    dma_obj->vaddr, dma_obj->dma_addr);
		}
	}

	drm_gem_object_release(gem_obj);
	kfree(dma_obj);
}

static void local_drm_gem_dma_object_free(struct drm_gem_object *obj)
{
	local_drm_gem_dma_free(to_drm_gem_dma_obj(obj));
}

static void local_drm_gem_dma_object_print_info(struct drm_printer *p,
						unsigned int indent,
						const struct drm_gem_object *obj)
{
	const struct drm_gem_dma_object *dma_obj = to_drm_gem_dma_obj(obj);

	drm_printf_indent(p, indent, "dma_addr=%pad\n", &dma_obj->dma_addr);
	drm_printf_indent(p, indent, "vaddr=%p\n", dma_obj->vaddr);
}

static struct sg_table *
local_drm_gem_dma_get_sg_table(struct drm_gem_dma_object *dma_obj)
{
	struct drm_gem_object *obj = &dma_obj->base;
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = dma_get_sgtable(obj->dev->dev, sgt, dma_obj->vaddr,
			      dma_obj->dma_addr, obj->size);
	if (ret < 0) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	return sgt;
}

static struct sg_table *
local_drm_gem_dma_object_get_sg_table(struct drm_gem_object *obj)
{
	return local_drm_gem_dma_get_sg_table(to_drm_gem_dma_obj(obj));
}

static int local_drm_gem_dma_vmap(struct drm_gem_dma_object *dma_obj,
				  struct iosys_map *map)
{
	iosys_map_set_vaddr(map, dma_obj->vaddr);
	return 0;
}

static int local_drm_gem_dma_object_vmap(struct drm_gem_object *obj,
					 struct iosys_map *map)
{
	return local_drm_gem_dma_vmap(to_drm_gem_dma_obj(obj), map);
}

static int local_drm_gem_dma_mmap(struct drm_gem_dma_object *dma_obj,
				  struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = &dma_obj->base;
	int ret;

	vma->vm_pgoff -= drm_vma_node_start(&obj->vma_node);
	vm_flags_mod(vma, VM_DONTEXPAND, VM_PFNMAP);

	if (dma_obj->map_noncoherent) {
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
		ret = dma_mmap_pages(dma_obj->base.dev->dev, vma,
				     vma->vm_end - vma->vm_start,
				     virt_to_page(dma_obj->vaddr));
	} else {
		ret = dma_mmap_wc(dma_obj->base.dev->dev, vma, dma_obj->vaddr,
				  dma_obj->dma_addr,
				  vma->vm_end - vma->vm_start);
	}

	if (ret)
		drm_gem_vm_close(vma);

	return ret;
}

static int local_drm_gem_dma_object_mmap(struct drm_gem_object *obj,
					 struct vm_area_struct *vma)
{
	return local_drm_gem_dma_mmap(to_drm_gem_dma_obj(obj), vma);
}

static const struct vm_operations_struct local_drm_gem_dma_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static const struct drm_gem_object_funcs local_drm_gem_dma_default_funcs = {
	.free = local_drm_gem_dma_object_free,
	.print_info = local_drm_gem_dma_object_print_info,
	.get_sg_table = local_drm_gem_dma_object_get_sg_table,
	.vmap = local_drm_gem_dma_object_vmap,
	.mmap = local_drm_gem_dma_object_mmap,
	.vm_ops = &local_drm_gem_dma_vm_ops,
};

static struct drm_gem_dma_object *
local___drm_gem_dma_create(struct drm_device *drm, size_t size, bool private)
{
	struct drm_gem_dma_object *dma_obj;
	struct drm_gem_object *gem_obj;
	int ret = 0;

	if (drm->driver->gem_create_object) {
		gem_obj = drm->driver->gem_create_object(drm, size);
		if (IS_ERR(gem_obj))
			return ERR_CAST(gem_obj);
		dma_obj = to_drm_gem_dma_obj(gem_obj);
	} else {
		dma_obj = kzalloc(sizeof(*dma_obj), GFP_KERNEL);
		if (!dma_obj)
			return ERR_PTR(-ENOMEM);
		gem_obj = &dma_obj->base;
	}

	if (!gem_obj->funcs)
		gem_obj->funcs = &local_drm_gem_dma_default_funcs;

	if (private) {
		drm_gem_private_object_init(drm, gem_obj, size);
		dma_obj->map_noncoherent = false;
	} else {
		ret = drm_gem_object_init(drm, gem_obj, size);
	}
	if (ret)
		goto err_free;

	ret = drm_gem_create_mmap_offset(gem_obj);
	if (ret) {
		drm_gem_object_release(gem_obj);
		goto err_free;
	}

	return dma_obj;

err_free:
	kfree(dma_obj);
	return ERR_PTR(ret);
}

static struct drm_gem_dma_object *
local_drm_gem_dma_create(struct drm_device *drm, size_t size)
{
	struct drm_gem_dma_object *dma_obj;
	int ret;

	size = round_up(size, PAGE_SIZE);
	dma_obj = local___drm_gem_dma_create(drm, size, false);
	if (IS_ERR(dma_obj))
		return dma_obj;

	if (dma_obj->map_noncoherent) {
		dma_obj->vaddr = dma_alloc_noncoherent(drm->dev, size,
						       &dma_obj->dma_addr,
						       DMA_TO_DEVICE,
						       GFP_KERNEL | __GFP_NOWARN);
	} else {
		dma_obj->vaddr = dma_alloc_wc(drm->dev, size, &dma_obj->dma_addr,
					      GFP_KERNEL | __GFP_NOWARN);
	}
	if (!dma_obj->vaddr) {
		drm_dbg(drm, "failed to allocate buffer with size %zu\n", size);
		ret = -ENOMEM;
		goto err_put;
	}

	return dma_obj;

err_put:
	drm_gem_object_put(&dma_obj->base);
	return ERR_PTR(ret);
}

static struct drm_gem_dma_object *
local_drm_gem_dma_create_with_handle(struct drm_file *file_priv,
				     struct drm_device *drm, size_t size,
				     uint32_t *handle)
{
	struct drm_gem_dma_object *dma_obj;
	struct drm_gem_object *gem_obj;
	int ret;

	dma_obj = local_drm_gem_dma_create(drm, size);
	if (IS_ERR(dma_obj))
		return dma_obj;

	gem_obj = &dma_obj->base;
	ret = drm_gem_handle_create(file_priv, gem_obj, handle);
	drm_gem_object_put(gem_obj);
	if (ret)
		return ERR_PTR(ret);

	return dma_obj;
}

static int local_drm_gem_dma_dumb_create_internal(struct drm_file *file_priv,
						  struct drm_device *drm,
						  struct drm_mode_create_dumb *args)
{
	unsigned int min_pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	struct drm_gem_dma_object *dma_obj;

	if (args->pitch < min_pitch)
		args->pitch = min_pitch;

	if (args->size < args->pitch * args->height)
		args->size = args->pitch * args->height;

	dma_obj = local_drm_gem_dma_create_with_handle(file_priv, drm, args->size,
						       &args->handle);
	return PTR_ERR_OR_ZERO(dma_obj);
}

static struct drm_gem_dma_object *
local_drm_fb_dma_get_gem_obj(struct drm_framebuffer *fb, unsigned int plane)
{
	struct drm_gem_object *gem = drm_gem_fb_get_obj(fb, plane);

	if (!gem)
		return NULL;

	return to_drm_gem_dma_obj(gem);
}

static struct drm_gem_object *
local_drm_gem_dma_prime_import_sg_table(struct drm_device *dev,
					struct dma_buf_attachment *attach,
					struct sg_table *sgt)
{
	struct drm_gem_dma_object *dma_obj;

	if (drm_prime_get_contiguous_size(sgt) < attach->dmabuf->size)
		return ERR_PTR(-EINVAL);

	dma_obj = local___drm_gem_dma_create(dev, attach->dmabuf->size, true);
	if (IS_ERR(dma_obj))
		return ERR_CAST(dma_obj);

	dma_obj->dma_addr = sg_dma_address(sgt->sgl);
	dma_obj->sgt = sgt;
	drm_dbg_prime(dev, "dma_addr = %pad, size = %zu\n",
		      &dma_obj->dma_addr, attach->dmabuf->size);

	return &dma_obj->base;
}

static struct drm_gem_object *
local_drm_gem_dma_prime_import_sg_table_vmap(struct drm_device *dev,
					     struct dma_buf_attachment *attach,
					     struct sg_table *sgt)
{
	struct drm_gem_dma_object *dma_obj;
	struct drm_gem_object *obj;
	struct iosys_map map;
	int ret;

	ret = dma_buf_vmap_unlocked(attach->dmabuf, &map);
	if (ret) {
		DRM_ERROR("Failed to vmap PRIME buffer\n");
		return ERR_PTR(ret);
	}

	obj = local_drm_gem_dma_prime_import_sg_table(dev, attach, sgt);
	if (IS_ERR(obj)) {
		dma_buf_vunmap_unlocked(attach->dmabuf, &map);
		return obj;
	}

	dma_obj = to_drm_gem_dma_obj(obj);
	dma_obj->vaddr = map.vaddr;

	return obj;
}

#define to_ascend_vdp_priv(drm) ((struct ascend_vdp_drm_private *)((drm)->dev_private))
#define to_ascend_vdp_plane(plane) container_of((plane), struct ascend_vdp_plane, base)
#define to_ascend_vdp_crtc(crtc) container_of((crtc), struct ascend_vdp_crtc, base)
#define to_ascend_vdp_connector(connector) container_of((connector), struct ascend_vdp_connector, base)
#define to_ascend_vdp_conn_state(state) container_of((state), struct ascend_vdp_connector_state, base)

struct ascend_vdp_sync_inv {
	u32 field_inv;
	u32 hs_inv;
	u32 vs_inv;
	u32 dv_inv;
};

static inline bool drm_hal_reg_ready(void)
{
	return g_gfbg_reg != TD_NULL;
}

static inline u32 drm_hal_read_reg32(volatile u32 *reg)
{
	if (!drm_hal_reg_ready())
		return 0;

	return readl((void __iomem *)reg);
}

static inline void drm_hal_write_reg32(volatile u32 *reg, u32 value)
{
	if (!drm_hal_reg_ready())
		return;

	writel(value, (void __iomem *)reg);
}

static inline u32 drm_hal_read_reg(const u32 __iomem *reg)
{
	if (reg == NULL)
		return 0;

	return readl(reg);
}

static inline void drm_hal_write_reg(u32 __iomem *reg, u32 value)
{
	if (reg == NULL)
		return;

	writel(value, reg);
}

static inline void drm_hal_set_reg(u32 __iomem *reg, u32 value, u8 width, u8 shift)
{
	u32 mask;
	u32 reg_val;

	if (reg == NULL || width == 0 || width > 32)
		return;

	mask = (width == 32) ? GENMASK(31, 0) : GENMASK(width - 1, 0);
	reg_val = drm_hal_read_reg(reg);
	reg_val &= ~(mask << shift);
	reg_val |= (value & mask) << shift;
	drm_hal_write_reg(reg, reg_val);
}

static inline u32 __iomem *drm_reg_ptr(void __iomem *base, u32 offset)
{
	if (base == NULL)
		return NULL;

	return (u32 __iomem *)((char __iomem *)base + offset);
}

static const struct ascend_vdp_sync_mode *drm_vo_intf_sync_info_func(u32 idx)
{
	if (idx >= ARRAY_SIZE(g_drm_vo_intf_sync_info))
		return NULL;

	return &g_drm_vo_intf_sync_info[idx];
}

static int vo_drv_get_pll_cfg_no_div(u32 sync, struct ascend_vdp_pll_cfg *pll_cfg)
{
	const struct ascend_vdp_pll_mode *pll_mode;

	if (pll_cfg == NULL)
		return -EINVAL;

	if (sync >= ARRAY_SIZE(g_drm_vo_pll_param_no_div)) {
		*pll_cfg = g_drm_default_pll_cfg;
		return -ENOENT;
	}

	pll_mode = &g_drm_vo_pll_param_no_div[sync];
	*pll_cfg = pll_mode->cfg;
	return 0;
}

static struct ascend_vdp_drm_dev_ctx *drm_get_dev_ctx(u32 dev_id)
{
	return &g_drm_dev[dev_id];
}

static void drm_set_init_irq(int irq)
{
	g_drm_irq = irq;
}

static int drm_request_irq(int irq, irq_handler_t handler,
			   unsigned long flags, const char *name, void *data)
{
	return request_threaded_irq(irq, handler, NULL, IRQF_SHARED | flags, name, data);
}

static void drm_free_irq(int irq, void *data)
{
	free_irq(irq, data);
}

u64 drm_sched_clock(void)
{
	return sched_clock();
}
EXPORT_SYMBOL(drm_sched_clock);

void hdmi_callback_drm(td_u32 id, int event, void *hdmi_id)
{
	(void)id;
	(void)hdmi_id;

	g_hdmi0_status = (event != ASCEND_VDP_HDMI_EVENT_HOTPLUG);
	drm_helper_hpd_irq_event(g_drm);
	pr_info("hdmi_callback_drm event=%d\n", event);
}

static void hal_vo_map_base_reg(void)
{
	if (g_media_subctrl_base != NULL && g_vdp_wrap_base != NULL &&
	    g_vdp_base != NULL && g_gpu_wrap_base != NULL)
		return;

	g_media_subctrl_base = osal_ioremap(ASCEND_VDP_MEDIA_SUBCTRL_REG,
					      ASCEND_VDP_MEDIA_SUBCTRL_SIZE);
	if (g_media_subctrl_base == NULL) {
		pr_err("%s: map media_subctrl failed\n", __func__);
		return;
	}

	g_vdp_wrap_base = osal_ioremap(ASCEND_VDP_VDP_WRAP_REG,
					 ASCEND_VDP_VDP_WRAP_SIZE);
	if (g_vdp_wrap_base == NULL) {
		pr_err("%s: map vdp_wrap failed\n", __func__);
		osal_iounmap(g_media_subctrl_base);
		g_media_subctrl_base = NULL;
		return;
	}

	g_vdp_base = osal_ioremap(ASCEND_VDP_VDP_REG, ASCEND_VDP_VDP_SIZE);
	if (g_vdp_base == NULL) {
		pr_err("%s: map vdp failed\n", __func__);
		osal_iounmap(g_media_subctrl_base);
		g_media_subctrl_base = NULL;
		osal_iounmap(g_vdp_wrap_base);
		g_vdp_wrap_base = NULL;
		return;
	}

	g_gpu_wrap_base = osal_ioremap(ASCEND_VDP_GPU_WRAP_REG,
					 ASCEND_VDP_GPU_WRAP_SIZE);
	if (g_gpu_wrap_base == NULL) {
		pr_err("%s: map gpu_wrap failed\n", __func__);
		osal_iounmap(g_media_subctrl_base);
		g_media_subctrl_base = NULL;
		osal_iounmap(g_vdp_wrap_base);
		g_vdp_wrap_base = NULL;
		osal_iounmap(g_vdp_base);
		g_vdp_base = NULL;
		return;
	}

	fb_hal_vou_init();
}

static void hal_vo_unmap_base_reg(void)
{
	fb_hal_vou_exit();

	if (g_media_subctrl_base != NULL) {
		osal_iounmap(g_media_subctrl_base);
		g_media_subctrl_base = NULL;
	}

	if (g_vdp_wrap_base != NULL) {
		osal_iounmap(g_vdp_wrap_base);
		g_vdp_wrap_base = NULL;
	}

	if (g_vdp_base != NULL) {
		osal_iounmap(g_vdp_base);
		g_vdp_base = NULL;
	}

	if (g_gpu_wrap_base != NULL) {
		osal_iounmap(g_gpu_wrap_base);
		g_gpu_wrap_base = NULL;
	}
}

static void hal_vo_map_qos_reg(void)
{
	if (g_media_smmu_base != NULL && g_cross_station_base != NULL &&
	    g_aa_media_base != NULL)
		return;

	g_media_smmu_base = osal_ioremap(ASCEND_VDP_MEDIA_SMMU_REG,
					   ASCEND_VDP_MEDIA_SMMU_SIZE);
	if (g_media_smmu_base == NULL) {
		pr_err("%s: map media_smmu failed\n", __func__);
		return;
	}

	g_cross_station_base = osal_ioremap(ASCEND_VDP_CROSS_STATION_REG,
					      ASCEND_VDP_CROSS_STATION_SIZE);
	if (g_cross_station_base == NULL) {
		pr_err("%s: map cross_station failed\n", __func__);
		osal_iounmap(g_media_smmu_base);
		g_media_smmu_base = NULL;
		return;
	}

	g_aa_media_base = osal_ioremap(ASCEND_VDP_AA_MEDIA_REG,
					 ASCEND_VDP_AA_MEDIA_SIZE);
	if (g_aa_media_base == NULL) {
		pr_err("%s: map aa_media failed\n", __func__);
		osal_iounmap(g_cross_station_base);
		g_cross_station_base = NULL;
		osal_iounmap(g_media_smmu_base);
		g_media_smmu_base = NULL;
	}
}

static void hal_vo_unmap_qos_reg(void)
{
	if (g_media_smmu_base != NULL) {
		osal_iounmap(g_media_smmu_base);
		g_media_smmu_base = NULL;
	}

	if (g_cross_station_base != NULL) {
		osal_iounmap(g_cross_station_base);
		g_cross_station_base = NULL;
	}

	if (g_aa_media_base != NULL) {
		osal_iounmap(g_aa_media_base);
		g_aa_media_base = NULL;
	}
}

static int hal_vo_wait_src_status(void)
{
	int retry = 2000000;
	bool done;
	u32 status0;
	u32 status1;
	int delay;

	if (g_vdp_base == NULL)
		return 0;

	do {
		retry--;
		delay = 10;
		status0 = drm_hal_read_reg(drm_reg_ptr(g_vdp_base, 0x10054));
		status1 = drm_hal_read_reg(drm_reg_ptr(g_vdp_base, 0x1005C));
		while (delay-- > 0)
			udelay(1);
		done = (status1 != 0U && status0 == 0U);
	} while (!done && retry > 0);

	return retry;
}

static void hal_vo_deinit_config(void)
{
	if (g_vdp_wrap_base == NULL || g_media_subctrl_base == NULL)
		return;

	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x10), 1);
	if (hal_vo_wait_src_status() <= 0) {
		pr_err("%s: src status timeout(1)\n", __func__);
		return;
	}

	drm_hal_write_reg(drm_reg_ptr(g_media_subctrl_base, 0xA80), 1);
	if (drm_hal_read_reg(drm_reg_ptr(g_media_subctrl_base, 0x5A80)) != 1U) {
		pr_err("%s: subctrl status mismatch\n", __func__);
		return;
	}

	drm_hal_write_reg(drm_reg_ptr(g_media_subctrl_base, 0x304), 255);
	if (drm_hal_read_reg(drm_reg_ptr(g_media_subctrl_base, 0x5300)) != 0U)
		pr_err("%s: deinit pll status mismatch\n", __func__);
}

static void hal_vo_init_config(void)
{
	if (g_media_subctrl_base == NULL || g_vdp_wrap_base == NULL ||
	    g_gpu_wrap_base == NULL)
		return;

	drm_hal_write_reg(drm_reg_ptr(g_media_subctrl_base, 0x180), 0);
	drm_hal_write_reg(drm_reg_ptr(g_media_subctrl_base, 0x188), 0);
	drm_hal_write_reg(drm_reg_ptr(g_media_subctrl_base, 0x18C), 0);
	drm_hal_write_reg(drm_reg_ptr(g_media_subctrl_base, 0x190), 0);
	drm_hal_write_reg(drm_reg_ptr(g_media_subctrl_base, 0x198), 0);
	drm_hal_write_reg(drm_reg_ptr(g_media_subctrl_base, 0x300), 255);
	if (drm_hal_read_reg(drm_reg_ptr(g_media_subctrl_base, 0x5300)) != 255U) {
		pr_err("%s: init pll status mismatch\n", __func__);
		return;
	}

	drm_hal_write_reg(drm_reg_ptr(g_media_subctrl_base, 0xA84), 1);
	if (drm_hal_read_reg(drm_reg_ptr(g_media_subctrl_base, 0x5A80)) != 0U) {
		pr_err("%s: subctrl init status mismatch\n", __func__);
		return;
	}

	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x10), 1);
	if (hal_vo_wait_src_status() <= 0) {
		pr_err("%s: src status timeout(1)\n", __func__);
		return;
	}

	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x10), 0);
	if (hal_vo_wait_src_status() <= 0) {
		pr_err("%s: src status timeout(0)\n", __func__);
		return;
	}

	drm_hal_write_reg(drm_reg_ptr(g_gpu_wrap_base, 0x24), 85);
	drm_hal_write_reg(drm_reg_ptr(g_gpu_wrap_base, 0x44), 85);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x24), 85);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x44), 85);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x824), 85);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x844), 85);
}

static void hal_vo_set_addr_ext(void)
{
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x0), 32);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x4), 32);
}

static void hal_vo_config_pll_pd(u32 dev_id)
{
	u32 __iomem *reg;

	if (g_media_subctrl_base == NULL)
		return;

	reg = (u32 __iomem *)((char __iomem *)g_media_subctrl_base + (dev_id << 6) + 4U);
	drm_hal_write_reg(reg, drm_hal_read_reg(reg) | 0x40000000U);
	drm_hal_write_reg(reg, drm_hal_read_reg(reg) | 0x20000000U);
	drm_hal_write_reg(reg, drm_hal_read_reg(reg) | 0x10000000U);
	drm_hal_write_reg(reg, drm_hal_read_reg(reg) | 0x02000000U);
}

static void hal_vo_set_pll_cfg(u32 dev_id, const struct ascend_vdp_pll_cfg *pll_cfg)
{
	u32 __iomem *base;
	u32 __iomem *cfg0_reg;
	u32 __iomem *cfg2_reg;
	u32 __iomem *ctrl_reg;
	u32 __iomem *lock_reg;
	u32 status = 0;
	u32 retry;
	u32 poll;
	u32 cfg0_lo;
	u32 cfg0_hi;
	u32 cfg1_lo;
	u32 cfg1_hi;

	if (pll_cfg == NULL || g_media_subctrl_base == NULL)
		return;

	cfg0_lo = lower_32_bits(pll_cfg->cfg0);
	cfg0_hi = upper_32_bits(pll_cfg->cfg0);
	cfg1_lo = lower_32_bits(pll_cfg->cfg1);
	cfg1_hi = upper_32_bits(pll_cfg->cfg1);

	base = (u32 __iomem *)((char __iomem *)g_media_subctrl_base + (dev_id << 6));
	cfg0_reg = base + 1;
	cfg2_reg = base + 2;
	ctrl_reg = base + 9;
	lock_reg = (u32 __iomem *)((char __iomem *)g_media_subctrl_base +
				    ASCEND_VDP_SC_PLL_LOCK_STATUS);

	drm_hal_write_reg(ctrl_reg, drm_hal_read_reg(ctrl_reg) | 0x1U);
	drm_hal_write_reg(base, drm_hal_read_reg(base) & ~0x2U);
	drm_hal_write_reg(base, drm_hal_read_reg(base) & ~0x1U);

	for (retry = 0; retry < 3; retry++) {
		hal_vo_config_pll_pd(dev_id);
		drm_hal_set_reg(cfg0_reg, cfg1_lo, 6, 0);
		drm_hal_set_reg(cfg0_reg, cfg0_lo, 12, 6);
		drm_hal_set_reg(cfg0_reg, cfg1_hi, 3, 18);
		drm_hal_set_reg(cfg0_reg, pll_cfg->div, 3, 21);
		drm_hal_set_reg(cfg0_reg, 0, 1, 26);
		drm_hal_set_reg(cfg2_reg, cfg0_hi, 32, 0);
		udelay(1);
		drm_hal_write_reg(cfg0_reg, drm_hal_read_reg(cfg0_reg) & ~0x02000000U);
		udelay(20);

		for (poll = 0; poll < 125; poll++) {
			status = drm_hal_read_reg(lock_reg) & 0x3U;
			udelay(1);
			status &= drm_hal_read_reg(lock_reg) & 0x3U;
			udelay(1);
			status &= drm_hal_read_reg(lock_reg) & 0x3U;
			udelay(1);
			if (status == 0x3U)
				break;
		}

		if (status == 0x3U)
			break;
	}

	if (status != 0x3U) {
		hal_vo_config_pll_pd(dev_id);
		pr_err("hal_vo_set_pll_cfg failed, dev=%u\n", dev_id);
		return;
	}

	drm_hal_write_reg(cfg0_reg, drm_hal_read_reg(cfg0_reg) & ~0x20000000U);
	drm_hal_write_reg(cfg0_reg, drm_hal_read_reg(cfg0_reg) | 0x10000000U);
	drm_hal_write_reg(cfg0_reg, drm_hal_read_reg(cfg0_reg) | 0x40000000U);
	drm_hal_write_reg(cfg0_reg, drm_hal_read_reg(cfg0_reg) & ~0x01000000U);
	udelay(1);
	drm_hal_write_reg(base, drm_hal_read_reg(base) | 0x1U);
	drm_hal_write_reg(base, drm_hal_read_reg(base) | 0x2U);
	drm_hal_write_reg(ctrl_reg, drm_hal_read_reg(ctrl_reg) | 0x2U);
	drm_hal_write_reg(ctrl_reg, drm_hal_read_reg(ctrl_reg) & ~0x2U);
	drm_hal_write_reg(ctrl_reg, drm_hal_read_reg(ctrl_reg) & ~0x1U);
}

static void hal_vdp_set_outstanding(void)
{
	u32 reg;

	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->mac_outstanding.u32);
	drm_hal_write_reg32(&g_gfbg_reg->mac_outstanding.u32,
			    (reg & 0xFFFFFF00U) | 0x6FU);
}

static void drm_hal_set_outstanding(void)
{
	hal_vdp_set_outstanding();
}

static void hal_vdp_qos_init(void)
{
	if (g_vdp_wrap_base == NULL || g_media_smmu_base == NULL ||
	    g_aa_media_base == NULL || g_cross_station_base == NULL)
		return;

	g_qos_default_reg = drm_hal_read_reg(drm_reg_ptr(g_vdp_wrap_base, 0x68));
	g_qos_smmu_reg0 = drm_hal_read_reg(drm_reg_ptr(g_media_smmu_base, 0xE0C));
	g_qos_aa_media_reg = drm_hal_read_reg(drm_reg_ptr(g_aa_media_base, 0x858));
	g_qos_smmu_reg1 = drm_hal_read_reg(drm_reg_ptr(g_media_smmu_base, 0xE10));
	g_qos_cross_station_reg = drm_hal_read_reg(drm_reg_ptr(g_cross_station_base, 0x2004));

	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x68), 67078);
	drm_hal_write_reg(drm_reg_ptr(g_media_smmu_base, 0xE0C), 2073572199U);
	drm_hal_write_reg(drm_reg_ptr(g_aa_media_base, 0x858), 4097);
	drm_hal_write_reg(drm_reg_ptr(g_media_smmu_base, 0xE10), 6092826);
	drm_hal_write_reg(drm_reg_ptr(g_cross_station_base, 0x2004), 656416807U);
}

static void hal_vdp_qos_exit(void)
{
	if (g_vdp_wrap_base == NULL || g_media_smmu_base == NULL ||
	    g_aa_media_base == NULL || g_cross_station_base == NULL)
		return;

	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x68), g_qos_default_reg);
	drm_hal_write_reg(drm_reg_ptr(g_media_smmu_base, 0xE0C), g_qos_smmu_reg0);
	drm_hal_write_reg(drm_reg_ptr(g_aa_media_base, 0x858), g_qos_aa_media_reg);
	drm_hal_write_reg(drm_reg_ptr(g_media_smmu_base, 0xE10), g_qos_smmu_reg1);
	drm_hal_write_reg(drm_reg_ptr(g_cross_station_base, 0x2004), g_qos_cross_station_reg);
}

static int hal_vdp_set_mpamid_config(u32 offset, u32 mask, u32 value)
{
	u32 reg;

	if (g_vdp_wrap_base == NULL)
		return -EINVAL;

	reg = drm_hal_read_reg(drm_reg_ptr(g_vdp_wrap_base, offset));
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, offset), (reg & mask) | value);
	return 0;
}

static void hal_vdp_set_priority(u32 qos)
{
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x68),
			  (qos + (qos << 8)) | 0x10000U);
}

static void hal_vdp_set_mpamid(u32 mpamid, u32 pmg)
{
	if (mpamid > 0xFFU) {
		pr_err("%s: invalid mpamid=%u\n", __func__, mpamid);
		return;
	}

	hal_vdp_set_mpamid_config(0x24, 0xFFF9FFFFU, pmg << 17);
	hal_vdp_set_mpamid_config(0x44, 0xFFF9FFFFU, pmg << 17);
	hal_vdp_set_mpamid_config(0x28, 0xFFC03FFFU, mpamid << 14);
	hal_vdp_set_mpamid_config(0x48, 0xFFC03FFFU, mpamid << 14);
}

static void hal_vdp_set_allow_aic(u32 threshold, u32 windows, u32 level0, u32 level1)
{
	u32 packed_lvl = level0 + (level1 << 16);
	u32 packed_th = threshold + 0x0F000000U;

	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x80), packed_th);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x94), packed_th);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x84), packed_lvl);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x98), packed_lvl);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x88), windows);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x9C), windows);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x8C), 1);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0xA0), 1);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x90), 0);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0x90), 1);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0xA4), 0);
	drm_hal_write_reg(drm_reg_ptr(g_vdp_wrap_base, 0xA4), 1);
	drm_hal_write_reg(drm_reg_ptr(g_media_subctrl_base, 0x3810), 0x4090);
	drm_hal_write_reg(drm_reg_ptr(g_media_subctrl_base, 0x3814), 0x4090);
	drm_hal_write_reg(drm_reg_ptr(g_media_subctrl_base, 0x3820), 9);
	drm_hal_write_reg(drm_reg_ptr(g_media_subctrl_base, 0x3818), 9);
	drm_hal_write_reg(drm_reg_ptr(g_media_subctrl_base, 0x3824), 0);
}

static void hal_vdp_get_allow_aic(u32 *threshold, u32 *windows, u32 *level0, u32 *level1)
{
	u32 packed_lvl;

	if (threshold != NULL)
		*threshold = drm_hal_read_reg(drm_reg_ptr(g_vdp_wrap_base, 0x80)) & 0xFFFFU;

	if (windows != NULL)
		*windows = drm_hal_read_reg(drm_reg_ptr(g_vdp_wrap_base, 0x88));

	packed_lvl = drm_hal_read_reg(drm_reg_ptr(g_vdp_wrap_base, 0x84));
	if (level0 != NULL)
		*level0 = packed_lvl & 0xFFFFU;
	if (level1 != NULL)
		*level1 = packed_lvl >> 16;
}

static int drm_set_qos_reg(int dev_id, const struct qos_master_config_type *cfg)
{
	if (dev_id != 0) {
		drm_err(g_drm, "dev_id:%d is wrong!\n", dev_id);
		return -EINVAL;
	}

	if (cfg == NULL || cfg->type != MASTER_VDP) {
		drm_err(g_drm, "qos cfg is NULL or cfg type is wrong!\n");
		return -EINVAL;
	}

	if (cfg->mode > 2U) {
		pr_err("%s: invalid qos mode=%u\n", __func__, cfg->mode);
		return -EINVAL;
	}

	if (cfg->mpamid > 0xFFU || cfg->pmg > 3U || cfg->qos > 0xFU) {
		drm_err(g_drm, "qos_master_config is ilegal!\n");
		return -EINVAL;
	}

	g_mpamid = cfg->mpamid;
	g_qos = cfg->qos;
	g_pmg = cfg->pmg;
	hal_vdp_set_mpamid(cfg->mpamid, cfg->pmg);
	hal_vdp_set_priority(cfg->qos);
	return 0;
}

static int drm_get_qos_reg(int dev_id, struct qos_master_config_type *cfg)
{
	if (dev_id != 0) {
		drm_err(g_drm, "dev_id:%d is wrong!\n", dev_id);
		return -EINVAL;
	}

	if (cfg == NULL || cfg->type != MASTER_VDP) {
		drm_err(g_drm, "qos cfg is NULL or cfg type is wrong!\n");
		return -EINVAL;
	}

	cfg->mpamid = g_mpamid;
	cfg->qos = g_qos;
	cfg->pmg = g_pmg;
	return 0;
}

static int drm_set_allow_qos_reg(int dev_id, const struct qos_allow_config_type *cfg)
{
	if (dev_id != 0) {
		drm_err(g_drm, "dev_id:%d is wrong!\n", dev_id);
		return -EINVAL;
	}

	if (cfg == NULL || cfg->master != MASTER_VDP) {
		drm_err(g_drm, "qos cfg is NULL or cfg master is wrong!\n");
		return -EINVAL;
	}

	if (cfg->qos_allow_mode > 1U) {
		pr_err("%s: invalid qos_allow_mode=%u\n", __func__, cfg->qos_allow_mode);
		return -EINVAL;
	}

	if (cfg->qos_allow_ctrl != 0U) {
		drm_err(g_drm, "qos_allow_mode or qos_allow_ctrl is wrong!\n");
		return -EINVAL;
	}

	if (cfg->qos_allow_mode == 1U &&
	    (cfg->qos_allow_lvl[0] >= cfg->qos_allow_lvl[1] ||
	     cfg->qos_allow_lvl[2] != 0U)) {
		drm_err(g_drm, "qos_allow_lvl is wrong!\n");
		return -EINVAL;
	}

	if (cfg->qos_allow_threshold > 0xFFFFU ||
	    cfg->qos_allow_windows > 0x0FFFFFFFU ||
	    cfg->qos_allow_lvl[0] == 0U ||
	    cfg->qos_allow_lvl[0] > 0xFFFFU ||
	    cfg->qos_allow_threshold > cfg->qos_allow_windows ||
	    (cfg->qos_allow_threshold == 0U &&
	     cfg->qos_allow_windows == 0U) ||
	    cfg->qos_allow_lvl[1] == 0U ||
	    cfg->qos_allow_lvl[1] > 0xFFFFU) {
		drm_err(g_drm, "qos param is wrong!\n");
		return -EINVAL;
	}

	hal_vdp_set_allow_aic(cfg->qos_allow_threshold,
			      cfg->qos_allow_windows,
			      cfg->qos_allow_lvl[0],
			      cfg->qos_allow_lvl[1]);
	return 0;
}

static int drm_get_allow_qos_reg(int dev_id, struct qos_allow_config_type *cfg)
{
	u32 threshold = 0;
	u32 windows = 0;
	u32 level0 = 0;
	u32 level1 = 0;

	if (dev_id != 0) {
		drm_err(g_drm, "dev_id:%d is wrong!\n", dev_id);
		return -EINVAL;
	}

	if (cfg == NULL || cfg->master != MASTER_VDP) {
		drm_err(g_drm, "qos cfg is NULL or cfg master is wrong!\n");
		return -EINVAL;
	}

	if (cfg->qos_allow_ctrl != 0U) {
		drm_err(g_drm, "qos_allow_ctrl is wrong!\n");
		return -EINVAL;
	}

	hal_vdp_get_allow_aic(&threshold, &windows, &level0, &level1);
	cfg->qos_allow_threshold = threshold;
	cfg->qos_allow_windows = windows;
	cfg->qos_allow_lvl[0] = level0;
	cfg->qos_allow_lvl[1] = level1;
	return 0;
}

static int drm_qos_node_register(struct ascend_vdp_drm_private *priv)
{
	int ret;

	memset(&priv->qos_master, 0, sizeof(priv->qos_master));
	if ((int)strscpy(priv->qos_master.name, ASCEND_VDP_QOS_NAME,
			 sizeof(priv->qos_master.name)) < 0) {
		dev_err(priv->dev, "MASTER_VDP copy name failed!\n");
		return -EINVAL;
	}

	priv->qos_master.cfg.type = MASTER_VDP;
	priv->qos_master.set = drm_set_qos_reg;
	priv->qos_master.get = drm_get_qos_reg;
	priv->qos_master.set_allow = drm_set_allow_qos_reg;
	priv->qos_master.get_allow = drm_get_allow_qos_reg;

	ret = hal_kernel_qos_node_register(&priv->qos_master);
	if (ret)
		dev_err(priv->dev, "MASTER_VDP register failed!\n");

	return ret;
}

static void drm_qos_node_unregister(struct ascend_vdp_drm_private *priv)
{
	(void)hal_kernel_qos_node_unregister(&priv->qos_master);
	memset(&priv->qos_master, 0, sizeof(priv->qos_master));
}

static void drm_qos_notify_online(void)
{
	pr_info("Notify QOS vo is enable\n");
	drm_hal_set_outstanding();
	hal_vdp_qos_init();
	(void)hal_kernel_qos_notify_module_online(0, MASTER_VDP);
}

static void drm_qos_notify_offline(void)
{
	pr_info("Notify QOS vo is disable\n");
	hal_vdp_qos_exit();
	(void)hal_kernel_qos_notify_module_offline(0, MASTER_VDP);
}

static int drm_hal_vou_init(void)
{
	return vou_graphics_resource_init();
}

static int drm_hal_vou_exit(void)
{
	return vou_graphics_resource_deinit();
}

static int drm_init_vou_resource(void)
{
	int ret;

	ret = drm_hal_vou_init();
	if (ret)
		return ret;

	return vou_graphics_init();
}

static int drm_deinit_vou_resource(void)
{
	(void)vou_graphics_deinit();
	return drm_hal_vou_exit();
}

static int drm_resource_init(void)
{
	return drm_init_vou_resource();
}

static void drm_resource_deinit(void)
{
	(void)drm_deinit_vou_resource();
}

static u64 drm_pack_xy(td_s32 x, td_s32 y)
{
	return (u64)(u16)x | ((u64)(u16)y << 32);
}

static u64 drm_pack_wh(u32 width, u32 height)
{
	return (u64)(u16)width | ((u64)(u16)height << 32);
}

static int drm_hal_set_layer_disp_rect(u32 layer_id, u64 pos, u64 size)
{
	ot_fb_rect rect = {
		.x = (td_s32)(u16)pos,
		.y = (td_s32)(u16)(pos >> 32),
		.width = (u32)(u16)size,
		.height = (u32)(u16)(size >> 32),
	};

	(void)layer_id;
	return fb_hal_video_set_layer_disp_rect(ASCEND_VDP_HW_LAYER, &rect) == TD_FALSE ? -EINVAL : 0;
}

static int drm_hal_set_layer_video_rect(u32 layer_id, u64 pos, u64 size)
{
	ot_fb_rect rect = {
		.x = (td_s32)(u16)pos,
		.y = (td_s32)(u16)(pos >> 32),
		.width = (u32)(u16)size,
		.height = (u32)(u16)(size >> 32),
	};

	(void)layer_id;
	return fb_hal_video_set_layer_video_rect(ASCEND_VDP_HW_LAYER, &rect) == TD_FALSE ? -EINVAL : 0;
}

static int drm_set_layer_disp_rect(u32 layer_id, u64 pos, u64 size)
{
	(void)drm_hal_set_layer_disp_rect(layer_id, pos, size);
	return 0;
}

static int drm_set_layer_video_rect(u32 layer_id, u64 pos, u64 size)
{
	(void)drm_hal_set_layer_video_rect(layer_id, pos, size);
	return 0;
}

static int drm_set_layer_rect(u32 layer_id, u64 src_pos, u64 src_size,
			      u64 dst_pos, u64 dst_size)
{
	(void)layer_id;
	if (drm_set_layer_disp_rect(ASCEND_VDP_LAYER, dst_pos, dst_size) != 0)
		return -EINVAL;
	return drm_set_layer_video_rect(ASCEND_VDP_LAYER, src_pos, src_size);
}

static int drm_hal_set_src_resolution(u32 layer_id, u64 pos, u64 size)
{
	ot_fb_rect rect = {
		.x = 0,
		.y = 0,
		.width = (u32)(u16)size,
		.height = (u32)(u16)(size >> 32),
	};

	(void)pos;
	(void)layer_id;
	if (fb_hal_layer_set_src_resolution(ASCEND_VDP_HW_LAYER, &rect) == TD_FALSE)
		return -EINVAL;
	return 0;
}

static int drm_hal_set_layer_in_rect(u32 layer_id, u64 pos, u64 size)
{
	ot_fb_rect rect = {
		.x = 0,
		.y = 0,
		.width = (u32)(u16)size,
		.height = (u32)(u16)(size >> 32),
	};

	(void)pos;
	(void)layer_id;
	return fb_hal_layer_set_layer_in_rect(ASCEND_VDP_HW_LAYER, &rect) == TD_FALSE ? -EINVAL : 0;
}

static int drm_set_gfx_src_reso(u32 layer_id, u64 pos, u64 size)
{
	(void)drm_hal_set_src_resolution(layer_id, pos, size);
	(void)drm_hal_set_layer_in_rect(layer_id, pos, size);
	return 0;
}

static int drm_set_gfx_zme(u32 layer_id, u32 src_w, u32 src_h,
			   u32 dst_w, u32 dst_h, bool enable)
{
	(void)layer_id;
	if (!enable) {
		u32 hratio = (u32)(((u64)src_w << 20) / dst_w);
		u32 vratio = (u32)(((u64)src_h << 12) / dst_h);

		hal_g0_zme_set_ck_gt_en(1);
		hal_g0_zme_set_out_width(dst_w);
		hal_g0_zme_set_hfir_en(0);
		hal_g0_zme_set_ahfir_mid_en(1);
		hal_g0_zme_set_lhfir_mid_en(1);
		hal_g0_zme_set_chfir_mid_en(1);
		hal_g0_zme_set_lhfir_mode(1);
		hal_g0_zme_set_ahfir_mode(1);
		hal_g0_zme_set_hfir_order(1);
		hal_g0_zme_set_hratio(hratio);
		hal_g0_zme_set_lhfir_offset(0);
		hal_g0_zme_set_chfir_offset(0);
		hal_g0_zme_set_out_pro(1);
		hal_g0_zme_set_out_height(dst_h);
		hal_g0_zme_set_vfir_en(0);
		hal_g0_zme_set_avfir_mid_en(1);
		hal_g0_zme_set_lvfir_mid_en(1);
		hal_g0_zme_set_cvfir_mid_en(1);
		hal_g0_zme_set_lvfir_mode(1);
		hal_g0_zme_set_vafir_mode(1);
		hal_g0_zme_set_vratio(vratio);
		hal_g0_zme_set_vtp_offset(0);
		hal_g0_zme_set_vbtm_offset((u32)-2048);
	}

	return 0;
}

static int drm_set_layer_reso(u32 layer_id, u64 src_pos, u64 src_size)
{
	(void)layer_id;
	return drm_set_gfx_src_reso(ASCEND_VDP_LAYER, src_pos, src_size);
}

static int drm_drv_enable_zme(u32 layer_id, u32 src_w, u32 src_h,
			      u32 dst_w, u32 dst_h, bool enable)
{
	(void)layer_id;
	return drm_set_gfx_zme(ASCEND_VDP_LAYER, src_w, src_h, dst_w, dst_h, enable);
}

static int drm_set_layer_alpha(u32 layer_id, u32 alpha_en, u32 global_alpha, u32 format)
{
	(void)format;
	(void)layer_id;
	return drm_set_gfx_galpha(ASCEND_VDP_LAYER, alpha_en,
				  (u8)(global_alpha & 0xff));
}

static int drm_set_gfx_galpha(u32 layer_id, u32 alpha_en, u8 global_alpha)
{
	(void)alpha_en;
	(void)drm_hal_set_layer_galpha(layer_id, global_alpha);
	return 0;
}

static int drm_hal_set_layer_data_fmt(u32 layer_id, u32 fmt)
{
	(void)layer_id;
	return fb_hal_layer_set_layer_data_fmt(ASCEND_VDP_HW_LAYER,
					       (hal_disp_pixel_format)fmt) == TD_FALSE ? -EINVAL : 0;
}

static int drm_hal_set_layer_galpha(u32 layer_id, u8 alpha)
{
	(void)layer_id;
	return fb_hal_layer_set_layer_galpha(ASCEND_VDP_HW_LAYER,
					     alpha) == TD_FALSE ? -EINVAL : 0;
}

static int drm_hal_set_gfx_stride(u32 layer_id, u16 stride)
{
	(void)layer_id;
	return fb_hal_graphic_set_gfx_stride(ASCEND_VDP_HW_LAYER,
					     stride) == TD_FALSE ? -EINVAL : 0;
}

static int drm_hal_set_gfx_addr(u32 layer_id, dma_addr_t addr)
{
	(void)layer_id;
	return fb_hal_graphic_set_gfx_addr(ASCEND_VDP_HW_LAYER,
					    addr) == TD_FALSE ? -EINVAL : 0;
}

static int drm_hal_set_layer_enable(u32 layer_id, u32 enable)
{
	(void)layer_id;
	return fb_hal_set_layer_enable(ASCEND_VDP_HW_LAYER,
				       enable ? TD_TRUE : TD_FALSE) == TD_TRUE ? 0 : -EINVAL;
}

static void drm_hal_set_layer_ck_gt_en(u32 layer_id, u32 enable)
{
	(void)layer_id;
	fb_hal_set_layer_ck_gt_en(ASCEND_VDP_HW_LAYER, enable ? TD_TRUE : TD_FALSE);
}

static int drm_hal_layer_set_reg_up(u32 layer_id, u32 enable)
{
	(void)layer_id;
	(void)enable;
	return fb_hal_layer_set_reg_up(ASCEND_VDP_HW_LAYER) == TD_FALSE ? -EINVAL : 0;
}

static int drm_set_gfx_data_fmt(u32 layer_id, u32 fmt)
{
	u32 hal_fmt = 73;

	(void)layer_id;
	if (fmt == 5U)
		hal_fmt = 72;
	else if (fmt == 7U)
		hal_fmt = 104;

	(void)drm_hal_set_layer_data_fmt(layer_id, hal_fmt);
	return 0;
}

static int drm_set_layer_fmt(u32 layer_id, u32 fmt)
{
	(void)layer_id;
	return drm_set_gfx_data_fmt(ASCEND_VDP_LAYER, fmt);
}

static int drm_set_gfx_stride(u32 layer_id, u16 stride)
{
	(void)drm_hal_set_gfx_stride(layer_id, stride >> 4);
	return 0;
}

static int drm_set_layer_stride(u32 layer_id, u32 stride)
{
	(void)layer_id;
	return drm_set_gfx_stride(ASCEND_VDP_LAYER, (u16)stride);
}

static int drm_set_gfx_addr(u32 layer_id, dma_addr_t addr)
{
	(void)drm_hal_set_gfx_addr(layer_id, addr);
	return 0;
}

static int drm_set_layer_addr(u32 layer_id, dma_addr_t addr)
{
	(void)layer_id;
	return drm_set_gfx_addr(ASCEND_VDP_LAYER, addr);
}

static int drm_set_gfx_enable(u32 layer_id, u32 enable)
{
	(void)drm_hal_set_layer_enable(layer_id, enable);
	drm_hal_set_layer_ck_gt_en(layer_id, enable);
	(void)drm_hal_layer_set_reg_up(layer_id, enable);
	return 0;
}

static int drm_set_layer(u32 layer_id, u32 enable)
{
	(void)layer_id;
	return drm_set_gfx_enable(ASCEND_VDP_LAYER, enable);
}

static void drm_hal_set_dac_cablectr(u32 channel, u32 cablectr)
{
	u32 reg;
	volatile u32 *dac_ctrl = NULL;

	if (!drm_hal_reg_ready())
		return;

	switch (channel) {
	case 0:
		dac_ctrl = &g_gfbg_reg->vo_dac0_ctrl.u32;
		break;
	case 1:
		dac_ctrl = &g_gfbg_reg->vo_dac1_ctrl.u32;
		break;
	case 2:
		dac_ctrl = &g_gfbg_reg->vo_dac2_ctrl.u32;
		break;
	case 3:
		dac_ctrl = &g_gfbg_reg->vo_dac3_ctrl.u32;
		break;
	default:
		return;
	}

	reg = drm_hal_read_reg32(dac_ctrl);
	reg = (reg & ~0x3U) | (cablectr & 0x3U);
	drm_hal_write_reg32(dac_ctrl, reg);
}

static void drm_hal_set_mux_sel(u32 sel)
{
	u32 reg;

	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->vo_mux.u32);
	reg = (reg & 0xF0FFFFFFU) | ((sel & 0xFU) << 24);
	drm_hal_write_reg32(&g_gfbg_reg->vo_mux.u32, reg);
}

static void drm_hal_set_dac_sel(u32 channel)
{
	u32 reg;

	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->vomux_dac.u32);
	if (channel == 2)
		reg = (reg & 0xFFFFF000U) | 0x987U;
	else
		reg &= 0xFFFF0FFFU;
	drm_hal_write_reg32(&g_gfbg_reg->vomux_dac.u32, reg);
}

static void drm_hal_set_vga_and_cvbs_dac_enable(bool enable)
{
	u32 reg;

	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->vo_dac_ctrl.u32);
	reg = (reg & 0xFFBFFFFFU) | ((enable ? 1U : 0U) << 22);
	drm_hal_write_reg32(&g_gfbg_reg->vo_dac_ctrl.u32, reg);
}

static void drm_hal_set_dac_chn_enable(u32 channel, bool enable)
{
	u32 reg;

	if (!drm_hal_reg_ready() || channel != 2)
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->vo_dac0_ctrl.u32);
	drm_hal_write_reg32(&g_gfbg_reg->vo_dac0_ctrl.u32,
			    (reg & 0x7FFFFFFFU) | ((enable ? 1U : 0U) << 31));
	reg = drm_hal_read_reg32(&g_gfbg_reg->vo_dac1_ctrl.u32);
	drm_hal_write_reg32(&g_gfbg_reg->vo_dac1_ctrl.u32,
			    (reg & 0x7FFFFFFFU) | ((enable ? 1U : 0U) << 31));
	reg = drm_hal_read_reg32(&g_gfbg_reg->vo_dac2_ctrl.u32);
	drm_hal_write_reg32(&g_gfbg_reg->vo_dac2_ctrl.u32,
			    (reg & 0x7FFFFFFFU) | ((enable ? 1U : 0U) << 31));
}

static void drm_hal_set_dac_gc(u32 channel, u8 gain)
{
	u32 reg;
	u32 value = ((u32)gain & 0x3FU) << 4;

	if (!drm_hal_reg_ready() || channel != 2)
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->vo_dac0_ctrl.u32);
	drm_hal_write_reg32(&g_gfbg_reg->vo_dac0_ctrl.u32,
			    (reg & 0xFFFFFC0FU) | value);
	reg = drm_hal_read_reg32(&g_gfbg_reg->vo_dac1_ctrl.u32);
	drm_hal_write_reg32(&g_gfbg_reg->vo_dac1_ctrl.u32,
			    (reg & 0xFFFFFC0FU) | value);
	reg = drm_hal_read_reg32(&g_gfbg_reg->vo_dac2_ctrl.u32);
	drm_hal_write_reg32(&g_gfbg_reg->vo_dac2_ctrl.u32,
			    (reg & 0xFFFFFC0FU) | value);
}

static void drm_hal_set_vga_sync_inv(const struct ascend_vdp_sync_inv *inv)
{
	u32 reg;

	if (!drm_hal_reg_ready() || inv == NULL)
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->intf_vga_sync_inv.u32);
	reg &= 0xFFFFFFF0U;
	reg |= (inv->dv_inv & 0x1U) << 3;
	reg |= (inv->vs_inv & 0x1U) << 2;
	reg |= (inv->hs_inv & 0x1U) << 1;
	reg |= inv->field_inv & 0x1U;
	drm_hal_write_reg32(&g_gfbg_reg->intf_vga_sync_inv.u32, reg);
}

static void drm_hal_set_sync_info(u32 dev_id, const hal_disp_syncinfo *sync_info)
{
	u32 reg;
	u16 hmid;

	(void)dev_id;
	if (!drm_hal_reg_ready() || sync_info == NULL)
		return;

	drm_hal_write_reg32(&g_gfbg_reg->dhd0_hsync1.u32,
			    ((u32)(sync_info->hbb - 1U) << 16) |
			    (u32)(sync_info->hact - 1U));

	hmid = sync_info->hmid == 0 ? 0 : (u16)(sync_info->hmid - 1U);
	drm_hal_write_reg32(&g_gfbg_reg->dhd0_hsync2.u32,
			    ((u32)hmid << 16) | (u32)(sync_info->hfb - 1U));

	drm_hal_write_reg32(&g_gfbg_reg->dhd0_vsync1.u32,
			    ((u32)(sync_info->vbb - 1U) << 16) |
			    (u32)(sync_info->vact - 1U));

	reg = drm_hal_read_reg32(&g_gfbg_reg->dhd0_vsync2.u32);
	reg = (reg & 0xFFFF0000U) | (u32)(sync_info->vfb - 1U);
	drm_hal_write_reg32(&g_gfbg_reg->dhd0_vsync2.u32, reg);

	reg = drm_hal_read_reg32(&g_gfbg_reg->dhd0_ctrl.u32);
	reg = (reg & 0xFFFFFFEFU) | ((sync_info->iop & 0x1U) << 4);
	drm_hal_write_reg32(&g_gfbg_reg->dhd0_ctrl.u32, reg);

	drm_hal_write_reg32(&g_gfbg_reg->dhd0_vplus1.u32,
			    ((u32)(sync_info->bvbb - 1U) << 16) |
			    (u32)(sync_info->bvact - 1U));

	reg = drm_hal_read_reg32(&g_gfbg_reg->dhd0_vplus2.u32);
	reg = (reg & 0xFFFF0000U) | (u32)(sync_info->bvfb - 1U);
	drm_hal_write_reg32(&g_gfbg_reg->dhd0_vplus2.u32, reg);

	reg = drm_hal_read_reg32(&g_gfbg_reg->dhd0_pwr.u32);
	reg = (reg & 0xFF000000U) |
	      ((u32)((sync_info->vpw - 1U) & 0xFFU) << 16) |
	      (u32)(sync_info->hpw - 1U);
	drm_hal_write_reg32(&g_gfbg_reg->dhd0_pwr.u32, reg);
}

static void drm_hal_set_dev_multi_chn_en(u32 dev_id, u8 enable)
{
	u32 reg;

	(void)dev_id;
	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->dhd0_pwr.u32);
	reg = (reg & 0xE7FFFFFFU) | (((u32)enable & 0x3U) << 27);
	drm_hal_write_reg32(&g_gfbg_reg->dhd0_pwr.u32, reg);
}

static void drm_hal_set_intf_hdmi_format(u32 dev_id, bool enable)
{
	u32 reg;

	(void)dev_id;
	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->intf_hdmi_ctrl.u32);
	reg = (reg & 0xFFFFFFBFU) | ((enable ? 1U : 0U) << 6);
	drm_hal_write_reg32(&g_gfbg_reg->intf_hdmi_ctrl.u32, reg);
}

static void drm_hal_set_vtthd_mode(u32 dev_id, bool mode)
{
	u32 reg;

	(void)dev_id;
	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->dhd0_vtthd.u32);
	reg = (reg & 0xFFFF7FFFU) | ((mode ? 1U : 0U) << 15);
	drm_hal_write_reg32(&g_gfbg_reg->dhd0_vtthd.u32, reg);
}

static void drm_hal_set_intf_enable(u32 dev_id, u8 enable)
{
	u32 reg;

	(void)dev_id;
	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->dhd0_ctrl.u32);
	reg = (reg & 0x7FFFFFFFU) | (((u32)enable & 0x1U) << 31);
	drm_hal_write_reg32(&g_gfbg_reg->dhd0_ctrl.u32, reg);
}

static void drm_hal_set_layer_up_mode(u32 layer_id, bool mode)
{
	u32 reg;

	(void)layer_id;
	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->g0_ctrl.u32);
	reg = (reg & 0xDFFFFFFFU) | ((mode ? 1U : 0U) << 29);
	drm_hal_write_reg32(&g_gfbg_reg->g0_ctrl.u32, reg);
}

static void drm_hal_set_reg_up(u32 dev_id)
{
	u32 reg;

	(void)dev_id;
	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->dhd0_ctrl.u32);
	drm_hal_write_reg32(&g_gfbg_reg->dhd0_ctrl.u32, reg | 0x1U);
}

static void drm_hal_set_layer_global_alpha(u32 layer_id, u32 alpha)
{
	u32 reg;

	(void)layer_id;
	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->g0_ctrl.u32);
	reg = (reg & 0xFFFFFF00U) | (alpha & 0xFFU);
	drm_hal_write_reg32(&g_gfbg_reg->g0_ctrl.u32, reg);
}

static void drm_hal_set_layer_alpha(u32 layer_id, u32 alpha)
{
	u32 reg;

	(void)layer_id;
	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->g0_alpha.u32);
	reg = (reg & 0xFFFFFF00U) | (alpha & 0xFFU);
	drm_hal_write_reg32(&g_gfbg_reg->g0_alpha.u32, reg);
}

static void drm_hal_set_pixel_alpha_range(u32 layer_id, bool enable)
{
	u32 reg;

	(void)layer_id;
	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->gfx_out_ctrl.u32);
	reg = (reg & 0xFFFFFFFEU) | (enable ? 0x1U : 0U);
	drm_hal_write_reg32(&g_gfbg_reg->gfx_out_ctrl.u32, reg);
}

static void drm_hal_set_cbm_mixer_prio(u32 layer_id, u32 slot, u32 dev_id)
{
	u32 reg;

	(void)layer_id;
	(void)dev_id;
	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->cbm_mix1.u32);
	switch (slot) {
	case 0:
		reg = (reg & 0xFFFFFFF0U) | 0x2U;
		break;
	case 1:
		reg = (reg & 0xFFFFFF0FU) | 0x20U;
		break;
	case 2:
		reg = (reg & 0xFFFFF0FFU) | 0x200U;
		break;
	case 3:
		reg = (reg & 0xFFFF0FFFU) | 0x2000U;
		break;
	case 4:
		reg = (reg & 0xFFF0FFFFU) | 0x20000U;
		break;
	default:
		return;
	}
	drm_hal_write_reg32(&g_gfbg_reg->cbm_mix1.u32, reg);
}

static void drm_hal_set_int_mask(u32 mask)
{
	u32 reg;

	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->vointmsk.u32);
	drm_hal_write_reg32(&g_gfbg_reg->vointmsk.u32, reg | mask);
}

static void drm_hal_clr_int_mask(u32 mask)
{
	u32 reg;

	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->vointmsk.u32);
	drm_hal_write_reg32(&g_gfbg_reg->vointmsk.u32, reg & ~mask);
}

static void drm_hal_set_vtthd(u32 dev_id, u16 thd1)
{
	u32 reg;

	(void)dev_id;
	if (!drm_hal_reg_ready())
		return;

	reg = drm_hal_read_reg32(&g_gfbg_reg->dhd0_vtthd.u32);
	reg = (reg & 0xE000E000U) | (thd1 & 0x1FFFU) | (((u32)thd1 & 0x1FFFU) << 16);
	drm_hal_write_reg32(&g_gfbg_reg->dhd0_vtthd.u32, reg);
}

static int drm_drv_get_sync_info(hal_disp_syncinfo *sync_info)
{
	if (sync_info == NULL)
		return -EINVAL;

	if (g_vo_sync >= ARRAY_SIZE(g_drm_sync_timing))
		return -EINVAL;

	*sync_info = g_drm_sync_timing[g_vo_sync];
	return 0;
}

static int drm_drv_get_sync_inv(struct ascend_vdp_sync_inv *sync_inv)
{
	hal_disp_syncinfo sync_info;
	int ret;

	if (sync_inv == NULL)
		return -EINVAL;

	ret = drm_drv_get_sync_info(&sync_info);
	if (ret)
		return ret;

	memset(sync_inv, 0, sizeof(*sync_inv));
	sync_inv->field_inv = 0;
	sync_inv->hs_inv = sync_info.ivs ? 1U : 0U;
	sync_inv->vs_inv = sync_info.ihs ? 1U : 0U;
	sync_inv->dv_inv = sync_info.idv ? 1U : 0U;
	return 0;
}

static void drm_set_vtth(u32 dev_id)
{
	hal_disp_syncinfo sync_info;
	u16 thd1;

	if (drm_drv_get_sync_info(&sync_info) != 0)
		return;

	if (sync_info.iop == 1U)
		thd1 = sync_info.vact + sync_info.vbb + sync_info.vfb;
	else
		thd1 = (sync_info.vact + sync_info.vbb + sync_info.vfb +
			sync_info.bvact + sync_info.bvbb + sync_info.bvfb) >> 1;

	drm_hal_set_vtthd(dev_id, (u16)(thd1 - 241U));
}

static int drm_bind_gfx_layer_dev(u32 layer_id, u32 dev_id)
{
	if (layer_id < HAL_DISP_LAYER_GFX0 || layer_id > HAL_DISP_LAYER_GFX4) {
		drm_err(g_drm, "gfx layer %d is illegal!\n", (int)layer_id);
		return -1;
	}

	drm_hal_set_cbm_mixer_prio(layer_id, 0, dev_id);
	drm_hal_set_reg_up(dev_id);
	return 0;
}

static int drm_bind_vo_dev(u32 layer_id, u32 dev_id)
{
	return drm_bind_gfx_layer_dev(layer_id, dev_id);
}

static void drm_init_default_setting(void)
{
	unsigned long flags;

	spin_lock_irqsave(&g_drm_lock, flags);
	drm_hal_set_dac_cablectr(2, 3);
	drm_hal_set_layer_global_alpha(ASCEND_VDP_LAYER, 0xFFFFFFFFU);
	drm_hal_set_layer_alpha(ASCEND_VDP_LAYER, 255);
	drm_hal_set_pixel_alpha_range(ASCEND_VDP_LAYER, true);
	drm_hal_set_cbm_mixer_prio(ASCEND_VDP_LAYER, 0, 0);
	drm_hal_set_reg_up(0);
	drm_hal_set_outstanding();
	drm_hal_clear_int_status(0xFFFFFFFFU);
	drm_hal_set_intf_hdmi_format(0, true);
	spin_unlock_irqrestore(&g_drm_lock, flags);
}

static int drm_register_irq(struct drm_crtc *crtc)
{
	int ret;

	drm_dbg(g_drm, "request_irq %u for drm device!\n", g_drm_irq);
	ret = drm_request_irq(g_drm_irq, drm_interrupt_route, 0, "DRM_0", crtc);
	if (ret) {
		drm_err(g_drm, "request_irq for drm failure!\n");
	}

	g_low_bandwidth_cnt = 0;
	g_bus_err_cnt = 0;
	return ret;
}

static void drm_unregister_irq(struct drm_crtc *crtc)
{
	if (g_drm_irq > 0) {
		drm_dbg(g_drm, "unrequest irq %u for drm device!\n", g_drm_irq);
		drm_free_irq(g_drm_irq, crtc);
	}
}

static void drm_dev_int_enable(u32 dev_id, int enable)
{
	struct ascend_vdp_drm_dev_ctx *dev_ctx;
	u32 int_mask = (dev_id <= 1U) ? 0x1U : 0U;

	dev_ctx = drm_get_dev_ctx(dev_id);
	if (enable == 1)
		drm_hal_set_int_mask(int_mask);
	else
		drm_hal_clr_int_mask(int_mask);

	dev_ctx->init_irq = (u32)enable;
}

static int drm_init(struct drm_crtc *crtc)
{
	struct ascend_vdp_drm_dev_ctx *dev_ctx;
	int ret;

	if (crtc == NULL) {
		if (g_drm != NULL)
			drm_err(g_drm, "crtc is NULL!\n");
		return -1;
	}

	dev_ctx = drm_get_dev_ctx(0);
	if (dev_ctx->init_irq == 1U) {
		pr_info("[drm] dev_ctx enabled!\n");
		return 0;
	}

	drm_init_default_setting();
	ret = drm_register_irq(crtc);
	if (ret)
		return ret;

	drm_dev_int_enable(0, 1);
	return 0;
}

static int drm_deinit(struct drm_crtc *crtc)
{
	struct ascend_vdp_drm_dev_ctx *dev_ctx;

	if (crtc == NULL) {
		if (g_drm != NULL)
			drm_err(g_drm, "crtc is NULL!\n");
		return -1;
	}

	dev_ctx = drm_get_dev_ctx(0);
	if (dev_ctx == NULL || dev_ctx->init_irq != 1U)
		return 0;

	drm_dev_int_enable(0, 0);
	msleep(50);
	drm_unregister_irq(crtc);
	return 0;
}

static void drm_drv_open(u32 dev_id)
{
	hal_disp_syncinfo sync_info;
	struct ascend_vdp_sync_inv sync_inv;

	if (drm_drv_get_sync_inv(&sync_inv) != 0)
		return;

	if (drm_drv_get_sync_info(&sync_info) != 0)
		return;

	drm_hal_set_mux_sel(dev_id);
	drm_hal_set_dac_sel(2);
	drm_hal_set_vga_and_cvbs_dac_enable(true);
	drm_hal_set_dac_chn_enable(2, true);
	drm_hal_set_dac_gc(2, 11);
	drm_hal_set_vga_sync_inv(&sync_inv);
	drm_hal_set_sync_info(dev_id, &sync_info);
	drm_hal_set_dev_multi_chn_en(dev_id, 1);
	drm_hal_set_vtthd_mode(dev_id, false);
	drm_hal_set_layer_up_mode(ASCEND_VDP_LAYER, false);
	drm_set_vtth(dev_id);
	drm_hal_set_intf_enable(dev_id, 1);
	drm_hal_set_reg_up(dev_id);
}

static int drm_open_dev(void)
{
	drm_drv_open(0);
	return 0;
}

static int drm_bind_dev(void)
{
	return drm_bind_vo_dev(ASCEND_VDP_LAYER, ASCEND_VDP_LAYER);
}

static int drm_open_layer(u32 layer_id)
{
	vo_gfxlayer_context *gfx_layer_ctx;
	unsigned long lock_flag = 0;
	u32 layer_index = layer_id;

	if (layer_index > GRAPHICS_LAYER_G4) {
		drm_err(g_drm, "gfx_layer(%u) is invalid!\n", layer_index);
		return -EINVAL;
	}

	gfx_layer_ctx = vou_get_gfx_layer_ctx();
	if (gfx_layer_ctx == NULL)
		return -EINVAL;

	gfx_spin_lock_irqsave(&gfx_layer_ctx[layer_index].spin_lock, &lock_flag);
	if (!gfx_layer_ctx[layer_index].binded) {
		gfx_spin_unlock_irqrestore(&gfx_layer_ctx[layer_index].spin_lock, &lock_flag);
		drm_err(g_drm, "graphics layer %u has not been binded!\n", layer_index);
		return -EINVAL;
	}

	gfx_layer_ctx[layer_index].opened = TD_TRUE;
	gfx_spin_unlock_irqrestore(&gfx_layer_ctx[layer_index].spin_lock, &lock_flag);
	return 0;
}

static int drm_get_irq_byname(struct platform_device *pdev, const char *name)
{
	return platform_get_irq_byname(pdev, name);
}

static u32 drm_hal_read_int_status(void)
{
	return drm_hal_read_reg32(&g_gfbg_reg->vomskintsta.u32);
}

static void drm_hal_clear_int_status(u32 int_status)
{
	drm_hal_write_reg32(&g_gfbg_reg->vomskintsta.u32, int_status);
}

static irqreturn_t drm_interrupt_route(int irq, void *data)
{
	struct drm_crtc *crtc = data;
	u32 int_status;

	(void)irq;
	int_status = drm_hal_read_int_status();
	if (int_status & 0x8) {
		g_low_bandwidth_cnt++;
		if (__ratelimit(&g_low_bandwidth_rs))
			pr_warn("drm_drv_low_bandwidth cnt=%u\n", g_low_bandwidth_cnt);
	}

	if (int_status & 0x1000) {
		g_bus_err_cnt++;
		if (__ratelimit(&g_bus_err_rs))
			pr_warn("drm_drv_bus_err cnt=%u\n", g_bus_err_cnt);
	}

	drm_hal_clear_int_status(int_status);
	if (crtc != NULL)
		drm_crtc_handle_vblank(crtc);

	return IRQ_HANDLED;
}

static void drm_irq_init(int irq)
{
	drm_set_init_irq(irq);
}

static int init_hmdi_resource(struct ascend_vdp_drm_private *priv)
{
	priv->hdmi0_phy = ioremap(0x400180000ULL, 0x10000);
	if (priv->hdmi0_phy == NULL)
		return -ENOMEM;
	(void)hdmi_set_phy(0, (char *)priv->hdmi0_phy);

	priv->hdmi0_reg = ioremap(0x4001A0000ULL, 0x20000);
	if (priv->hdmi0_reg == NULL)
		return -ENOMEM;
	(void)hdmi_set_reg(0, (char *)priv->hdmi0_reg);

	priv->hdmi1_phy = ioremap(0x400190000ULL, 0x10000);
	if (priv->hdmi1_phy == NULL)
		return -ENOMEM;
	(void)hdmi_set_phy(1, (char *)priv->hdmi1_phy);

	priv->hdmi1_reg = ioremap(0x4001C0000ULL, 0x20000);
	if (priv->hdmi1_reg == NULL)
		return -ENOMEM;
	(void)hdmi_set_reg(1, (char *)priv->hdmi1_reg);

	(void)hdmi_drv_mod_init();
	drv_hdmi_phy_product_power_set_param();
	return 0;
}

static void release_hdmi_resource(struct ascend_vdp_drm_private *priv)
{
	hdmi_drv_mod_exit();
	(void)hdmi_set_reg(0, NULL);
	(void)hdmi_set_reg(1, NULL);

	if (priv->hdmi0_phy != NULL) {
		iounmap(priv->hdmi0_phy);
		priv->hdmi0_phy = NULL;
	}

	if (priv->hdmi0_reg != NULL) {
		iounmap(priv->hdmi0_reg);
		priv->hdmi0_reg = NULL;
	}

	if (priv->hdmi1_phy != NULL) {
		iounmap(priv->hdmi1_phy);
		priv->hdmi1_phy = NULL;
	}

	if (priv->hdmi1_reg != NULL) {
		iounmap(priv->hdmi1_reg);
		priv->hdmi1_reg = NULL;
	}
}

static int set_hdmi_timing_from_mode(u32 vact, u32 hact, u32 refresh,
				     u32 *hdmi_format, u32 *pixclk)
{
	int ret;

	ret = drm_hdmi_get_timing_from_mode(vact, hact, refresh, hdmi_format, pixclk);
	if (ret != TD_SUCCESS) {
		*hdmi_format = OT_HDMI_VIDEO_FORMAT_1080P_60;
		if (*pixclk == 0)
			*pixclk = 148500;
		return ret;
	}

	return 0;
}

static u32 set_hdmi_format_to_hdmi_timing(u32 hdmi_format)
{
	return drm_hdmi_format_to_hdmi_timing(hdmi_format);
}

static int set_hdmi_default_attr(u32 pixclk)
{
	memset(&g_hdmi_property, 0, sizeof(g_hdmi_property));
	g_hdmi_property.hdmi_id = 0;
	g_hdmi_property.prop.enable_hdmi = TD_TRUE;
	g_hdmi_property.prop.enable_video = TD_TRUE;
	g_hdmi_property.prop.enable_audio = TD_FALSE;
	g_hdmi_property.prop.enable_avi_infoframe = TD_TRUE;
	g_hdmi_property.prop.enable_aud_infoframe = TD_FALSE;
	g_hdmi_property.prop.hdmi_action = ASCEND_HDMI_DEFAULT_ACTION_HDMI;
	g_hdmi_property.prop.out_color_space = ASCEND_HDMI_COLORSPACE_YCBCR444;
	g_hdmi_property.prop.in_color_space = ASCEND_HDMI_COLORSPACE_YCBCR444;
	g_hdmi_property.prop.deep_color_mode = ASCEND_HDMI_DEEP_COLOR_24BIT;
	g_hdmi_property.prop.out_csc_quantization = ASCEND_HDMI_QUANTIZATION_RANGE_LIMITED;
	g_hdmi_property.prop.pix_clk = pixclk;
	g_hdmi_property.prop.enable_vid_mode_adapt = TD_FALSE;
	g_hdmi_property.prop.enable_deep_clr_adapt = TD_FALSE;
	g_hdmi_property.prop.auth_mode = TD_FALSE;
	g_hdmi_property.prop.disp_fmt = g_hdmi_default_format;
	g_hdmi_property.prop.video_timing = set_hdmi_format_to_hdmi_timing(g_hdmi_default_format);
	return drm_hdmi_set_attr(&g_hdmi_property, TD_FALSE);
}

static int set_hdmi_user_attr(void)
{
	return drm_hdmi_set_attr(&g_hdmi_property, TD_FALSE);
}

static int set_hdmi_default_infoframe(u32 hdmi_timing)
{
	memset(&g_hdmi_infoframe, 0, sizeof(g_hdmi_infoframe));
	g_hdmi_infoframe.hdmi_id = 0;
	g_hdmi_infoframe.infoframe_id = ASCEND_HDMI_INFOFRAME_TYPE_AVI;
	g_hdmi_infoframe.infoframe.avi_infoframe.type = ASCEND_HDMI_INFOFRAME_TYPE_AVI;
	g_hdmi_infoframe.infoframe.avi_infoframe.version = 2;
	g_hdmi_infoframe.infoframe.avi_infoframe.length = ASCEND_HDMI_AVI_INFOFRAME_SIZE;
	g_hdmi_infoframe.infoframe.avi_infoframe.colorspace = ASCEND_HDMI_COLORSPACE_YCBCR444;
	g_hdmi_infoframe.infoframe.avi_infoframe.active_info_valid = TD_TRUE;
	g_hdmi_infoframe.infoframe.avi_infoframe.colorimetry = ASCEND_HDMI_COLORIMETRY_ITU_709;
	g_hdmi_infoframe.infoframe.avi_infoframe.picture_aspect = ASCEND_HDMI_PICTURE_ASPECT_16_9;
	g_hdmi_infoframe.infoframe.avi_infoframe.active_aspect = ASCEND_HDMI_ACTIVE_ASPECT_PICTURE;
	g_hdmi_infoframe.infoframe.avi_infoframe.quantization_range =
		ASCEND_HDMI_QUANTIZATION_RANGE_LIMITED;
	g_hdmi_infoframe.infoframe.avi_infoframe.ycc_quantization_range =
		ASCEND_HDMI_YCC_QUANTIZATION_RANGE_LIMITED;
	g_hdmi_infoframe.infoframe.avi_infoframe.video_timing = hdmi_timing;
	g_hdmi_infoframe.infoframe.avi_infoframe.disp_fmt = g_hdmi_default_format;
	return drm_hdmi_set_infoframe(&g_hdmi_infoframe, TD_FALSE);
}

static int set_hdmi_user_infoframe(void)
{
	return drm_hdmi_set_infoframe(&g_hdmi_infoframe, TD_FALSE);
}

static int open_hdmi_device(void)
{
	struct ascend_drm_hdmi_open open = {
		.hdmi_id = 0,
		.default_mode = ASCEND_HDMI_DEFAULT_ACTION_HDMI,
	};

	return drm_hdmi_open(&open, TD_FALSE);
}

static int set_hdmi_close(void)
{
	int hdmi_id = 0;

	return drm_hdmi_close(&hdmi_id, TD_FALSE);
}

static int set_hdmi_start(void)
{
	int hdmi_id = 0;

	return drm_hdmi_start(&hdmi_id, TD_FALSE);
}

static int soc_drm_get_capability(struct ascend_vdp_drm_private *priv)
{
	priv->init_done = 0;
	priv->plane_count = 1;
	priv->crtc_count = 1;
	priv->encoder_count = 1;
	priv->connector_count = 1;
	return 1;
}

static int ext_adp_check_plane_coor(struct drm_plane_state *state)
{
	struct ascend_vdp_plane *vdp_plane;
	struct drm_rect src_rect;
	int screen_w;
	int screen_h;

	if (state == NULL || state->fb == NULL || state->crtc == NULL)
		return -EINVAL;

	vdp_plane = to_ascend_vdp_plane(state->plane);
	drm_rect_fp_to_int(&src_rect, &state->src);
	vdp_plane->src_x = (u32)src_rect.x1;
	vdp_plane->src_y = (u32)src_rect.y1;
	vdp_plane->src_w = (u32)drm_rect_width(&src_rect);
	vdp_plane->src_h = (u32)drm_rect_height(&src_rect);
	screen_w = state->crtc->state ? state->crtc->state->mode.hdisplay : ASCEND_VDP_DEFAULT_WIDTH;
	screen_h = state->crtc->state ? state->crtc->state->mode.vdisplay : ASCEND_VDP_DEFAULT_HEIGHT;

	if (drm_rect_width(&src_rect) != drm_rect_width(&state->dst) ||
	    drm_rect_height(&src_rect) != drm_rect_height(&state->dst))
		return -EINVAL;

	if (src_rect.x1 != state->dst.x1 || src_rect.y1 != state->dst.y1)
		return -EINVAL;

	if (src_rect.x2 > state->fb->width || src_rect.y2 > state->fb->height)
		return -EINVAL;

	if (state->dst.x2 > screen_w || state->dst.y2 > screen_h)
		return -EINVAL;

	return 0;
}

static int adp_plane_atomic_check(struct drm_plane *plane,
				  struct drm_atomic_state *state)
{
	struct ascend_vdp_plane *vdp_plane = to_ascend_vdp_plane(plane);
	int ret;

	(void)state;

	if (vdp_plane->layer_opened == 1U)
		return 0;

	ret = drm_open_layer(vdp_plane->layer_id);
	if (ret == 0) {
		ret = drm_set_layer(vdp_plane->layer_id, 0);
		if (ret == 0)
			vdp_plane->layer_opened = 1U;
	}

	if (ret != 0)
		drm_err(plane->dev, "drv_gfbg_adp_open_layer error, ret=%#x\n", ret);

	return ret;
}

static u32 adp_map_drm_format(u32 drm_format)
{
	switch (drm_format) {
	case DRM_FORMAT_XRGB8888:
		return 7;
	case DRM_FORMAT_RGB888:
		return 1;
	case DRM_FORMAT_ARGB8888:
		return 7;
	case DRM_FORMAT_ARGB4444:
		return 5;
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_ARGB1555:
		return 6;
	case DRM_FORMAT_RGB565:
		return 0;
	default:
		return 37;
	}
}

static void adp_plane_atomic_update(struct drm_plane *plane,
				    struct drm_atomic_state *state)
{
	struct ascend_vdp_plane *vdp_plane = to_ascend_vdp_plane(plane);
	struct drm_plane_state *new_state = plane->state;
	struct drm_framebuffer *fb;
	struct drm_gem_dma_object *gem_obj;
	struct drm_rect src_rect;
	u64 src_pos;
	u64 src_size;
	u64 dst_pos;
	u64 dst_size;
	u32 src_w;
	u32 src_h;
	u32 dst_w;
	u32 dst_h;
	u32 layer_fmt;
	int ret;

	(void)state;
	if (new_state == NULL || new_state->crtc == NULL || new_state->fb == NULL) {
		drm_err(plane->dev, "NULL ptr err!\n");
		return;
	}

	if (ext_adp_check_plane_coor(new_state)) {
		drm_err(plane->dev, "plane check coor failed!\n");
		return;
	}

	fb = new_state->fb;
	gem_obj = local_drm_fb_dma_get_gem_obj(fb, 0);
	if (gem_obj == NULL || fb->format == NULL) {
		drm_err(plane->dev, "fb->fomat is NULL, update failed.\n");
		return;
	}

	drm_rect_fp_to_int(&src_rect, &new_state->src);
	src_w = drm_rect_width(&src_rect);
	src_h = drm_rect_height(&src_rect);
	dst_w = drm_rect_width(&new_state->dst);
	dst_h = drm_rect_height(&new_state->dst);
	src_pos = drm_pack_xy(src_rect.x1, src_rect.y1);
	src_size = drm_pack_wh(src_w, src_h);
	dst_pos = drm_pack_xy(new_state->dst.x1, new_state->dst.y1);
	dst_size = drm_pack_wh(dst_w, dst_h);
	layer_fmt = adp_map_drm_format(fb->format->format);

	ret = drm_set_layer_rect(vdp_plane->layer_id, src_pos, src_size,
				 dst_pos, dst_size);
	if (ret) {
		drm_err(plane->dev, "drm_set_layer_rect failed, ret=%#x\n", ret);
		return;
	}

	ret = drm_set_layer_reso(vdp_plane->layer_id, src_pos, src_size);
	if (ret) {
		drm_err(plane->dev, "drm_set_layer_reso failed, ret=%#x\n", ret);
		return;
	}

	(void)drm_drv_enable_zme(vdp_plane->layer_id, src_w, src_h, dst_w, dst_h, false);
	vdp_plane->alpha_en = (fb->format->format == DRM_FORMAT_ARGB8888 ||
			       fb->format->format == DRM_FORMAT_ARGB4444 ||
			       fb->format->format == DRM_FORMAT_ARGB1555);
	vdp_plane->premul_en = 1;
	vdp_plane->global_alpha = 0xFFFFFF;

	ret = drm_set_layer_alpha(vdp_plane->layer_id, vdp_plane->alpha_en,
				  vdp_plane->global_alpha, DRM_FORMAT_ARGB1555);
	if (ret) {
		drm_err(plane->dev, "drv_gfbg_adp_set_layer_alpha err, ret=%#x\n", ret);
		return;
	}

	ret = drm_set_layer_fmt(vdp_plane->layer_id, layer_fmt);
	if (ret) {
		drm_err(plane->dev, "drv_gfbg_adp_set_layer_data_fmt err, ret=%#x\n", ret);
		return;
	}

	ret = drm_set_layer_stride(vdp_plane->layer_id, fb->pitches[0]);
	if (ret) {
		drm_err(plane->dev, "drv_gfbg_adp_set_layer_stride err, ret=%#x\n", ret);
		return;
	}

	ret = drm_set_layer_addr(vdp_plane->layer_id, gem_obj->dma_addr);
	if (ret) {
		drm_err(plane->dev, "drv_gfbg_adp_set_layer_addr err, ret=%#x\n", ret);
		return;
	}

	ret = drm_set_layer(vdp_plane->layer_id, 1);
	if (ret) {
		drm_err(plane->dev, "drv_gfbg_adp_set_enable err, ret=%#x\n", ret);
		return;
	}

	vdp_plane->update_cnt++;
}

static void adp_plane_atomic_disable(struct drm_plane *plane,
				     struct drm_atomic_state *state)
{
	struct ascend_vdp_plane *vdp_plane = to_ascend_vdp_plane(plane);

	(void)state;
	(void)drm_set_layer(vdp_plane->layer_id, 0);
}

static int adp_disable_plane(struct drm_plane *plane, struct drm_modeset_acquire_ctx *ctx)
{
	int ret;

	ret = drm_atomic_helper_disable_plane(plane, ctx);
	if (ret)
		drm_err(plane->dev, "ext_drm_disable_plane, ret=%#x\n", ret);

	return ret;
}

static int adp_update_plane(struct drm_plane *plane,
			    struct drm_crtc *crtc,
			    struct drm_framebuffer *fb,
			    int crtc_x,
			    int crtc_y,
			    unsigned int crtc_w,
			    unsigned int crtc_h,
			    uint32_t src_x,
			    uint32_t src_y,
			    uint32_t src_w,
			    uint32_t src_h,
			    struct drm_modeset_acquire_ctx *ctx)
{
	int ret;

	ret = drm_atomic_helper_update_plane(plane, crtc, fb, crtc_x, crtc_y,
					     crtc_w, crtc_h, src_x, src_y,
					     src_w, src_h, ctx);
	if (ret)
		drm_err(plane->dev, "ext_drm_update_plane err, ret=%#x\n", ret);

	return ret;
}

static const struct drm_plane_helper_funcs soc_drm_plane_helper_funcs = {
	.atomic_check = adp_plane_atomic_check,
	.atomic_update = adp_plane_atomic_update,
	.atomic_disable = adp_plane_atomic_disable,
};

static const struct drm_plane_funcs soc_drm_plane_funcs = {
	.update_plane = adp_update_plane,
	.disable_plane = adp_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static int adp_plane_get_funcs(const struct drm_plane_funcs **funcs,
			       const struct drm_plane_helper_funcs **helper_funcs,
			       u32 idx)
{
	(void)idx;
	if (funcs == NULL || helper_funcs == NULL)
		return -EINVAL;

	*funcs = &soc_drm_plane_funcs;
	*helper_funcs = &soc_drm_plane_helper_funcs;
	return 0;
}

static int soc_drm_plane_init(struct drm_device *drm)
{
	struct ascend_vdp_drm_private *priv = to_ascend_vdp_priv(drm);
	const struct drm_plane_funcs *funcs;
	const struct drm_plane_helper_funcs *helper_funcs;
	int ret;

	ret = adp_plane_get_funcs(&funcs, &helper_funcs, 0);
	if (ret)
		return ret;

	priv->plane.layer_id = ASCEND_VDP_LAYER;
	priv->plane.layer_opened = 0;
	ret = drm_universal_plane_init(drm, &priv->plane.base, 1,
				       funcs,
				       ext_graphics_formats,
				       ARRAY_SIZE(ext_graphics_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;

	drm_plane_helper_add(&priv->plane.base, helper_funcs);
	return 0;
}

static void soc_drm_plane_deinit(struct drm_device *drm)
{
	(void)drm;
}

static void adp_crtc_atomic_enable(struct drm_crtc *crtc,
				   struct drm_atomic_state *state)
{
	struct ascend_vdp_drm_private *priv;
	int ret;

	(void)state;
	if (crtc == NULL) {
		if (g_drm != NULL)
			drm_err(g_drm, "soc_crtc is NULL!\n");
		return;
	}

	priv = to_ascend_vdp_priv(crtc->dev);
	if (priv->crtc.enabled) {
		pr_info("[drm] crtc already opened!\n");
		return;
	}

	ret = drm_init(crtc);
	if (ret) {
		drm_err(crtc->dev, "vdp start error, ret = %d\n", ret);
		return;
	}

	drm_crtc_vblank_on(crtc);
	priv->crtc.enabled = 1;
}

static void adp_crtc_atomic_disable(struct drm_crtc *crtc,
				    struct drm_atomic_state *state)
{
	struct ascend_vdp_drm_private *priv;
	int ret;

	(void)state;
	if (crtc == NULL) {
		if (g_drm != NULL)
			drm_err(g_drm, "soc_crtc is NULL!\n");
		return;
	}

	priv = to_ascend_vdp_priv(crtc->dev);
	drm_crtc_vblank_off(crtc);
	if (!priv->crtc.enabled) {
		pr_info("[drm] crtc already disabled!\n");
		return;
	}

	(void)drm_set_layer(0, 0);
	msleep(50);
	ret = drm_deinit(crtc);
	if (ret) {
		drm_err(crtc->dev, "disp_set_enalbe error, ret = %d\n", ret);
		return;
	}

	priv->crtc.enabled = 0;
}

static void adp_crtc_atomic_flush(struct drm_crtc *crtc,
				  struct drm_atomic_state *state)
{
	unsigned long flags;
	struct drm_pending_vblank_event *event;

	(void)state;
	if (crtc == NULL) {
		if (g_drm != NULL)
			drm_err(g_drm, "adp_crtc_atomic_flush crtc is NULL.\n");
		return;
	}

	if (crtc->dev == NULL || crtc->state == NULL) {
		drm_err(g_drm, "adp_crtc_atomic_flush dev:%d state:%d.\n",
			crtc->dev != NULL, crtc->state != NULL);
		return;
	}

	if (crtc->state->event == NULL)
		return;

	event = crtc->state->event;
	crtc->state->event = NULL;

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (drm_crtc_vblank_get(crtc) != 0) {
		drm_crtc_send_vblank_event(crtc, event);
	} else {
		drm_crtc_arm_vblank_event(crtc, event);
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static int adp_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct ascend_vdp_crtc *vdp_crtc;

	if (crtc == NULL) {
		if (g_drm != NULL)
			drm_err(g_drm, "soc_crtc is NULL!\n");
		return -1;
	}

	vdp_crtc = container_of(crtc, struct ascend_vdp_crtc, base);
	vdp_crtc->vblank_enabled = 1;
	return 0;
}

static void adp_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct ascend_vdp_crtc *vdp_crtc;

	if (crtc == NULL) {
		if (g_drm != NULL)
			drm_err(g_drm, "soc_crtc is NULL!\n");
		return;
	}

	vdp_crtc = container_of(crtc, struct ascend_vdp_crtc, base);
	vdp_crtc->vblank_enabled = 0;
}

static int adp_crtc_set_config(struct drm_mode_set *set,
			       struct drm_modeset_acquire_ctx *ctx)
{
	int ret;

	ret = drm_atomic_helper_set_config(set, ctx);
	if (ret)
		drm_err(g_drm, "drm_atomic_helper_set_config error, ret = %d\n",
			ret);

	return ret;
}

static int adp_crtc_page_flip(struct drm_crtc *crtc,
			      struct drm_framebuffer *fb,
			      struct drm_pending_vblank_event *event,
			      uint32_t flags,
			      struct drm_modeset_acquire_ctx *ctx)
{
	int ret;

	ret = drm_atomic_helper_page_flip(crtc, fb, event, flags, ctx);
	if (ret)
		drm_err(g_drm, "drm_atomic_helper_page_flip, ret = %d\n", ret);

	return ret;
}

static void adp_crtc_destroy(struct drm_crtc *crtc)
{
	drm_crtc_cleanup(crtc);
}

static void adp_crtc_base_timing(const struct drm_display_mode *mode,
				 u32 *vact, u32 *hact)
{
	*vact = mode->vdisplay;
	*hact = mode->hdisplay;
}

static int adp_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	const struct ascend_vdp_sync_mode *sync_mode;
	struct drm_display_mode *mode;
	u32 vact;
	u32 hact;
	u32 refresh;
	u32 idx;
	struct ascend_vdp_pll_cfg pll_cfg = g_drm_default_pll_cfg;
	int ret;

	if (crtc == NULL || crtc->state == NULL) {
		if (crtc != NULL)
			drm_err(crtc->dev, "adp_crtc_mode_set_nofb state is NULL.\n");
		return -EINVAL;
	}

	mode = &crtc->state->mode;
	adp_crtc_base_timing(mode, &vact, &hact);
	refresh = drm_mode_vrefresh(mode);

	for (idx = 0; idx < ARRAY_SIZE(g_drm_vo_intf_sync_info); idx++) {
		sync_mode = drm_vo_intf_sync_info_func(idx);
		if (sync_mode == NULL)
			continue;
		if (sync_mode->hact == hact &&
		    sync_mode->vact == vact &&
		    sync_mode->refresh == refresh) {
			(void)vo_drv_get_pll_cfg_no_div(sync_mode->sync, &pll_cfg);
			g_vo_sync = sync_mode->sync;
			break;
		}
	}
	hal_vo_set_pll_cfg(0, &pll_cfg);

	ret = drm_init(crtc);
	if (ret) {
		drm_err(crtc->dev, "vdp start error, ret = %d\n", ret);
		return ret;
	}

	ret = drm_open_dev();
	if (ret)
		return ret;

	ret = drm_bind_dev();
	if (ret)
		drm_err(crtc->dev, "disp_set_enalbe error, ret = %d\n", ret);

	return ret;
}

static const struct drm_crtc_helper_funcs soc_drm_crtc_helper_funcs = {
	.atomic_enable = adp_crtc_atomic_enable,
	.atomic_disable = adp_crtc_atomic_disable,
	.atomic_flush = adp_crtc_atomic_flush,
	.mode_set_nofb = adp_crtc_mode_set_nofb,
};

static const struct drm_crtc_funcs soc_drm_crtc_funcs = {
	.destroy = adp_crtc_destroy,
	.set_config = adp_crtc_set_config,
	.page_flip = adp_crtc_page_flip,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = adp_crtc_enable_vblank,
	.disable_vblank = adp_crtc_disable_vblank,
};

static int adp_crtc_get_funcs(const struct drm_crtc_funcs **funcs,
			      const struct drm_crtc_helper_funcs **helper_funcs)
{
	if (funcs == NULL || helper_funcs == NULL)
		return -EINVAL;

	*funcs = &soc_drm_crtc_funcs;
	*helper_funcs = &soc_drm_crtc_helper_funcs;
	return 0;
}

static int soc_drm_crtc_init(struct drm_device *drm)
{
	struct ascend_vdp_drm_private *priv = to_ascend_vdp_priv(drm);
	const struct drm_crtc_funcs *funcs;
	const struct drm_crtc_helper_funcs *helper_funcs;
	int ret;

	ret = adp_crtc_get_funcs(&funcs, &helper_funcs);
	if (ret)
		return ret;

	ret = drm_crtc_init_with_planes(drm, &priv->crtc.base, &priv->plane.base,
					NULL, funcs, NULL);
	if (ret)
		return ret;

	drm_crtc_helper_add(&priv->crtc.base, helper_funcs);
	return 0;
}

static void soc_drm_crtc_deinit(struct drm_device *drm)
{
	(void)drm;
}

static enum drm_mode_status adp_encoder_mode_valid(struct drm_encoder *encoder,
						   const struct drm_display_mode *mode)
{
	u32 hdmi_fmt = OT_HDMI_VIDEO_FORMAT_1080P_60;
	u32 pixclk = mode->clock;
	enum drm_mode_status status = MODE_OK;

	(void)encoder;
	if (drm_hdmi_get_timing_from_mode(mode->vdisplay, mode->hdisplay,
					  drm_mode_vrefresh(mode),
					  &hdmi_fmt, &pixclk) != TD_SUCCESS)
		status = (drm_dbg(g_drm, "hdmi_mode not valid \n"), MODE_H_ILLEGAL);

	drm_dbg(g_drm, "adp_encoder_mode_valid out\n");
	return status;
}

static void adp_encoder_enable(struct drm_encoder *encoder,
			       struct drm_atomic_state *state)
{
	(void)encoder;
	(void)state;
	(void)set_hdmi_start();
}

static void adp_encoder_disable(struct drm_encoder *encoder,
				struct drm_atomic_state *state)
{
	(void)encoder;
	(void)state;
	(void)set_hdmi_close();
}

static void adp_encoder_mode_set(struct drm_encoder *encoder,
				 struct drm_display_mode *mode,
				 struct drm_display_mode *adj_mode)
{
	u32 hdmi_fmt = OT_HDMI_VIDEO_FORMAT_1080P_60;
	u32 hdmi_timing;
	u32 pixclk;

	(void)encoder;
	(void)adj_mode;
	pixclk = mode->clock;
	(void)set_hdmi_timing_from_mode(mode->vdisplay, mode->hdisplay,
					drm_mode_vrefresh(mode), &hdmi_fmt, &pixclk);
	g_hdmi_default_format = hdmi_fmt;
	hdmi_timing = set_hdmi_format_to_hdmi_timing(hdmi_fmt);

	(void)open_hdmi_device();
	(void)set_hdmi_default_attr(pixclk);
	(void)set_hdmi_default_infoframe(hdmi_timing);
	(void)set_hdmi_user_attr();
	(void)set_hdmi_user_infoframe();
	drm_dbg(g_drm, "hdmi fmt:%u pixclk:%u\n", hdmi_fmt, pixclk);
}

static const struct drm_encoder_helper_funcs soc_drm_encoder_helper_funcs = {
	.mode_valid = adp_encoder_mode_valid,
	.mode_set = adp_encoder_mode_set,
	.atomic_enable = adp_encoder_enable,
	.atomic_disable = adp_encoder_disable,
};

static void adp_encoder_destroy(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
}

static const struct drm_encoder_funcs soc_drm_encoder_funcs = {
	.destroy = adp_encoder_destroy,
};

static int adp_encoder_get_funcs(const struct drm_encoder_funcs **funcs,
				 const struct drm_encoder_helper_funcs **helper_funcs)
{
	if (funcs == NULL || helper_funcs == NULL)
		return -EINVAL;

	*funcs = &soc_drm_encoder_funcs;
	*helper_funcs = &soc_drm_encoder_helper_funcs;
	return 0;
}

static int soc_drm_encoder_init(struct drm_device *drm)
{
	struct ascend_vdp_drm_private *priv = to_ascend_vdp_priv(drm);
	const struct drm_encoder_funcs *funcs;
	const struct drm_encoder_helper_funcs *helper_funcs;
	int ret;

	ret = adp_encoder_get_funcs(&funcs, &helper_funcs);
	if (ret)
		return ret;

	ret = drm_encoder_init(drm, &priv->encoder, funcs,
			       DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;

	drm_encoder_helper_add(&priv->encoder, helper_funcs);
	priv->encoder.possible_crtcs = 1;
	return 0;
}

static void soc_drm_encoder_deinit(struct drm_device *drm)
{
	(void)drm;
}

static void adp_conn_reset(struct drm_connector *connector)
{
	struct ascend_vdp_connector_state *conn_state;

	conn_state = kzalloc(sizeof(*conn_state), GFP_KERNEL);
	if (conn_state == NULL)
		return;

	__drm_atomic_helper_connector_reset(connector, &conn_state->base);
	conn_state->brightness = 50;
	conn_state->contrast = 50;
	conn_state->saturation = 50;
	conn_state->hue = 50;
}

static struct drm_connector_state *
adp_conn_atomic_duplicate_state(struct drm_connector *connector)
{
	struct ascend_vdp_connector_state *state;

	if (connector->state == NULL)
		return NULL;

	state = kmemdup(to_ascend_vdp_conn_state(connector->state),
			sizeof(*state), GFP_KERNEL);
	if (state == NULL) {
		drm_err(g_drm, "drm_atomic_helper_connector_duplicate_state err\n");
		return NULL;
	}

	__drm_atomic_helper_connector_duplicate_state(connector, &state->base);
	return &state->base;
}

static void adp_conn_atomic_destroy_state(struct drm_connector *connector,
					  struct drm_connector_state *state)
{
	__drm_atomic_helper_connector_destroy_state(state);
	kfree(to_ascend_vdp_conn_state(state));
}

static enum drm_connector_status
adp_conn_detect(struct drm_connector *connector, bool force)
{
	struct ascend_vdp_connector *vdp_connector;
	enum drm_connector_status status;

	(void)force;
	drm_dbg(g_drm, "drm_connector_status in\n");
	if (connector == NULL) {
		if (g_drm != NULL)
			drm_err(g_drm, "connector is NULL\n");
		return connector_status_disconnected;
	}

	vdp_connector = to_ascend_vdp_connector(connector);
	if (g_hdmi0_status != 0) {
		status = (g_hdmi0_status == 1U) ?
			 connector_status_disconnected :
			 connector_status_unknown;
	} else if (vdp_connector->cached_status != connector_status_connected) {
		status = connector_status_connected;
		connector->status = status;
	} else {
		status = vdp_connector->cached_status;
	}

	vdp_connector->cached_status = status;
	return status;
}

static int adp_conn_get_modes(struct drm_connector *connector)
{
	struct pppoe_tag *tag;
	int ret;

	drm_dbg(g_drm, "adp_conn_get_modes in\n");
	(void)open_hdmi_device();

	tag = kzalloc(sizeof(*tag) + ASCEND_VDP_HDMI_EDID_LEN, GFP_KERNEL);
	if (tag == NULL) {
		drm_err(g_drm, "sample_tag kzalloc failed\n");
		return 0;
	}

	tag->tag_len = cpu_to_be16(ASCEND_VDP_HDMI_EDID_LEN);
	drm_hdmi_update_edid(tag);
	memcpy(g_drm_edid_raw_inner, tag->tag_data, ASCEND_VDP_HDMI_EDID_LEN);
	ret = drm_connector_update_edid_property(connector,
						 (const struct edid *)g_drm_edid_raw_inner);
	if (ret) {
		drm_err(connector->dev,
			"drm_mode_connector_update_edid_property err, ret=%#x\n",
			ret);
		kfree(tag);
		return 0;
	}

	ret = drm_add_edid_modes(connector, (struct edid *)g_drm_edid_raw_inner);
	kfree(tag);
	return ret;
}

static int adp_conn_fill_modes(struct drm_connector *connector, u32 max_x, u32 max_y)
{
	return drm_helper_probe_single_connector_modes(connector, max_x, max_y);
}

static int adp_conn_atomic_check(struct drm_connector *connector,
				 struct drm_atomic_state *state)
{
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;

	if (connector == NULL || state == NULL)
		return 0;

	conn_state = drm_atomic_get_new_connector_state(state, connector);
	if (conn_state == NULL || conn_state->crtc == NULL)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, conn_state->crtc);
	if (crtc_state == NULL)
		return 0;

	if (to_ascend_vdp_connector(connector)->cached_status == connector_status_connected)
		crtc_state->connectors_changed = true;

	return 0;
}

static enum drm_mode_status adp_conn_mode_valid(struct drm_connector *connector,
						struct drm_display_mode *mode)
{
	(void)connector;
	if (mode == NULL)
		return MODE_ERROR;

	if (mode->hdisplay > ASCEND_VDP_MAX_MODE) {
		drm_dbg(g_drm, "hdisplay out of range\n");
		drm_dbg(g_drm,
			"check invalid mode: %dx%dp%d %d %d %d %d %d %d %d %d %d status:%d\n",
			mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode),
			mode->clock, mode->htotal, mode->vtotal,
			mode->hsync_start, mode->hsync_end, mode->hskew,
			mode->vsync_start, mode->vsync_end, mode->vscan,
			MODE_H_ILLEGAL);
		return MODE_H_ILLEGAL;
	}

	if (mode->vdisplay > ASCEND_VDP_MAX_MODE) {
		drm_dbg(g_drm, "vdisplay out of range\n");
		drm_dbg(g_drm,
			"check invalid mode: %dx%dp%d %d %d %d %d %d %d %d %d %d status:%d\n",
			mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode),
			mode->clock, mode->htotal, mode->vtotal,
			mode->hsync_start, mode->hsync_end, mode->hskew,
			mode->vsync_start, mode->vsync_end, mode->vscan,
			MODE_V_ILLEGAL);
		return MODE_V_ILLEGAL;
	}

	drm_dbg(g_drm,
		"valid mode:%dx%dp%d %d %d %d %d %d %d %d %d %d status:%d\n",
		mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode),
		mode->clock, mode->htotal, mode->vtotal,
		mode->hsync_start, mode->hsync_end, mode->hskew,
		mode->vsync_start, mode->vsync_end, mode->vscan,
		MODE_OK);

	return MODE_OK;
}

static struct drm_encoder *adp_conn_best_encoder(struct drm_connector *connector)
{
	struct ascend_vdp_drm_private *priv;

	if (connector == NULL || connector->dev == NULL) {
		drm_err(g_drm, "connector or connector->dev is NULL\n");
		return NULL;
	}

	priv = to_ascend_vdp_priv(connector->dev);
	return &priv->encoder;
}

static int adp_conn_atomic_set_property(struct drm_connector *connector,
					struct drm_connector_state *state,
					struct drm_property *property, uint64_t val)
{
	struct ascend_vdp_connector *vdp_connector = to_ascend_vdp_connector(connector);
	struct ascend_vdp_connector_state *conn_state = to_ascend_vdp_conn_state(state);

	if (vdp_connector->brightness_prop == property) {
		conn_state->brightness = val;
		return 0;
	}

	if (vdp_connector->contrast_prop == property) {
		conn_state->contrast = val;
		return 0;
	}

	if (vdp_connector->hue_prop == property) {
		conn_state->hue = val;
		return 0;
	}

	if (vdp_connector->saturation_prop == property) {
		conn_state->saturation = val;
		return 0;
	}

	drm_err(connector->dev, "[CONNECTOR:%u:%s] unknown property [PROP:%u:%s]]\n",
		connector->base.id, connector->name,
		property->base.id, property->name);
	return -EINVAL;
}

static int adp_conn_atomic_get_property(struct drm_connector *connector,
					const struct drm_connector_state *state,
					struct drm_property *property, uint64_t *val)
{
	const struct ascend_vdp_connector *vdp_connector = to_ascend_vdp_connector(connector);
	const struct ascend_vdp_connector_state *conn_state = to_ascend_vdp_conn_state(state);

	if (vdp_connector->brightness_prop == property) {
		*val = conn_state->brightness;
		return 0;
	}

	if (vdp_connector->contrast_prop == property) {
		*val = conn_state->contrast;
		return 0;
	}

	if (vdp_connector->hue_prop == property) {
		*val = conn_state->hue;
		return 0;
	}

	if (vdp_connector->saturation_prop == property) {
		*val = conn_state->saturation;
		return 0;
	}

	return -EINVAL;
}

static int adp_conn_private_properties_init(struct drm_device *drm,
					    struct ascend_vdp_connector *connector)
{
	connector->brightness_prop = drm_property_create_range(drm, 0, "brightness", 0, 100);
	if (connector->brightness_prop == NULL) {
		drm_err(drm, "tv_brightness_property create err\n");
		return -ENOMEM;
	}

	connector->contrast_prop = drm_property_create_range(drm, 0, "contrast", 0, 100);
	if (connector->contrast_prop == NULL) {
		drm_err(drm, "tv_contrast_property create err\n");
		return -ENOMEM;
	}

	connector->hue_prop = drm_property_create_range(drm, 0, "hue", 0, 100);
	if (connector->hue_prop == NULL) {
		drm_err(drm, "tv_hue_property create err\n");
		return -ENOMEM;
	}

	connector->saturation_prop = drm_property_create_range(drm, 0, "saturation", 0, 100);
	if (connector->saturation_prop == NULL) {
		drm_err(drm, "tv_saturation_property create err\n");
		return -ENOMEM;
	}

	drm_object_attach_property(&connector->base.base, connector->brightness_prop, 50);
	drm_object_attach_property(&connector->base.base, connector->contrast_prop, 50);
	drm_object_attach_property(&connector->base.base, connector->hue_prop, 50);
	drm_object_attach_property(&connector->base.base, connector->saturation_prop, 50);
	return 0;
}

static void adp_conn_private_properties_deinit(struct drm_device *drm,
					       struct ascend_vdp_connector *connector)
{
	(void)drm;
	if (connector == NULL)
		return;

	connector->brightness_prop = NULL;
	connector->contrast_prop = NULL;
	connector->hue_prop = NULL;
	connector->saturation_prop = NULL;
}

static void adp_conn_destroy(struct drm_connector *connector)
{
	struct ascend_vdp_connector *vdp_connector;

	if (connector == NULL)
		return;

	vdp_connector = to_ascend_vdp_connector(connector);
	if (connector->connector_type != DRM_MODE_CONNECTOR_DisplayPort)
		adp_conn_private_properties_deinit(connector->dev, vdp_connector);

	drm_connector_cleanup(connector);
}

static const struct drm_connector_helper_funcs soc_drm_connector_helper_funcs = {
	.get_modes = adp_conn_get_modes,
	.mode_valid = adp_conn_mode_valid,
	.best_encoder = adp_conn_best_encoder,
	.atomic_check = adp_conn_atomic_check,
};

static const struct drm_connector_funcs soc_drm_connector_funcs = {
	.fill_modes = adp_conn_fill_modes,
	.detect = adp_conn_detect,
	.destroy = adp_conn_destroy,
	.reset = adp_conn_reset,
	.atomic_duplicate_state = adp_conn_atomic_duplicate_state,
	.atomic_destroy_state = adp_conn_atomic_destroy_state,
	.atomic_set_property = adp_conn_atomic_set_property,
	.atomic_get_property = adp_conn_atomic_get_property,
};

static int adp_connector_register_callback(struct drm_device *drm)
{
	g_drm = drm;
	return 0;
}

static void adp_connect_statues_init(struct ascend_vdp_connector *connector)
{
	enum drm_connector_status status;

	if (connector == NULL)
		return;

	if (drm_hdmi_get_connect_status(0) == TD_SUCCESS)
		status = connector_status_connected;
	else
		status = connector_status_disconnected;

	connector->base.status = status;
	connector->cached_status = status;
}

static int adp_connector_get_funcs_from_bridge(
	const struct drm_connector_funcs **funcs,
	const struct drm_connector_helper_funcs **helper_funcs)
{
	if (funcs == NULL || helper_funcs == NULL || *funcs == NULL || *helper_funcs == NULL)
		return -EINVAL;

	return 0;
}

static int adp_connector_get_funcs(const struct drm_connector_funcs **funcs,
				   const struct drm_connector_helper_funcs **helper_funcs)
{
	if (funcs == NULL || helper_funcs == NULL)
		return -1;

	*funcs = &soc_drm_connector_funcs;
	*helper_funcs = &soc_drm_connector_helper_funcs;
	return adp_connector_get_funcs_from_bridge(funcs, helper_funcs);
}

static int soc_drm_connector_init(struct drm_device *drm)
{
	struct ascend_vdp_drm_private *priv = to_ascend_vdp_priv(drm);
	const struct drm_connector_funcs *funcs;
	const struct drm_connector_helper_funcs *helper_funcs;
	int ret;

	ret = adp_connector_get_funcs(&funcs, &helper_funcs);
	if (ret)
		return ret;

	ret = adp_connector_register_callback(drm);
	if (ret)
		return ret;

	ret = drm_connector_init(drm, &priv->connector.base, funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ret;

	drm_connector_helper_add(&priv->connector.base, helper_funcs);
	adp_connect_statues_init(&priv->connector);
	priv->connector.base.polled = DRM_CONNECTOR_POLL_HPD;
	ret = adp_conn_private_properties_init(drm, &priv->connector);
	if (ret)
		return ret;

	drm_connector_attach_encoder(&priv->connector.base, &priv->encoder);
	ret = drm_connector_register(&priv->connector.base);
	if (ret == 0)
		priv->connector_registered = 1;

	return ret;
}

static void soc_drm_connector_deinit(struct drm_device *drm)
{
	struct ascend_vdp_drm_private *priv = to_ascend_vdp_priv(drm);

	if (priv->connector_registered) {
		drm_connector_unregister(&priv->connector.base);
		priv->connector_registered = 0;
	}
}

static void soc_drm_component_deinit(struct drm_device *drm)
{
	soc_drm_connector_deinit(drm);
	soc_drm_encoder_deinit(drm);
	soc_drm_crtc_deinit(drm);
	soc_drm_plane_deinit(drm);
}

static struct drm_framebuffer *
ext_drm_fb_create(struct drm_device *drm, struct drm_file *file,
		  const struct drm_mode_fb_cmd2 *cmd)
{
	const struct drm_format_info *info;

	info = drm_get_format_info(drm, cmd->pixel_format, cmd->modifier[0]);
	return drm_gem_fb_create(drm, file, info, cmd);
}

static int ext_drm_atomic_helper_check(struct drm_device *drm,
				       struct drm_atomic_state *state)
{
	return drm_atomic_helper_check(drm, state);
}

static int ext_drm_atomic_helper_commit(struct drm_device *drm,
					struct drm_atomic_state *state,
					bool nonblock)
{
	return drm_atomic_helper_commit(drm, state, nonblock);
}

static const struct drm_mode_config_funcs ext_drm_mode_config_funcs = {
	.fb_create = ext_drm_fb_create,
	.atomic_check = ext_drm_atomic_helper_check,
	.atomic_commit = ext_drm_atomic_helper_commit,
};

static int ext_drv_drm_open(struct drm_device *drm, struct drm_file *file)
{
	(void)drm;
	(void)file;
	drm_qos_notify_online();
	return 0;
}

static void ext_drv_drm_lastclose(struct drm_device *drm)
{
	drm_qos_notify_offline();
	drm_atomic_helper_shutdown(drm);
}

static void ext_drv_drm_postclose(struct drm_device *drm, struct drm_file *file)
{
	(void)file;
	mutex_lock(&drm->filelist_mutex);
	if (list_empty(&drm->filelist)) {
		mutex_unlock(&drm->filelist_mutex);
		ext_drv_drm_lastclose(drm);
		return;
	}
	mutex_unlock(&drm->filelist_mutex);
}

static int ext_drm_gem_dumb_create(struct drm_file *file_priv,
				   struct drm_device *drm,
				   struct drm_mode_create_dumb *args)
{
	return local_drm_gem_dma_dumb_create_internal(file_priv, drm, args);
}

static const struct file_operations ext_drm_driver_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = drm_gem_mmap,
	.fop_flags = FOP_UNSIGNED_OFFSET,
	DRM_GEM_DMA_UNMAPPED_AREA_FOPS
};

static struct drm_driver ext_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &ext_drm_driver_fops,
	.open = ext_drv_drm_open,
	.postclose = ext_drv_drm_postclose,
	.dumb_create = ext_drm_gem_dumb_create,
	.gem_prime_import_sg_table = local_drm_gem_dma_prime_import_sg_table_vmap,
	.name = ASCEND_VDP_DRM_NAME,
	.desc = ASCEND_VDP_DRM_DESC,
	.major = 0,
	.minor = 1,
};

static int ext_drm_platform_probe(struct platform_device *pdev)
{
	struct drm_device *drm;
	struct ascend_vdp_drm_private *priv;
	int ret;

	drm = drm_dev_alloc(&ext_drm_driver, &pdev->dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (priv == NULL) {
		ret = -ENOMEM;
		goto err_put;
	}

	priv->dev = &pdev->dev;
	drm->dev_private = priv;
	platform_set_drvdata(pdev, drm);
	g_drm = drm;

	hal_vo_map_base_reg();
	hal_vo_map_qos_reg();
	hal_vo_deinit_config();
	hal_vo_init_config();

	ret = drm_resource_init();
	if (ret)
		goto err_put;

	ret = drm_qos_node_register(priv);
	if (ret)
		goto err_resource;

	ret = of_reserved_mem_device_init_by_idx(&pdev->dev, pdev->dev.of_node, 0);
	if (ret)
		dev_info(&pdev->dev, "reserved memory init failed: %d\n", ret);

	ret = dma_set_mask(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		goto err_reserved;

	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		goto err_reserved;

	hal_vo_set_addr_ext();
	priv->irq = drm_get_irq_byname(pdev, "drm0");
	if (priv->irq <= 0) {
		ret = -EINVAL;
		drm_err(drm, "cannot find drm IRQ\n");
		goto err_reserved;
	}

	drm_irq_init(priv->irq);

	ret = init_hmdi_resource(priv);
	if (ret)
		goto err_reserved;

	(void)open_hdmi_device();

	ret = drmm_mode_config_init(drm);
	if (ret)
		goto err_hdmi;

	drm->mode_config.funcs = &ext_drm_mode_config_funcs;
	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_width = 8192;
	drm->mode_config.max_height = 8192;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.prefer_shadow = 0;

	soc_drm_get_capability(priv);
	ret = soc_drm_plane_init(drm);
	if (ret) {
		drm_err(drm, "component:plane init err, ret=%#x\n", ret);
		goto err_mode_config;
	}

	ret = soc_drm_crtc_init(drm);
	if (ret) {
		drm_err(drm, "component:crtc init err, ret=%#x\n", ret);
		goto err_mode_config;
	}

	ret = soc_drm_encoder_init(drm);
	if (ret) {
		drm_err(drm, "component:encoder init err, ret=%#x\n", ret);
		goto err_mode_config;
	}

	ret = soc_drm_connector_init(drm);
	if (ret) {
		drm_err(drm, "component:connector init err, ret=%#x\n", ret);
		goto err_mode_config;
	}

	drm_mode_config_reset(drm);
	drm_kms_helper_poll_init(drm);

	ret = drm_vblank_init(drm, 1);
	if (ret) {
		drm_err(drm, "drm_vblank_init err, ret=%#x\n", ret);
		goto err_poll;
	}

	ret = drm_dev_register(drm, 0);
	if (ret) {
		drm_err(drm, "drm_dev_init err, ret=%#x\n", ret);
		goto err_poll;
	}

	priv->init_done = 1;

	return 0;

err_poll:
	drm_kms_helper_poll_fini(drm);
err_mode_config:
	drm_mode_config_cleanup(drm);
err_hdmi:
	(void)set_hdmi_close();
	release_hdmi_resource(priv);
err_reserved:
	of_reserved_mem_device_release(&pdev->dev);
	drm_qos_node_unregister(priv);
err_resource:
	drm_resource_deinit();
	hal_vo_deinit_config();
	hal_vo_unmap_qos_reg();
	hal_vo_unmap_base_reg();
err_put:
	if (drm != NULL && drm->dev_private != NULL)
		to_ascend_vdp_priv(drm)->init_done = 0;
	if (g_drm == drm)
		g_drm = NULL;
	drm_dev_put(drm);
	return ret;
}

static void ext_drm_platform_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);
	struct ascend_vdp_drm_private *priv;
	struct ascend_vdp_drm_dev_ctx *dev_ctx;

	if (drm == NULL)
		return;

	priv = to_ascend_vdp_priv(drm);
	(void)set_hdmi_close();
	release_hdmi_resource(priv);
	dev_ctx = drm_get_dev_ctx(0);
	if (priv != NULL && dev_ctx != NULL && dev_ctx->init_irq == 1U)
		(void)drm_deinit(&priv->crtc.base);
	drm_dev_unregister(drm);
	priv->init_done = 0;
	priv->connector_registered = 0;
	drm_kms_helper_poll_fini(drm);
	soc_drm_component_deinit(drm);
	drm_mode_config_cleanup(drm);
	of_reserved_mem_device_release(&pdev->dev);
	drm_qos_node_unregister(priv);
	drm_resource_deinit();
	hal_vo_deinit_config();
	hal_vo_unmap_qos_reg();
	hal_vo_unmap_base_reg();
	if (g_drm == drm)
		g_drm = NULL;
	drm_dev_put(drm);
	platform_set_drvdata(pdev, NULL);
}

static int ext_pm_resume(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	int ret;

	hal_vo_init_config();
	dev_info(dev, "resume start\n");
	ret = drm_mode_config_helper_resume(drm);
	dev_info(dev, "resume finish ret=%d\n", ret);
	return ret;
}

static int ext_pm_suspend(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	int ret;

	dev_info(dev, "suspend start\n");
	ret = drm_mode_config_helper_suspend(drm);
	dev_info(dev, "suspend finish ret=%d\n", ret);
	hal_vo_deinit_config();
	return ret;
}

static const struct of_device_id ext_drm_dt_ids[] = {
	{ .compatible = ASCEND_VDP_DRM_COMPATIBLE },
	{ }
};
MODULE_DEVICE_TABLE(of, ext_drm_dt_ids);

static const struct dev_pm_ops ext_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(ext_pm_suspend, ext_pm_resume)
};

static struct platform_driver ext_drm_platform_driver = {
	.probe = ext_drm_platform_probe,
	.remove = ext_drm_platform_remove,
	.driver = {
		.name = ASCEND_VDP_DRM_PLATFORM,
		.of_match_table = ext_drm_dt_ids,
		.pm = &ext_pm_ops,
	},
};

static int __init ext_drv_drm_mod_init(void)
{
	return platform_driver_register(&ext_drm_platform_driver);
}

static void __exit ext_drv_drm_mod_exit(void)
{
	platform_driver_unregister(&ext_drm_platform_driver);
}

module_init(ext_drv_drm_mod_init);
module_exit(ext_drv_drm_mod_exit);

MODULE_DESCRIPTION(ASCEND_VDP_DRM_DESC);
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS("DMA_BUF");
