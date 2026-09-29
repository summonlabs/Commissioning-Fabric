#include "cxf/types/generations.hpp"

namespace cxf {

std::vector<std::string> FacilityGenerations::differences(
    const FacilityGenerations& other) const {
  std::vector<std::string> moved;
  if (topology != other.topology) {
    moved.emplace_back("topology");
  }
  if (power != other.power) {
    moved.emplace_back("power");
  }
  if (cooling != other.cooling) {
    moved.emplace_back("cooling");
  }
  if (network != other.network) {
    moved.emplace_back("network");
  }
  if (policy != other.policy) {
    moved.emplace_back("policy");
  }
  if (dependency != other.dependency) {
    moved.emplace_back("dependency");
  }
  if (firmware != other.firmware) {
    moved.emplace_back("firmware");
  }
  if (hardware != other.hardware) {
    moved.emplace_back("hardware");
  }
  if (lifecycle != other.lifecycle) {
    moved.emplace_back("lifecycle");
  }
  return moved;
}

}  // namespace cxf
