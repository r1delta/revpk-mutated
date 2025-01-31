#ifndef __TYTI_STEAM_VDF_PARSER_H__
#define __TYTI_STEAM_VDF_PARSER_H__

#include <string>
#include <unordered_map>
#include <memory>
#include <fstream>
#include <sstream>
#include <vector>
#include <stdexcept>
#include <system_error>

namespace tyti
{
namespace vdf
{

// Options struct, mostly a no-op in this CSV implementation.
struct Options
{
    bool strip_escape_symbols;
    bool ignore_all_platform_conditionals;
    bool ignore_includes;
    Options()
        : strip_escape_symbols(true)
        , ignore_all_platform_conditionals(false)
        , ignore_includes(false)
    {}
};

// WriteOptions struct, also mostly a no-op for CSV.
struct WriteOptions
{
    bool escape_symbols;
    WriteOptions() : escape_symbols(true) {}
};

// Basic object node. The interface remains the same, but we store CSV rows.
template <typename CharT>
struct basic_object
{
    using char_type = CharT;
    std::basic_string<char_type> name;
    // For each node, we have key->value pairs:
    std::unordered_map<std::basic_string<char_type>,
                       std::basic_string<char_type>> attribs;
    // And child nodes:
    std::unordered_map<std::basic_string<char_type>,
                       std::shared_ptr<basic_object<char_type>>> childs;

    void add_attribute(std::basic_string<char_type> key,
                       std::basic_string<char_type> value)
    {
        attribs.emplace(std::move(key), std::move(value));
    }

    void add_child(std::unique_ptr<basic_object<char_type>> child)
    {
        std::shared_ptr<basic_object<char_type>> obj{child.release()};
        childs.emplace(obj->name, obj);
    }

    void set_name(std::basic_string<char_type> n)
    {
        name = std::move(n);
    }
};

using object = basic_object<char>;

// -----------------------------------------------------------------------
// Minimal CSV parse: 
//   - We read the first line as headers. 
//   - If there's a "lang" column, we treat the document as multi-language.
//   - Otherwise, it's single-language. 
//   - We store the CSV row data into child objects (or sub-child objects).
//   - We hard-code "filePath" as the identifying column name for the child.
// -----------------------------------------------------------------------
template <typename iStreamT>
object read(iStreamT &inStream, const Options &opt = Options{})
{
    object root;
    // For compatibility with code that checks doc.name == "BuildManifest":
    root.name = "BuildManifest";

    // 1) Read first line as CSV header
    std::string headerLine;
    if (!std::getline(inStream, headerLine))
        return root; // empty file

    std::vector<std::string> headers;
    {
        std::stringstream ss(headerLine);
        std::string col;
        while (std::getline(ss, col, ','))
            headers.push_back(col);
    }

    // 2) Find indices of "lang" and "filePath" if they exist
    int langCol = -1;
    int filePathCol = -1;
    for (int i = 0; i < (int)headers.size(); i++)
    {
        if (headers[i] == "lang")      langCol = i;
        if (headers[i] == "filePath") filePathCol = i;
    }

    // 3) Parse subsequent lines
    std::string line;
    while (std::getline(inStream, line))
    {
        if (line.empty()) continue;

        // split by comma
        std::vector<std::string> cols;
        {
            std::stringstream ss(line);
            std::string col;
            while (std::getline(ss, col, ','))
                cols.push_back(col);
        }

        // skip if we don't even have filePath
        if (filePathCol < 0 || filePathCol >= (int)cols.size())
            continue;

        const std::string &filePath = cols[filePathCol];

        // 4) If there's a lang column, treat data as multiLang
        if (langCol >= 0 && langCol < (int)cols.size())
        {
            const std::string &language = cols[langCol];

            // find or create the language child
            auto langIt = root.childs.find(language);
            if (langIt == root.childs.end())
            {
                std::unique_ptr<object> newLang(new object());
                newLang->name = language;
                root.add_child(std::move(newLang));
                langIt = root.childs.find(language);
            }
            auto langObj = langIt->second.get();

            // find or create the filePath child
            auto fIt = langObj->childs.find(filePath);
            if (fIt == langObj->childs.end())
            {
                std::unique_ptr<object> newFile(new object());
                newFile->name = filePath;
                langObj->add_child(std::move(newFile));
                fIt = langObj->childs.find(filePath);
            }
            auto fileObj = fIt->second.get();

            // fill in attributes from all other columns
            for (int i = 0; i < (int)cols.size(); i++)
            {
                if (i == langCol || i == filePathCol) 
                    continue; // skip these two "key" columns
                if (i < (int)headers.size())
                    fileObj->attribs[headers[i]] = cols[i];
            }
        }
        else
        {
            // Single-language CSV
            // find or create the filePath child
            auto fIt = root.childs.find(filePath);
            if (fIt == root.childs.end())
            {
                std::unique_ptr<object> newFile(new object());
                newFile->name = filePath;
                root.add_child(std::move(newFile));
                fIt = root.childs.find(filePath);
            }
            auto fileObj = fIt->second.get();

            // fill in attributes from all columns except filePath
            for (int i = 0; i < (int)cols.size(); i++)
            {
                if (i == filePathCol) 
                    continue;
                if (i < (int)headers.size())
                    fileObj->attribs[headers[i]] = cols[i];
            }
        }
    }

    return root;
}

// Overload that sets an error_code on failure; here it always "succeeds."
template <typename iStreamT>
object read(iStreamT &inStream, std::error_code &ec, const Options &opt = Options{})
{
    ec.clear();
    try
    {
        return read(inStream, opt);
    }
    catch (...)
    {
        ec = std::make_error_code(std::errc::invalid_argument);
        return object{};
    }
}

// Overload that sets a bool *ok. Again, always "succeeds" unless an exception.
template <typename iStreamT>
object read(iStreamT &inStream, bool *ok, const Options &opt = Options{})
{
    if (ok) *ok = true;
    try
    {
        return read(inStream, opt);
    }
    catch (...)
    {
        if (ok) *ok = false;
        return object{};
    }
}

// Write a CSV. We check if root has second-level children (meaning multiLang).
template <typename oStreamT>
void write(oStreamT &os, const object &root, const WriteOptions &wopt = WriteOptions{})
{
    // 1) Determine if multiLang: if root.childs[...] has its own children
    bool isMultiLang = false;
    for (auto &childPair : root.childs)
    {
        if (!childPair.second->childs.empty())
        {
            isMultiLang = true;
            break;
        }
    }

    // 2) Collect all attribute names from every file object
    // If multiLang, we look under each language->file
    // If single, we look under root->file
    std::vector<std::string> columns;
    if (isMultiLang)
    {
        columns.push_back("lang");
        columns.push_back("filePath");
    }
    else
    {
        columns.push_back("filePath");
    }

    // Gather attribute keys
    std::unordered_map<std::string, bool> attrMap;
    if (isMultiLang)
    {
        for (auto &langPair : root.childs)
        {
            auto langObj = langPair.second.get();
            for (auto &filePair : langObj->childs)
            {
                auto fileObj = filePair.second.get();
                for (auto &attribPair : fileObj->attribs)
                    attrMap[attribPair.first] = true;
            }
        }
    }
    else
    {
        for (auto &filePair : root.childs)
        {
            auto fileObj = filePair.second.get();
            for (auto &attribPair : fileObj->attribs)
                attrMap[attribPair.first] = true;
        }
    }

    // Add those attributes to columns
    for (auto &am : attrMap)
        columns.push_back(am.first);

    // 3) Write the CSV header
    for (size_t i = 0; i < columns.size(); i++)
    {
        os << columns[i];
        if (i + 1 < columns.size())
            os << ",";
    }
    os << "\n";

    // 4) Write each row
    if (isMultiLang)
    {
        // We have "lang" -> "filePath" -> attributes
        for (auto &langPair : root.childs)
        {
            const std::string &lang = langPair.first;
            auto langObj = langPair.second.get();

            for (auto &filePair : langObj->childs)
            {
                const std::string &filePath = filePair.first;
                auto fileObj = filePair.second.get();

                // build a row of columns.size() columns
                std::vector<std::string> row(columns.size(), "");
                row[0] = lang;
                row[1] = filePath;

                // fill attributes
                for (size_t c = 2; c < columns.size(); c++)
                {
                    auto it = fileObj->attribs.find(columns[c]);
                    if (it != fileObj->attribs.end())
                        row[c] = it->second;
                }

                // write out row
                for (size_t c = 0; c < row.size(); c++)
                {
                    os << row[c];
                    if (c + 1 < row.size())
                        os << ",";
                }
                os << "\n";
            }
        }
    }
    else
    {
        // Single language: root -> file
        for (auto &filePair : root.childs)
        {
            const std::string &filePath = filePair.first;
            auto fileObj = filePair.second.get();

            std::vector<std::string> row(columns.size(), "");
            row[0] = filePath;

            // fill attributes
            for (size_t c = 1; c < columns.size(); c++)
            {
                auto it = fileObj->attribs.find(columns[c]);
                if (it != fileObj->attribs.end())
                    row[c] = it->second;
            }

            for (size_t c = 0; c < row.size(); c++)
            {
                os << row[c];
                if (c + 1 < row.size())
                    os << ",";
            }
            os << "\n";
        }
    }
}

// Overload that takes an output type AND a stream type, e.g. read<tyti::vdf::object>(std::ifstream, ...)
template <typename OutputT, typename iStreamT>
OutputT read(iStreamT &inStream, const Options &opt = Options{})
{
    // For CSV, we always parse into a basic_object<char> shape
    // then return it as the OutputT. Typically, OutputT == basic_object<char>.
    // 1) Create an empty object of the expected type
    OutputT root;
    root.name = "BuildManifest"; // or let parse overwrite if you prefer

    // 2) Actually read lines from the stream
    std::string headerLine;
    if (!std::getline(inStream, headerLine))
        return root; // empty file => empty object

    // CSV parsing for the first row as headers
    std::vector<std::string> headers;
    {
        std::stringstream ss(headerLine);
        std::string col;
        while (std::getline(ss, col, ','))
            headers.push_back(col);
    }

    // detect if we have "lang" or not
    int langCol = -1;
    int filePathCol = -1;
    for (int i = 0; i < (int)headers.size(); ++i)
    {
        if (headers[i] == "lang")      langCol = i;
        if (headers[i] == "filePath") filePathCol = i;
    }

    // read each subsequent CSV row
    std::string line;
    while (std::getline(inStream, line))
    {
        if (line.empty()) continue;
        std::vector<std::string> cols;

        {
            std::stringstream ss(line);
            std::string col;
            while (std::getline(ss, col, ','))
                cols.push_back(col);
        }

        // skip if we don't have a valid filePath column
        if (filePathCol < 0 || filePathCol >= (int)cols.size())
            continue;
        std::string filePath = cols[filePathCol];

        if (langCol >= 0 && langCol < (int)cols.size())
        {
            // multi-language mode
            std::string lang = cols[langCol];

            // find or create language child
            auto langIt = root.childs.find(lang);
            if (langIt == root.childs.end())
            {
                auto newLang = std::make_unique<OutputT>();
                newLang->name = lang;
                root.add_child(std::move(newLang));
                langIt = root.childs.find(lang);
            }
            auto langObj = langIt->second.get();

            // find or create the file child
            auto fileIt = langObj->childs.find(filePath);
            if (fileIt == langObj->childs.end())
            {
                auto newFile = std::make_unique<OutputT>();
                newFile->name = filePath;
                langObj->add_child(std::move(newFile));
                fileIt = langObj->childs.find(filePath);
            }
            auto fileObj = fileIt->second.get();

            // fill attributes from other columns
            for (int i = 0; i < (int)cols.size(); i++)
            {
                if (i == langCol || i == filePathCol) continue;
                if (i < (int)headers.size())
                    fileObj->attribs[headers[i]] = cols[i];
            }
        }
        else
        {
            // single-language mode
            auto fileIt = root.childs.find(filePath);
            if (fileIt == root.childs.end())
            {
                auto newFile = std::make_unique<OutputT>();
                newFile->name = filePath;
                root.add_child(std::move(newFile));
                fileIt = root.childs.find(filePath);
            }
            auto fileObj = fileIt->second.get();

            // fill attributes
            for (int i = 0; i < (int)cols.size(); i++)
            {
                if (i == filePathCol) continue;
                if (i < (int)headers.size())
                    fileObj->attribs[headers[i]] = cols[i];
            }
        }
    }

    return root;
}


} // namespace vdf
} // namespace tyti

#endif // __TYTI_STEAM_VDF_PARSER_H__
