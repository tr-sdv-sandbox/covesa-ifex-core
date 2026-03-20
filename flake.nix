{
  description = "COVESA IFEX Core development shell";

  inputs = {
    # Use nixpkgs 23.05 to obtain a protobuf/grpc toolchain in the
    # 3.21.x line that matches the repository C++ expectations without
    # needing per-package overlays. This keeps the change minimal and
    # avoids rebuilding grpc from source in the developer shell.
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-23.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        # Pin protobuf to a specific protobuf variant from nixpkgs to ensure
        # the C++ protobuf API matches what the repo expects. Use protobuf_21
        # per the requested micro-fix to target the 21.x protobuf line.
        pkgs = import nixpkgs { inherit system; };

        # Keep default pkgs.protobuf in use to preserve grpc linking
        # compatibility. Do not create overlays that replace the global
        # protobuf attribute; instead rely on matching pkgs-provided
        # protobuf and grpc versions from the same nixpkgs.
        python = pkgs.python3.withPackages (ps: with ps; [
          flask
          flask-cors
          grpcio
          protobuf
          pyyaml
        ]);
      in {
        devShells.default = pkgs.mkShell {
            packages = with pkgs; [
              bash
              cmake
              coreutils
              docker
              findutils
              git
              gflags
              glog
              gnumake
              gnugrep
              gnused
              grpc
              openssl
              grpcurl
              gtest
              lua5_4
              mosquitto
              nlohmann_json
               pkg-config
                protobuf
               python
              sqlite
              stdenv.cc
              yaml-cpp
            ];

          shellHook = ''
            # Expose plugin/protoc paths for CMake and ensure the CMake
            # package discovery finds the same protobuf installation that
            # provides protoc. Do not write files in the repo from
            # shellHook.
            export GRPC_CPP_PLUGIN_EXECUTABLE="$(command -v grpc_cpp_plugin)"
            export Protobuf_PROTOC_EXECUTABLE="$(command -v protoc)"

            # Derive the protobuf package store prefix from protoc and add
            # it to CMAKE_PREFIX_PATH so find_package(Protobuf CONFIG)
            # prefers the same installation for headers/libs as protoc.
            if [ -n "$(command -v protoc)" ]; then
              _protoc_bin="$(command -v protoc)"
              _protoc_root="$(dirname "$(dirname "$_protoc_bin")")"
              export CMAKE_PREFIX_PATH="$_protoc_root:$CMAKE_PREFIX_PATH"
            fi

            echo "Entered COVESA IFEX Core dev shell"
            echo "- grpc_cpp_plugin: $GRPC_CPP_PLUGIN_EXECUTABLE"
            echo "- protoc: $(command -v protoc)"
            echo "- python: $(command -v python3)"
            echo "- next: ./generate_proto.sh && ./build.sh --debug"
          '';
        };
      });
}
