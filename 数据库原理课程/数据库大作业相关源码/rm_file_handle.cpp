#include "rm_file_handle.h"
#define DEBUG 0
int ONCE = 0;

/**
 * 从指定位置获取记录
 * @param rid 记录位置
 * @param context 操作上下文
 * @return 记录的智能指针，若记录不存在则data为nullptr
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid &rid, Context *context) const {
    if (RmFileHandle::is_record(rid)) {
        RmPageHandle page_handle = fetch_page_handle(rid.page_no);
        return std::unique_ptr<RmRecord>(new RmRecord({file_hdr_.record_size, page_handle.get_slot(rid.slot_no)}));
    }
    return std::unique_ptr<RmRecord>(new RmRecord({file_hdr_.record_size, nullptr}));
}

/**
 * 插入新记录
 * @param buf 记录数据
 * @param context 操作上下文
 * @return 插入后的记录位置
 */
Rid RmFileHandle::insert_record(char *buf, Context *context) {
    if (!(--ONCE)) std::cout << "max_record_size:" << file_hdr_.num_records_per_page << std::endl;
    
    RmPageHandle page_handle = create_page_handle();
    int new_slot_no = Bitmap::first_bit(0, page_handle.bitmap, file_hdr_.num_records_per_page);
    
    Bitmap::set(page_handle.bitmap, new_slot_no);
    memcpy(page_handle.get_slot(new_slot_no), buf, file_hdr_.record_size);
    page_handle.page_hdr->num_records++;
    
    if (DEBUG) std::cout << "insert_record: " << page_handle.page_hdr->num_records 
                         << " in " << page_handle.page->GetPageId().page_no << std::endl;
    
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
    }
    
    buffer_pool_manager_->UnpinPage(page_handle.page->GetPageId(), true);
    
    if (DEBUG) std::cout << "here: " << new_slot_no << " in " 
                         << page_handle.page->GetPageId().page_no << std::endl;
    
    return Rid{page_handle.page->GetPageId().page_no, new_slot_no};
}

/**
 * 删除指定位置的记录
 * @param rid 记录位置
 * @param context 操作上下文
 */
void RmFileHandle::delete_record(const Rid &rid, Context *context) {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records--;
    
    if (DEBUG) std::cout << "del: " << page_handle.page->GetPageId().page_no << std::endl;
    
    if (page_handle.page_hdr->num_records + 1 == file_hdr_.num_records_per_page) {
        release_page_handle(page_handle);
    }
    
    buffer_pool_manager_->UnpinPage(page_handle.page->GetPageId(), true);
}

/**
 * 更新指定位置的记录
 * @param rid 记录位置
 * @param buf 新记录数据
 * @param context 操作上下文
 */
void RmFileHandle::update_record(const Rid &rid, char *buf, Context *context) {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    
    if (DEBUG) std::cout << "update:" << rid.slot_no << " in " << rid.page_no << std::endl;
    
    buffer_pool_manager_->UnpinPage(page_handle.page->GetPageId(), true);
}

/** -- 以下为辅助函数 -- */

/**
 * 获取指定页号的页面句柄
 * @param page_no 页号
 * @return 页面句柄（需外部unpin）
 * @throws PageNotExistError 页号无效时抛出
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    if (page_no == INVALID_PAGE_ID) throw(PageNotExistError("hhh", page_no));
    
    Page* page = buffer_pool_manager_->FetchPage(PageId{fd_, page_no});
    // if(DEBUG) std::cout<<"fetch: "<<page->GetPageId().page_no<<std::endl;
    
    return RmPageHandle(&file_hdr_, page);
}

/**
 * 创建新页面句柄
 * @return 新页面句柄
 */
RmPageHandle RmFileHandle::create_new_page_handle() {
    Page* new_page = buffer_pool_manager_->NewPage(new PageId({fd_}));
    RmPageHandle new_page_handle = RmPageHandle(&file_hdr_, new_page);
    
    new_page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = new_page->GetPageId().page_no;
    
    if (DEBUG) std::cout << "create: " << file_hdr_.first_free_page_no 
                         << "->" << new_page_handle.page_hdr->next_free_page_no << std::endl;
    
    new_page_handle.page_hdr->num_records = 0;
    file_hdr_.num_pages++;
    
    Bitmap::init(new_page_handle.bitmap, file_hdr_.bitmap_size);
    
    return new_page_handle;
}

/**
 * 创建或获取空闲页面句柄
 * @return 空闲页面句柄（需外部unpin）
 */
RmPageHandle RmFileHandle::create_page_handle() {
    if (file_hdr_.first_free_page_no == -1) {
        return create_new_page_handle();
    }
    return fetch_page_handle(file_hdr_.first_free_page_no);
}

/**
 * 将页面标记为可用（从满变为未满时调用）
 * @param page_handle 页面句柄
 * @note 仅用于delete_record()
 */
void RmFileHandle::release_page_handle(RmPageHandle &page_handle) {
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_handle.page->GetPageId().page_no;
    
    if (DEBUG) std::cout << "release: " << file_hdr_.first_free_page_no << std::endl;
}

/**
 * 恢复专用：在指定位置插入记录
 * @param rid 目标位置
 * @param buf 记录数据
 */
void RmFileHandle::insert_record(const Rid &rid, char *buf) {
    if (rid.page_no < file_hdr_.num_pages) {
        create_new_page_handle();
    }
    
    RmPageHandle pageHandle = fetch_page_handle(rid.page_no);
    
    Bitmap::set(pageHandle.bitmap, rid.slot_no);
    pageHandle.page_hdr->num_records++;
    
    if (pageHandle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = pageHandle.page_hdr->next_free_page_no;
    }

    char *slot = pageHandle.get_slot(rid.slot_no);
    memcpy(slot, buf, file_hdr_.record_size);

    buffer_pool_manager_->UnpinPage(pageHandle.page->GetPageId(), true);
}