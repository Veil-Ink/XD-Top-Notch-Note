#include "buffer_pool_manager.h"

/**
 * 从free_list或replacer中获取可替换帧ID
 * @param frame_id 输出参数，存储找到的可用帧ID
 * @return 成功找到返回true，否则false
 */
bool BufferPoolManager::FindVictimPage(frame_id_t *frame_id) {
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    return replacer_->Victim(frame_id);
}

/**
 * 更新页面数据并同步到磁盘
 * @param page 待更新的页面指针
 * @param new_page_id 新的页面ID
 * @param new_frame_id 新的帧ID
 */
void BufferPoolManager::UpdatePage(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    PageId old_page_id = page->GetPageId();
    
    if (page->IsDirty()) {
        disk_manager_->write_page(old_page_id.fd, old_page_id.page_no, page->GetData(), PAGE_SIZE);
        page->is_dirty_ = false;
        page->pin_count_ = 0;
    }

    page_table_.erase(old_page_id);
    page->ResetMemory();
    
    page_table_[new_page_id] = new_frame_id;
    Page &new_page = pages_[new_frame_id];
    
    new_page.id_ = new_page_id;
    new_page.is_dirty_ = false;
    new_page.pin_count_ = 1;
    
    replacer_->Pin(new_frame_id);
    disk_manager_->read_page(new_page_id.fd, new_page_id.page_no, new_page.GetData(), PAGE_SIZE);
}

/**
 * 从缓冲池获取页面
 * @param page_id 待获取的页面ID
 * @return 页面指针，失败时返回nullptr
 */
Page *BufferPoolManager::FetchPage(PageId page_id) {
    std::scoped_lock lock{latch_};
    
    // 1. 检查页表
    if (page_table_.count(page_id)) {
        frame_id_t frame_id = page_table_[page_id];
        pages_[frame_id].pin_count_++;
        replacer_->Pin(frame_id);
        return &pages_[frame_id];
    }
    
    // 2. 未找到则寻找可替换页
    frame_id_t frame_id;
    if (!FindVictimPage(&frame_id)) {
        return nullptr;
    }
    
    // 3. 更新页面
    UpdatePage(&pages_[frame_id], page_id, frame_id);
    return &pages_[frame_id];
}

/**
 * 取消固定页面
 * @param page_id 页面ID
 * @param is_dirty 是否标记为脏页
 * @return 操作成功返回true，失败返回false
 */
bool BufferPoolManager::UnpinPage(PageId page_id, bool is_dirty) {
    std::scoped_lock lock{latch_};
    
    if (!page_table_.count(page_id)) {
        return false;
    }
    
    frame_id_t frame_id = page_table_[page_id];
    Page &page = pages_[frame_id];
    
    if (page.pin_count_ <= 0) {
        return false;
    }
    
    page.pin_count_--;
    page.is_dirty_ = is_dirty;
    
    if (page.pin_count_ == 0) {
        replacer_->Unpin(frame_id);
    }
    
    return true;
}

/**
 * 将页面刷新到磁盘
 * @param page_id 页面ID
 * @return 操作成功返回true，失败返回false
 */
bool BufferPoolManager::FlushPage(PageId page_id) {
    std::scoped_lock lock{latch_};
    
    if (!page_table_.count(page_id)) {
        return false;
    }
    
    frame_id_t frame_id = page_table_[page_id];
    Page &page = pages_[frame_id];
    
    disk_manager_->write_page(page.id_.fd, page.id_.page_no, page.GetData(), PAGE_SIZE);
    page.is_dirty_ = false;
    
    return true;
}

/**
 * 创建新页面
 * @param page_id 输出参数，存储新页面ID
 * @return 新页面指针，失败时返回nullptr
 */
Page *BufferPoolManager::NewPage(PageId *page_id) {
    std::scoped_lock lock{latch_};
    
    // 1. 寻找可替换页
    frame_id_t frame_id;
    if (!FindVictimPage(&frame_id)) {
        page_id->page_no = INVALID_PAGE_ID;
        return nullptr;
    }
    
    Page &page = pages_[frame_id];
    
    // 2. 若页面脏，写回磁盘
    if (page.is_dirty_) {
        disk_manager_->write_page(page.id_.fd, page.id_.page_no, page.GetData(), PAGE_SIZE);
        page.is_dirty_ = false;
        page.pin_count_ = 0;
    }
    
    // 3. 分配新页
    page_id->page_no = disk_manager_->AllocatePage(page_id->fd);
    
    // 4. 更新页表
    page_table_.erase(page.id_);
    page_table_[*page_id] = frame_id;
    
    // 5. 重置页面状态
    page.ResetMemory();
    page.id_ = *page_id;
    page.pin_count_ = 1;
    page.is_dirty_ = false;
    
    replacer_->Pin(frame_id);
    return &page;
}

/**
 * 删除页面
 * @param page_id 页面ID
 * @return 操作成功返回true，失败返回false
 */
bool BufferPoolManager::DeletePage(PageId page_id) {
    std::scoped_lock lock{latch_};
    
    // 1. 检查页表
    if (!page_table_.count(page_id)) {
        return true;
    }
    
    frame_id_t frame_id = page_table_[page_id];
    Page &page = pages_[frame_id];
    
    // 2. 检查pin count
    if (page.pin_count_ > 0) {
        return false;
    }
    
    // 3. 释放页面
    disk_manager_->DeallocatePage(page_id.page_no);
    page_table_.erase(page_id);
    
    // 4. 重置页面状态并加入free list
    page.id_.page_no = INVALID_PAGE_ID;
    page.pin_count_ = 0;
    page.is_dirty_ = false;
    
    free_list_.push_back(frame_id);
    return true;
}

/**
 * 刷新指定文件的所有页面到磁盘
 * @param fd 文件描述符
 */
void BufferPoolManager::FlushAllPages(int fd) {
    std::scoped_lock lock{latch_};
    
    for (size_t i = 0; i < pool_size_; i++) {
        Page *page = &pages_[i];
        
        if (page->GetPageId().fd == fd && 
            page->GetPageId().page_no != INVALID_PAGE_ID) {
            
            disk_manager_->write_page(
                page->GetPageId().fd, 
                page->GetPageId().page_no, 
                page->GetData(), 
                PAGE_SIZE
            );
            
            page->is_dirty_ = false;
        }
    }
}