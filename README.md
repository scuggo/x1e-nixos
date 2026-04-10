# x1e-nixos

NixOS flake providing kernel and hardware support for the **Microsoft Surface Laptop 7** (Snapdragon X Elite / x1e80100).

## Usage

```nix
# flake.nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    x1e-nixos.url = "git+https://git.scug.io/nikkuss/x1e-nixos.git";

    # Required: provides pkgs.slbounce, pkgs.tcblaunch,
    # pkgs.x1e80100-firmware, pkgs.x1e80100-linux-firmware
    custom-pkgs = {
      url = "git+https://git.scug.io/nikkuss/pkgs.git";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { nixpkgs, x1e-nixos, custom-pkgs, ... }: {
    nixosConfigurations.my-surface = nixpkgs.lib.nixosSystem {
      system = "aarch64-linux";
      modules = [
        { nixpkgs.overlays = [ custom-pkgs.overlays.default ]; }
        x1e-nixos.nixosModules.default
        ./configuration.nix
      ];
    };
  };
}
```

The `default` module imports everything (kernel, modules, boot, hardware). To pick individual pieces:

```nix
x1e-nixos.nixosModules.kernel         # boot.kernelPackages + initrd config
x1e-nixos.nixosModules.kernel-modules # out-of-tree .ko modules
x1e-nixos.nixosModules.boot           # systemd-boot + slbounce + firmware paths
x1e-nixos.nixosModules.hardware       # DTB + firmware packages
```

## Optional modules

Some modules don't auto-load via device tree / PCI and are behind config flags:

```nix
{
  x1e.model = "15";       # "13" (default) or "15" — selects the correct DTB
  x1e.cpuParking = true;  # loads cpu_parking at boot
  x1e.ecReboot = true;    # loads ec_reboot at boot, exposes /sys/kernel/ec_reboot/reboot
}
```

## Touchpad not working

After a lid close or certain sleep/wake cycles, the EC may cut power to the touchpad sensor in a way that Linux cannot recover from on its own. If the touchpad stops responding:

**Option A — sysfs (no reboot required on its own, but the EC reset will reboot the machine):**

```sh
modprobe ec_reboot
echo 1 > /sys/kernel/ec_reboot/reboot
```

**Option B — hard power off:**

Hold the power button for **10 seconds** until the machine fully shuts off, then power it back on.

Both methods force a full EC power cycle, which resets the touchpad sensor state.

## EL2

The EL2 boot specialisation (slbounce) is currently broken. The default boot entry runs in EL1.
