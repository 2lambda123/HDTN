/**
 * @file MemoryInFiles.cpp
 * @author  Brian Tomko <brian.j.tomko@nasa.gov>
 *
 * @copyright Copyright © 2021 United States Government as represented by
 * the National Aeronautics and Space Administration.
 * No copyright is claimed in the United States under Title 17, U.S.Code.
 * All Other Rights Reserved.
 *
 * @section LICENSE
 * Released under the NASA Open Source Agreement (NOSA)
 * See LICENSE.md in the source root directory for more information.
 */

#ifndef _WIN32
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#endif

#include "MemoryInFiles.h"
#include "FragmentSet.h"
#include <boost/thread.hpp>
#include <memory>
#include <unordered_map>
#include <vector>
#include <queue>
#include <forward_list>
#include "Logger.h"
#include <boost/make_unique.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/format.hpp>
#ifdef _WIN32
#include <windows.h> //must be included after boost
#endif

static constexpr hdtn::Logger::SubProcess subprocess = hdtn::Logger::SubProcess::none;

static constexpr uint64_t BLOCK_SIZE_MULTIPLE = 4096;
static constexpr uint64_t BLOCK_SIZE_MULTIPLE_MASK = BLOCK_SIZE_MULTIPLE - 1;
static constexpr uint64_t BLOCK_SIZE_MULTIPLE_SHIFT = 12; //(1<<12) == 4096
uint64_t MemoryInFiles::CeilToNearestBlockMultiple(const uint64_t minimumDesiredBytes) {
    //const uint64_t totalBlocksRequired = (minimumDesiredBytes / BLOCK_SIZE_MULTIPLE) + ((minimumDesiredBytes % BLOCK_SIZE_MULTIPLE) == 0 ? 0 : 1);
    const bool addOne = ((minimumDesiredBytes & BLOCK_SIZE_MULTIPLE_MASK) != 0); //((minimumDesiredBytes % BLOCK_SIZE_MULTIPLE) == 0 ? 0 : 1);
    const uint64_t totalBlocksRequired = (minimumDesiredBytes >> BLOCK_SIZE_MULTIPLE_SHIFT) + addOne;
    const uint64_t allocationSize = (totalBlocksRequired << BLOCK_SIZE_MULTIPLE_SHIFT);
    return allocationSize;
}

struct MemoryInFiles::Impl : private boost::noncopyable {

    

    Impl() = delete;
    Impl(boost::asio::io_service& ioServiceRef,
        const boost::filesystem::path& workingDirectory,
        const uint64_t newFileAggregationTimeMs,
        const uint64_t estimatedMaxAllocatedBlocks);
    ~Impl();
    void Stop();
    uint64_t AllocateNewWriteMemoryBlock(uint64_t totalSize);
    uint64_t Resize(const uint64_t memoryBlockId, uint64_t newSize); //returns the new size
    uint64_t GetSizeOfMemoryBlock(const uint64_t memoryBlockId) const noexcept;
    void ForceDeleteMemoryBlock(const uint64_t memoryBlockId); //intended only for boost::asio::post calls to defer delete when io operations in progress
    bool DeleteMemoryBlock(const uint64_t memoryBlockId);
    bool WriteMemoryAsync(const deferred_write_t& deferredWrite, std::shared_ptr<write_memory_handler_t>& handlerPtr);
    bool WriteMemoryAsync(const std::vector<deferred_write_t>& deferredWritesVec, std::shared_ptr<write_memory_handler_t>& handlerPtr);
    bool ReadMemoryAsync(const deferred_read_t& deferredRead, std::shared_ptr<read_memory_handler_t>& handlerPtr);
    bool ReadMemoryAsync(const std::vector<deferred_read_t>& deferredReadsVec, std::shared_ptr<read_memory_handler_t>& handlerPtr);

private:
    struct MemoryBlockInfo;
    uint64_t Resize(MemoryBlockInfo& mbi, uint64_t newSize); //returns the new size
    bool SetupNextFileIfNeeded();

#ifdef _WIN32
    typedef boost::asio::windows::random_access_handle file_handle_t;
#else
    typedef boost::asio::posix::stream_descriptor file_handle_t;
#endif

    

    struct io_operation_t : private boost::noncopyable {
        io_operation_t() = delete;
        io_operation_t(
            MemoryBlockInfo& memoryBlockInfoRef,
            uint64_t offsetWithinFile,
            uint64_t length,
            const void* writeLocationPtr,
            std::shared_ptr<write_memory_handler_t>& writeHandlerPtr);

        io_operation_t(
            MemoryBlockInfo& memoryBlockInfoRef,
            std::shared_ptr<read_memory_handler_t>& readHandlerPtr,
            uint64_t offsetWithinFile,
            uint64_t length,
            void* readLocationPtr);

        MemoryBlockInfo& m_memoryBlockInfoRef; //references to unordered_map never get invalidated, only iterators
        uint64_t m_offsetWithinFile;
        uint64_t m_length;
        void* m_readToThisLocationPtr;
        const void* m_writeFromThisLocationPtr;
        std::shared_ptr<read_memory_handler_t> m_readHandlerPtr;
        std::shared_ptr<write_memory_handler_t> m_writeHandlerPtr;
    };

    struct FileInfo : private boost::noncopyable {
        FileInfo() = delete;
        FileInfo(const boost::filesystem::path& filePath, boost::asio::io_service & ioServiceRef, MemoryInFiles::Impl& implRef);
        ~FileInfo();
        bool WriteMemoryAsync(MemoryBlockInfo& memoryBlockInfoRef, const uint64_t offsetWithinFile, const void* data, uint64_t length, std::shared_ptr<write_memory_handler_t>& handlerPtr);
        bool ReadMemoryAsync(MemoryBlockInfo& memoryBlockInfoRef, const uint64_t offsetWithinFile, void* data, uint64_t length, std::shared_ptr<read_memory_handler_t>& handlerPtr);
    private:
        void HandleDiskWriteCompleted(const boost::system::error_code& error, std::size_t bytes_transferred);
        void HandleDiskReadCompleted(const boost::system::error_code& error, std::size_t bytes_transferred);
        void FinishCompletionHandler(io_operation_t& op);
        void TryStartNextQueuedIoOperation();
        
        
        std::queue<io_operation_t> m_queueIoOperations;
        std::unique_ptr<file_handle_t> m_fileHandlePtr;
        boost::filesystem::path m_filePath;
        MemoryInFiles::Impl& m_implRef;
        bool m_diskOperationInProgress;
        bool m_valid;
    };

    struct MemoryBlockFragmentInfo : private boost::noncopyable {
        MemoryBlockFragmentInfo() = delete;
        MemoryBlockFragmentInfo(const std::shared_ptr<FileInfo>& fileInfoPtr, const uint64_t baseOffsetWithinFile, const uint64_t lengthAlignedToBlockSize);
        std::shared_ptr<FileInfo> m_fileInfoPtr;
        const uint64_t m_baseOffsetWithinFile;
        const uint64_t m_length;
    };

    struct MemoryBlockInfo : private boost::noncopyable {
        MemoryBlockInfo() = delete;
        MemoryBlockInfo(const uint64_t memoryBlockId);
        uint64_t Resize(const std::shared_ptr<FileInfo>& currentFileInfoPtr, //returns the increase in size
            const uint64_t currentBaseOffsetWithinFile, const uint64_t newLengthAlignedToBlockSize);
        bool DoWriteOrRead(const uint64_t deferredOffset, const uint64_t deferredLength,
            void* readToThisLocationPtr, const void* writeFromThisLocationPtr, void* handlerPtrPtr);
        const uint64_t m_memoryBlockId;
        typedef std::forward_list<MemoryBlockFragmentInfo> fragment_flist_t;
        fragment_flist_t m_memoryBlockFragmentInfoFlist;
        fragment_flist_t::iterator m_memoryBlockFragmentInfoFlistLastIterator;
        uint64_t m_lengthAlignedToBlockSize;
        unsigned int m_queuedOperationsReferenceCount;
        bool m_markedForDeletion;
    };

    void TryRestartNewFileAggregationTimer();
    void OnNewFileAggregation_TimerExpired(const boost::system::error_code& e);

    typedef std::unordered_map<uint64_t, MemoryBlockInfo> id_to_memoryblockinfo_map_t;

    


    id_to_memoryblockinfo_map_t m_mapIdToMemoryBlockInfo;
    std::shared_ptr<FileInfo> m_currentWritingFileInfoPtr;

    boost::asio::io_service& m_ioServiceRef;
    boost::asio::deadline_timer m_newFileAggregationTimer;
    const boost::filesystem::path m_rootStorageDirectory;
    const uint64_t m_newFileAggregationTimeMs;
    const boost::posix_time::time_duration m_newFileAggregationTimeDuration;
    bool m_newFileAggregationTimerIsRunning;
    uint64_t m_nextMemoryBlockId;
    uint64_t m_nextFileId;
    uint64_t m_nextOffsetOfCurrentFile;
public: //stats
    uint64_t m_countTotalFilesCreated;
    uint64_t m_countTotalFilesActive;
};

MemoryInFiles::Impl::io_operation_t::io_operation_t(
    MemoryBlockInfo& memoryBlockInfoRef,
    uint64_t offsetWithinFile,
    uint64_t length,
    const void* writeLocationPtr,
    std::shared_ptr<write_memory_handler_t>& writeHandlerPtr) :
    ///
    m_memoryBlockInfoRef(memoryBlockInfoRef),
    m_offsetWithinFile(offsetWithinFile),
    m_length(length),
    m_readToThisLocationPtr(NULL),
    m_writeFromThisLocationPtr(writeLocationPtr),
    m_writeHandlerPtr(writeHandlerPtr) {}

MemoryInFiles::Impl::io_operation_t::io_operation_t(
    MemoryBlockInfo& memoryBlockInfoRef,
    std::shared_ptr<read_memory_handler_t>& readHandlerPtr,
    uint64_t offsetWithinFile,
    uint64_t length,
    void* readLocationPtr) :
    ///
    m_memoryBlockInfoRef(memoryBlockInfoRef),
    m_offsetWithinFile(offsetWithinFile),
    m_length(length),
    m_readToThisLocationPtr(readLocationPtr),
    m_writeFromThisLocationPtr(NULL),
    m_readHandlerPtr(readHandlerPtr) {}

MemoryInFiles::Impl::FileInfo::FileInfo(const boost::filesystem::path& filePath, boost::asio::io_service& ioServiceRef, MemoryInFiles::Impl& implRef) :
    m_filePath(filePath),
    m_implRef(implRef),
    m_diskOperationInProgress(false),
    m_valid(false)
{
    const boost::filesystem::path::value_type* filePathCstr = m_filePath.c_str();
#ifdef _WIN32
    //
    //https://docs.microsoft.com/en-us/windows/win32/fileio/synchronous-and-asynchronous-i-o
    //In synchronous file I/O, a thread starts an I/O operation and immediately enters a wait state until the I/O request has completed.
    //A thread performing asynchronous file I/O sends an I/O request to the kernel by calling an appropriate function.
    //If the request is accepted by the kernel, the calling thread continues processing another job until the kernel signals to
    //the thread that the I/O operation is complete. It then interrupts its current job and processes the data from the I/O operation as necessary.
    //Asynchronous I/O is also referred to as overlapped I/O.
    HANDLE hFile = CreateFileW(filePathCstr,                // name of the file
        GENERIC_READ | GENERIC_WRITE,          // open for reading and writing
        0,                      // do not share
        NULL,                   // default security
        //CREATE_ALWAYS : Creates a new file, always. If the specified file exists and is writable, the function overwrites the file
        //OPEN_EXISTING : Opens a file or device, only if it exists.  If the specified file or device does not exist, the function fails and the last - error code is set to ERROR_FILE_NOT_FOUND(2).
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,  // normal file
        NULL);                  // no attr. template

    if (hFile == INVALID_HANDLE_VALUE) {
        LOG_ERROR(subprocess) << "error opening " << m_filePath;
        return;
    }
    
#else
    int hFile = open(filePathCstr, (O_CREAT | O_RDWR | O_TRUNC | O_LARGEFILE), DEFFILEMODE);
    if (hFile < 0) {
        LOG_ERROR(subprocess) << "error opening " << m_filePath;
        return;
    }
#endif
    m_fileHandlePtr = boost::make_unique<file_handle_t>(ioServiceRef, hFile);
    m_valid = true;
}
MemoryInFiles::Impl::FileInfo::~FileInfo() {
    //last shared_ptr to delete calls this destructor
    --m_implRef.m_countTotalFilesActive;
    if (m_fileHandlePtr) {
        m_fileHandlePtr->close();
        m_fileHandlePtr.reset(); //delete it
    }
    if (!boost::filesystem::remove(m_filePath)) {
        LOG_ERROR(subprocess) << "error deleting file " << m_filePath;
    }
}
void MemoryInFiles::Impl::FileInfo::TryStartNextQueuedIoOperation() {
    if (m_queueIoOperations.size() && (!m_diskOperationInProgress)) {
        io_operation_t& op = m_queueIoOperations.front();
        m_diskOperationInProgress = true;
        if (op.m_readToThisLocationPtr) {
#ifdef _WIN32
            boost::asio::async_read_at(*m_fileHandlePtr, op.m_offsetWithinFile,
#else
            lseek64(m_fileHandlePtr->native_handle(), op.m_offsetWithinFile, SEEK_SET);
            boost::asio::async_read(*m_fileHandlePtr,
#endif
                boost::asio::buffer(op.m_readToThisLocationPtr, op.m_length),
                boost::bind(&MemoryInFiles::Impl::FileInfo::HandleDiskReadCompleted, this,
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::bytes_transferred));
        }
        else { //write operation
#ifdef _WIN32
            boost::asio::async_write_at(*m_fileHandlePtr, op.m_offsetWithinFile,
#else
            lseek64(m_fileHandlePtr->native_handle(), op.m_offsetWithinFile, SEEK_SET);
            boost::asio::async_write(*m_fileHandlePtr,
#endif
                boost::asio::const_buffer(op.m_writeFromThisLocationPtr, op.m_length),
                boost::bind(&MemoryInFiles::Impl::FileInfo::HandleDiskWriteCompleted, this,
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::bytes_transferred));
        }
    }
}
bool MemoryInFiles::Impl::FileInfo::WriteMemoryAsync(MemoryBlockInfo& memoryBlockInfoRef, const uint64_t offsetWithinFile, const void* data, uint64_t length, std::shared_ptr<write_memory_handler_t>& handlerPtr) {
    if (m_valid) {
        m_queueIoOperations.emplace(memoryBlockInfoRef, offsetWithinFile, length, data, handlerPtr);
        ++memoryBlockInfoRef.m_queuedOperationsReferenceCount;
        TryStartNextQueuedIoOperation();
    }
    return m_valid;
}
void MemoryInFiles::Impl::FileInfo::HandleDiskWriteCompleted(const boost::system::error_code& error, std::size_t bytes_transferred) {
    io_operation_t& op = m_queueIoOperations.front();
    if (error) {
        LOG_ERROR(subprocess) << "HandleDiskWriteCompleted: " << error.message();
    }
    else {
        if ((op.m_writeHandlerPtr) && (op.m_writeHandlerPtr.use_count() == 1)) { //only the last reference to a handler should be called to complete a multi-operation
            write_memory_handler_t& handler = *op.m_writeHandlerPtr;
            if (handler) {
                handler();
            }
        }
    }
    FinishCompletionHandler(op);
}

bool MemoryInFiles::Impl::FileInfo::ReadMemoryAsync(MemoryBlockInfo& memoryBlockInfoRef, const uint64_t offsetWithinFile, void* data, uint64_t length, std::shared_ptr<read_memory_handler_t>& handlerPtr) {
    if (m_valid) {
        m_queueIoOperations.emplace(memoryBlockInfoRef, handlerPtr, offsetWithinFile, length, data);
        ++memoryBlockInfoRef.m_queuedOperationsReferenceCount;
        TryStartNextQueuedIoOperation();
    }
    return m_valid;
}
void MemoryInFiles::Impl::FileInfo::HandleDiskReadCompleted(const boost::system::error_code& error, std::size_t bytes_transferred) {
    io_operation_t& op = m_queueIoOperations.front();
    bool success = true;
    if (error) {
        LOG_ERROR(subprocess) << "HandleDiskReadCompleted: " << error.message();
        success = false;
    }
    if ((op.m_readHandlerPtr) && (op.m_readHandlerPtr.use_count() == 1)) { //only the last reference to a handler should be called to complete a multi-operation
        read_memory_handler_t& handler = *op.m_readHandlerPtr;
        if (handler) {
            handler(success);
        }
    }
    FinishCompletionHandler(op);
}
void MemoryInFiles::Impl::FileInfo::FinishCompletionHandler(io_operation_t& op) {
    --op.m_memoryBlockInfoRef.m_queuedOperationsReferenceCount;
    if (op.m_memoryBlockInfoRef.m_markedForDeletion && (op.m_memoryBlockInfoRef.m_queuedOperationsReferenceCount == 0)) {
        //don't potentially delete "this" (FileInfo) while in a FileInfo function when deleting a memoryBlockInfo which has a shared_ptr to the FileInfo
        boost::asio::post(m_implRef.m_ioServiceRef, boost::bind(&MemoryInFiles::Impl::ForceDeleteMemoryBlock, &m_implRef, op.m_memoryBlockInfoRef.m_memoryBlockId));
    }
    m_queueIoOperations.pop();
    m_diskOperationInProgress = false;
    TryStartNextQueuedIoOperation();
}

//restarts the token refresh timer if it is not running from now
void MemoryInFiles::Impl::TryRestartNewFileAggregationTimer() {
    if (!m_newFileAggregationTimerIsRunning) {
        const boost::posix_time::ptime nowPtime = boost::posix_time::microsec_clock::universal_time();
        m_newFileAggregationTimer.expires_at(nowPtime + m_newFileAggregationTimeDuration);
        m_newFileAggregationTimer.async_wait(boost::bind(&MemoryInFiles::Impl::OnNewFileAggregation_TimerExpired, this, boost::asio::placeholders::error));
        m_newFileAggregationTimerIsRunning = true;
    }
}
void MemoryInFiles::Impl::OnNewFileAggregation_TimerExpired(const boost::system::error_code& e) {
    if (e != boost::asio::error::operation_aborted) {
        // Timer was not cancelled, take necessary action.
        m_currentWritingFileInfoPtr.reset();
        m_newFileAggregationTimerIsRunning = false;
    }
}

MemoryInFiles::Impl::MemoryBlockFragmentInfo::MemoryBlockFragmentInfo(
    const std::shared_ptr<FileInfo>& fileInfoPtr,
    const uint64_t baseOffsetWithinFile,
    const uint64_t lengthAlignedToBlockSize) :
    //
    m_fileInfoPtr(fileInfoPtr),
    m_baseOffsetWithinFile(baseOffsetWithinFile),
    m_length(lengthAlignedToBlockSize) {}


MemoryInFiles::Impl::MemoryBlockInfo::MemoryBlockInfo(const uint64_t memoryBlockId) :
    m_memoryBlockId(memoryBlockId),
    m_lengthAlignedToBlockSize(0),
    m_queuedOperationsReferenceCount(0),
    m_markedForDeletion(false)
{

}

//returns the increase in size
uint64_t MemoryInFiles::Impl::MemoryBlockInfo::Resize(const std::shared_ptr<FileInfo>& currentFileInfoPtr,
    const uint64_t currentBaseOffsetWithinFile, const uint64_t newLengthAlignedToBlockSize)
{
    if (newLengthAlignedToBlockSize <= m_lengthAlignedToBlockSize) {
        return 0; //0-byte increase in file size
    }
    const uint64_t diffSize = newLengthAlignedToBlockSize - m_lengthAlignedToBlockSize;
    m_lengthAlignedToBlockSize = newLengthAlignedToBlockSize;

    //insert into list (order by FIFO, so newest elements will be last)
    if (m_memoryBlockFragmentInfoFlist.empty()) {
        m_memoryBlockFragmentInfoFlist.emplace_front(currentFileInfoPtr, currentBaseOffsetWithinFile, diffSize);
        m_memoryBlockFragmentInfoFlistLastIterator = m_memoryBlockFragmentInfoFlist.begin();
    }
    else {
        m_memoryBlockFragmentInfoFlistLastIterator = m_memoryBlockFragmentInfoFlist.emplace_after(
            m_memoryBlockFragmentInfoFlistLastIterator,
            currentFileInfoPtr, currentBaseOffsetWithinFile, diffSize);
    }
    return diffSize;
}

MemoryInFiles::Impl::Impl(boost::asio::io_service& ioServiceRef,
    const boost::filesystem::path& workingDirectory,
    const uint64_t newFileAggregationTimeMs, const uint64_t estimatedMaxAllocatedBlocks) :
    m_ioServiceRef(ioServiceRef),
    m_newFileAggregationTimer(ioServiceRef),
    m_rootStorageDirectory(workingDirectory / boost::filesystem::unique_path()),
    m_newFileAggregationTimeMs(newFileAggregationTimeMs),
    m_newFileAggregationTimeDuration(boost::posix_time::milliseconds(newFileAggregationTimeMs)),
    m_newFileAggregationTimerIsRunning(false),
    m_nextMemoryBlockId(1), //0 reserved for error
    m_nextFileId(0),
    m_nextOffsetOfCurrentFile(0),
    m_countTotalFilesCreated(0),
    m_countTotalFilesActive(0)
{
    m_mapIdToMemoryBlockInfo.reserve(estimatedMaxAllocatedBlocks);
    if (boost::filesystem::is_directory(workingDirectory)) {
        if (!boost::filesystem::is_directory(m_rootStorageDirectory)) {
            if (!boost::filesystem::create_directory(m_rootStorageDirectory)) {
                LOG_ERROR(subprocess) << "Unable to create MemoryInFiles storage directory: " << m_rootStorageDirectory;
            }
            else {
                LOG_INFO(subprocess) << "Created MemoryInFiles storage directory: " << m_rootStorageDirectory;
            }
        }
    }
    else {
        LOG_ERROR(subprocess) << "MemoryInFiles working directory does not exist: " << workingDirectory;
    }
}
MemoryInFiles::Impl::~Impl() {
    m_newFileAggregationTimer.cancel();
    m_mapIdToMemoryBlockInfo.clear();
    m_currentWritingFileInfoPtr.reset();

    if (boost::filesystem::is_directory(m_rootStorageDirectory)) {
        if (!boost::filesystem::remove_all(m_rootStorageDirectory)) {
            LOG_ERROR(subprocess) << "Unable to remove directory " << m_rootStorageDirectory;
        }
    }
}

bool MemoryInFiles::Impl::SetupNextFileIfNeeded() {
    if (!m_currentWritingFileInfoPtr) {
        m_nextOffsetOfCurrentFile = 0;
        static const boost::format fmtTemplate("ltp_%09d.bin");
        boost::format fmt(fmtTemplate);
        fmt% m_nextFileId++;
        const boost::filesystem::path fullFilePath = m_rootStorageDirectory / boost::filesystem::path(fmt.str());

        if (boost::filesystem::exists(fullFilePath)) {
            LOG_ERROR(subprocess) << "MemoryInFiles::Impl::WriteMemoryAsync: " << fullFilePath << " already exists";
            return false;
        }
        m_currentWritingFileInfoPtr = std::make_shared<FileInfo>(fullFilePath, m_ioServiceRef, *this);
        TryRestartNewFileAggregationTimer(); //only start timer on new write allocation to prevent periodic empty files from being created
        ++m_countTotalFilesCreated;
        ++m_countTotalFilesActive;
    }
    return true;
}

uint64_t MemoryInFiles::Impl::AllocateNewWriteMemoryBlock(uint64_t totalSize) {
    const uint64_t memoryBlockId = m_nextMemoryBlockId;
    //try_emplace does not work with piecewise_construct
    std::pair<id_to_memoryblockinfo_map_t::iterator, bool> ret = m_mapIdToMemoryBlockInfo.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(memoryBlockId),
            std::forward_as_tuple(memoryBlockId));
    //(true if insertion happened, false if it did not).
    if (ret.second) {
        ++m_nextMemoryBlockId;
        MemoryBlockInfo& mbi = ret.first->second;
        if (Resize(mbi, totalSize) == 0) {
            return 0; //fail
        }
        return memoryBlockId;
    }
    else {
        //LOG_ERROR(subprocess) << "Unable to insert memoryBlockId " << memoryBlockId;
        return 0;
    }
}
uint64_t MemoryInFiles::Impl::Resize(const uint64_t memoryBlockId, uint64_t newSize) { //returns the new size
    id_to_memoryblockinfo_map_t::iterator it = m_mapIdToMemoryBlockInfo.find(memoryBlockId);
    if (it == m_mapIdToMemoryBlockInfo.end()) {
        return 0; //fail
    }
    MemoryBlockInfo& mbi = it->second;
    return Resize(mbi, newSize);
}
uint64_t MemoryInFiles::Impl::Resize(MemoryBlockInfo& mbi, uint64_t newSize) { //returns the new size
    if (!SetupNextFileIfNeeded()) {
        return 0;
    }
    newSize = CeilToNearestBlockMultiple(newSize);
    const uint64_t fileSizeIncrease = mbi.Resize(m_currentWritingFileInfoPtr, m_nextOffsetOfCurrentFile, newSize);
    m_nextOffsetOfCurrentFile += fileSizeIncrease;
    return newSize;
}
uint64_t MemoryInFiles::Impl::GetSizeOfMemoryBlock(const uint64_t memoryBlockId) const noexcept {
    id_to_memoryblockinfo_map_t::const_iterator it = m_mapIdToMemoryBlockInfo.find(memoryBlockId);
    if (it == m_mapIdToMemoryBlockInfo.cend()) {
        return false;
    }
    const MemoryBlockInfo& mbi = it->second;
    return mbi.m_lengthAlignedToBlockSize;
}
void MemoryInFiles::Impl::ForceDeleteMemoryBlock(const uint64_t memoryBlockId) {
    if (m_mapIdToMemoryBlockInfo.erase(memoryBlockId)) {
        LOG_DEBUG(subprocess) << "Deferred delete successful of memoryBlockId=" << memoryBlockId;
    }
    else {
        LOG_ERROR(subprocess) << "Deferred delete failed of memoryBlockId=" << memoryBlockId;
    }
}
bool MemoryInFiles::Impl::DeleteMemoryBlock(const uint64_t memoryBlockId) {
    id_to_memoryblockinfo_map_t::iterator it = m_mapIdToMemoryBlockInfo.find(memoryBlockId);
    if (it == m_mapIdToMemoryBlockInfo.end()) {
        return false;
    }
    MemoryBlockInfo& mbi = it->second;
    if (mbi.m_queuedOperationsReferenceCount) {
        if (mbi.m_markedForDeletion) { //already marked for deletion (double call to DeleteMemoryBlock)
            return false;
        }
        mbi.m_markedForDeletion = true;
        LOG_DEBUG(subprocess) << "DeleteMemoryBlock called while i/o operations in progress. Deferring deletion of memoryBlockId=" << memoryBlockId;
        return true;
    }
    m_mapIdToMemoryBlockInfo.erase(it);
    return true;
}
bool MemoryInFiles::Impl::MemoryBlockInfo::DoWriteOrRead(
    const uint64_t deferredOffset, const uint64_t deferredLength, void* readToThisLocationPtr, const void* writeFromThisLocationPtr,
    void * handlerPtrPtr)
{
    if (m_markedForDeletion) {
        LOG_ERROR(subprocess) << ((readToThisLocationPtr != NULL) ? "ReadMemoryAsync" : "WriteMemoryAsync")
            << " called on marked for deletion block with memoryBlockId = " << m_memoryBlockId;
        return false;
    }

    if ((deferredOffset + deferredLength) > m_lengthAlignedToBlockSize) {
        //LOG_ERROR(subprocess) << "out of bounds " << (deferredOffset + deferredLength) << " > " << m_lengthAlignedToBlockSize;
        return false;
    }

    //endIndex shall be treated as endIndexPlus1
    const FragmentSet::data_fragment_t fullBlock(deferredOffset, deferredOffset + deferredLength);
    FragmentSet::data_fragment_t thisFragment(0, 0);
    //LOG_DEBUG(subprocess) << "DoWriteOrRead";
    uint64_t operationPtrOffset = 0;
    for (MemoryBlockInfo::fragment_flist_t::const_iterator it = m_memoryBlockFragmentInfoFlist.cbegin(); it != m_memoryBlockFragmentInfoFlist.cend(); ++it) {
        const MemoryBlockFragmentInfo& frag = *it;
        thisFragment.endIndex += frag.m_length;
        //LOG_DEBUG(subprocess) << "Frag owf=" << frag.m_baseOffsetWithinFile << " len=" << frag.m_length;
        FragmentSet::data_fragment_t overlap;
        if (thisFragment.GetOverlap(fullBlock, overlap)) {
            //LOG_DEBUG(subprocess) << "overlap fb[" << fullBlock.beginIndex << "," << fullBlock.endIndex << "] frag[" << thisFragment.beginIndex << "," << thisFragment.endIndex 
            //    << "] overlap[" << overlap.beginIndex << "," << overlap.endIndex << "]";
            const uint64_t offsetWithinFragment = (overlap.beginIndex - thisFragment.beginIndex);
            const uint64_t offsetWithinFile = frag.m_baseOffsetWithinFile + offsetWithinFragment;
            const uint64_t thisFragmentOperationLength = overlap.endIndex - overlap.beginIndex;
            if (writeFromThisLocationPtr) {
                std::shared_ptr<write_memory_handler_t>& handlerPtr = *(reinterpret_cast<std::shared_ptr<write_memory_handler_t> *>(handlerPtrPtr));
                //LOG_DEBUG(subprocess) << "Write owfile=" << offsetWithinFile << " owfrag=" << offsetWithinFragment
                //    << " oplen=" << thisFragmentOperationLength << " bi=" << overlap.beginIndex << " ei=" << overlap.endIndex;
                if (!frag.m_fileInfoPtr->WriteMemoryAsync(*this, offsetWithinFile,
                    ((const uint8_t*)writeFromThisLocationPtr) + operationPtrOffset, thisFragmentOperationLength, handlerPtr))
                {
                    return false;
                }
            }
            else {
                std::shared_ptr<read_memory_handler_t>& handlerPtr = *(reinterpret_cast<std::shared_ptr<read_memory_handler_t> *>(handlerPtrPtr));
                //LOG_DEBUG(subprocess) << "read owf=" << offsetWithinFile << " oplen=" << thisFragmentOperationLength
                //    << " bi=" << overlap.beginIndex << " ei=" << overlap.endIndex;
                if (!frag.m_fileInfoPtr->ReadMemoryAsync(*this, offsetWithinFile,
                    ((uint8_t*)readToThisLocationPtr) + operationPtrOffset, thisFragmentOperationLength, handlerPtr))
                {
                    return false;
                }
            }
            operationPtrOffset += thisFragmentOperationLength;
        }
        //else {
            //LOG_DEBUG(subprocess) << "no overlap fb[" << fullBlock.beginIndex << "," << fullBlock.endIndex << "] frag[" << thisFragment.beginIndex << "," << thisFragment.endIndex << "]";
        //}
        
        thisFragment.beginIndex = thisFragment.endIndex;
    }
  
    //old behavior before resize capability and fragment list were added:
    //const uint64_t offsetWithinFile = mbi.m_baseOffsetWithinFile + deferredWrite.offset;
    //return mbi.m_fileInfoPtr->WriteMemoryAsync(mbi, offsetWithinFile, deferredWrite.writeFromThisLocationPtr, deferredWrite.length, handlerPtr);

    return true;
}
bool MemoryInFiles::Impl::WriteMemoryAsync(const deferred_write_t& deferredWrite, std::shared_ptr<write_memory_handler_t>& handlerPtr) {
    
    id_to_memoryblockinfo_map_t::iterator it = m_mapIdToMemoryBlockInfo.find(deferredWrite.memoryBlockId);
    if (it == m_mapIdToMemoryBlockInfo.end()) {
        return false;
    }
    MemoryBlockInfo& mbi = it->second;
    return mbi.DoWriteOrRead(deferredWrite.offset, deferredWrite.length, NULL, deferredWrite.writeFromThisLocationPtr, &handlerPtr);
}
bool MemoryInFiles::Impl::WriteMemoryAsync(const std::vector<deferred_write_t>& deferredWritesVec, std::shared_ptr<write_memory_handler_t>& handlerPtr) {
    for (std::size_t i = 0; i < deferredWritesVec.size(); ++i) {
        if (!WriteMemoryAsync(deferredWritesVec[i], handlerPtr)) {
            return false;
        }
    }
    return true;
}
bool MemoryInFiles::Impl::ReadMemoryAsync(const deferred_read_t& deferredRead, std::shared_ptr<read_memory_handler_t>& handlerPtr) {
    id_to_memoryblockinfo_map_t::iterator it = m_mapIdToMemoryBlockInfo.find(deferredRead.memoryBlockId);
    if (it == m_mapIdToMemoryBlockInfo.end()) {
        return false;
    }
    MemoryBlockInfo& mbi = it->second;
    return mbi.DoWriteOrRead(deferredRead.offset, deferredRead.length, deferredRead.readToThisLocationPtr, NULL, &handlerPtr);
}
bool MemoryInFiles::Impl::ReadMemoryAsync(const std::vector<deferred_read_t>& deferredReadsVec, std::shared_ptr<read_memory_handler_t>& handlerPtr) {
    for (std::size_t i = 0; i < deferredReadsVec.size(); ++i) {
        if (!ReadMemoryAsync(deferredReadsVec[i], handlerPtr)) {
            return false;
        }
    }
    return true;
}


MemoryInFiles::MemoryInFiles(boost::asio::io_service& ioServiceRef,
    const boost::filesystem::path& rootStorageDirectory,
    const uint64_t newFileAggregationTimeMs, const uint64_t estimatedMaxAllocatedBlocks) :
    m_pimpl(boost::make_unique<MemoryInFiles::Impl>(ioServiceRef, rootStorageDirectory, newFileAggregationTimeMs, estimatedMaxAllocatedBlocks))
{}
MemoryInFiles::~MemoryInFiles() {}

uint64_t MemoryInFiles::AllocateNewWriteMemoryBlock(uint64_t totalSize) {
    return m_pimpl->AllocateNewWriteMemoryBlock(totalSize);
}
uint64_t MemoryInFiles::GetSizeOfMemoryBlock(const uint64_t memoryBlockId) const noexcept {
    return m_pimpl->GetSizeOfMemoryBlock(memoryBlockId);
}
uint64_t MemoryInFiles::Resize(const uint64_t memoryBlockId, uint64_t newSize) {
    return m_pimpl->Resize(memoryBlockId, newSize);
}
bool MemoryInFiles::DeleteMemoryBlock(const uint64_t memoryBlockId) {
    return m_pimpl->DeleteMemoryBlock(memoryBlockId);
}

bool MemoryInFiles::WriteMemoryAsync(const deferred_write_t& deferredWrite, const write_memory_handler_t& handler) {
    std::shared_ptr<write_memory_handler_t> hPtr = std::make_shared<write_memory_handler_t>(handler);
    return m_pimpl->WriteMemoryAsync(deferredWrite, hPtr);
}
bool MemoryInFiles::WriteMemoryAsync(const deferred_write_t& deferredWrite, write_memory_handler_t&& handler) {
    std::shared_ptr<write_memory_handler_t> hPtr = std::make_shared<write_memory_handler_t>(std::move(handler));
    return m_pimpl->WriteMemoryAsync(deferredWrite, hPtr);
}
bool MemoryInFiles::WriteMemoryAsync(const std::vector<deferred_write_t>& deferredWritesVec, const write_memory_handler_t& handler) {
    std::shared_ptr<write_memory_handler_t> hPtr = std::make_shared<write_memory_handler_t>(handler);
    return m_pimpl->WriteMemoryAsync(deferredWritesVec, hPtr);
}
bool MemoryInFiles::WriteMemoryAsync(const std::vector<deferred_write_t>& deferredWritesVec, write_memory_handler_t&& handler) {
    std::shared_ptr<write_memory_handler_t> hPtr = std::make_shared<write_memory_handler_t>(std::move(handler));
    return m_pimpl->WriteMemoryAsync(deferredWritesVec, hPtr);
}

bool MemoryInFiles::ReadMemoryAsync(const deferred_read_t& deferredRead, const read_memory_handler_t& handler) {
    std::shared_ptr<read_memory_handler_t> hPtr = std::make_shared<read_memory_handler_t>(handler);
    return m_pimpl->ReadMemoryAsync(deferredRead, hPtr);
}
bool MemoryInFiles::ReadMemoryAsync(const deferred_read_t& deferredRead, read_memory_handler_t&& handler) {
    std::shared_ptr<read_memory_handler_t> hPtr = std::make_shared<read_memory_handler_t>(std::move(handler));
    return m_pimpl->ReadMemoryAsync(deferredRead, hPtr);
}
bool MemoryInFiles::ReadMemoryAsync(const std::vector<deferred_read_t>& deferredReadsVec, const read_memory_handler_t& handler) {
    std::shared_ptr<read_memory_handler_t> hPtr = std::make_shared<read_memory_handler_t>(handler);
    return m_pimpl->ReadMemoryAsync(deferredReadsVec, hPtr);
}
bool MemoryInFiles::ReadMemoryAsync(const std::vector<deferred_read_t>& deferredReadsVec, read_memory_handler_t&& handler) {
    std::shared_ptr<read_memory_handler_t> hPtr = std::make_shared<read_memory_handler_t>(std::move(handler));
    return m_pimpl->ReadMemoryAsync(deferredReadsVec, hPtr);
}

uint64_t MemoryInFiles::GetCountTotalFilesCreated() const {
    return m_pimpl->m_countTotalFilesCreated;
}
uint64_t MemoryInFiles::GetCountTotalFilesActive() const {
    return m_pimpl->m_countTotalFilesActive;
}
