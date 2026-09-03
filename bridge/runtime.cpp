#include "runtime.h"

namespace sphanorama::bridge {

Runtime& Runtime::Instance() {
  static Runtime runtime;
  return runtime;
}

}  // namespace sphanorama::bridge
