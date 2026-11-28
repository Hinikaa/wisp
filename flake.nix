{
  description = "Small Linux shell written in C++17 with Lua 5.4 embedded for config and structured pipelines";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: {
        wisp = pkgs.stdenv.mkDerivation {
          pname = "wisp";
          version = "3.2.0";
          src = self;

          nativeBuildInputs = [ pkgs.pkg-config ];
          buildInputs = [ pkgs.lua5_4 ];
          nativeCheckInputs = [ pkgs.python3 ];

          makeFlags = [ "STATIC_CXX=" ];
          buildFlags = [ "wisp" ];

          doCheck = true;
          checkTarget = "test";

          installPhase = ''
            runHook preInstall
            install -Dm755 wisp "$out/bin/wisp"
            install -Dm644 example-init.lua "$out/share/doc/wisp/example-init.lua"
            install -Dm644 LICENSE "$out/share/licenses/wisp/LICENSE"
            install -Dm644 LINENOISE_LICENSE "$out/share/licenses/wisp/LINENOISE_LICENSE"
            runHook postInstall
          '';

          meta = with pkgs.lib; {
            description = "Small Linux shell with Lua 5.4 embedded for config and structured pipelines";
            homepage = "https://github.com/Hinikaa/wisp";
            license = [ licenses.mit licenses.bsd2 ];
            platforms = platforms.linux;
            mainProgram = "wisp";
          };
        };

        default = self.packages.${pkgs.stdenv.hostPlatform.system}.wisp;
      });
    };
}
