{ pkgs, lib, ... }:
{
  boot = {
    kernelPackages =
      let
        linux_x1e = pkgs.buildLinux rec {
          version = "7.0.0-rc5";
          modDirVersion = "7.0.0-rc5";
          src = pkgs.fetchFromGitHub {
            owner = "torvalds";
            repo = "linux";
            rev = "d1d81e9d1a4dd846aee9ae77ff9ecc2800d72148"; # v7.0-rc5
            hash = "sha256-UN1xOwSyn5YzdxQzEF6vTKev6vtN3iE2aiv7OT7TBAM=";
          };
          ignoreConfigErrors = true;
          structuredExtraConfig = with lib.kernel; {
            SURFACE_PLATFORMS = yes;
            SURFACE_AGGREGATOR = module;
            SURFACE_AGGREGATOR_BUS = yes;
            SURFACE_AGGREGATOR_REGISTRY = module;
            SURFACE_AGGREGATOR_CDEV = module;
            SURFACE_ACPI_NOTIFY = module;
            SURFACE_PLATFORM_PROFILE = module;
            SENSORS_SURFACE_FAN = module;
            SENSORS_SURFACE_TEMP = module;
            SERIAL_DEV_BUS = yes;
            SERIAL_DEV_CTRL_TTYPORT = yes;
            STRICT_DEVMEM = lib.kernel.no;
            IO_STRICT_DEVMEM = lib.kernel.no;
          };
        };
      in
      lib.recurseIntoAttrs (pkgs.linuxPackagesFor linux_x1e);

    # No blacklist needed: the out-of-tree copies of spi-geni-qcom,
    # ath12k and ath12k_wifi7 built by kernel-modules.nix land in
    # lib/modules/.../extra/, which depmod prioritises over the in-tree
    # kernel/ versions. Blacklisting by name would block auto-load via
    # PCI/DT modalias.

    initrd = {
      includeDefaultModules = lib.mkForce false;
      systemd = {
        enable = true;
        emergencyAccess = true;
        tpm2.enable = false;
      };
      kernelModules = [
        "msm"
        "qrtr"
        "drm_exec"
        "tcsrcc_x1e80100"
        "gpucc_x1e80100"
        "dispcc_x1e80100"
        "phy_qcom_edp"
        "panel_edp"
        "pmic_glink_altmode"
        "ps883x"
        "gpu_sched"
        "i2c_hid_of"
        "i2c_qcom_geni"
      ];
      availableKernelModules = [
        "btrfs"
        "garp"
        "mrp"
        "stp"
        "llc"
        "sch_fq_codel"
        "nfnetlink"
        "dmi_sysfs"
        "autofs4"
        "isofs"
        "cdc_ether"
        "onboard_usb_dev"
        "usbhid"
        "uas"
        "usb_storage"
        "snd_soc_hdmi_codec"
        "input_leds"
        "hid_generic"
        "hid"
        "nvme"
        "pm8941_pwrkey"
        "nvme_core"
        "crc_itu_t"
        "libarc4"
        "mhi"
        "qcom_spmi_temp_alarm"
        "industrialio"
        "leds_qcom_lpg"
        "qcom_pbs"
        "led_class_multicolor"
        "qcom_pon"
        "nvmem_qcom_spmi_sdam"
        "reboot_mode"
        "snd_soc_lpass_rx_macro"
        "snd_soc_lpass_wsa_macro"
        "snd_soc_lpass_tx_macro"
        "snd_soc_lpass_va_macro"
        "snd_soc_lpass_macro_common"
        "qcom_q6v5_pas"
        "qcom_pil_info"
        "qcom_q6v5"
        "snd_soc_core"
        "ac97_bus"
        "qcom_sysmon"
        "snd_pcm"
        "pinctrl_sm8550_lpass_lpi"
        "qcom_common"
        "qcom_spmi_pmic"
        "snd_timer"
        "ghash_ce"
        "pinctrl_lpass_lpi"
        "qcom_stats"
        "phy_qcom_edp"
        "i2c_qcom_geni"
        "qcom_geni_serial"
        "snd"
        "lpasscc_sc8280xp"
        "dispcc_x1e80100"
        "qcom_glink_smem"
        "icc_bwmon"
        "ucsi_glink"
        "arm_smccc_trng"
        "typec_ucsi"
        "qcom_battmgr"
        "fixed"
        "socinfo"
        "leds_gpio"
        "uio"
        "pwm_bl"
        "efi_pstore"
        "nls_iso8859_1"
        "gpucc_x1e80100"
        "tcsrcc_x1e80100"
        "qrtr"
        "msm"
        "mdt_loader"
        "ocmem"
        "drm_exec"
        "gpu_sched"
        "overlay"
        "phy_qcom_qmp_combo"
        "phy_snps_eusb2"
        "phy_qcom_eusb2_repeater"
        "phy_qcom_qmp_pcie"
        "tcsrcc_x1e80100"
        "dispcc-x1e80100"
        "gpucc-x1e80100"
        "phy_qcom_edp"
        "panel_edp"
        "ps883x"
        "pmic_glink_altmode"
        "i2c_hid_of"
        "surface_aggregator"
        "surface_aggregator_registry"
        "surface_aggregator_cdev"
        "surface_acpi_notify"
        "surface_platform_profile"
        "surface_fan"
        "surface_temp"
      ];
    };
  };
}
