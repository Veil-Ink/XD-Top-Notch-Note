#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化扫描器，定位首个有效记录位置
 * @param file_handle 记录文件句柄
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    RmFileHdr file_hdr = file_handle_->file_hdr_;  // 获取文件头元数据
    int max_records = file_hdr.num_records_per_page;  // 每页最大记录数

    if (file_hdr.num_pages == 1) {  // 文件只有1页时，初始位置设为无效槽位
        rid_.slot_no = max_records;
        rid_.page_no = 0;
    } else {  // 多页时，从第1页开始找首个有效槽位
        RmPageHandle page_handle = file_handle_->fetch_page_handle(1);
        rid_.slot_no = Bitmap::first_bit(1, page_handle.bitmap, max_records);
        rid_.page_no = 1;
    }
}

/**
 * @brief 定位下一个有效记录位置
 */
void RmScan::next() {
    int curr_page_no = rid_.page_no;  // 当前扫描页号
    RmPageHandle page_handle = file_handle_->fetch_page_handle(curr_page_no);  // 获取当前页句柄
    RmFileHdr file_hdr = file_handle_->file_hdr_;  // 文件头元数据
    int max_records = file_hdr.num_records_per_page;  // 每页最大记录数

    // 查找当前页内下一个有效槽位
    int next_slot = Bitmap::next_bit(1, page_handle.bitmap, max_records, rid_.slot_no);
    int next_page = curr_page_no;  // 初始化为当前页

    if (next_slot == max_records) {  // 当前页无后续有效槽位，遍历后续页
        for (int i = curr_page_no + 1; i < file_hdr.num_pages; ++i) {
            RmPageHandle curr_page = file_handle_->fetch_page_handle(i);
            next_slot = Bitmap::first_bit(1, curr_page.bitmap, max_records);
            if (next_slot != max_records) {  // 找到有效槽位，更新页号
                next_page = i;
                break;
            }
        }
    }

    rid_.slot_no = next_slot;  // 更新槽位号
    rid_.page_no = next_page;  // 更新页号
}

/**
 * @brief 判断扫描是否到达文件末尾（槽位号等于每页最大记录数）
 */
bool RmScan::is_end() const {
    RmFileHdr file_hdr = file_handle_->file_hdr_;
    return rid_.slot_no == file_hdr.num_records_per_page;
}

/**
 * @brief 获取当前扫描的记录位置（Rid）
 */
Rid RmScan::rid() const {
    return rid_;
}