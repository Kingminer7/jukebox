#include "index_manager.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include <matjson.hpp>
#include "Geode/loader/Event.hpp"
#include "Geode/loader/Log.hpp"
#include "Geode/utils/Result.hpp"
#include "Geode/utils/general.hpp"
#include "Geode/utils/web.hpp"

#include "../../include/nong.hpp"
#include "../events/song_download_progress_event.hpp"
#include "../events/song_error_event.hpp"
#include "../events/song_state_changed_event.hpp"
#include "../index/index_serialize.hpp"
#include "../ui/indexes_setting.hpp"
#include "nong_manager.hpp"

namespace jukebox {

using namespace jukebox::index;

bool IndexManager::init() {
    if (m_initialized) {
        return true;
    }

    std::filesystem::path path = this->baseIndexesPath();
    if (!std::filesystem::exists(path)) {
        std::filesystem::create_directory(path);
        return true;
    }

    if (std::string err = this->fetchIndexes().error(); !err.empty()) {
        SongErrorEvent(false, "Failed to fetch indexes: {}", err).post();
        return false;
    }

    m_initialized = true;
    return true;
}

Result<std::vector<IndexSource>> IndexManager::getIndexes() {
    auto setting = Mod::get()->getSettingValue<Indexes>("indexes");
    log::info("Indexes: {}", setting.indexes.size());
    for (const auto index : setting.indexes) {
        log::info("Index({}): {}", index.m_enabled, index.m_url);
    }
    return Ok(setting.indexes);
}

std::filesystem::path IndexManager::baseIndexesPath() {
    static std::filesystem::path path =
        Mod::get()->getSaveDir() / "indexes-cache";
    return path;
}

Result<> IndexManager::loadIndex(std::filesystem::path path) {
    if (!std::filesystem::exists(path)) {
        return Err("Index file does not exist");
    }

    std::ifstream input(path);
    if (!input.is_open()) {
        return Err(
            fmt::format("Couldn't open file: {}", path.filename().string()));
    }

    std::string contents;
    input.seekg(0, std::ios::end);
    contents.resize(input.tellg());
    input.seekg(0, std::ios::beg);
    input.read(&contents[0], contents.size());
    input.close();

    std::string error;
    std::optional<matjson::Value> jsonRes = matjson::parse(contents, error);

    if (!jsonRes.has_value()) {
        return Err(error);
    }

    matjson::Value jsonObj = jsonRes.value();

    Result<IndexMetadata> indexRes =
        matjson::Serialize<IndexMetadata>::from_json(jsonObj);

    if (indexRes.isErr()) {
        return Err(indexRes.error());
    }

    std::unique_ptr<IndexMetadata> index =
        std::make_unique<IndexMetadata>(indexRes.unwrap());

    this->cacheIndexName(index->m_id, index->m_name);

    for (const auto& [key, ytNong] : jsonObj["nongs"]["youtube"].as_object()) {
        Result<IndexSongMetadata> r =
            matjson::Serialize<IndexSongMetadata>::from_json(ytNong);
        if (r.isErr()) {
            log::error("{}", r.error());
            continue;
        }
        std::unique_ptr<IndexSongMetadata> song =
            std::make_unique<IndexSongMetadata>(r.unwrap());
        song->uniqueID = key;
        song->parentID = index.get();

        for (int gdSongID : song->songIDs) {
            if (!m_indexNongs.contains(gdSongID)) {
                m_indexNongs.emplace(gdSongID, Nongs(gdSongID));
            }
            if (auto err = m_indexNongs.at(gdSongID).add(YTSong(
                    SongMetadata(gdSongID, key, ytNong["name"].as_string(),
                                 ytNong["artist"].as_string(), std::nullopt,
                                 ytNong.contains("startOffset")
                                     ? ytNong["startOffset"].as_int()
                                     : 0),
                    ytNong["ytID"].as_string(), index->m_id, std::nullopt));
                err.isErr()) {
                SongErrorEvent(false, "Failed to add YT song from index: {}",
                               err.error())
                    .post();
            }
        }
    }

    for (const auto& [key, hostedNong] :
         jsonObj["nongs"]["hosted"].as_object()) {
        const std::vector<matjson::Value>& gdSongIDs =
            hostedNong["songs"].as_array();
        for (const matjson::Value& gdSongIDValue : gdSongIDs) {
            int gdSongID = gdSongIDValue.as_int();
            if (!m_indexNongs.contains(gdSongID)) {
                m_indexNongs.emplace(gdSongID, Nongs(gdSongID));
            }
            if (auto err = m_indexNongs.at(gdSongID).add(HostedSong(
                    SongMetadata(gdSongID, key, hostedNong["name"].as_string(),
                                 hostedNong["artist"].as_string(), std::nullopt,
                                 hostedNong.contains("startOffset")
                                     ? hostedNong["startOffset"].as_int()
                                     : 0),
                    hostedNong["url"].as_string(), index->m_id, std::nullopt));
                err.isErr()) {
                SongErrorEvent(false,
                               "Failed to add Hosted song from index: {}",
                               err.error())
                    .post();
            }
        }
    }

    IndexMetadata* ref = index.get();

    m_loadedIndexes.emplace(ref->m_id, std::move(index));

    log::info("Index \"{}\" ({}) loaded. Total index objects: {}.", ref->m_name,
              ref->m_id, m_indexNongs.size());

    return Ok();
}

Result<> IndexManager::fetchIndexes() {
    m_indexListeners.clear();
    m_indexNongs.clear();
    m_downloadSongListeners.clear();

    const Result<std::vector<IndexSource>> indexesRes = this->getIndexes();
    if (indexesRes.isErr()) {
        return Err(indexesRes.error());
    }
    const std::vector<IndexSource> indexes = std::move(indexesRes.unwrap());

    for (const IndexSource& index : indexes) {
        log::info("Fetching index {}", index.m_url);
        if (!index.m_enabled || index.m_url.size() < 3) {
            continue;
        }

        // Hash url to use as a filename per index
        std::hash<std::string> hasher;
        std::size_t hashValue = hasher(index.m_url);
        std::stringstream hashStream;
        hashStream << std::hex << hashValue;

        std::filesystem::path filepath =
            this->baseIndexesPath() / fmt::format("{}.json", hashStream.str());

        FetchIndexTask task =
            web::WebRequest()
                .timeout(std::chrono::seconds(30))
                .get(index.m_url)
                .map(
                    [this, filepath, index](
                        web::WebResponse* response) -> FetchIndexTask::Value {
                        if (response->ok() && response->string().isOk()) {
                            std::string error;
                            std::optional<matjson::Value> jsonObj =
                                matjson::parse(response->string().value(),
                                               error);

                            if (!jsonObj.has_value()) {
                                return Err(error);
                            }

                            if (!jsonObj.value().is_object()) {
                                return Err("Index supposed to be an object");
                            }
                            jsonObj.value().set("url", index.m_url);
                            const auto indexRes =
                                matjson::Serialize<IndexMetadata>::from_json(
                                    jsonObj.value());

                            if (indexRes.isErr()) {
                                return Err(indexRes.error());
                            }

                            std::ofstream output(filepath);
                            if (!output.is_open()) {
                                return Err(fmt::format("Couldn't open file: {}",
                                                       filepath));
                            }
                            output << jsonObj.value().dump(
                                matjson::NO_INDENTATION);
                            output.close();

                            return Ok();
                        }
                        return Err("Web request failed");
                    },
                    [](web::WebProgress* progress) -> FetchIndexTask::Progress {
                        return progress->downloadProgress().value_or(0) / 100.f;
                    });

        auto listener = EventListener<FetchIndexTask>();
        listener.bind([this, index, filepath](FetchIndexTask::Event* event) {
            if (float* progress = event->getProgress()) {
                return;
            }

            m_indexListeners.erase(index.m_url);

            if (FetchIndexTask::Value* result = event->getValue()) {
                if (result->isErr()) {
                    SongErrorEvent(false, "Failed to fetch index: {}",
                                   result->error())
                        .post();
                } else {
                    log::info("Index fetched and cached: {}", index.m_url);
                }
            } else if (event->isCancelled()) {
            }

            if (auto err = this->loadIndex(filepath).error(); !err.empty()) {
                SongErrorEvent(false, "Failed to load index: {}", err).post();
            }
        });
        listener.setFilter(task);
        m_indexListeners.emplace(index.m_url, std::move(listener));
    }

    return Ok();
}

std::optional<float> IndexManager::getSongDownloadProgress(
    const std::string& uniqueID) {
    if (m_downloadSongListeners.contains(uniqueID)) {
        return m_downloadProgress.at(uniqueID);
    }
    return std::nullopt;
}

std::optional<std::string> IndexManager::getIndexName(
    const std::string& indexID) {
    auto jsonObj =
        Mod::get()->getSavedValue<matjson::Value>("cached-index-names");
    if (!jsonObj.contains(indexID)) {
        return std::nullopt;
    }
    return jsonObj[indexID].as_string();
}

void IndexManager::cacheIndexName(const std::string& indexId,
                                  const std::string& indexName) {
    auto jsonObj =
        Mod::get()->getSavedValue<matjson::Value>("cached-index-names", {});
    jsonObj.set(indexId, indexName);
    Mod::get()->setSavedValue("cached-index-names", jsonObj);
}

Result<std::vector<Song*>> IndexManager::getNongs(int gdSongID) {
    std::vector<Song*> nongs;
    std::optional<Nongs*> opt = NongManager::get().getNongs(gdSongID);
    if (!opt.has_value()) {
        return Err("Failed to get nongs");
    }

    Nongs* localNongs = opt.value();

    std::optional<Nongs*> indexNongs =
        m_indexNongs.contains(gdSongID)
            ? std::optional(&m_indexNongs.at(gdSongID))
            : std::nullopt;

    nongs.push_back(localNongs->defaultSong());

    for (std::unique_ptr<LocalSong>& song : localNongs->locals()) {
        nongs.push_back(song.get());
    }

    std::vector<std::string> addedIndexSongs;
    for (std::unique_ptr<YTSong>& song : localNongs->youtube()) {
        // Check if song is from an index
        if (indexNongs.has_value() && song->indexID().has_value()) {
            for (std::unique_ptr<YTSong>& indexSong :
                 indexNongs.value()->youtube()) {
                if (song->metadata()->uniqueID ==
                    indexSong->metadata()->uniqueID) {
                    addedIndexSongs.push_back(song->metadata()->uniqueID);
                }
            }
        }
        nongs.push_back(song.get());
    }

    for (std::unique_ptr<HostedSong>& song : localNongs->hosted()) {
        // Check if song is from an index
        if (indexNongs.has_value() && song->indexID().has_value()) {
            for (std::unique_ptr<HostedSong>& indexSong :
                 indexNongs.value()->hosted()) {
                if (song->metadata()->uniqueID ==
                    indexSong->metadata()->uniqueID) {
                    addedIndexSongs.push_back(song->metadata()->uniqueID);
                }
            }
        }
        nongs.push_back(song.get());
    }

    if (indexNongs.has_value()) {
        for (std::unique_ptr<YTSong>& song : indexNongs.value()->youtube()) {
            // Check if song is not already added
            if (std::find(addedIndexSongs.begin(), addedIndexSongs.end(),
                          song->metadata()->uniqueID) ==
                addedIndexSongs.end()) {
                nongs.push_back(song.get());
            }
        }

        for (std::unique_ptr<HostedSong>& song : indexNongs.value()->hosted()) {
            // Check if song is not already added
            if (std::find(addedIndexSongs.begin(), addedIndexSongs.end(),
                          song->metadata()->uniqueID) ==
                addedIndexSongs.end()) {
                nongs.push_back(song.get());
            }
        }
    }

    std::unordered_map<NongType, int> sortedNongType = {
        {NongType::LOCAL, 1}, {NongType::HOSTED, 2}, {NongType::YOUTUBE, 3}};

    std::sort(
        nongs.begin(), nongs.end(),
        [&sortedNongType,
         defaultUniqueID = localNongs->defaultSong()->metadata()->uniqueID](
            const Song* a, const Song* b) {
            std::optional<std::string> aIndexID = std::nullopt;
            std::optional<std::string> bIndexID = std::nullopt;

            // Place the object with isDefault == true at the front
            if (a->metadata()->uniqueID == defaultUniqueID) {
                return true;
            }
            if (b->metadata()->uniqueID == defaultUniqueID) {
                return false;
            }

            // Next, those without an index
            if (!a->indexID().has_value() && b->indexID().has_value()) {
                return true;
            }
            if (a->indexID().has_value() && !b->indexID().has_value()) {
                return false;
            }

            // Next, compare whether path exists or not
            if (a->path().has_value() &&
                std::filesystem::exists(a->path().value())) {
                if (!b->path().has_value() ||
                    !std::filesystem::exists(b->path().value())) {
                    return true;
                }
            } else if (b->path().has_value() &&
                       std::filesystem::exists(b->path().value())) {
                return false;
            }

            // Next, compare by type
            if (a->type() != b->type()) {
                return sortedNongType.at(a->type()) <
                       sortedNongType.at(b->type());
            }

            // Next, compare whether indexID exists or not (std::nullopt
            // should be first)
            if (a->indexID().has_value() != b->indexID().has_value()) {
                return !a->indexID().has_value() && b->indexID().has_value();
            }

            // Finally, compare by name
            return a->metadata()->name < b->metadata()->name;
        });

    return Ok(std::move(nongs));
}

Result<> IndexManager::downloadSong(int gdSongID, const std::string& uniqueID) {
    Result<std::vector<Song*>> nongs = IndexManager::get().getNongs(gdSongID);
    if (!nongs.has_value()) {
        return Err("GD song {} not initialized in manifest", gdSongID);
    }
    for (Song* nong : nongs.value()) {
        if (nong->metadata()->uniqueID == uniqueID) {
            return this->downloadSong(nong);
        }
    }

    return Err("Song {} not found in manifest", uniqueID);
}

Result<> IndexManager::downloadSong(Song* nong) {
    if (nong->type() == NongType::LOCAL) {
        return Err("Can't download local song");
    }
    const std::string id = nong->metadata()->uniqueID;
    int gdSongID = nong->metadata()->gdID;

    if (m_downloadSongListeners.contains(id)) {
        m_downloadSongListeners.at(id).getFilter().cancel();
    }
    DownloadSongTask task;

    if (nong->type() == NongType::YOUTUBE) {  // yt
        EventListener<web::WebTask>* cobaltMetadataListener =
            new EventListener<web::WebTask>();
        EventListener<web::WebTask>* cobaltSongListener =
            new EventListener<web::WebTask>();

        YTSong* yt = static_cast<YTSong*>(nong);

        task = DownloadSongTask::runWithCallback(
            [this, yt, cobaltMetadataListener, cobaltSongListener](
                utils::MiniFunction<void(DownloadSongTask::Value)> finish,
                utils::MiniFunction<void(DownloadSongTask::Progress)> progress,
                utils::MiniFunction<bool()> hasBeenCancelled) {
                if (yt->youtubeID().length() != 11) {
                    return finish(Err("Invalid YouTube ID"));
                }

                std::function<void(std::string)> finishErr =
                    [finish, cobaltMetadataListener,
                     cobaltSongListener](std::string err) {
                        delete cobaltSongListener;
                        delete cobaltMetadataListener;
                        finish(Err(err));
                    };

                cobaltMetadataListener->bind([this, hasBeenCancelled,
                                              cobaltMetadataListener,
                                              cobaltSongListener, yt, finishErr,
                                              finish](
                                                 web::WebTask::Event* event) {
                    if (hasBeenCancelled() || event->isCancelled()) {
                        return finishErr(
                            "Cancelled while fetching song metadata from "
                            "Cobalt");
                    }

                    if (event->getProgress() != nullptr) {
                        float progress =
                            event->getProgress()->downloadProgress().value_or(
                                0) /
                            1000.f;
                        m_downloadProgress[yt->metadata()->uniqueID] = progress;
                        SongDownloadProgressEvent(yt->metadata()->gdID,
                                                  yt->metadata()->uniqueID,
                                                  progress)
                            .post();
                        return;
                    }

                    if (event->getValue() == nullptr) {
                        return;
                    }

                    if (!event->getValue()->ok() ||
                        !event->getValue()->json().isOk()) {
                        return finishErr(
                            "Unable to get/parse Cobalt metadata response");
                    }

                    matjson::Value jsonObj = event->getValue()->json().unwrap();

                    if (!jsonObj.contains("status") ||
                        jsonObj["status"] != "stream") {
                        return finishErr(
                            "Cobalt metadata response is not a stream");
                    }

                    if (!jsonObj.contains("url") ||
                        !jsonObj["url"].is_string()) {
                        return finishErr("Cobalt metadata bad response");
                    }

                    std::string audio_url = jsonObj["url"].as_string();
                    log::info("Cobalt metadata response: {}", audio_url);

                    cobaltSongListener->bind(
                        [this, hasBeenCancelled, cobaltMetadataListener,
                         cobaltSongListener, finishErr, yt,
                         finish](web::WebTask::Event* event) {
                            if (hasBeenCancelled() || event->isCancelled()) {
                                return finishErr(
                                    "Cancelled while fetching song data from "
                                    "Cobalt");
                            }

                            if (event->getProgress() != nullptr) {
                                float progress = event->getProgress()
                                                         ->downloadProgress()
                                                         .value_or(0) /
                                                     100.f * 0.9f +
                                                 0.1f;
                                m_downloadProgress[yt->metadata()->uniqueID] =
                                    progress;
                                SongDownloadProgressEvent(
                                    yt->metadata()->gdID,
                                    yt->metadata()->uniqueID, progress)
                                    .post();
                                return;
                            }

                            if (event->getValue() == nullptr) {
                                return;
                            }

                            if (!event->getValue()->ok()) {
                                return finishErr(
                                    "Unable to get Cobalt song response");
                            }

                            ByteVector data = event->getValue()->data();

                            auto destination =
                                NongManager::get().generateSongFilePath("mp3");
                            std::ofstream file(
                                destination, std::ios::out | std::ios::binary);
                            file.write(
                                reinterpret_cast<const char*>(data.data()),
                                data.size());
                            file.close();

                            delete cobaltSongListener;
                            delete cobaltMetadataListener;
                            finish(Ok(destination));
                        });

                    cobaltSongListener->setFilter(
                        web::WebRequest()
                            .timeout(std::chrono::seconds(30))
                            .get(audio_url));
                });

                cobaltMetadataListener->setFilter(
                    web::WebRequest()
                        .timeout(std::chrono::seconds(30))
                        .bodyJSON(matjson::Object{
                            {"url",
                             fmt::format("https://www.youtube.com/watch?v={}",
                                         yt->youtubeID())},
                            {"aFormat", "mp3"},
                            {"isAudioOnly", "true"}})
                        .header("Accept", "application/json")
                        .header("Content-Type", "application/json")
                        .post("https://api.cobalt.tools/api/json"));
            },
            "Download a YouTube song from Cobalt");
    } else {  // hosted
        HostedSong* hosted = static_cast<HostedSong*>(nong);
        task =
            web::WebRequest()
                .timeout(std::chrono::seconds(30))
                .get(hosted->url())
                .map(
                    [this](
                        web::WebResponse* response) -> DownloadSongTask::Value {
                        if (response->ok()) {
                            std::filesystem::path destination =
                                NongManager::get().generateSongFilePath("mp3");
                            std::ofstream file(
                                destination, std::ios::out | std::ios::binary);
                            file.write(reinterpret_cast<const char*>(
                                           response->data().data()),
                                       response->data().size());
                            file.close();

                            return Ok(destination);
                        }
                        return Err("Web request failed");
                    },
                    [](web::WebProgress* progress)
                        -> DownloadSongTask::Progress {
                        return progress->downloadProgress().value_or(0) / 100.f;
                    });
    }

    auto listener = EventListener<DownloadSongTask>();

    listener.bind([this, gdSongID, id, nong](DownloadSongTask::Event* event) {
        if (float* progress = event->getProgress()) {
            m_downloadProgress[id] = *progress;
            SongDownloadProgressEvent(gdSongID, id, *event->getProgress())
                .post();
            return;
        }
        m_downloadProgress.erase(id);
        m_downloadSongListeners.erase(id);
        if (event->isCancelled()) {
            SongErrorEvent(false, "Failed to fetch song: cancelled").post();
            SongStateChangedEvent(gdSongID).post();
            return;
        }
        DownloadSongTask::Value* result = event->getValue();
        if (result->isErr()) {
            SongErrorEvent(true, "Failed to fetch song: {}", result->error())
                .post();
            SongStateChangedEvent(gdSongID).post();
            return;
        }

        if (result->value().string().size() == 0) {
            SongStateChangedEvent(gdSongID).post();
            return;
        }

        nong->setIndexID(id);

        if (auto res = NongManager::get().setActiveSong(gdSongID, id);
            res.isErr()) {
            SongErrorEvent(true, "Failed to set song as active: {}",
                           res.error())
                .post();
            SongStateChangedEvent(gdSongID).post();
            return;
        }

        SongStateChangedEvent(gdSongID).post();
    });
    listener.setFilter(task);
    m_downloadSongListeners.emplace(id, std::move(listener));
    m_downloadProgress[id] = 0.f;
    SongDownloadProgressEvent(gdSongID, id, 0.f).post();
    return Ok();
}

};  // namespace jukebox
