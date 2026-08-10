################################################################################
#
# camera-gui  --  EGT GUI + UDP :5001 sensor (AESDLinuxEgtProject) -- Sprint 2
#
################################################################################

CAMERA_GUI_VERSION = 0.2
# NOTE: BR2_EXTERNAL_MCHP_PATH is the path macro of the external tree named
# "MCHP" (from external.desc). If you drop this package into an external with a
# different name, replace MCHP below with that name (macro: BR2_EXTERNAL_<NAME>_PATH).
CAMERA_GUI_SITE = $(BR2_EXTERNAL_MCHP_PATH)/package/camera-gui/src
CAMERA_GUI_SITE_METHOD = local
CAMERA_GUI_LICENSE = Apache-2.0

# Sprint 2 links against EGT only; the UDP listener uses libc/pthread (via the
# Makefile's -pthread). (Sprint 3 adds gstreamer1 + gst1-plugins-base.)
CAMERA_GUI_DEPENDENCIES = egt

define CAMERA_GUI_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D)
endef

define CAMERA_GUI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/AESDLinuxEgtProject \
		$(TARGET_DIR)/usr/bin/AESDLinuxEgtProject
	$(INSTALL) -D -m 0755 $(CAMERA_GUI_PKGDIR)/camera-gui-start.sh \
		$(TARGET_DIR)/usr/bin/camera-gui-start.sh
endef

# Install the systemd service and enable it (started after boot via
# multi-user.target). The service runs camera-gui-start.sh, which stops the
# egtdemo service (if running), brings up the network so UDP :5001 can be
# received, then launches the application.
define CAMERA_GUI_INSTALL_INIT_SYSTEMD
	$(INSTALL) -D -m 0644 $(CAMERA_GUI_PKGDIR)/camera-gui.service \
		$(TARGET_DIR)/usr/lib/systemd/system/camera-gui.service
	mkdir -p $(TARGET_DIR)/etc/systemd/system/multi-user.target.wants
	ln -sf /usr/lib/systemd/system/camera-gui.service \
		$(TARGET_DIR)/etc/systemd/system/multi-user.target.wants/camera-gui.service
endef

$(eval $(generic-package))
