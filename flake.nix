{
  description = "Cask: a Wine prefix manager written in C++/Qt";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    # This branch holds only the Nix files; the program itself is taken from `main`.
    cask-src = {
      url = "github:ndiruhniu/cask/v0.1.4";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      cask-src,
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      # nix/release.json is rewritten when a release is published (see the Build workflow on `main`):
      # if cask-src is exactly the release commit, "About" shows the release number, otherwise the short hash.
      release = builtins.fromJSON (builtins.readFile ./nix/release.json);
      caskVersion =
        if release.rev != null && (cask-src.rev or null) == release.rev then
          release.version
        else
          cask-src.shortRev or "dev";
      callCask = pkgs: pkgs.callPackage ./nix/package.nix { caskSrc = cask-src; version = caskVersion; };
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      # nix build, nix run
      packages = forAllSystems (pkgs: rec {
        cask = callCask pkgs;
        cask-fhs = cask.fhs;
        default = cask;
      });

      apps = forAllSystems (pkgs: {
        default = {
          type = "app";
          program = nixpkgs.lib.getExe self.packages.${pkgs.stdenv.hostPlatform.system}.cask;
        };
      });

      # nix develop: everything to build, debug and run Cask. Run it inside a checkout of `main`
      # (e.g. `nix develop github:ndiruhniu/cask/v0.1.4
      #   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && build/Cask/cask
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.cask ];
          packages = with pkgs; [
            gdb
            valgrind
            clang-tools
            python3
            winetricks
            cabextract
            unzip
          ];
          shellHook = ''
            # Keep a development run away from your real Cask folder.
            export CASK_ROOT="$PWD/.cask-dev"
            echo "Cask dev shell. Build: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build"
            echo "Run: build/Cask/cask   (data in \$CASK_ROOT = $CASK_ROOT)"
          '';
        };
      });

      # For other flakes: `inputs.cask.overlays.default` gives `pkgs.cask`.
      overlays.default = final: _prev: {
        cask = callCask final;
      };

      # For NixOS configurations: `imports = [ inputs.cask.nixosModules.default ];`
      nixosModules.default =
        { lib, pkgs, ... }:
        {
          imports = [ ./nix/program.nix ];
          programs.cask.package = lib.mkDefault self.packages.${pkgs.stdenv.hostPlatform.system}.cask;
        };
      nixosModules.cask = self.nixosModules.default;
    };
}
