#include "storage/os/replacement/replacement_policy.h"

#include <algorithm>
#include <list>
#include <strings.h>
#include <unordered_map>
#include <vector>

#include "common/lang/sstream.h"

namespace {

struct FrameIdHasher
{
  size_t operator()(const FrameId &frame_id) const { return frame_id.hash(); }
};

class OrderedReplacementPolicy : public ReplacementPolicy
{
public:
  explicit OrderedReplacementPolicy(bool touch_on_access) : touch_on_access_(touch_on_access) {}

  void on_insert(const FrameId &frame_id) override
  {
    on_remove(frame_id);
    order_.push_back(frame_id);
    positions_[frame_id] = std::prev(order_.end());
  }

  void on_access(const FrameId &frame_id) override
  {
    if (!touch_on_access_) {
      return;
    }
    auto iter = positions_.find(frame_id);
    if (iter == positions_.end()) {
      return;
    }
    order_.splice(order_.end(), order_, iter->second);
    iter->second = std::prev(order_.end());
  }

  void on_pin(const FrameId &) override {}
  void on_unpin(const FrameId &) override {}

  void on_remove(const FrameId &frame_id) override
  {
    auto iter = positions_.find(frame_id);
    if (iter == positions_.end()) {
      return;
    }
    order_.erase(iter->second);
    positions_.erase(iter);
  }

  bool choose_victim(const std::function<bool(const FrameId &)> &is_replaceable, FrameId &victim) override
  {
    for (const FrameId &frame_id : order_) {
      if (is_replaceable(frame_id)) {
        victim = frame_id;
        return true;
      }
    }
    return false;
  }

  string metadata(const FrameId &frame_id) const override
  {
    size_t index = 0;
    for (const FrameId &candidate : order_) {
      if (candidate == frame_id) {
        stringstream ss;
        ss << "order=" << index;
        return ss.str();
      }
      ++index;
    }
    return "order=unknown";
  }

protected:
  bool touch_on_access_;
  std::list<FrameId> order_;  // oldest/least-recent at front
  std::unordered_map<FrameId, std::list<FrameId>::iterator, FrameIdHasher> positions_;
};

class LRUReplacementPolicy final : public OrderedReplacementPolicy
{
public:
  LRUReplacementPolicy() : OrderedReplacementPolicy(true) {}
  const char *name() const override { return "LRU"; }
};

class FIFOReplacementPolicy final : public OrderedReplacementPolicy
{
public:
  FIFOReplacementPolicy() : OrderedReplacementPolicy(false) {}
  const char *name() const override { return "FIFO"; }
};

class ClockReplacementPolicy final : public ReplacementPolicy
{
public:
  void on_insert(const FrameId &frame_id) override
  {
    on_remove(frame_id);
    ring_.push_back(frame_id);
    referenced_[frame_id] = true;
  }

  void on_access(const FrameId &frame_id) override
  {
    auto iter = referenced_.find(frame_id);
    if (iter != referenced_.end()) {
      iter->second = true;
    }
  }

  void on_pin(const FrameId &) override {}
  void on_unpin(const FrameId &) override {}

  void on_remove(const FrameId &frame_id) override
  {
    auto iter = std::find(ring_.begin(), ring_.end(), frame_id);
    if (iter == ring_.end()) {
      referenced_.erase(frame_id);
      return;
    }

    const size_t index = static_cast<size_t>(std::distance(ring_.begin(), iter));
    if (index < hand_ && hand_ > 0) {
      --hand_;
    }
    ring_.erase(iter);
    referenced_.erase(frame_id);
    if (ring_.empty() || hand_ >= ring_.size()) {
      hand_ = 0;
    }
  }

  bool choose_victim(const std::function<bool(const FrameId &)> &is_replaceable, FrameId &victim) override
  {
    if (ring_.empty()) {
      return false;
    }

    const size_t scan_limit = ring_.size() * 2;
    for (size_t scanned = 0; scanned < scan_limit; ++scanned) {
      const FrameId candidate = ring_[hand_];
      hand_ = (hand_ + 1) % ring_.size();

      if (!is_replaceable(candidate)) {
        continue;
      }

      bool &referenced = referenced_[candidate];
      if (referenced) {
        referenced = false;
        continue;
      }

      victim = candidate;
      return true;
    }
    return false;
  }

  string metadata(const FrameId &frame_id) const override
  {
    const auto ref_iter = referenced_.find(frame_id);
    const auto pos_iter = std::find(ring_.begin(), ring_.end(), frame_id);
    if (ref_iter == referenced_.end() || pos_iter == ring_.end()) {
      return "reference=unknown";
    }
    stringstream ss;
    ss << "reference=" << (ref_iter->second ? 1 : 0)
       << ",clock_slot=" << std::distance(ring_.begin(), pos_iter);
    return ss.str();
  }

  const char *name() const override { return "CLOCK"; }

private:
  std::vector<FrameId> ring_;
  std::unordered_map<FrameId, bool, FrameIdHasher> referenced_;
  size_t hand_ = 0;
};

}  // namespace

const char *buffer_pool_replacement_policy_name(BufferPoolReplacementPolicy policy)
{
  switch (policy) {
    case BufferPoolReplacementPolicy::LRU: return "LRU";
    case BufferPoolReplacementPolicy::FIFO: return "FIFO";
    case BufferPoolReplacementPolicy::CLOCK: return "CLOCK";
  }
  return "UNKNOWN";
}

bool parse_buffer_pool_replacement_policy(const string &name, BufferPoolReplacementPolicy &policy)
{
  if (strcasecmp(name.c_str(), "lru") == 0) {
    policy = BufferPoolReplacementPolicy::LRU;
    return true;
  }
  if (strcasecmp(name.c_str(), "fifo") == 0) {
    policy = BufferPoolReplacementPolicy::FIFO;
    return true;
  }
  if (strcasecmp(name.c_str(), "clock") == 0) {
    policy = BufferPoolReplacementPolicy::CLOCK;
    return true;
  }
  return false;
}

unique_ptr<ReplacementPolicy> create_replacement_policy(BufferPoolReplacementPolicy policy)
{
  switch (policy) {
    case BufferPoolReplacementPolicy::LRU: return make_unique<LRUReplacementPolicy>();
    case BufferPoolReplacementPolicy::FIFO: return make_unique<FIFOReplacementPolicy>();
    case BufferPoolReplacementPolicy::CLOCK: return make_unique<ClockReplacementPolicy>();
  }
  return make_unique<LRUReplacementPolicy>();
}
