/**
 * @file replacement_policy.h
 * @brief 可插拔的 Frame 淘汰策略接口
 * @ingroup BufferPool
 * @details 策略只保存淘汰所需的元数据，不拥有 Frame，也不做任何磁盘 I/O。
 * 能不能淘汰由调用方通过 is_replaceable 回调告知，最终判据是 Frame 的引用计数为 0。
 */
#pragma once

#include <functional>

#include "common/lang/memory.h"
#include "common/lang/string.h"
#include "storage/os/page/frame.h"

enum class BufferPoolReplacementPolicy
{
  LRU,    ///< 淘汰最久未访问的可替换页
  LRU_K,  ///< K 取 2，优先淘汰访问不足两次的冷页
  FIFO,   ///< 淘汰最早进入缓存的可替换页，命中不改变顺序
  CLOCK,  ///< 用引用位与 clock hand 实现第二次机会
};

/** @brief 把策略枚举转成名字，用于日志与 Trace */
const char *buffer_pool_replacement_policy_name(BufferPoolReplacementPolicy policy);
/**
 * @brief 解析命令行传入的策略名，大小写不敏感
 * @param name 策略名，LRU-K 支持 lru-k、lruk、lru_k 三种写法
 * @param policy 输出参数，解析成功时写入对应枚举
 * @return 无法识别时返回 false，由调用方决定回退策略
 */
bool parse_buffer_pool_replacement_policy(const string &name, BufferPoolReplacementPolicy &policy);

/**
 * Frame 淘汰策略接口。调用方在持有 BPFrameManager 锁时调用这些方法。
 */
class ReplacementPolicy
{
public:
  virtual ~ReplacementPolicy() = default;

/** @brief 新页进入缓存时调用，策略在此登记该页 */
  virtual void on_insert(const FrameId &frame_id) = 0;
/** @brief 缓存命中时调用，LRU 与 LRU-K 在这里更新访问记录，FIFO 与 CLOCK 只更新引用位 */
  virtual void on_access(const FrameId &frame_id) = 0;
/** @brief 页被 pin 时调用。是否可淘汰仍以 Frame 的实际引用计数为准，策略不据此判断 */
  virtual void on_pin(const FrameId &frame_id) = 0;
/** @brief 页被 unpin 时调用，与 on_pin 对称 */
  virtual void on_unpin(const FrameId &frame_id) = 0;
/** @brief 页被移出缓存时调用，策略在此清理该页的全部元数据 */
  virtual void on_remove(const FrameId &frame_id) = 0;

/**
 * @brief 选出一个可淘汰的页
 * @param is_replaceable 由调用方提供的判定回调，返回 true 表示该页当前可以淘汰
 * @param victim 输出参数，选中时写入该页标识
 * @return 找到返回 true；没有任何可淘汰的页时返回 false
 */
  virtual bool choose_victim(
      const std::function<bool(const FrameId &)> &is_replaceable, FrameId &victim) = 0;
/** @brief 返回该页在策略内部的元数据字符串，仅供诊断快照使用 */
  virtual string metadata(const FrameId &frame_id) const = 0;
/** @brief 返回策略名字 */
  virtual const char *name() const = 0;
};

/** @brief 策略工厂。传入无法识别的值时兜底返回 LRU */
unique_ptr<ReplacementPolicy> create_replacement_policy(BufferPoolReplacementPolicy policy);
