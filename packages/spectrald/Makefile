include $(TOPDIR)/rules.mk

PKG_NAME:=spectrald
PKG_VERSION:=1.0.0
PKG_RELEASE:=6
PKG_LICENSE:=GPL-2.0-only

PKG_INSTALL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/spectrald
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Wireless
  TITLE:=On-demand Wi-Fi spectrum analyzer for ath10k
  DEPENDS:=+libubox +libubus +libnl-tiny
endef

define Package/spectrald/description
  ubus service (object "spectral") that drives ath10k spectral scan on
  demand: band sweeps on an idle radio, or the operating channel of a
  busy one. Scanning stops by itself once nobody polls for frames.
  Needs kmod-ath10k(-ct) built with CONFIG_PACKAGE_ATH_SPECTRAL.
endef

TARGET_CFLAGS += -I$(STAGING_DIR)/usr/include/libnl-tiny

define Package/spectrald/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/spectrald $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/spectrald.init $(1)/etc/init.d/spectrald
endef

$(eval $(call BuildPackage,spectrald))
