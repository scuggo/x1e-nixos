{
  description = "Surface Laptop 7 (x1e80100 / Snapdragon X Elite) kernel and hardware support";

  outputs = { self }: {
    nixosModules = {
      default = self.nixosModules.all;

      all = { ... }: {
        imports = [
          self.nixosModules.kernel
          self.nixosModules.kernel-modules
          self.nixosModules.boot
          self.nixosModules.hardware
        ];
      };

      kernel = import ./kernel.nix;
      kernel-modules = import ./kernel-modules.nix;
      boot = import ./boot.nix;
      hardware = import ./hardware.nix;
    };
  };
}
