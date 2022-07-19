#include <c10/core/SymbolicIntNode.h>

namespace c10 {

uint64_t SymIntTable::addNode(std::shared_ptr<SymbolicIntNode> sin) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto index = nodes_.size();
  nodes_.push_back(sin);
  return index;
}
std::shared_ptr<SymbolicIntNode> SymIntTable::getNode(size_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  TORCH_CHECK(index < nodes_.size());
  return nodes_[index];
}

c10::SymInt SymbolicIntNode::toSymInt() {
  // We will need to figure out a way
  // to dedup nodes
  auto sit_sp = this->shared_from_this();
  return SymInt::toSymInt(sit_sp);
}

SymIntTable& getSymIntTable() {
  static SymIntTable sit;
  return sit;
}
} // namespace c10
