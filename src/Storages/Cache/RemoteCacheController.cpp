#include <Storages/Cache/RemoteCacheController.h>
#include <Storages/Cache/ExternalDataSourceCache.h>
#include <Storages/RemoteFileMetadataFactory.h>
#include <IO/ReadBuffer.h>
#include <IO/ReadSettings.h>
#include <Poco/JSON/JSON.h>
#include <Poco/JSON/Parser.h>
#include <fstream>

namespace DB
{
namespace fs = std::filesystem;
namespace ErrorCodes
{
    extern const int OK;
    extern const int BAD_ARGUMENTS;
    extern const int LOGICAL_ERROR;
    extern const int END_OF_FILE;
}

bool RemoteCacheController::loadInnerInformation(const fs::path & file_path)
{
    if (!fs::exists(file_path))
        return false;
    std::ifstream info_file(file_path);
    Poco::JSON::Parser info_parser;
    auto info_json = info_parser.parse(info_file).extract<Poco::JSON::Object::Ptr>();
    file_status = static_cast<LocalFileStatus>(info_json->get("file_status").convert<Int32>());
    metadata_class = info_json->get("metadata_class").convert<String>();
    info_file.close();
    return true;
}

std::shared_ptr<RemoteCacheController> RemoteCacheController::recover(const std::filesystem::path & local_path_)
{
    auto * log = &Poco::Logger::get("RemoteCacheController");

    if (!std::filesystem::exists(local_path_ / "data.bin"))
    {
        LOG_TRACE(log, "Invalid cached directory:{}", local_path_.string());
        return nullptr;
    }

    auto cache_controller = std::make_shared<RemoteCacheController>(nullptr, local_path_, 0);
    if (!cache_controller->loadInnerInformation(local_path_ / "info.txt")
            || cache_controller->file_status != DOWNLOADED)
    {
        LOG_INFO(log, "Recover cached file failed. local path:{}", local_path_.string());
        return nullptr;
    }
    try
    {
        cache_controller->file_metadata_ptr = RemoteFileMetadataFactory::instance().get(cache_controller->metadata_class);
    }
    catch(const Exception & e)
    {
        LOG_ERROR(log, "Get metadata class failed for {}. {}", cache_controller->metadata_class, e.message());
        cache_controller->file_metadata_ptr = nullptr;
    }
    if (!cache_controller->file_metadata_ptr)
    {
        // do not load this invalid cached file and clear it. the clear action is in
        // ExternalDataSourceCache::recoverCachedFilesMetadata(), because deleting directories during iteration will
        // cause unexpected behaviors
        LOG_ERROR(log, "Cannot create the metadata class : {}. The cached file is invalid and will be remove. path:{}",
                cache_controller->metadata_class,
                local_path_.string());
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Invalid metadata class:{}", cache_controller->metadata_class);
    }
    std::ifstream metadata_file(local_path_ / "metadata.txt");
    if (!cache_controller->file_metadata_ptr->fromString(std::string((std::istreambuf_iterator<char>(metadata_file)),
                    std::istreambuf_iterator<char>())))
    {
        LOG_ERROR(log, "Cannot load the metadata. The cached file is invalid and will be remove. path:{}",
                local_path_.string());
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Invalid metadata file({}) for meta class {}", local_path_.string(), cache_controller->metadata_class);
    }

    cache_controller->current_offset = fs::file_size(local_path_ / "data.bin");

    ExternalDataSourceCache::instance().updateTotalSize(cache_controller->file_metadata_ptr->file_size);
    return cache_controller;
}

RemoteCacheController::RemoteCacheController(
    IRemoteFileMetadataPtr file_metadata_,
    const std::filesystem::path & local_path_,
    size_t cache_bytes_before_flush_)
    : file_metadata_ptr(file_metadata_)
    , local_path(local_path_)
    , valid(true)
    , local_cache_bytes_read_before_flush(cache_bytes_before_flush_)
    , current_offset(0)
{
    // on recover, file_metadata_ptr is null, but it will be allocated after loading from metadata.txt
    // when we allocate a whole new file cache ， file_metadata_ptr must not be null.
    if (file_metadata_ptr)
    {
        metadata_class = file_metadata_ptr->getName();
        auto metadata_file_writer = std::make_unique<WriteBufferFromFile>((local_path_ / "metadata.txt").string());
        auto str_buf = file_metadata_ptr->toString();
        metadata_file_writer->write(str_buf.c_str(), str_buf.size());
        metadata_file_writer->close();
    }
}

ErrorCodes::ErrorCode RemoteCacheController::waitMoreData(size_t start_offset_, size_t end_offset_)
{
    std::unique_lock lock{mutex};
    if (file_status == DOWNLOADED)
    {
        // finish reading
        if (start_offset_ >= current_offset)
        {
            lock.unlock();
            return ErrorCodes::END_OF_FILE;
        }
    }
    else // block until more data is ready
    {
        if (current_offset >= end_offset_)
        {
            lock.unlock();
            return ErrorCodes::OK;
        }
        else
            more_data_signal.wait(lock, [this, end_offset_] { return file_status == DOWNLOADED || current_offset >= end_offset_; });
    }
    lock.unlock();
    return ErrorCodes::OK;
}

bool RemoteCacheController::isModified(IRemoteFileMetadataPtr file_metadata_)
{
    return !(file_metadata_ptr->getVersion() == file_metadata_->getVersion());
}

void RemoteCacheController::startBackgroundDownload(std::unique_ptr<ReadBuffer> in_readbuffer_, BackgroundSchedulePool & thread_pool)
{
    data_file_writer = std::make_unique<WriteBufferFromFile>((fs::path(local_path) / "data.bin").string());
    flush(true);
    ReadBufferPtr in_readbuffer(in_readbuffer_.release());
    download_task_holder = thread_pool.createTask("download remote file",
            [this, in_readbuffer]{ backgroundDownload(in_readbuffer); });
    download_task_holder->activateAndSchedule();
}

void RemoteCacheController::backgroundDownload(ReadBufferPtr remote_read_buffer)
{
    file_status = DOWNLOADING;
    size_t before_unflush_bytes = 0;
    size_t total_bytes = 0;
    while (!remote_read_buffer->eof())
    {
        size_t bytes = remote_read_buffer->available();

        data_file_writer->write(remote_read_buffer->position(), bytes);
        remote_read_buffer->position() += bytes;
        total_bytes += bytes;
        before_unflush_bytes += bytes;
        if (before_unflush_bytes >= local_cache_bytes_read_before_flush)
        {
            std::unique_lock lock(mutex);
            current_offset += total_bytes;
            total_bytes = 0;
            flush();
            lock.unlock();
            more_data_signal.notify_all();
            before_unflush_bytes = 0;
        }
    }
    std::unique_lock lock(mutex);
    current_offset += total_bytes;
    file_status = DOWNLOADED;
    flush(true);
    data_file_writer.reset();
    lock.unlock();
    more_data_signal.notify_all();
    ExternalDataSourceCache::instance().updateTotalSize(file_metadata_ptr->file_size);
    LOG_TRACE(log, "Finish download into local path: {}, file metadata:{} ", local_path.string(), file_metadata_ptr->toString());
}

void RemoteCacheController::flush(bool need_flush_status)
{
    if (data_file_writer)
    {
        data_file_writer->sync();
    }
    if (need_flush_status)
    {
        auto file_writer = std::make_unique<WriteBufferFromFile>(local_path / "info.txt");
        Poco::JSON::Object jobj;
        jobj.set("file_status", static_cast<Int32>(file_status));
        jobj.set("metadata_class", metadata_class);
        std::stringstream buf; // STYLE_CHECK_ALLOW_STD_STRING_STREAM
        jobj.stringify(buf);
        file_writer->write(buf.str().c_str(), buf.str().size());
        file_writer->close();
    }
}

RemoteCacheController::~RemoteCacheController()
{
    if (download_task_holder)
        download_task_holder->deactivate();
}

void RemoteCacheController::close()
{
    // delete directory
    LOG_TRACE(log, "Removing the local cache. local path: {}", local_path.string());
    std::filesystem::remove_all(local_path);
}

std::unique_ptr<ReadBufferFromFileBase> RemoteCacheController::allocFile()
{
    ReadSettings settings;
    //settings.local_fs_method = LocalFSReadMethod::read;
    auto file_buffer = createReadBufferFromFileBase((local_path / "data.bin").string(), settings);

    if (file_buffer)
    {
        std::lock_guard lock{mutex};
        opened_file_buffer_refs.insert(reinterpret_cast<uintptr_t>(file_buffer.get()));
    }
    return file_buffer;
}

void RemoteCacheController::deallocFile(std::unique_ptr<ReadBufferFromFileBase> file_buffer)
{
    if (!file_buffer)
    {
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Try to release a null file buffer for {}", local_path.string());
    }
    auto buffer_ref = reinterpret_cast<uintptr_t>(file_buffer.get());
    std::lock_guard lock{mutex};
    auto it = opened_file_buffer_refs.find(buffer_ref);
    if (it == opened_file_buffer_refs.end())
    {
        throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "Try to deallocate file with invalid handler remote path: {}, local path: {}",
                file_metadata_ptr->remote_path,
                local_path.string());
    }
    opened_file_buffer_refs.erase(it);
}

}
