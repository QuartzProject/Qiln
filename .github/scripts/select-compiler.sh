#!/usr/bin/env bash
# Installs Clang 23 and writes QILN_CMAKE_ARGS for a qiln that carries its C++ runtime.
set -euo pipefail

retry() {
    local tries="$1" attempt=1
    shift
    until "$@"; do
        if [ "$attempt" -ge "$tries" ]; then
            echo "giving up on: $*" >&2
            return 1
        fi
        sleep $((attempt * 15))
        attempt=$((attempt + 1))
    done
}

case "$(uname -s)" in
Linux)
    # Ubuntu 24.04's libstdc++ 14 lacks parts of C++23 (std::format's "{:?}"); Clang picks the newest GCC it finds.
    retry 4 sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
    retry 4 sudo apt-get install -y --no-install-recommends g++-15
    retry 4 wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
    retry 4 sudo bash /tmp/llvm.sh 23
    retry 4 sudo apt-get install -y --no-install-recommends ninja-build
    echo "QILN_CMAKE_ARGS=-DCMAKE_C_COMPILER=clang-23 -DCMAKE_CXX_COMPILER=clang++-23 \"-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc\"" >>"$GITHUB_ENV"
    ;;
Darwin)
    retry 4 brew install llvm ninja
    llvm="$(brew --prefix llvm)"
    # Homebrew's libc++ is newer than the system one: link its static archive so qiln needs only the system.
    echo "QILN_CMAKE_ARGS=-DCMAKE_C_COMPILER=$llvm/bin/clang -DCMAKE_CXX_COMPILER=$llvm/bin/clang++ \"-DCMAKE_CXX_STANDARD_LIBRARIES=-lc++ $llvm/lib/c++/libc++.a\" -DCMAKE_CXX_FLAGS=-D_LIBCPP_DISABLE_AVAILABILITY" >>"$GITHUB_ENV"
    ;;
MINGW* | MSYS*)
    echo "QILN_CMAKE_ARGS=-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXE_LINKER_FLAGS=-static" >>"$GITHUB_ENV"
    ;;
*)
    echo "QILN_CMAKE_ARGS=" >>"$GITHUB_ENV"
    ;;
esac
