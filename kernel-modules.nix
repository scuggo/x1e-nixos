{ config, lib, pkgs, ... }:
let
      kernel = config.boot.kernelPackages.kernel;

      spi-hid = pkgs.stdenv.mkDerivation {
        pname = "spi-hid";
        version = "0.3.1-${kernel.version}";
        src = ./kernel/modules/spi-hid;
        hardeningDisable = [
          "pic"
          "format"
        ];
        nativeBuildInputs = kernel.moduleBuildDependencies ++ [ pkgs.kmod ];
        makeFlags = [
          "KERNELRELEASE=${kernel.modDirVersion}"
          "KERNEL_DIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
          "INSTALL_MOD_PATH=$(out)"
        ];
        buildPhase = ''
          runHook preBuild
          make -C ${kernel.dev}/lib/modules/${kernel.modDirVersion}/build \
            M=$(pwd) \
            ARCH=${pkgs.stdenv.hostPlatform.linuxArch} \
            modules
          runHook postBuild
        '';
        installPhase = ''
          runHook preInstall
          install -D -m 644 spi-hid.ko $out/lib/modules/${kernel.modDirVersion}/extra/spi-hid.ko
          runHook postInstall
        '';
        enableParallelBuilding = true;
        meta = with lib; {
          description = "HID over SPI (HIDSPI v3) QSPI transport driver";
          license = licenses.gpl2Only;
          platforms = platforms.linux;
        };
      };


      ath12k-norfkill = pkgs.stdenv.mkDerivation {
        pname = "ath12k-norfkill";
        inherit (kernel)
          src
          version
          postPatch
          nativeBuildInputs
          ;
        kernel_dev = kernel.dev;
        kernelVersion = kernel.modDirVersion;
        patches = [
          ./kernel/modules/ath12k/disable-rfkill.patch
        ];
        buildPhase = ''
          BUILT_KERNEL=$kernel_dev/lib/modules/$kernelVersion/build
          cp $BUILT_KERNEL/Module.symvers .
          cp $BUILT_KERNEL/.config        .
          cp $kernel_dev/vmlinux          .
          make "-j$NIX_BUILD_CORES" modules_prepare
          make "-j$NIX_BUILD_CORES" M=drivers/net/wireless/ath/ath12k modules
        '';
        installPhase = ''
          install -D -m 644 drivers/net/wireless/ath/ath12k/ath12k.ko \
            $out/lib/modules/${kernel.modDirVersion}/extra/ath12k.ko
          if [ -f drivers/net/wireless/ath/ath12k/wifi7/ath12k_wifi7.ko ]; then
            install -D -m 644 drivers/net/wireless/ath/ath12k/wifi7/ath12k_wifi7.ko \
              $out/lib/modules/${kernel.modDirVersion}/extra/ath12k_wifi7.ko
          fi
        '';
        meta = with lib; {
          description = "ath12k with rfkill config early-return workaround";
          license = licenses.bsd3;
          platforms = platforms.linux;
        };
      };

      gpi-qspi = pkgs.stdenv.mkDerivation {
        pname = "gpi-qspi";
        inherit (kernel)
          src
          version
          postPatch
          nativeBuildInputs
          ;
        kernel_dev = kernel.dev;
        kernelVersion = kernel.modDirVersion;
        patches = [
          ./kernel/modules/qcom-qspi/gpi.patch
        ];
        buildPhase = ''
          BUILT_KERNEL=$kernel_dev/lib/modules/$kernelVersion/build
          cp $BUILT_KERNEL/Module.symvers .
          cp $BUILT_KERNEL/.config        .
          cp $kernel_dev/vmlinux          .
          make "-j$NIX_BUILD_CORES" modules_prepare
          make "-j$NIX_BUILD_CORES" M=drivers/dma/qcom modules
        '';
        installPhase = ''
          make \
            INSTALL_MOD_PATH="$out" \
            XZ="xz -T$NIX_BUILD_CORES" \
            M="drivers/dma/qcom" \
            modules_install
        '';
        meta = with lib; {
          description = "Qualcomm GPI DMA with QSPI protocol 9 support";
          license = licenses.gpl2Only;
          platforms = platforms.linux;
        };
      };

      spi-geni-qcom-qspi = pkgs.stdenv.mkDerivation {
        pname = "spi-geni-qcom-qspi";
        inherit (kernel)
          src
          version
          postPatch
          nativeBuildInputs
          ;
        kernel_dev = kernel.dev;
        kernelVersion = kernel.modDirVersion;
        patches = [
          ./kernel/modules/qcom-qspi/gpi.patch
          ./kernel/modules/qcom-qspi/spi-geni-qcom.patch
        ];
        buildPhase = ''
          BUILT_KERNEL=$kernel_dev/lib/modules/$kernelVersion/build
          cp $BUILT_KERNEL/Module.symvers .
          cp $BUILT_KERNEL/.config        .
          cp $kernel_dev/vmlinux          .
          make "-j$NIX_BUILD_CORES" modules_prepare
          make "-j$NIX_BUILD_CORES" M=drivers/spi CONFIG_SPI_QCOM_GENI=m
        '';
        installPhase = ''
          install -D -m 644 drivers/spi/spi-geni-qcom.ko \
            $out/lib/modules/${kernel.modDirVersion}/extra/spi-geni-qcom.ko
        '';
        meta = with lib; {
          description = "Qualcomm GENI SPI with QSPI 1-4-4 support";
          license = licenses.gpl2Only;
          platforms = platforms.linux;
        };
      };

      cpu-parking = pkgs.stdenv.mkDerivation {
        pname = "cpu-parking";
        version = "0.1.0-${kernel.version}";
        src = ./kernel/modules/cpu-parking;
        hardeningDisable = [
          "pic"
          "format"
        ];
        nativeBuildInputs = kernel.moduleBuildDependencies ++ [ pkgs.kmod ];
        makeFlags = [
          "KERNELRELEASE=${kernel.modDirVersion}"
          "KERNEL_DIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
          "INSTALL_MOD_PATH=$(out)"
        ];
        buildPhase = ''
          runHook preBuild
          make -C ${kernel.dev}/lib/modules/${kernel.modDirVersion}/build \
            M=$(pwd) \
            ARCH=${pkgs.stdenv.hostPlatform.linuxArch} \
            modules
          runHook postBuild
        '';
        installPhase = ''
          runHook preInstall
          install -D -m 644 cpu_parking.ko $out/lib/modules/${kernel.modDirVersion}/extra/cpu_parking.ko
          runHook postInstall
        '';
        enableParallelBuilding = true;
        meta = with lib; {
          description = "CPU core parking for Snapdragon X Elite (x1e80100)";
          license = licenses.gpl2Only;
          platforms = platforms.linux;
        };
      };

      ec-reboot = pkgs.stdenv.mkDerivation {
        pname = "ec-reboot";
        version = "0.1.0-${kernel.version}";
        src = ./kernel/modules/ec-reboot;
        hardeningDisable = [
          "pic"
          "format"
        ];
        nativeBuildInputs = kernel.moduleBuildDependencies ++ [ pkgs.kmod ];
        makeFlags = [
          "KERNELRELEASE=${kernel.modDirVersion}"
          "KERNEL_DIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
          "INSTALL_MOD_PATH=$(out)"
        ];
        buildPhase = ''
          runHook preBuild
          make -C ${kernel.dev}/lib/modules/${kernel.modDirVersion}/build \
            M=$(pwd) \
            ARCH=${pkgs.stdenv.hostPlatform.linuxArch} \
            modules
          runHook postBuild
        '';
        installPhase = ''
          runHook preInstall
          install -D -m 644 ec-reboot.ko $out/lib/modules/${kernel.modDirVersion}/extra/ec-reboot.ko
          runHook postInstall
        '';
        enableParallelBuilding = true;
        meta = with lib; {
          description = "Trigger EC hard reset via SSAM for Surface Laptop 7";
          license = licenses.gpl2Only;
          platforms = platforms.linux;
        };
      };

      platform-profile = pkgs.stdenv.mkDerivation {
        pname = "platform-profile-no-acpi";
        inherit (kernel)
          src
          version
          postPatch
          nativeBuildInputs
          ;
        kernel_dev = kernel.dev;
        kernelVersion = kernel.modDirVersion;
        patches = [
          ./kernel/modules/platform-profile/platform-profile-no-acpi.patch
        ];
        buildPhase = ''
          BUILT_KERNEL=$kernel_dev/lib/modules/$kernelVersion/build
          cp $BUILT_KERNEL/Module.symvers .
          cp $BUILT_KERNEL/.config        .
          cp $kernel_dev/vmlinux          .
          make "-j$NIX_BUILD_CORES" modules_prepare
          make "-j$NIX_BUILD_CORES" M=drivers/acpi modules
        '';
        installPhase = ''
          make \
            INSTALL_MOD_PATH="$out" \
            XZ="xz -T$NIX_BUILD_CORES" \
            M="drivers/acpi" \
            modules_install
        '';
        meta = with lib; {
          description = "Platform profile module patched for DT-based systems";
          license = licenses.gpl2Only;
          platforms = platforms.linux;
        };
      };
    in
{
  options.x1e = {
    cpuParking = lib.mkEnableOption "CPU core parking for Snapdragon X Elite";
    ecReboot = lib.mkEnableOption "EC hard-reset sysfs trigger (/sys/kernel/ec_reboot/reboot)";
  };

  config = {
    boot.extraModulePackages = [
      spi-hid
      gpi-qspi
      spi-geni-qcom-qspi
      ath12k-norfkill
      platform-profile
    ]
    ++ lib.optional config.x1e.ecReboot ec-reboot
    ++ lib.optional config.x1e.cpuParking cpu-parking;

    boot.kernelModules =
      lib.optional config.x1e.ecReboot "ec_reboot"
      ++ lib.optional config.x1e.cpuParking "cpu_parking";
  };
}
