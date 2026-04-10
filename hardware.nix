{ config, lib, pkgs, ... }:
let
  dtbName = {
    "13" = "qcom/x1e80100-microsoft-romulus13.dtb";
    "15" = "qcom/x1e80100-microsoft-romulus15.dtb";
  }.${config.x1e.model};
in
{
  options.x1e.model = lib.mkOption {
    type = lib.types.enum [ "13" "15" ];
    default = "13";
    description = "Surface Laptop 7 display size (13.8\" or 15\").";
  };

  config = {
    nixpkgs.hostPlatform = "aarch64-linux";

    hardware = {
      enableRedistributableFirmware = lib.mkForce true;
      enableAllFirmware = lib.mkForce true;
      firmware = [
        pkgs.x1e80100-firmware
        pkgs.x1e80100-linux-firmware
      ];
      deviceTree = {
        enable = true;
        name = dtbName;
        filter = "*romulus*";
        overlays = [
          {
            name = "surface-laptop-7-sam";
            dtsFile = ./kernel/dtb-overlays/surface-laptop-7-sam.dts;
          }
          {
            name = "surface-laptop-7-touchpad";
            dtsFile = ./kernel/dtb-overlays/surface-laptop-7-touchpad.dts;
          }
          {
            name = "surface-laptop-7-thermal";
            dtsFile = ./kernel/dtb-overlays/surface-laptop-7-thermal.dts;
          }
        ];
      };
    };
  };
}
