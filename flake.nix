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
        wayland-canvas = pkgs.clangStdenv.mkDerivation rec {
          pname = "wayland-canvas";
          # No tags reach a flake: tools/version.sh's untagged form, from
          # the flake's own revision, handed to it as the tarball's VERSION.
          version = "0.0.0.r${toString (self.revCount or 0)}.g${self.shortRev or "dirty"}";

          src = ./.;

          postPatch = ''
            patchShebangs tools/version.sh
            printf '%s\n' ${version} > VERSION
          '';

          strictDeps = true;

          # meson looks up wayland-scanner with `native: true`.
          depsBuildBuild = [ pkgs.pkg-config ];

          nativeBuildInputs = with pkgs; [
            makeWrapper
            meson
            ninja
            pkg-config
            wayland-scanner
          ];

          buildInputs = with pkgs; [
            cairo
            glib
            json-glib
            libdrm
            libinput
            librsvg
            libxcb # X11 windows through Xwayland
            libxcb-wm # xcb_ewmh.h, which wlroots' xwayland.h includes
            libxkbcommon
            pango
            pixman
            sqlite
            systemdLibs # logind: the lock on `loginctl lock-session` and before sleep
            wayland
            wayland-protocols
            wlroots_0_20
          ];

          # The suites a build can run: the packaging suite drives the Arch and
          # Debian install flows through stubs, and the scenarios need a display.
          doCheck = true;
          mesonCheckFlags = [ "--suite" "unit" ];
          # What the unit suite reads from the machine: the desktop's GSettings
          # schemas (theme-prefs) and the zone database (agent-usage's days).
          preCheck = ''
            export XDG_DATA_DIRS=${pkgs.glib.getSchemaDataDirPath pkgs.gsettings-desktop-schemas}''${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}
            export TZDIR=${pkgs.tzdata}/share/zoneinfo
          '';

          # The compositor starts the bar's `qs` and the recorder's `ffmpeg` by
          # name (config.h), so the wrapper puts both on its PATH.
          postFixup = ''
            wrapProgram $out/bin/wayland-canvas \
              --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.ffmpeg pkgs.quickshell ]}
          '';

          meta = {
            description = "An infinite-canvas Wayland compositor";
            platforms = pkgs.lib.platforms.linux;
            mainProgram = "wayland-canvas";
          };
        };

        default = wayland-canvas;
      });

      devShells = forAllSystems (pkgs: {
        default = (pkgs.mkShell.override { stdenv = pkgs.clangStdenv; }) {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.wayland-canvas ];
        };
      });
    };
}
