#pragma once

// Saves events to disk so they survive a server restart or crash
// (protocol spec v2, Persistence).
//
// One file per event (party) in the data directory, holding the organizer,
// date and time, every guest with their RSVP, and every item with its
// claimer. Saving writes a temporary file and renames it over the old one, so
// a crash mid-write leaves either the old or the new version, never a broken
// one. File names are the event code in hex: event codes may contain
// characters such as '/', and they are case-sensitive while macOS and Windows
// file names are not.
//
// File format (fields never contain spaces, so each line splits cleanly):
//   PARTYON-EVENT 1
//   code Party1
//   organizer Andrew
//   date 10/31/2026
//   time 07:00PM
//   guest Andrew Yes
//   item Chips Giovanna
//   item Soda                (unclaimed)
//   end

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "AppState.hpp"

class EventStore {
public:
    explicit EventStore(std::filesystem::path directory);

    const std::filesystem::path& directory() const { return directory_; }

    // Creates the directory if needed
    bool open(std::string& error);

    // Every valid event file. Files that cannot be read are skipped and
    // described in `warnings`; leftover temporary files are removed.
    std::vector<Event> loadAll(std::vector<std::string>& warnings) const;

    bool save(const Event& event, std::string& error) const;
    bool remove(const std::string& eventCode, std::string& error) const;

    static std::string fileNameFor(const std::string& eventCode);
    static std::string serialize(const Event& event);
    static std::optional<Event> parse(const std::string& text, std::string& error);

private:
    std::filesystem::path directory_;
};
