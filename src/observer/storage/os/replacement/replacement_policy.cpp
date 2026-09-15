/**
 * @file replacement_policy.cpp
 * @brief 四种淘汰策略的实现：LRU、FIFO、LRU-K、CLOCK
 * @ingroup BufferPool
 * @details LRU 与 FIFO 共用 OrderedReplacementPolicy，差别只在一个布尔值；
 * LRU-K 维护每个页最近 K 次访问的时间戳；CLOCK 用环形数组加引用位实现第二次机会。
 */
#include "storage/os/replacement/replacement_policy.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <list>
#include <strings.h>
#include <unordered_map>
#include <vector>

#include "common/lang/sstream.h"

namespace {

/** @brief FrameId 的哈希适配器，供策略内部的哈希表使用 */
struct FrameIdHasher
{
  size_t operator()(const FrameId &frame_id) const { return frame_id.hash(); }
};

/**
 * @brief 用一条有序链表实现 LRU 与 FIFO
 * @details 链表队首是最旧、队尾是最新。LRU 与 FIFO 的差别只有一点：
 * 缓存命中时要不要把该页移到队尾，由构造函数传入的 touch_on_access 决定。
 * 用 std::list 而不是 vector，是因为移动节点只需要 splice，不拷贝也不重新分配内存。
 */
class OrderedReplacementPolicy : public ReplacementPolicy
{
public:
/**
 * @brief 构造
 * @param touch_on_access 命中时是否更新顺序。LRU 传 true，FIFO 传 false
 */
  explicit OrderedReplacementPolicy(bool touch_on_access) : touch_on_access_(touch_on_access) {}

/** @brief 新页入队到队尾。先调 on_remove 保证同一个页不会重复登记 */
  void on_insert(const FrameId &frame_id) override
  {
    on_remove(frame_id);
    order_.push_back(frame_id);
    positions_[frame_id] = std::prev(order_.end());
  }

/**
 * @brief 命中时更新顺序
 * @details FIFO 直接返回，什么都不做；LRU 用 splice 把该页的节点从链表中间摘下来挂到队尾。
 * 这是 O(1) 操作，正是选用 std::list 的原因。
 */
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

/** @brief pin 与 unpin 不影响顺序，两个回调都是空的 */
  void on_pin(const FrameId &) override {}
  void on_unpin(const FrameId &) override {}

/** @brief 把该页从链表和位置索引中一并移除 */
  void on_remove(const FrameId &frame_id) override
  {
    auto iter = positions_.find(frame_id);
    if (iter == positions_.end()) {
      return;
    }
    order_.erase(iter->second);
    positions_.erase(iter);
  }

/**
 * @brief 从队首开始找第一个可淘汰的页
 * @details 队首最旧，所以最先被检查到的可淘汰页就是最该被淘汰的那一个。
 * @return 全部页面都不可淘汰时返回 false
 */
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

/**
 * @brief 返回该页在链表中的序号
 * @details 需要遍历链表，是 O(n) 操作，只用于诊断快照，不在淘汰路径上。
 * @return 形如 order=3 的字符串
 */
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
/**
 * @brief 是否在命中时更新顺序
 * @details order_ 队首最旧；positions_ 让链表节点可以被 O(1) 定位与移动。
 */
  bool touch_on_access_;
  std::list<FrameId> order_;  // oldest/least-recent at front
  std::unordered_map<FrameId, std::list<FrameId>::iterator, FrameIdHasher> positions_;
};

/** @brief 最近最少使用策略，命中会刷新新近性 */
class LRUReplacementPolicy final : public OrderedReplacementPolicy
{
public:
/** @brief 构造 LRU，开启命中时更新顺序 */
  LRUReplacementPolicy() : OrderedReplacementPolicy(true) {}
  const char *name() const override { return "LRU"; }
};

/** @brief 先进先出策略，命中不改变进入顺序 */
class FIFOReplacementPolicy final : public OrderedReplacementPolicy
{
public:
/** @brief 构造 FIFO，关闭命中时更新顺序 */
  FIFOReplacementPolicy() : OrderedReplacementPolicy(false) {}
  const char *name() const override { return "FIFO"; }
};

/**
 * @brief LRU-K 策略，K 固定为 2
 * @details 为每个页保存最近两次访问的时间戳。访问不足两次的页属于冷页，
 * 优先被淘汰；冷热相同时比较倒数第二次访问时间，更旧的先淘汰。
 * 这样一次性顺序扫描产生的页会停留在冷页集合里被优先换出，
 * 不会把反复访问的热点页挤出缓存。
 */
class LRUKReplacementPolicy final : public ReplacementPolicy
{
public:
/** @brief 新页入缓存，先清掉旧记录再记一次访问 */
  void on_insert(const FrameId &frame_id) override
  {
    on_remove(frame_id);
    record_access(frame_id);
  }

/** @brief 命中时追加一次访问记录 */
  void on_access(const FrameId &frame_id) override { record_access(frame_id); }
/** @brief pin 与 unpin 不参与 LRU-K 的冷热判断，两个回调都是空的 */
  void on_pin(const FrameId &) override {}
  void on_unpin(const FrameId &) override {}

/** @brief 页被移出缓存时丢弃它的访问历史，避免陈旧记录影响后续判断 */
  void on_remove(const FrameId &frame_id) override { histories_.erase(frame_id); }

/**
 * @brief 两阶段选择 victim
 * @details 遍历所有候选页，按两条规则选出最该淘汰的一个：
 * 第一，冷页优先。访问次数不足 K 次的页属于冷页，只要候选里有冷页就一定选冷页。
 * 第二，同类之间比时间。冷页与冷页比，或者热页与热页比时，
 * 都取 history.front() 更小者，也就是倒数第二次访问更早的那一个。
 * @return 没有任何可淘汰页时返回 false
 */
  bool choose_victim(const std::function<bool(const FrameId &)> &is_replaceable, FrameId &victim) override
  {
    bool found = false;
    bool victim_is_cold = false;
    uint64_t oldest_relevant_access = std::numeric_limits<uint64_t>::max();

    for (const auto &[frame_id, history] : histories_) {
      if (history.empty() || !is_replaceable(frame_id)) {
        continue;
      }

      const bool cold = history.size() < K;
      const uint64_t relevant_access = history.front();
      if (!found || (cold && !victim_is_cold) ||
          (cold == victim_is_cold && relevant_access < oldest_relevant_access)) {
        victim = frame_id;
        victim_is_cold = cold;
        oldest_relevant_access = relevant_access;
        found = true;
      }
    }
    return found;
  }

/**
 * @brief 返回该页的策略元数据
 * @return 形如 k=2,history=1,class=cold 的字符串，可供 CLI 与 Web 直接展示
 */
  string metadata(const FrameId &frame_id) const override
  {
    const auto iter = histories_.find(frame_id);
    if (iter == histories_.end()) {
      return "history=unknown";
    }
    stringstream ss;
    ss << "k=2,history=" << iter->second.size()
       << ",class=" << (iter->second.size() < K ? "cold" : "hot");
    return ss.str();
  }

/** @brief 策略名 */
  const char *name() const override { return "LRU-K"; }

private:
/**
 * @brief 记录一次访问
 * @details 队列长度达到 K 就先弹出最旧的一个，再压入新的时间戳，
 * 因此队列里保存的始终是最近 K 次访问。front() 就是倒数第二次访问的时间。
 */
  void record_access(const FrameId &frame_id)
  {
    auto &history = histories_[frame_id];
    if (history.size() == K) {
      history.pop_front();
    }
    history.push_back(++current_timestamp_);
  }

/**
 * @brief 阶段常数 K 固定为 2
 * @details current_timestamp_ 是单调递增的逻辑时钟，仅用于比较先后，与真实时间无关。
 */
  static constexpr size_t K = 2;
  uint64_t current_timestamp_ = 0;
  std::unordered_map<FrameId, std::deque<uint64_t>, FrameIdHasher> histories_;
};

/**
 * @brief CLOCK 策略
 * @details 环形数组保存全部页，引用位表示该页最近是否被访问过，hand 是扫描指针。
 * 选 victim 时沿环扫描：引用位为 1 的清零并给第二次机会，为 0 的才淘汰。
 */
class ClockReplacementPolicy final : public ReplacementPolicy
{
public:
/** @brief 新页加入环，引用位置 1 */
  void on_insert(const FrameId &frame_id) override
  {
    on_remove(frame_id);
    ring_.push_back(frame_id);
    referenced_[frame_id] = true;
  }

/** @brief 命中时把引用位置回 1，等于告诉下一次扫描再给它一次机会 */
  void on_access(const FrameId &frame_id) override
  {
    auto iter = referenced_.find(frame_id);
    if (iter != referenced_.end()) {
      iter->second = true;
    }
  }

/** @brief pin 与 unpin 不改变引用位，两个回调都是空的 */
  void on_pin(const FrameId &) override {}
  void on_unpin(const FrameId &) override {}

/**
 * @brief 从环中移除一个页，并维护扫描指针
 * @details 这是本文件最需要小心的一段。被移除的页如果在 hand 的前面，
 * hand 必须往前挪一格，否则接下来的扫描会跳过元素或者越界；
 * 移除后若环为空或 hand 已越界，就把 hand 归零。
 */
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

/**
 * @brief 沿环扫描选 victim
 * @details 每轮先取走 hand 指向的页再推进 hand，这是 CLOCK 的标准写法。
 * 不可淘汰的页直接跳过；引用位为 1 的页清零后继续扫描，相当于给它第二次机会；
 * 遇到引用位为 0 且可淘汰的页就选中它。
 * 扫描次数上限是环长的两倍，避免全部页都被 pin 时无限循环。
 * @return 扫满上限仍未找到可淘汰页时返回 false
 */
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

/**
 * @brief 返回该页的策略元数据
 * @return 形如 reference=1,clock_slot=3 的字符串
 */
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

/** @brief 策略名 */
  const char *name() const override { return "CLOCK"; }

private:
/**
 * @brief 环形页数组与扫描指针
 * @details referenced_ 记录引用位，ring_ 决定扫描顺序，hand_ 是当前指针位置。
 */
  std::vector<FrameId> ring_;
  std::unordered_map<FrameId, bool, FrameIdHasher> referenced_;
  size_t hand_ = 0;
};

}  // namespace

/** @brief 把策略枚举转成名字 */
const char *buffer_pool_replacement_policy_name(BufferPoolReplacementPolicy policy)
{
  switch (policy) {
    case BufferPoolReplacementPolicy::LRU: return "LRU";
    case BufferPoolReplacementPolicy::LRU_K: return "LRU-K";
    case BufferPoolReplacementPolicy::FIFO: return "FIFO";
    case BufferPoolReplacementPolicy::CLOCK: return "CLOCK";
  }
  return "UNKNOWN";
}

/**
 * @brief 解析命令行传入的策略名，大小写不敏感
 * @details LRU-K 额外接受 lruk 与 lru_k 两种写法。
 * @return 无法识别时返回 false，由上层决定回退策略
 */
bool parse_buffer_pool_replacement_policy(const string &name, BufferPoolReplacementPolicy &policy)
{
  if (strcasecmp(name.c_str(), "lru") == 0) {
    policy = BufferPoolReplacementPolicy::LRU;
    return true;
  }
  if (strcasecmp(name.c_str(), "lru-k") == 0 || strcasecmp(name.c_str(), "lruk") == 0 ||
      strcasecmp(name.c_str(), "lru_k") == 0) {
    policy = BufferPoolReplacementPolicy::LRU_K;
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

/**
 * @brief 策略工厂
 * @details 让 DiskBufferPool 与 FrameManager 里不出现任何按策略分支的 if/else，
 * 新增策略只需要在这里加一个分支。
 * @return 传入无法识别的枚举值时兜底返回 LRU
 */
unique_ptr<ReplacementPolicy> create_replacement_policy(BufferPoolReplacementPolicy policy)
{
  switch (policy) {
    case BufferPoolReplacementPolicy::LRU: return make_unique<LRUReplacementPolicy>();
    case BufferPoolReplacementPolicy::LRU_K: return make_unique<LRUKReplacementPolicy>();
    case BufferPoolReplacementPolicy::FIFO: return make_unique<FIFOReplacementPolicy>();
    case BufferPoolReplacementPolicy::CLOCK: return make_unique<ClockReplacementPolicy>();
  }
  return make_unique<LRUReplacementPolicy>();
}
