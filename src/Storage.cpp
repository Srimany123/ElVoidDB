#include "Storage.hpp"
#include <cstring>
#include <sstream>
#include "BufferPool.hpp"
#include <algorithm>

namespace elvoiddb::storage {

/* ─── BlockFile ─────────────────────────────────────────────── */

BlockFile::BlockFile(const fs::path& p, bool create) : path_(p)
{
    auto mode = std::ios::binary | std::ios::in | std::ios::out;
    if (create) mode |= std::ios::trunc;
    file_.open(path_, mode);
    if (!file_) throw StorageError("cannot open " + path_.string());

    if (create) {
        Page meta;
        writePage(0, meta);                     // page-0 reserved for metadata
    }
}

/*void BlockFile::writePage(size_t n, const Page& pg)
{
    file_.seekp(n * PAGE_SIZE);
    file_.write(pg.raw(), PAGE_SIZE);
    if (!file_) throw StorageError("write fail " + path_.string());
    file_.flush();
}

void BlockFile::readPage(size_t n, Page& pg) const
{
    file_.seekg(n * PAGE_SIZE);
    file_.read(pg.raw(), PAGE_SIZE);
    if (!file_) throw StorageError("read fail " + path_.string());
}*/

void BlockFile::readPage(size_t n, Page& pg) const
{
    // fetch from buffer pool → copy into caller-supplied Page
    Page& frame = gBufPool.get(path_, n);             // pins frame
    std::memcpy(pg.raw(), frame.raw(), PAGE_SIZE);
    gBufPool.unpin(path_, n);
}

void BlockFile::writePage(size_t n, const Page& pg)
{
    // 1. update buffer-pool frame
    Page& frame = gBufPool.get(path_, n);          // pins frame
    std::memcpy(frame.raw(), pg.raw(), PAGE_SIZE);
    gBufPool.markDirty(path_, n);
    gBufPool.unpin(path_, n);

    // 2. write-through so metadata reaches disk immediately
    /*std::fstream f(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) {                                      // file might not exist yet
        f.open(path_, std::ios::binary | std::ios::out); // create
        f.close();
        f.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    }
    f.seekp(n * PAGE_SIZE);
    f.write(pg.raw(), PAGE_SIZE);
    f.flush();*/
}


size_t BlockFile::pageCount()
{
    file_.seekg(0, std::ios::end);
    return static_cast<size_t>(file_.tellg()) / PAGE_SIZE;
}

/* ─── helpers: row (de)serialisation ────────────────────────── */

std::string TableFile::serializeRow(const std::vector<std::string>& row)
{
    std::string out;
    uint16_t colCnt = row.size();
    out.append(reinterpret_cast<const char*>(&colCnt), sizeof(uint16_t));

    for (const auto& col : row) {
        uint16_t len = col.size();
        out.append(reinterpret_cast<const char*>(&len), sizeof(uint16_t));
        out.append(col);
    }
    return out;
}

std::vector<std::string>
TableFile::deserializeRow(const char* data, uint16_t len)
{
    std::vector<std::string> out;
    const char* end = data + len;                    // hard limit

    if (data + sizeof(uint16_t) > end) return {};    // corrupt

    const char* ptr = data;
    uint16_t colCnt = *reinterpret_cast<const uint16_t*>(ptr);
    ptr += sizeof(uint16_t);

    while (colCnt--) {
        if (ptr + sizeof(uint16_t) > end) return {}; // corrupt
        uint16_t slen = *reinterpret_cast<const uint16_t*>(ptr);
        ptr += sizeof(uint16_t);
        if (ptr + slen > end) return {};             // corrupt
        out.emplace_back(ptr, slen);
        ptr += slen;
    }
    return out;
}

/* ─── TableFile ─────────────────────────────────────────────── */

TableFile::TableFile(const std::string& t, bool create,
                     const std::vector<std::string>& cols)
    : bf_(t + ".tbl", create)
{
    if (create) {
        Page meta;
        std::string hdr = "cols:";
        for (size_t i = 0; i < cols.size(); ++i) {
            hdr += cols[i];
            if (i + 1 != cols.size()) hdr += ',';
        }
        std::memcpy(meta.raw(), hdr.data(), hdr.size());
        bf_.writePage(0, meta);
    }
}

void TableFile::appendRow(const std::vector<std::string>& row)
{
    std::string bytes = serializeRow(row);

    size_t last = bf_.pageCount() - 1;

    // page 0 is metadata – if it’s the only page, allocate page 1 first
    if (last == 0) {
        Page fresh;
        if (fresh.insertRecord(bytes) == -1)
            throw StorageError("row too large");
        bf_.writePage(1, fresh);
        return;
    }

    Page pg;
    bf_.readPage(last, pg);

    if (pg.insertRecord(bytes) == -1) {        // full → add new page
        Page fresh;
        if (fresh.insertRecord(bytes) == -1)
            throw StorageError("row too large");
        bf_.writePage(last + 1, fresh);
    } else {
        bf_.writePage(last, pg);
    }
}

void TableFile::loadAllRows(std::vector<std::vector<std::string>>& dest)
{
    for (size_t p = 1; p < bf_.pageCount(); ++p) {      // skip page-0
        Page pg;
        bf_.readPage(p, pg);
        pg.forEachRecord([&](const char* rec, uint16_t len) {
            dest.push_back(deserializeRow(rec, len));
        });
    }
}

std::vector<std::string> TableFile::columnList() const
{
    Page meta;
    bf_.readPage(0, meta);
    std::string header(meta.raw(), PAGE_SIZE);
    auto pos = header.find("cols:");
    if (pos == std::string::npos) return {};
    std::string list = header.substr(pos + 5);        // after "cols:"
    list.erase(std::find_if(list.begin(), list.end(), [](char c){ return c==0; }), list.end());
    std::vector<std::string> cols;
    std::string token;
    std::istringstream ss(list);
    while (std::getline(ss, token, ',')) cols.push_back(token);
    return cols;
}


/* ─── FileManager ───────────────────────────────────────────── */

void FileManager::createTable(const std::string& n,
                              const std::vector<std::string>& cols)
{
    if (fs::exists(n + ".tbl")) throw StorageError("exists");
    open_[n] = std::make_unique<TableFile>(n, true, cols);
}

elvoiddb::storage::TableFile* FileManager::openTable(const std::string& n)
{
    if (auto it = open_.find(n); it != open_.end()) return it->second.get();
    if (!fs::exists(n + ".tbl")) return nullptr;
    open_[n] = std::make_unique<TableFile>(n, false);
    return open_[n].get();
}

} // namespace elvoiddb::storage
