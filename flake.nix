{
  description = "libchromadec: composite video chroma decoding library";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    let
      # Exported separately from the per-system outputs so a consumer can add it
      # to their own pkgs and get libchromadec built against *their* nixpkgs pin
      # and their own dependency overrides:
      #
      #   pkgs = import nixpkgs { overlays = [ chromadec.overlays.default ]; };
      #   pkgs.libchromadec.override { onnxruntime = myPrebuiltOrt; }
      #
      overlay = final: prev: {
        libchromadec = final.callPackage ./nix/libchromadec.nix {
          # nixpkgs before the apple-sdk rework has no such attribute; the
          # derivation treats null as "frameworks come from the stdenv".
          apple-sdk = final.apple-sdk_15 or null;
          onnxruntime = final.onnxruntime;
        };
      };
    in
    {
      overlays.default = overlay;
    }
    // flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [ overlay ];
        };
      in
      {
        packages = {
          default = pkgs.libchromadec;
          libchromadec = pkgs.libchromadec;

          # No inference backend at all: the classic (PAL/NTSC comb, Transform
          # PAL) decoders only. Useful as a fast smoke build and for consumers
          # that never touch the NN paths.
          libchromadec-noml = pkgs.libchromadec.override {
            onnxruntime = null;
            withCoreml = false;
          };
        }
        // nixpkgs.lib.optionalAttrs (system == "x86_64-linux") {
          libchromadec-cuda = pkgs.libchromadec.override {
            cudaSupport = true;
            cudaPackages = pkgs.cudaPackages;
          };
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ pkgs.libchromadec ];
          packages = [ pkgs.gdb pkgs.clang-tools ];
        };
      });
}
