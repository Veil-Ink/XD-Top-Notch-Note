#include "storage/disk_manager.h"
#include <assert.h>     
#include <string.h>     
#include <sys/stat.h>   
#include <unistd.h>     
#include <fcntl.h>      
#include "defs.h"      

// 构造函数：正确初始化原子数组
DiskManager::DiskManager() {
    for (int i = 0; i < MAX_FD; ++i) {
        fd2pageno_[i].store(0, std::memory_order_relaxed); // 原子操作初始化，避免memset破坏
    }
}

// 写页到磁盘：检查lseek和write返回值，处理异常
void DiskManager::write_page(int fd, page_id_t page_no, const char *offset, int num_bytes) {
    if (page_no < 0) return; // 非法页号检查
    
    int off = page_no * PAGE_SIZE;
    int curpos = lseek(fd, off, SEEK_SET);
    if (curpos == -1) throw UnixError(); // 定位失败
    
    ssize_t bytes_written = write(fd, offset, num_bytes);
    if (bytes_written != num_bytes) throw UnixError(); // 写失败
}

// 从磁盘读页：检查lseek和read返回值，处理异常
void DiskManager::read_page(int fd, page_id_t page_no, char *offset, int num_bytes) {
    if (page_no < 0) return; // 非法页号检查
    
    int off = page_no * PAGE_SIZE;
    int curpos = lseek(fd, off, SEEK_SET);
    if (curpos == -1) throw UnixError(); // 定位失败
    
    ssize_t bytes_read = read(fd, offset, num_bytes);
    if (bytes_read != num_bytes) throw UnixError(); // 读失败
}

// 分配页：简单自增策略（需结合头页 bitmap 完善，此处保持逻辑）
page_id_t DiskManager::AllocatePage(int fd) {
    return fd2pageno_[fd]++; // 原子自增，保证线程安全
}

// 释放页：占位实现（需后续通过头页 bitmap 完善）
void DiskManager::DeallocatePage(__attribute__((unused)) page_id_t page_id) {}

// 判断是否为目录
bool DiskManager::is_dir(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// 创建目录（调用系统命令，需处理异常）
void DiskManager::create_dir(const std::string &path) {
    std::string cmd = "mkdir " + path;
    if (system(cmd.c_str()) < 0) throw UnixError();
}

// 删除目录（递归，调用系统命令，需处理异常）
void DiskManager::destroy_dir(const std::string &path) {
    std::string cmd = "rm -r " + path;
    if (system(cmd.c_str()) < 0) throw UnixError();
}

// 判断是否为文件（修复内存泄漏，改用栈变量）
bool DiskManager::is_file(const std::string &path) {
    struct stat file_stat; // 栈变量，避免malloc泄漏
    int flag = stat(path.c_str(), &file_stat);
    return (flag == 0) && S_ISREG(file_stat.st_mode); // 正则文件判断
}

// 创建文件（检查重复，处理open异常）
void DiskManager::create_file(const std::string &path) {
    if (is_file(path)) throw FileExistsError(path); // 已存在则抛异常
    
    int fd = open(path.c_str(), O_CREAT, 0666); // 读写权限
    if (fd == -1) throw FileNotOpenError(fd);    // 打开失败
    
    close(fd); // 创建后关闭，后续通过open_file打开
}

// 删除文件（检查存在性、关闭状态，处理unlink异常）
void DiskManager::destroy_file(const std::string &path) {
    if (!is_file(path)) throw FileNotFoundError(path);   // 不存在则抛异常
    if (path2fd_.count(path)) throw FileNotClosedError(path); // 未关闭则抛异常
    
    int ret = unlink(path.c_str());
    if (ret == -1) throw UnixError(); // 删除失败
}

// 打开文件（检查重复，更新映射，处理open异常）
int DiskManager::open_file(const std::string &path) {
    if (!is_file(path)) throw FileNotFoundError(path);    // 不存在则抛异常
    if (path2fd_.count(path)) throw FileNotClosedError(path); // 已打开则抛异常
    
    int fd = open(path.c_str(), O_RDWR);
    if (fd == -1) throw FileNotOpenError(fd);             // 打开失败
    
    fd2path_[fd] = path;  // 记录文件描述符->路径映射
    path2fd_[path] = fd;  // 记录路径->文件描述符映射
    return fd;
}

// 关闭文件（检查状态，更新映射，处理close异常）
void DiskManager::close_file(int fd) {
    if (!fd2path_.count(fd)) throw FileNotOpenError(fd); // 未打开则抛异常
    
    std::string path = fd2path_[fd];
    fd2path_.erase(fd);    // 移除文件描述符映射
    path2fd_.erase(path);  // 移除路径映射（原代码错误：path.c_str()改为path）
    
    if (close(fd) == -1) throw UnixError(); // 关闭失败
}

// 获取文件大小
int DiskManager::GetFileSize(const std::string &file_name) {
    struct stat stat_buf;
    int rc = stat(file_name.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

// 通过文件描述符获取文件名
std::string DiskManager::GetFileName(int fd) {
    if (!fd2path_.count(fd)) throw FileNotOpenError(fd);
    return fd2path_[fd];
}

// 通过路径获取文件描述符（不存在则打开）
int DiskManager::GetFileFd(const std::string &file_name) {
    if (path2fd_.count(file_name)) return path2fd_[file_name];
    return open_file(file_name); // 不存在则尝试打开
}

// 读取日志（从指定偏移开始，处理文件边界）
bool DiskManager::ReadLog(char *log_data, int size, int offset, int prev_log_end) {
    if (log_fd_ == -1) log_fd_ = open_file(LOG_FILE_NAME); // 延迟打开日志文件
    
    offset += prev_log_end;
    int file_size = GetFileSize(LOG_FILE_NAME);
    if (offset >= file_size) return false; // 无新日志
    
    size = std::min(size, file_size - offset);
    lseek(log_fd_, offset, SEEK_SET);
    
    ssize_t bytes_read = read(log_fd_, log_data, size);
    if (bytes_read != size) throw UnixError();
    return true;
}

// 写入日志（追加到文件末尾，处理写失败）
void DiskManager::WriteLog(char *log_data, int size) {
    if (log_fd_ == -1) log_fd_ = open_file(LOG_FILE_NAME); // 延迟打开日志文件
    
    lseek(log_fd_, 0, SEEK_END); // 定位到文件末尾
    ssize_t bytes_write = write(log_fd_, log_data, size);
    if (bytes_write != size) throw UnixError();
}
