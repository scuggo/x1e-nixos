{ lib, pkgs, ... }:
{
  specialisation.el2.configuration = {
    hardware.deviceTree = {
      overlays = lib.mkAfter [
        {
          name = "x1e-el2";
          dtboFile = "${pkgs.slbounce}/share/slbounce/dtbo/x1e-el2.dtbo";
        }
      ];
    };
    boot.kernelParams = [ "id_aa64mmfr0.ecv=1" ];
    systemd.services.el2-boot-indicator = {
      description = "EL2 boot indicator (blink Caps Lock LED)";
      wantedBy = [ "multi-user.target" ];
      after = [ "multi-user.target" ];
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
      };
      script = ''
        LED=$(find /sys/class/leds -name "*capslock*" 2>/dev/null | head -1)
        if [ -z "$LED" ]; then
          LED=$(find /sys/class/leds -name "*kbd*" 2>/dev/null | head -1)
        fi
        if [ -n "$LED" ]; then
          for i in 1 2 3 4 5; do
            echo 1 > "$LED/brightness" 2>/dev/null || true
            sleep 0.3
            echo 0 > "$LED/brightness" 2>/dev/null || true
            sleep 0.3
          done
          echo 1 > "$LED/brightness" 2>/dev/null || true
        fi
      '';
    };
  };

  boot = {
    initrd.extraFirmwarePaths = [
      "ath12k/WCN7850/hw2.0/amss.bin"
      "ath12k/WCN7850/hw2.0/board-2.bin"
      "ath12k/QCN9274/hw2.0/board-2.bin"
      "ath12k/QCN9274/hw2.0/firmware-2.bin"
      "ath12k/WCN7850/hw2.0/m3.bin"
      "qcom/x1e80100/microsoft/Romulus/adsp_dtbs.elf"
      "qcom/x1e80100/microsoft/Romulus/adspr.jsn"
      "qcom/x1e80100/microsoft/Romulus/adsps.jsn"
      "qcom/x1e80100/microsoft/Romulus/adspua.jsn"
      "qcom/x1e80100/microsoft/Romulus/battmgr.jsn"
      "qcom/x1e80100/microsoft/Romulus/cdsp_dtbs.elf"
      "qcom/x1e80100/microsoft/Romulus/cdspr.jsn"
      "qcom/x1e80100/microsoft/Romulus/qcadsp8380.mbn"
      "qcom/x1e80100/microsoft/Romulus/qccdsp8380.mbn"
      "qcom/x1e80100/microsoft/qcdxkmsuc8380.mbn"
      "qcom/gen70500_sqe.fw"
      "qcom/gen70500_sqe.fw.zst"
      "qcom/gen70500_gmu.bin"
    ];

    kernelParams = [
      "clk_ignore_unused"
      "pd_ignore_unused"
      "iomem=relaxed"
      "mem_sleep_default=s2idle"
    ];

    blacklistedKernelModules = [
      "qcom_battmgr"
      "qcrypto"
    ];

    supportedFilesystems = {
      btrfs = true;
      zfs = lib.mkForce false;
      cifs = lib.mkForce false;
    };

    consoleLogLevel = 7;

    loader.systemd-boot = {
      edk2-uefi-shell.enable = true;
      extraFiles = {
        "EFI/systemd/drivers/slbouncea64.efi" = "${pkgs.slbounce}/share/slbounce/slbounce.efi";
        "tcblaunch.exe" = "${pkgs.tcblaunch}/share/tcblaunch/tcblaunch.exe";
        "sltest.efi" = "${pkgs.slbounce}/share/slbounce/sltest.efi";
        "dtbhack.efi" = "${pkgs.slbounce}/share/slbounce/dtbhack.efi";
        "firmware/qcom/x1e80100/microsoft/Romulus/qcadsp8380.mbn" =
          "${pkgs.x1e80100-firmware}/lib/firmware/qcom/x1e80100/microsoft/Romulus/qcadsp8380.mbn";
        "firmware/qcom/x1e80100/microsoft/Romulus/adsp_dtbs.elf" =
          "${pkgs.x1e80100-firmware}/lib/firmware/qcom/x1e80100/microsoft/Romulus/adsp_dtbs.elf";
        "firmware/qcom/x1e80100/microsoft/Romulus/qccdsp8380.mbn" =
          "${pkgs.x1e80100-firmware}/lib/firmware/qcom/x1e80100/microsoft/Romulus/qccdsp8380.mbn";
        "firmware/qcom/x1e80100/microsoft/Romulus/cdsp_dtbs.elf" =
          "${pkgs.x1e80100-firmware}/lib/firmware/qcom/x1e80100/microsoft/Romulus/cdsp_dtbs.elf";
      };
    };
  };
}
