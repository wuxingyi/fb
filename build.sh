set -ex
root="$(cd $(dirname ${BASH_SOURCE[0]}) && pwd)"
if [[ -z ${CC} ]]; then export CC=/usr/bin/gcc; fi
if [[ -z ${CXX} ]]; then export CXX=/usr/bin/g++; fi

# Change Debug via  -DCMAKE_BUILD_TYPE=Debug
cmake -DCMAKE_BUILD_TYPE=Release \
  -B$root/build \
  -H$root \
  -DCMAKE_C_COMPILER=$CC \
  -DCMAKE_CXX_COMPILER=$CXX \
  "$@"

# (cd $root/build && ninja -j `grep -c ^processor /proc/cpuinfo`)
