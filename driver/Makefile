# SPDX-License-Identifier: GPL-2.0
#
# Ascend310B Driver - Flattened Build System with Parallel Compilation
#
# Usage:
#   make driver_device KERNEL_DIR=... CROSS_COMPILE=... PRODUCT=... build_device=true [-jN]
#   make driver_host   KERNEL_DIR=... CROSS_COMPILE=... PRODUCT=... [-jN]
#   make clean

SHELL := /bin/bash

DRIVER_DIR := $(CURDIR)
KERNEL_DIR ?= $(DRIVER_DIR)/kernel/linux-source
CROSS_COMPILE ?= aarch64-linux-gnu-
ARCH ?= arm64
PRODUCT ?= ascend310Brc
PRODUCT_SIDE ?= device
JOBS ?= $(shell nproc)

-include $(DRIVER_DIR)/config/feature_config/feature.mk

OUT_DEVICE := $(DRIVER_DIR)/out/device
OUT_HOST := $(DRIVER_DIR)/out/host

# Common kernel module build command
KMAKE = $(MAKE) -C $(KERNEL_DIR) \
	ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
	PRODUCT=$(PRODUCT) PRODUCT_SIDE=$(PRODUCT_SIDE) \
	FEATURE_MK_PATH=$(DRIVER_DIR)/config/feature_config/feature.mk \
	KBUILD_MODPOST_WARN=1

# Some modules trip Linux modfinal races when the nested sub-make inherits the
# outer -j value. Keep those module-local builds serialized while preserving
# cross-module parallelism in this top-level driver build.
SERIAL_SUBMAKE_MODULES := hsm_firmware_update sys

# ============================================================================
# Per-module build targets (enables parallel make -jN)
# ============================================================================

# Generate a build target for each module directory
define module_target
.PHONY: build-$(1)
build-$(1):
	@if [ -f "$(DRIVER_DIR)/$(1)/Kbuild" ]; then \
		echo "  [BUILD] $(1)"; \
		$(KMAKE) $(if $(filter $(1),$(SERIAL_SUBMAKE_MODULES)),-j1,) PRODUCT_SIDE=$(PRODUCT_SIDE) M=$(DRIVER_DIR)/$(1) modules \
			|| echo "  [FAIL] $(1)"; \
	else \
		echo "  [SKIP] $(1) (no Kbuild)"; \
	fi
endef

# Modules outside the current allowlist should not be built, but some kept
# targets still name them as ordering prerequisites. Keep a silent no-op
# placeholder so dependency chains remain valid without compiling them.
define skipped_module_target
.PHONY: build-$(1)
build-$(1):
	@:
endef

# ============================================================================
# Device-side modules (dependency layers for ordering)
# ============================================================================

DEVICE_L0_ALL := bbox bootdot qos hsm_flash hsm_firmware_update dbl_board_config dbl_cfg_persistent dbl_chip_config \
	dbl_algorithm dbl_dev_identity dbl_runenv_config \
	soc_resmng uda urd event_sched xsmem \
	davinci_intf icm ascend_ctl pkicms fpdc memory mem_ctrl soc_misc drv_notify \
	soft_fault ipcdrv pcie_rc_sr sysmonitor svm \
	trs_id_allocator trs_id_pool trs_tsmng trs_mia_agent \
	i2c_uniform sys_ctrl_uniform usb_uniform spi_uniform gpio_uniform pwm \
	mttcan eeprom serdes \
	dms_mng dms_dtm dms_smf hwmon

DEVICE_L1_ALL := trs_chan trs_stars trs_sub_stars dvpp_osal base sys dfm

DEVICE_L2_ALL := hns3 ts_platform upgrade user_cfg hiusbc pcie ts_channel \
	audio vdp hdmi mipi_rx

DEVICE_L3_ALL := dms virtmng virtmng_resmgr virtmng_host_stub vpcie vpc_agent trs_core

DEVICE_L4_ALL := hdcdrv tsdrv vtsdrv dp_proc_mng trs_cdqm trs_pm_adapt

ifneq ($(filter $(PRODUCT), ascend910 ascend310p),)
DEVICE_L4_ALL += vhdc
endif

DEVICE_L5_ALL := logdrv prof trs_adapt_agent trs_mia_adapt

DEVICE_EXTRA_ALL := lpm/drv_lpm lpm/pm lpm/drv_lpm_devmng trs_shrid soc_platform
DEVICE_EXTRA_ALL += kbox dvpp_pngd

# White-listed device targets requested by the user. Keep only modules whose
# produced .ko names are in the approved list.
DEVICE_L0 := bbox bootdot qos hsm_flash hsm_firmware_update dbl_board_config dbl_cfg_persistent dbl_chip_config \
	dbl_algorithm dbl_dev_identity dbl_runenv_config \
	soc_resmng uda urd event_sched xsmem \
	davinci_intf icm ascend_ctl pkicms fpdc memory mem_ctrl soc_misc \
	soft_fault ipcdrv pcie_rc_sr sysmonitor svm \
	trs_id_allocator trs_id_pool trs_tsmng \
	i2c_uniform sys_ctrl_uniform spi_uniform gpio_uniform pwm \
	mttcan eeprom serdes \
	dms_mng dms_dtm dms_smf hwmon

DEVICE_L1 := trs_chan trs_stars dvpp_osal base sys dfm

DEVICE_L2 := hns3 ts_platform upgrade user_cfg hiusbc ts_channel \
	audio hdmi mipi_rx

DEVICE_L3 := dms virtmng trs_core

DEVICE_L4 := dp_proc_mng trs_pm_adapt

DEVICE_L5 := logdrv prof trs_mia_adapt

DEVICE_EXTRA := lpm/drv_lpm lpm/pm lpm/drv_lpm_devmng kbox

ALL_DEVICE := $(DEVICE_L0_ALL) $(DEVICE_L1_ALL) $(DEVICE_L2_ALL) $(DEVICE_L3_ALL) \
	$(DEVICE_L4_ALL) $(DEVICE_L5_ALL) $(DEVICE_EXTRA_ALL)
ACTIVE_DEVICE := $(DEVICE_L0) $(DEVICE_L1) $(DEVICE_L2) $(DEVICE_L3) \
	$(DEVICE_L4) $(DEVICE_L5) $(DEVICE_EXTRA)
INACTIVE_DEVICE := $(filter-out $(ACTIVE_DEVICE),$(ALL_DEVICE))

# Generate build targets for active modules and no-op placeholders for modules
# explicitly removed from the current whitelist.
$(foreach mod,$(ACTIVE_DEVICE),$(eval $(call module_target,$(mod))))
$(foreach mod,$(INACTIVE_DEVICE),$(eval $(call skipped_module_target,$(mod))))

# Modules that import symbols from other out-of-tree modules must wait for the
# provider's Module.symvers to be generated before running modpost.
# (auto-generated from KBUILD_EXTRA_SYMBOLS analysis)
build-audio: build-dvpp_osal build-base build-dms_dtm build-dms_smf build-svm
build-base: build-dvpp_osal
build-davinci_intf: build-svm
build-dfm: build-bbox
build-dms: build-bbox build-davinci_intf build-dbl_chip_config build-dfm build-dms_dtm build-dms_smf build-icm build-trs_tsmng build-uda build-urd build-user_cfg build-virtmng
build-dms_smf: build-dms_dtm build-fpdc build-urd
build-dp_proc_mng: build-davinci_intf build-dms build-uda
build-dvpp_osal: build-svm
build-dvpp_pngd: build-base build-dvpp_osal build-event_sched
build-eeprom: build-user_cfg
build-event_sched: build-davinci_intf build-dms build-soc_resmng build-trs_chan build-uda build-user_cfg build-virtmng
build-hdcdrv: build-dbl_chip_config build-pcie build-uda build-vpcie
build-hdmi: build-base build-bootdot build-dvpp_osal build-vdp
build-hns3: build-bbox build-bootdot build-dms_smf build-eeprom build-qos
build-icm: build-ipcdrv
build-ipcdrv: build-bootdot
build-hsm_flash:
build-hsm_firmware_update:
build-logdrv: build-dbl_chip_config build-dms build-icm build-soc_resmng build-trs_chan build-trs_kunpeng build-trs_pm_adapt build-tsdrv build-uda
build-lpm/drv_lpm: build-bbox build-dms build-dms_dtm build-dms_smf build-fpdc build-icm build-uda
build-lpm/drv_lpm_devmng: build-dms build-icm build-uda build-urd build-user_cfg
build-lpm/pm: build-serdes
build-memory: build-dms build-dms_dtm build-dms_smf build-fpdc build-icm build-soc_resmng build-uda build-urd
build-mipi_rx: build-base build-bootdot build-dvpp_osal
build-mttcan: build-bootdot build-urd build-user_cfg
build-pcie: build-dbl_chip_config build-dbl_runenv_config build-dfm build-uda
build-pcie_rc_sr: build-bootdot
build-pkicms: build-user_cfg build-hsm_firmware_update
build-prof: build-dbl_chip_config build-dms build-soc_resmng build-trs_chan build-trs_kunpeng build-trs_pm_adapt build-uda build-virtmng
build-serdes: build-bootdot build-hsm_flash
build-sys: build-base build-dvpp_osal
build-soc_misc: build-dfm build-dms build-dms_dtm build-dms_smf build-fpdc build-uda build-urd build-user_cfg
build-soc_platform: build-pcie build-soc_resmng build-uda
build-soft_fault: build-dms_dtm build-dms_smf build-uda build-urd
build-trs_adapt_agent: build-dms build-pcie build-soc_resmng build-trs_core build-trs_id_pool build-trs_kunpeng build-trs_pm_adapt build-trs_tsmng build-uda
build-trs_cdqm: build-davinci_intf build-dms build-pcie build-soc_resmng build-trs_id_allocator
build-trs_chan: build-trs_id_allocator
build-trs_core: build-davinci_intf build-dms build-event_sched build-trs_chan build-trs_id_allocator
build-trs_kunpeng: build-dbl_runenv_config build-soc_resmng build-trs_chan build-trs_core build-trs_id_allocator build-trs_id_pool build-trs_sec_eh build-uda
build-trs_mia_adapt: build-soc_resmng build-trs_chan build-trs_core build-trs_id_allocator build-trs_id_pool build-trs_kunpeng build-trs_pm_adapt build-trs_sec_eh build-virtmng
build-trs_mia_agent: build-pcie build-trs_adapt_agent build-trs_id_pool build-trs_kunpeng build-trs_mia_adapt
build-trs_pm_adapt: build-dbl_chip_config build-dbl_runenv_config build-dms build-soc_resmng build-svm build-trs_chan build-trs_core build-trs_id_allocator build-trs_id_pool build-trs_stars build-trs_sub_stars build-trs_tsmng build-uda build-urd
build-trs_sec_eh: build-dms build-pcie build-soc_resmng build-trs_chan build-trs_core build-trs_id_allocator build-trs_pm_adapt build-uda build-urd build-virtmng_host_stub build-vpc_agent
build-trs_shrid: build-davinci_intf build-dms build-event_sched build-trs_core build-uda
build-trs_stars: build-dms build-soc_resmng build-uda
build-trs_sub_stars: build-soc_resmng build-uda
build-trs_tsmng: build-icm build-uda
build-ts_channel: build-dfm build-dms
build-ts_platform: build-bbox build-bootdot build-dbl_chip_config build-dfm build-dms build-dms_dtm build-dms_smf build-fpdc build-icm build-ipcdrv build-qos build-soc_resmng build-trs_chan build-trs_id_pool build-trs_pm_adapt build-trs_tsmng build-uda build-user_cfg build-virtmng
build-tsdrv: build-bbox build-davinci_intf build-dfm build-dms build-event_sched build-pcie build-svm build-uda
build-uda: build-davinci_intf
build-upgrade: build-hsm_flash build-hsm_firmware_update build-pkicms build-user_cfg
build-urd: build-davinci_intf build-user_cfg
build-vdp: build-base build-dvpp_osal
build-vhdc: build-vpcie build-vpc_agent
build-virtmng: build-dbl_chip_config build-dbl_runenv_config build-icm build-soc_resmng build-uda build-user_cfg
build-virtmng_resmgr: build-davinci_intf build-virtmng
build-vpcie: build-uda build-vpc_agent
build-vtsdrv: build-davinci_intf build-dms build-uda build-vpc_agent build-vpcie
build-xsmem: build-davinci_intf build-dbl_chip_config build-dms
build-user_cfg: build-hsm_flash

# Layer targets: each layer depends on the previous, but modules within a layer are parallel
.PHONY: device-l0 device-l1 device-l2 device-l3 device-l4 device-l5 device-extra

device-l0: $(addprefix build-,$(DEVICE_L0))
device-l1: device-l0 $(addprefix build-,$(DEVICE_L1))
device-l2: device-l1 $(addprefix build-,$(DEVICE_L2))
device-l3: device-l2 $(addprefix build-,$(DEVICE_L3))
device-l4: device-l3 $(addprefix build-,$(DEVICE_L4))
device-l5: device-l4 $(addprefix build-,$(DEVICE_L5))
device-extra: device-l5 $(addprefix build-,$(DEVICE_EXTRA))

# ============================================================================
# Main targets
# ============================================================================

.PHONY: driver_device driver_host clean

driver_device: PRODUCT_SIDE := device
driver_device:
	@$(MAKE) --no-print-directory clean >/dev/null
	@mkdir -p $(OUT_DEVICE)
	@echo "=== Building device-side modules (parallel within layers) ==="
	$(MAKE) -j$(JOBS) device-extra PRODUCT_SIDE=device
	@echo "=== Collecting device .ko files ==="
	@for mod in $(ACTIVE_DEVICE); do \
		find "$(DRIVER_DIR)/$$mod" -name '*.ko' -not -path '*/linux-source/*' -not -path '*/out/*' \
			-exec cp -f {} $(OUT_DEVICE)/ \; 2>/dev/null || true; \
	done
	@echo "Device modules: $$(ls $(OUT_DEVICE)/*.ko 2>/dev/null | wc -l) .ko files"

driver_host: PRODUCT_SIDE := host
driver_host:
	@mkdir -p $(OUT_HOST)
	@echo "=== Building host-side modules ==="
	@echo "Host build not yet implemented (modules share same Kbuild)"
	@echo "Host modules: 0 .ko files"

clean:
	@rm -rf $(OUT_DEVICE) $(OUT_HOST) out/
	@find $(DRIVER_DIR) \( -name '*.o' -o -name '*.ko' -o -name '*.mod' \
		-o -name '*.mod.c' -o -name '.*.cmd' -o -name 'Module.symvers' \
		-o -name 'modules.order' -o -name '.tmp_versions' \
		-o -name '.cache.mk' -o -name '.module-common.o' \) \
		-not -path '*/linux-source/*' -exec rm -rf {} + 2>/dev/null || true
	@echo "Driver tree cleaned"
