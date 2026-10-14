{
  description = "An infinite-canvas Wayland compositor (wlroots 0.20)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = f:
        nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: rec {
        wayland-canvas = pkgs.stdenv.mkDerivation {
          pname = "wayland-canvas";
          version = "0.1.0";

          src = ./.;

          strictDeps = true;

          # meson looks up wayland-scanner with `native: true`.
          depsBuildBuild = [ pkgs.pkg-config ];

          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
            wayland-scanner
          ];

          buildInputs = with pkgs; [
            cairo
            glib
            libdrm
            libinput
            librsvg
            libxkbcommon
            pango
            pixman
            wayland
            wayland-protocols
            wlroots_0_20
          ];

          doCheck = true;

          meta = {
            description = "An infinite-canvas Wayland compositor";
            platforms = pkgs.lib.platforms.linux;
            mainProgram = "wayland-canvas";
          };
        };

        default = wayland-canvas;
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.wayland-canvas ];
        };
      });
    };
}
