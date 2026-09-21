#pragma once

#include <GameDefinitions/Base/Base.h>

BEGIN_SE()

enum class FileType
{
    Unknown,
    MemBuffer,
    MemoryMapped
};

struct FileReader : public Noncopyable<FileReader>
{
    bool IsLoaded{ false };
    void* DataPtr{ nullptr };
    void* ReadPtr{ nullptr };
    uint64_t FileSize{ 0 };
    uint64_t FileHandle{ 0 };
    uint64_t FileHandle2{ 0 };
    ScratchBuffer Buffer;
    FileType Type{ FileType::Unknown };
    void* FileObject{ nullptr };
};

class FileReaderPin
{
public:
    inline FileReaderPin(FileReader* reader)
        : reader_(reader)
    {}

    ~FileReaderPin();

    FileReaderPin(FileReaderPin const&) = delete;
    FileReaderPin& operator =(FileReaderPin const&) = delete;
    FileReaderPin& operator =(FileReaderPin&&) = delete;

    inline FileReaderPin(FileReaderPin&& other) noexcept
    {
        reader_ = other.reader_;
        if (&other != this) {
            other.reader_ = nullptr;
        }
    }

    bool IsLoaded() const
    {
        return reader_ != nullptr && reader_->IsLoaded;
    }

    void* Buf() const
    {
        if (IsLoaded()) {
            return reader_->DataPtr;
        } else {
            return nullptr;
        }
    }

    std::size_t Size() const
    {
        if (IsLoaded()) {
            return reader_->FileSize;
        } else {
            return 0;
        }
    }

    STDString ToString() const;

private:
    FileReader* reader_;
};

END_SE()