#include "lru_replacer.h"
#include <algorithm>

LRUReplacer::LRUReplacer(size_t num_pages) {
    max_size_ = num_pages;  // 初始化最大页面数
}

LRUReplacer::~LRUReplacer() = default;  // 默认析构函数

bool LRUReplacer::Victim(frame_id_t *frame_id) {
    std::scoped_lock lock{latch_};  // 线程安全锁    
    if (LRUlist_.empty()) {  // 无可用淘汰页
        *frame_id = -1;
        return false;
    }    
    *frame_id = LRUlist_.back();       // 取链表尾部（最久未使用页）
    LRUhash_.erase(*frame_id);         // 从哈希表移除
    LRUlist_.pop_back();               // 从链表移除
    return true;
}

void LRUReplacer::Pin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};    
    if (LRUhash_.count(frame_id)) {    // 若页存在于LRU结构中
        auto it = LRUhash_[frame_id];  // 获取链表迭代器
        LRUlist_.erase(it);            // 从链表删除
        LRUhash_.erase(frame_id);      // 从哈希表删除
    }
}

void LRUReplacer::Unpin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};    
    if (LRUhash_.count(frame_id)) return;  // 已在LRU中则不重复添加    
    LRUlist_.push_front(frame_id);         // 添加到链表头部（最近使用）
    LRUhash_[frame_id] = LRUlist_.begin(); // 更新哈希表映射
}

size_t LRUReplacer::Size() {
    std::scoped_lock lock{latch_};
    return LRUlist_.size();  // 返回当前可淘汰页数量
}