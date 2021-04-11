#include "stdafx.h"

#include "logging.h"
#include "lyric_source.h"
#include "preferences.h"
#include "winstr_util.h"

static std::string GetLyricsDir()
{
    std::string lyric_dir_path(core_api::get_profile_path());
    lyric_dir_path += "\\lyrics\\";
    return lyric_dir_path;
}

static const GUID src_guid = { 0x76d90970, 0x1c98, 0x4fe2, { 0x94, 0x4e, 0xac, 0xe4, 0x93, 0xf3, 0x8e, 0x85 } };

class LocalFileSource : public LyricSourceBase
{
    const GUID& id() const final { return src_guid; }
    const TCHAR* friendly_name() const final { return _T("Configuration Folder Files"); }
    bool can_save() const final { return true; }

    LyricDataRaw query(metadb_handle_ptr track, abort_callback& abort) final;
    std::string save(metadb_handle_ptr track, bool is_timestamped, std::string_view lyrics, abort_callback& abort) final;
};
static const LyricSourceFactory<LocalFileSource> src_factory;

static bool ComputeFileTitle(metadb_handle_ptr track, std::string& out_title)
{
    const char* save_format = preferences::saving::filename_format();
    titleformat_object::ptr format_script;
    bool compile_success = titleformat_compiler::get()->compile(format_script, save_format);
    if (!compile_success)
    {
        return false;
    }

    pfc::string8 save_file_title;
    bool format_success = track->format_title(nullptr, save_file_title, format_script, nullptr);
    save_file_title.fix_filename_chars();
    out_title = std::string(save_file_title.c_str(), save_file_title.length());

    return format_success;
}

LyricDataRaw LocalFileSource::query(metadb_handle_ptr track, abort_callback& abort)
{
    std::string file_title;
    if(!ComputeFileTitle(track, file_title))
    {
        LOG_ERROR("Failed to determine query file title");
        return {};
    }
    LOG_INFO("Querying for lyrics in local files for %s...", file_title.c_str());

    std::string lyric_path_prefix = GetLyricsDir();
    lyric_path_prefix += file_title;

    LyricDataRaw result = {};
    result.source_id = id();
    result.persistent_storage_path = lyric_path_prefix;

    // TODO: LyricShow3 has a "Choose Lyrics" and "Next Lyrics" option...if we have .txt and .lrc we should possibly communicate that?
    // TODO: Should these extensions be configurable?
    const char* extensions[] = { ".lrc", ".txt" };
    for (const char* ext : extensions)
    {
        std::string file_path = lyric_path_prefix;
        file_path += ext;
        LOG_INFO("Querying for lyrics from %s...", file_path.c_str());

        try
        {
            if (filesystem::g_exists(file_path.c_str(), abort))
            {
                file_ptr file;
                filesystem::g_open_read(file, file_path.c_str(), abort);

                pfc::string8 file_contents;
                file->read_string_raw(file_contents, abort);

                result.persistent_storage_path = file_path;
                result.text = file_contents;
                LOG_INFO("Successfully retrieved lyrics from %s", file_path.c_str());
                return result;
            }
        }
        catch(const std::exception& e)
        {
            LOG_WARN("Failed to open lyrics file %s: %s", file_path.c_str(), e.what());
        }
    }

    LOG_INFO("Failed to find lyrics in local files for %s", file_title.c_str());
    return result;
}

std::string LocalFileSource::save(metadb_handle_ptr track, bool is_timestamped, std::string_view lyrics, abort_callback& abort)
{
    LOG_INFO("Saving lyrics to a local file...");
    std::string save_file_title;
    if(!ComputeFileTitle(track, save_file_title))
    {
        throw std::exception("Failed to determine save file title");
    }

    std::string output_dir = GetLyricsDir();
    if(!filesystem::g_exists(output_dir.c_str(), abort))
    {
        LOG_INFO("Lyrics directory %s does not exist. Creating it...", output_dir.c_str());
        filesystem::g_create_directory(output_dir.c_str(), abort);
    }

    std::string output_path = output_dir;
    output_path += save_file_title.c_str();

    const char* extension = is_timestamped ? ".lrc" : ".txt";
    output_path += extension;
    LOG_INFO("Saving lyrics to %s...", output_path.c_str());

    TCHAR temp_path_str[MAX_PATH+1];
    DWORD temp_path_str_len = GetTempPath(MAX_PATH+1, temp_path_str);
    std::string tmp_path = tchar_to_string(temp_path_str, temp_path_str_len);
    tmp_path += save_file_title;

    {
        // NOTE: Scoping to close the file and flush writes to disk (hopefully preventing "file in use" errors)
        file_ptr tmp_file;
        filesystem::g_open_write_new(tmp_file, tmp_path.c_str(), abort);
        tmp_file->write_object(lyrics.data(), lyrics.size(), abort);
    }

    service_ptr_t<filesystem> fs = filesystem::get(output_path.c_str());
    if(fs->is_our_path(tmp_path.c_str()))
    {
        fs->move_overwrite(tmp_path.c_str(), output_path.c_str(), abort);
        LOG_INFO("Successfully saved lyrics to %s", output_path.c_str());
    }
    else
    {
        LOG_WARN("Cannot save lyrics file. Temp path (%s) and output path (%s) are on different filesystems", tmp_path.c_str(), output_path.c_str());
    }

    return output_path;
}

