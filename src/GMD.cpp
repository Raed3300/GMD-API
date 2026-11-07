#include "Shared.hpp"
#include <GMD.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/base64.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <Geode/utils/JsonValidation.hpp>
#include <Geode/cocos/support/base64.h>

using namespace geode::prelude;
using namespace gmd;

// The public API in GMD.hpp still uses std::filesystem::path, but the newer
// Geode SDK file utilities take asp::fs::PathView, which can't be built from
// a std::filesystem::path. So we convert to a UTF-8 std::string at the call
// site and pass `.c_str()` (a const char*, same as a string literal) to the SDK.
// Keep the std::string alive for the whole call!
static std::string pathToUtf8(std::filesystem::path const& path) {
    auto u8 = path.u8string();
    return std::string(reinterpret_cast<char const*>(u8.data()), u8.size());
}

static std::string extensionWithoutDot(std::filesystem::path const& path) {
    auto ext = path.extension().string();
    if (ext.size()) {
        return ext.substr(1);
    }
    return "";
}
static bool verifySongFileName(std::string const& name) {
    // Make sure that the song name is .mp3 and the name is parseable as a number
    if (name.ends_with(".mp3")) {
        try {
            (void)std::stoi(name.substr(0, name.size() - 4));
            return true;
        }
        catch(...) {}
    }
    return false;
}

// Compression level passed as the 4th argument of ZipUtils::ccDeflateMemory.
// The current SDK requires 4 arguments. If the compiler complains about this
// argument's type, check the "candidate function" note in the build log.
static constexpr int DEFLATE_LEVEL = 9;

ImportGmdFile::ImportGmdFile(
    std::filesystem::path const& path
) : m_path(path) {}

bool ImportGmdFile::tryInferType() {
    if (auto ext = gmdTypeFromString(extensionWithoutDot(m_path).c_str())) {
        m_type = ext.value();
        return true;
    }
    return false;
}

ImportGmdFile& ImportGmdFile::inferType() {
    if (auto ext = gmdTypeFromString(extensionWithoutDot(m_path).c_str())) {
        m_type = ext.value();
    } else {
        m_type = DEFAULT_GMD_TYPE;
    }
    return *this;
}

ImportGmdFile ImportGmdFile::from(std::filesystem::path const& path) {
    return ImportGmdFile(path);
}

ImportGmdFile& ImportGmdFile::setImportSong(bool song) {
    m_importSong = song;
    return *this;
}

geode::Result<std::string> ImportGmdFile::getLevelData() const {
    if (!m_type) {
        return Err(
            "No file type set; either it couldn't have been inferred from the "
            "file or the developer of the mod forgot to call inferType"
        );
    }

    // keep alive for the duration of every SDK call below
    auto const pathStr = pathToUtf8(m_path);

    switch (m_type.value()) {
        case GmdFileType::Gmd: {
            return file::readString(pathStr.c_str()).mapErr([&pathStr](std::string err) {
                return fmt::format("Unable to read {}: {}", pathStr, err);
            });
        } break;
    
        case GmdFileType::Lvl: {
            GEODE_UNWRAP_INTO(auto data, file::readBinary(pathStr.c_str()));
            unsigned char* unzippedData;
            auto count = ZipUtils::ccInflateMemory(
                data.data(),
                data.size(),
                &unzippedData
            );
            if (!count) {
                return Err("Unable to decompress level data");
            }
            auto str = std::string(
                reinterpret_cast<const char*>(unzippedData),
                count
            );
            free(unzippedData);
            return Ok(str);
        } break;

        case GmdFileType::Gmd2: {
            try {
                // Read the whole archive into memory and open it from bytes.
                // `fileData` must outlive `unzip`, so keep it in this scope.
                GEODE_UNWRAP_INTO(
                    auto fileData, file::readBinary(pathStr.c_str())
                        .mapErr([](std::string err) { return fmt::format("Unable to read file: {}", err); })
                );

                GEODE_UNWRAP_INTO(
                    auto unzip, file::Unzip::create(fileData)
                        .mapErr([](std::string err) { return fmt::format("Unable to read file: {}", err); })
                );

                // read metadata
                GEODE_UNWRAP_INTO(
                    auto jsonData, unzip.extract("level.meta")
                        .mapErr([](std::string err) { return fmt::format("Unable to read metadata: {}", err); })
                );

                GEODE_UNWRAP_INTO(
                    auto json, matjson::parse(std::string(jsonData.begin(), jsonData.end()))
                        .mapErr([](std::string err) { return fmt::format("Unable to parse metadata: {}", err); })
                );

                JsonExpectedValue root(json, "[level.meta]");
                
                // unzip song
                std::string songFile;
                root.has("song-file").into(songFile);
                if (m_importSong && songFile.size()) {
                    // make sure the song file name is legit. without this check 
                    // it's possible to do arbitary code execution with gmd2
                    if (!verifySongFileName(songFile)) {
                        return Err("Song file name '{}' is invalid!", songFile);
                    }

                    GEODE_UNWRAP_INTO(
                        auto songData, unzip.extract(songFile.c_str())
                            .mapErr([](std::string err) { return fmt::format("Unable to read song file: {}", err); })
                    );

                    std::filesystem::path songTargetPath;
                    if (root.has("song-is-custom").get<bool>()) {
                        songTargetPath = std::string(MusicDownloadManager::sharedState()->pathForSong(
                            std::stoi(songFile.substr(0, songFile.find_first_of(".")))
                        ));
                    } else {
                        songTargetPath = "Resources/" + songFile;
                    }

                    // if we're replacing a file, figure out a different name 
                    // for the old one
                    std::filesystem::path oldSongPath = songTargetPath;
                    while (std::filesystem::exists(oldSongPath)) {
                        // @geode-ignore(unknown-resource)
                        oldSongPath.replace_filename(oldSongPath.stem().string() + "_.mp3");
                    }
                    if (std::filesystem::exists(oldSongPath)) {
                        std::filesystem::rename(songTargetPath, oldSongPath);
                    }

                    auto const songTargetStr = pathToUtf8(songTargetPath);
                    (void)file::writeBinary(songTargetStr.c_str(), songData);
                }

                GEODE_UNWRAP_INTO(
                    auto levelData, unzip.extract("level.data")
                        .mapErr([](std::string err) { return fmt::format("Unable to read level data: {}", err); })
                );

                return Ok(std::string(levelData.begin(), levelData.end()));
            } catch(std::exception& e) {
                return Err("Unable to read zip: " + std::string(e.what()));
            }
        } break;

        default: {
            return Err("Unknown file type");
        } break;
    }
}

geode::Result<GJGameLevel*> ImportGmdFile::intoLevel() const {
    GEODE_UNWRAP_INTO(auto value, getLevelData());

    auto isOldFile = handlePlistDataForParsing(value);

    auto dict = std::make_unique<DS_Dictionary>();
    if (!dict.get()->loadRootSubDictFromString(value)) {
        return Err("Unable to parse level data");
    }
    dict->stepIntoSubDictWithKey("root");

    auto level = GJGameLevel::create();
    level->dataLoaded(dict.get());

    level->m_isEditable = true;
    level->m_levelType = GJLevelType::Editor;
    level->m_stars = 0;
    level->m_demon = 0;
    level->m_dailyID = 0;
    level->m_gauntletLevel = false;
    level->m_gauntletLevel2 = false;

    // old gdshare double base64 encoded the description,
    // so we decode it again
    if (isOldFile && level->m_levelDesc.size()) {
        if(auto res = base64::decodeString(level->m_levelDesc)) {
            level->m_levelDesc = res.unwrap();
        }
    }

    // this is required for supporting pre-1.9 gmds
    if(!level->m_levelString.size()) {
        level->m_levelString = dict.get()->getStringForKey("k4");
    }

    return Ok(level);
}

ExportGmdFile::ExportGmdFile(GJGameLevel* level) : m_level(level) {}

ExportGmdFile ExportGmdFile::from(GJGameLevel* level) {
    return ExportGmdFile(level);
}

geode::Result<std::string> ExportGmdFile::getLevelData() const {
    if (!m_level) {
        return Err("No level set");
    }
    auto dict = std::make_unique<DS_Dictionary>();
    m_level->encodeWithCoder(dict.get());
    auto data = dict->saveRootSubDictToString();
    return Ok(std::string(data));
}

ExportGmdFile& ExportGmdFile::setIncludeSong(bool song) {
    m_includeSong = song;
    return *this;
}

geode::Result<ByteVector> ExportGmdFile::intoBytes() const {
    if (!m_type) {
        return Err(
            "No file type set; seems like the developer of the mod "
            "forgot to set it"
        );
    }
    switch (m_type.value()) {
        case GmdFileType::Gmd: {
            GEODE_UNWRAP_INTO(auto data, this->getLevelData());
            return Ok(ByteVector(data.begin(), data.end()));
        } break;

        case GmdFileType::Lvl: {
            GEODE_UNWRAP_INTO(auto data, this->getLevelData());
            unsigned char* zippedData = nullptr;
            auto count = ZipUtils::ccDeflateMemory(
                reinterpret_cast<unsigned char*>(data.data()),
                data.size(),
                &zippedData,
                DEFLATE_LEVEL
            );
            if (!count) {
                return Err("Unable to compress level data");
            }
            auto bytes = ByteVector(
                reinterpret_cast<uint8_t*>(zippedData), 
                reinterpret_cast<uint8_t*>(zippedData + count)
            );
            if (zippedData) {
                delete[] zippedData;
            }
            return Ok(bytes);
        } break;

        case GmdFileType::Gmd2: {
            GEODE_UNWRAP_INTO(auto data, this->getLevelData());
            GEODE_UNWRAP_INTO(auto zip, file::Zip::create());

            auto json = matjson::Value();
            if (m_includeSong) {
                auto path = std::filesystem::path(std::string(m_level->getAudioFileName()));
                auto const pathStr = pathToUtf8(path);
                auto const fileName = pathToUtf8(path.filename());

                json["song-file"] = fileName;
                json["song-is-custom"] = m_level->m_songID;

                // zip.addFrom(path) takes a PathView now, so read the song
                // ourselves and add it as bytes instead
                GEODE_UNWRAP_INTO(auto songData, file::readBinary(pathStr.c_str()));
                GEODE_UNWRAP(zip.add(fileName.c_str(), songData));
            }
            GEODE_UNWRAP(zip.add("level.meta", json.dump()));
            GEODE_UNWRAP(zip.add("level.data", data));

            return Ok(zip.getData());
        } break;

        default: {
            return Err("Unknown file type");
        } break;
    }
}

geode::Result<> ExportGmdFile::intoFile(std::filesystem::path const& path) const {
    GEODE_UNWRAP_INTO(auto data, this->intoBytes());
    auto const pathStr = pathToUtf8(path);
    GEODE_UNWRAP(file::writeBinary(pathStr.c_str(), data));
    return Ok();
}

geode::Result<> gmd::exportLevelAsGmd(
    GJGameLevel* level,
    std::filesystem::path const& to,
    GmdFileType type
) {
    return ExportGmdFile::from(level).setType(type).intoFile(to);
}

geode::Result<GJGameLevel*> gmd::importGmdAsLevel(std::filesystem::path const& from) {
    return ImportGmdFile::from(from).inferType().intoLevel();
}

GmdFileKind gmd::getGmdFileKind(std::filesystem::path const& path) {
    auto ext = extensionWithoutDot(path);
    if (gmdListTypeFromString(ext.c_str())) {
        return GmdFileKind::List;
    }
    if (gmdTypeFromString(ext.c_str())) {
        return GmdFileKind::Level;
    }
    return GmdFileKind::None;
}
