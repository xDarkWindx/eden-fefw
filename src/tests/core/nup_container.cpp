// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <openssl/evp.h>
#include <zstd.h>

#include "common/common_types.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"
#include "core/loader/nup.h"

// Builds NUP v1 containers in memory so the reader can be tested without the packer.
namespace {

constexpr u32 NoDir = 0xFFFFFFFF;
constexpr u64 BaseTitleId = 0x0100000000010000ULL;

struct Node {
    std::string name;
    bool is_dir{};
    std::vector<u8> data;
    bool zstd{};
    std::vector<Node> children;
};

Node Dir(std::string name, std::vector<Node> children) {
    return Node{std::move(name), true, {}, false, std::move(children)};
}

Node File(std::string name, std::string_view text, bool zstd = false) {
    return Node{std::move(name), false, std::vector<u8>(text.begin(), text.end()), zstd, {}};
}

struct Tlv {
    u16 type;
    std::vector<u8> payload;
};

Tlv TextTlv(u16 type, std::string_view text) {
    return Tlv{type, std::vector<u8>(text.begin(), text.end())};
}

struct Package {
    u64 title_id;
    u32 version;
    u8 kind;
    u8 flags;
    Node root;
    std::vector<Tlv> meta;
};

template <typename T>
void Put(std::vector<u8>& out, std::size_t offset, T value) {
    if (out.size() < offset + sizeof(T)) {
        out.resize(offset + sizeof(T));
    }
    std::memcpy(out.data() + offset, &value, sizeof(T));
}

template <typename T>
void Append(std::vector<u8>& out, T value) {
    Put(out, out.size(), value);
}

std::array<u8, 0x20> Sha256(const std::vector<u8>& data) {
    std::array<u8, 0x20> out{};
    unsigned int length = 0;
    EVP_Digest(data.data(), data.size(), out.data(), &length, EVP_sha256(), nullptr);
    return out;
}

u64 Align16(u64 value) {
    return (value + 0xF) & ~u64{0xF};
}

struct Built {
    std::vector<u8> bytes;
    u64 blob_offset{};
};

Built Build(const std::vector<Package>& packages) {
    struct FlatDir {
        const Node* node;
        u32 first_child{};
    };
    struct FlatFile {
        const Node* node;
        std::vector<u8> stored;
        u64 extent_offset{};
    };
    std::vector<FlatDir> dirs;
    std::vector<FlatFile> files;
    std::vector<u32> package_root;

    // Pre-order: every directory gets an index before any file is numbered.
    const auto collect_dirs = [&](auto&& self, const Node& node) -> void {
        dirs.push_back({&node});
        for (const auto& child : node.children) {
            if (child.is_dir) {
                self(self, child);
            }
        }
    };
    for (const auto& package : packages) {
        package_root.push_back(static_cast<u32>(dirs.size()));
        collect_dirs(collect_dirs, package.root);
    }
    const auto dir_index = [&](const Node* node) {
        for (u32 i = 0; i < dirs.size(); ++i) {
            if (dirs[i].node == node) {
                return i;
            }
        }
        return NoDir;
    };

    std::vector<u32> children;
    for (auto& dir : dirs) {
        dir.first_child = static_cast<u32>(children.size());
        for (const auto& child : dir.node->children) {
            if (child.is_dir) {
                children.push_back(dir_index(&child));
            } else {
                children.push_back(0); // patched once the file count is known
                files.push_back({&child, {}});
            }
        }
    }
    std::size_t file_cursor = 0;
    for (const auto& dir : dirs) {
        for (std::size_t i = 0; i < dir.node->children.size(); ++i) {
            if (!dir.node->children[i].is_dir) {
                children[dir.first_child + i] = static_cast<u32>(dirs.size() + file_cursor++);
            }
        }
    }

    std::string pool;
    const auto intern = [&](const std::string& name) {
        const auto offset = static_cast<u32>(pool.size());
        pool += name;
        return std::pair{offset, static_cast<u32>(name.size())};
    };

    const u64 needed = 0x30 + packages.size() * 0x40 + dirs.size() * 0x18 + files.size() * 0x50;
    const u64 pool_offset = needed + children.size() * sizeof(u32);
    std::vector<std::pair<u32, u32>> dir_names;
    std::vector<std::pair<u32, u32>> file_names;
    for (const auto& dir : dirs) {
        dir_names.push_back(intern(dir.node->name));
    }
    for (const auto& file : files) {
        file_names.push_back(intern(file.node->name));
    }
    const u64 toc_size = pool_offset + pool.size();
    const u64 blob_offset = Align16(0x80 + toc_size);

    std::vector<u8> blob;
    for (auto& file : files) {
        file.stored = file.node->data;
        if (file.node->zstd) {
            std::vector<u8> compressed(ZSTD_compressBound(file.node->data.size()));
            const auto size = ZSTD_compress(compressed.data(), compressed.size(),
                                            file.node->data.data(), file.node->data.size(), 3);
            compressed.resize(size);
            file.stored = std::move(compressed);
        }
        if (file.node->data.empty()) {
            continue;
        }
        blob.resize(Align16(blob.size()));
        file.extent_offset = blob_offset + blob.size();
        blob.insert(blob.end(), file.stored.begin(), file.stored.end());
    }
    blob.resize(Align16(blob.size()));

    // Meta blocks follow the blob.
    u64 meta_cursor = blob_offset + blob.size();
    std::vector<u8> meta_region;
    std::vector<std::pair<u64, u32>> meta_location;
    for (const auto& package : packages) {
        std::vector<u8> block;
        for (const auto& tlv : package.meta) {
            Append<u16>(block, tlv.type);
            Append<u16>(block, 0);
            Append<u32>(block, static_cast<u32>(tlv.payload.size()));
            block.insert(block.end(), tlv.payload.begin(), tlv.payload.end());
        }
        meta_location.emplace_back(block.empty() ? 0 : meta_cursor + meta_region.size(),
                                   static_cast<u32>(block.size()));
        meta_region.insert(meta_region.end(), block.begin(), block.end());
    }

    std::vector<u8> toc;
    Append<u32>(toc, 0x01434F54);
    Append<u32>(toc, static_cast<u32>(packages.size()));
    Append<u64>(toc, dirs.size());
    Append<u64>(toc, files.size());
    Append<u64>(toc, pool_offset);
    Append<u64>(toc, pool.size());
    Append<u64>(toc, 0);
    for (std::size_t i = 0; i < packages.size(); ++i) {
        const auto& package = packages[i];
        const auto start = toc.size();
        toc.resize(start + 0x40);
        u32 exefs = NoDir;
        u32 romfs = NoDir;
        for (const auto& child : package.root.children) {
            if (child.is_dir && child.name == "exefs") {
                exefs = dir_index(&child);
            } else if (child.is_dir && child.name == "romfs") {
                romfs = dir_index(&child);
            }
        }
        Put<u64>(toc, start + 0x00, package.title_id);
        Put<u32>(toc, start + 0x08, package_root[i]);
        Put<u32>(toc, start + 0x0C, exefs);
        Put<u32>(toc, start + 0x10, romfs);
        Put<u32>(toc, start + 0x14, package.version);
        Put<u8>(toc, start + 0x18, package.kind);
        Put<u8>(toc, start + 0x19, package.flags);
        Put<u64>(toc, start + 0x20, meta_location[i].first);
        Put<u32>(toc, start + 0x28, meta_location[i].second);
    }
    for (std::size_t i = 0; i < dirs.size(); ++i) {
        Append<u32>(toc, dir_names[i].first);
        Append<u32>(toc, dir_names[i].second);
        Append<u32>(toc, dirs[i].first_child);
        Append<u32>(toc, static_cast<u32>(dirs[i].node->children.size()));
        Append<u64>(toc, 0);
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];
        const auto start = toc.size();
        toc.resize(start + 0x50);
        const bool empty = file.node->data.empty();
        Put<u32>(toc, start + 0x00, file_names[i].first);
        Put<u32>(toc, start + 0x04, file_names[i].second);
        Put<u64>(toc, start + 0x08, file.node->data.size());
        Put<u64>(toc, start + 0x10, empty ? 0 : file.extent_offset);
        Put<u64>(toc, start + 0x18, empty ? 0 : file.stored.size());
        const auto hash = Sha256(file.node->data);
        std::memcpy(toc.data() + start + 0x20, hash.data(), hash.size());
        Put<u8>(toc, start + 0x40, file.node->zstd ? 1 : 0);
    }
    for (const u32 child : children) {
        Append<u32>(toc, child);
    }
    toc.insert(toc.end(), pool.begin(), pool.end());
    REQUIRE(toc.size() == toc_size);

    Built built;
    built.blob_offset = blob_offset;
    auto& out = built.bytes;
    out.assign({'N', 'U', 'P', 0x01});
    Put<u16>(out, 0x04, 0x80);
    Put<u16>(out, 0x06, 1);
    Put<u64>(out, 0x08, 0x80);
    Put<u64>(out, 0x10, toc_size);
    const auto toc_hash = Sha256(toc);
    out.resize(0x80);
    std::memcpy(out.data() + 0x18, toc_hash.data(), toc_hash.size());
    Put<u64>(out, 0x38, blob_offset);
    Put<u32>(out, 0x48, static_cast<u32>(packages.size()));
    out.insert(out.end(), toc.begin(), toc.end());
    out.resize(blob_offset);
    out.insert(out.end(), blob.begin(), blob.end());
    out.insert(out.end(), meta_region.begin(), meta_region.end());
    Put<u64>(out, 0x40, out.size());
    return built;
}

const std::string BigText(4096, 'z');

Package BasePackage() {
    return Package{
        BaseTitleId,
        0,
        0,
        0,
        Dir("base", {Dir("exefs", {File("main", "MAIN-NSO"), File("main.npdm", "NPDM")}),
                     Dir("romfs", {File("data.txt", "base data"), File("only_base.txt", BigText, true),
                                   File("empty.bin", ""), Dir("sub", {File("x.bin", "x")})})}),
        {TextTlv(0x0005, "Test Title"), TextTlv(0x0006, "Test Publisher"),
         TextTlv(0x0004, "1.0.0"), Tlv{0x0003, {0xFF, 0xD8, 0xFF}}},
    };
}

Package UpdatePackage() {
    return Package{
        BaseTitleId | 0x800,
        0x00020000,
        1,
        0,
        Dir("update", {Dir("romfs", {File("data.txt", "update data"),
                                     Dir("sub", {File("y.bin", "y")})})}),
        {TextTlv(0x0004, "2.0.0")},
    };
}

FileSys::VirtualFile AsFile(std::vector<u8> bytes) {
    return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "test.nup");
}

std::filesystem::path SpillDir() {
    return std::filesystem::temp_directory_path() / "eden-nup-test";
}

std::string ReadText(const FileSys::VirtualFile& file) {
    REQUIRE(file != nullptr);
    const auto bytes = file->ReadAllBytes();
    return std::string(bytes.begin(), bytes.end());
}

} // namespace

TEST_CASE("NUP container parses packages and metadata", "[nup]") {
    const auto built = Build({BasePackage(), UpdatePackage()});
    FileSys::NupContainer nup{AsFile(built.bytes), FileSys::NupDecompressCache::DefaultCap,
                              SpillDir()};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages().size() == 2);
    REQUIRE(nup.FindBasePackage() == std::optional<std::size_t>{0});
    REQUIRE(nup.FindBestUpdatePackage() == std::optional<std::size_t>{1});

    const auto& base = nup.GetPackages()[0];
    REQUIRE(base.title_id == BaseTitleId);
    REQUIRE(base.title_name == "Test Title");
    REQUIRE(base.publisher == "Test Publisher");
    REQUIRE(base.display_version == "1.0.0");
    REQUIRE(base.icon == std::vector<u8>{0xFF, 0xD8, 0xFF});

    const auto exefs = nup.GetExeFSDir(0);
    REQUIRE(exefs != nullptr);
    REQUIRE(ReadText(exefs->GetFile("main")) == "MAIN-NSO");
    REQUIRE(nup.GetExeFSDir(1) == nullptr);

    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
}

TEST_CASE("NUP update RomFS shadows the base RomFS", "[nup]") {
    const auto built = Build({BasePackage(), UpdatePackage()});
    FileSys::NupContainer nup{AsFile(built.bytes), FileSys::NupDecompressCache::DefaultCap,
                              SpillDir()};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);

    const auto merged =
        FileSys::NupContainer::MergeShadowDirs(nup.GetRomFSDir(0), nup.GetRomFSDir(1));
    REQUIRE(merged != nullptr);
    REQUIRE(ReadText(merged->GetFile("data.txt")) == "update data");
    REQUIRE(ReadText(merged->GetFile("only_base.txt")) == BigText);
    REQUIRE(merged->GetFile("empty.bin") != nullptr);
    REQUIRE(merged->GetFile("empty.bin")->GetSize() == 0);
    const auto sub = merged->GetSubdirectory("sub");
    REQUIRE(sub != nullptr);
    REQUIRE(ReadText(sub->GetFile("x.bin")) == "x");
    REQUIRE(ReadText(sub->GetFile("y.bin")) == "y");
}

TEST_CASE("NUP zstd members larger than half the cache are spilled and still read", "[nup]") {
    const auto built = Build({BasePackage()});
    // A 1 KiB cache forces the 4 KiB member through the temporary-file path.
    FileSys::NupContainer nup{AsFile(built.bytes), 1024, SpillDir()};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    const auto romfs = nup.GetRomFSDir(0);
    REQUIRE(ReadText(romfs->GetFile("only_base.txt")) == BigText);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
}

TEST_CASE("NUP rejects a modified table of contents", "[nup]") {
    auto built = Build({BasePackage()});
    built.bytes[0x80 + 0x31] ^= 0x01;
    FileSys::NupContainer nup{AsFile(built.bytes), FileSys::NupDecompressCache::DefaultCap,
                              SpillDir()};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::ErrorNUPBadFormat);

    REQUIRE_FALSE(FileSys::NupContainer::IsNupFile(AsFile(std::vector<u8>(0x100, 0))));
}

TEST_CASE("NUP integrity check catches modified file data", "[nup]") {
    auto built = Build({BasePackage()});
    // The first extent is exefs/main; the TOC hash still matches, only the member does not.
    built.bytes[built.blob_offset] ^= 0x01;
    FileSys::NupContainer nup{AsFile(built.bytes), FileSys::NupDecompressCache::DefaultCap,
                              SpillDir()};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::ErrorIntegrityVerificationFailed);
}

TEST_CASE("NUP loader exposes title, icon, version and the merged RomFS", "[nup]") {
    const auto file = AsFile(Build({BasePackage(), UpdatePackage()}).bytes);
    REQUIRE(Loader::IdentifyFile(file) == Loader::FileType::NUP);
    REQUIRE(Loader::GuessFromFilename("Game.NUP") == Loader::FileType::NUP);
    REQUIRE(Loader::IsBootableGameContainer(file, Loader::FileType::NUP));

    Loader::AppLoader_NUP loader{file};
    REQUIRE(loader.GetFileType() == Loader::FileType::NUP);

    u64 program_id{};
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
    REQUIRE(program_id == BaseTitleId);

    std::string title;
    REQUIRE(loader.ReadTitle(title) == Loader::ResultStatus::Success);
    REQUIRE(title == "Test Title");

    std::vector<u8> icon;
    REQUIRE(loader.ReadIcon(icon) == Loader::ResultStatus::Success);
    REQUIRE(icon == std::vector<u8>{0xFF, 0xD8, 0xFF});

    FileSys::NACP control;
    REQUIRE(loader.ReadControlData(control) == Loader::ResultStatus::Success);
    REQUIRE(control.GetApplicationName() == "Test Title");
    REQUIRE(control.GetDeveloperName() == "Test Publisher");
    // The newest update's display version wins over the base's.
    REQUIRE(control.GetVersionString() == "2.0.0");

    FileSys::VirtualFile romfs;
    REQUIRE(loader.ReadRomFS(romfs) == Loader::ResultStatus::Success);
    const auto extracted = FileSys::ExtractRomFS(romfs);
    REQUIRE(extracted != nullptr);
    REQUIRE(ReadText(extracted->GetFile("data.txt")) == "update data");
    REQUIRE(ReadText(extracted->GetFile("only_base.txt")) == BigText);
    REQUIRE_FALSE(loader.IsRomFSUpdatable());
}

TEST_CASE("NUP standalone update boots without a base package", "[nup]") {
    auto update = UpdatePackage();
    update.flags = 0x01;
    update.root.children.insert(
        update.root.children.begin(),
        Dir("exefs", {File("main", "UPDATED-NSO"), File("main.npdm", "NPDM")}));
    const auto file = AsFile(Build({update}).bytes);
    REQUIRE(Loader::IsBootableGameContainer(file, Loader::FileType::NUP));

    Loader::AppLoader_NUP loader{file};
    u64 program_id{};
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
    REQUIRE(program_id == (BaseTitleId | 0x800));
}

TEST_CASE("NUP with only add-on content is not bootable", "[nup]") {
    Package addon{BaseTitleId | 0x1000, 0, 2, 0, Dir("aoc", {Dir("romfs", {File("a", "a")})}), {}};
    const auto file = AsFile(Build({addon}).bytes);
    REQUIRE_FALSE(Loader::IsBootableGameContainer(file, Loader::FileType::NUP));

    Loader::AppLoader_NUP loader{file};
    u64 program_id{};
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::ErrorNotInitialized);
}
