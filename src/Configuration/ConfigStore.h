#pragma once

#include "Configuration/AppConfig.h"
#include "Core/Json.h"

#include <filesystem>
#include <string>

namespace aiie {

// Persistent configuration. Stored as JSON (UTF-8) at
// %APPDATA%\AIInputEngine\config.json. Writes are atomic (temp + rename).
// The config file is the single source of truth shared by the tray app, the
// hook engine, and the TSF text service (which reloads it on activation).
class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path path = DefaultPath());

    static std::filesystem::path DefaultPath();
    static std::filesystem::path DefaultLogDir();

    // Returns false (and leaves config_ untouched) on parse failure.
    bool Load();
    bool Save() const;

    const AppConfig& Get() const { return config_; }
    void Set(AppConfig config) { config_ = std::move(config); }

private:
    std::filesystem::path path_;
    AppConfig config_;

    static AppConfig FromJson(const json::Value& root);
    static json::Value ToJson(const AppConfig& config);
};

} // namespace aiie