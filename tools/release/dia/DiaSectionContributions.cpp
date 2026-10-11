// Build-side reader for locally generated PDB section contributions. This is
// never linked into, registered by, or distributed with the application.
#include <windows.h>
#include <dia2.h>
#include <wrl/client.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace
{
void require(HRESULT result, const char* operation)
{
    if (result != S_OK)
    {
        std::ostringstream message;
        message << operation << " failed: 0x" << std::hex << static_cast<unsigned long>(result);
        throw std::runtime_error(message.str());
    }
}

std::string jsonString(const wchar_t* value, int length)
{
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length, nullptr, 0, nullptr, nullptr);
    if (length && !size) throw std::runtime_error("Invalid Unicode in DIA symbol name");
    std::string text(static_cast<std::size_t>(size), '\0');
    if (size) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length, text.data(), size, nullptr, nullptr);
    std::ostringstream result;
    result << '"';
    for (const unsigned char c : text)
    {
        if (c == '"' || c == '\\') result << '\\' << static_cast<char>(c);
        else if (c < 0x20) result << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        else result << static_cast<char>(c);
    }
    result << '"';
    return result.str();
}

std::string symbolName(IDiaSymbol* symbol)
{
    BSTR name = nullptr;
    const auto result = symbol->get_name(&name);
    if (result != S_FALSE) require(result, "Symbol name");
    const auto encoded = result == S_OK ? jsonString(name, static_cast<int>(SysStringLen(name))) : "null";
    SysFreeString(name);
    return encoded;
}

void functionSymbols(IDiaSymbol* global, const wchar_t* guidText, DWORD age)
{
    ComPtr<IDiaEnumSymbols> compilands;
    require(global->findChildren(SymTagCompiland, nullptr, nsNone, &compilands), "Enumerate function compilands");
    std::cout << "{\"schema\":\"singleeditor.dia-function-symbols\",\"version\":1,\"identity\":{\"guid\":"
        << jsonString(guidText, static_cast<int>(wcslen(guidText))) << ",\"age\":" << age << "},\"functions\":[\n";
    unsigned long long observed = 0, compilandCount = 0;
    for (;;)
    {
        ComPtr<IDiaSymbol> compiland; ULONG count = 0;
        const auto result = compilands->Next(1, &compiland, &count);
        if (result == S_FALSE) break;
        require(result, "Read function compiland");
        if (count != 1) throw std::runtime_error("Unexpected function compiland count");
        ++compilandCount;
        const auto compilandName = symbolName(compiland.Get());
        ComPtr<IDiaEnumSymbols> functions;
        require(compiland->findChildren(SymTagFunction, nullptr, nsNone, &functions), "Enumerate functions");
        LONG expected = 0, functionsRead = 0;
        require(functions->get_Count(&expected), "Function count");
        for (;;)
        {
            ComPtr<IDiaSymbol> function; count = 0;
            const auto next = functions->Next(1, &function, &count);
            if (next == S_FALSE) break;
            require(next, "Read function");
            if (count != 1) throw std::runtime_error("Unexpected function count");
            ++functionsRead;
            DWORD id = 0, section = 0, offset = 0, rva = 0;
            ULONGLONG length = 0;
            require(function->get_symIndexId(&id), "Function symbol ID");
            const auto optional = [](HRESULT value, const char* operation) {
                if (value != S_FALSE) require(value, operation);
                return value == S_OK;
            };
            const bool hasSection = optional(function->get_addressSection(&section), "Function section");
            const bool hasOffset = optional(function->get_addressOffset(&offset), "Function offset");
            const bool hasRva = optional(function->get_relativeVirtualAddress(&rva), "Function RVA");
            const bool hasLength = optional(function->get_length(&length), "Function length");
            ComPtr<IDiaSymbol> parent;
            const bool hasParent = optional(function->get_classParent(&parent), "Function class parent");
            ComPtr<IDiaSymbol> lexical;
            const bool hasLexical = optional(function->get_lexicalParent(&lexical), "Function lexical parent");
            if (observed++) std::cout << ",\n";
            std::cout << "{\"id\":" << id << ",\"name\":" << symbolName(function.Get())
                << ",\"compiland\":" << compilandName
                << ",\"lexical_parent\":" << (hasLexical && lexical ? symbolName(lexical.Get()) : "null")
                << ",\"class_parent\":" << (hasParent && parent ? symbolName(parent.Get()) : "null")
                << ",\"section\":" << (hasSection ? std::to_string(section) : "null")
                << ",\"offset\":" << (hasOffset ? std::to_string(offset) : "null")
                << ",\"rva\":" << (hasRva ? std::to_string(rva) : "null")
                << ",\"bytes\":" << (hasLength ? std::to_string(length) : "null") << '}';
        }
        if (functionsRead != expected) throw std::runtime_error("DIA function count changed");
    }
    std::cout << "\n],\"count\":" << observed << ",\"compiland_count\":" << compilandCount << "}\n";
}

void dataSymbols(IDiaSymbol* global, const wchar_t* guidText, DWORD age)
{
    struct Row { std::string properties; std::set<std::string> compilands; bool global = false; };
    std::map<DWORD, Row> rows;
    unsigned long long enumerated = 0, compilandCount = 0;
    const auto optional = [](HRESULT value, const char* operation) {
        if (value != S_FALSE) require(value, operation);
        return value == S_OK;
    };
    const auto scan = [&](IDiaSymbol* scope, const std::string& compilandName) {
        ComPtr<IDiaEnumSymbols> symbols;
        require(scope->findChildren(SymTagData, nullptr, nsNone, &symbols), "Enumerate data symbols");
        LONG expected = 0, observed = 0;
        require(symbols->get_Count(&expected), "Data symbol count");
        for (;;)
        {
            ComPtr<IDiaSymbol> data; ULONG count = 0;
            const auto next = symbols->Next(1, &data, &count);
            if (next == S_FALSE) break;
            require(next, "Read data symbol");
            if (count != 1) throw std::runtime_error("Unexpected data symbol count");
            ++observed; ++enumerated;
            DWORD id = 0;
            require(data->get_symIndexId(&id), "Data symbol ID");
            auto& row = rows[id];
            if (compilandName.empty()) row.global = true;
            else row.compilands.insert(compilandName);
            if (!row.properties.empty()) continue;
            DWORD location = 0, kind = 0, section = 0, offset = 0, rva = 0, typeId = 0, typeTag = 0, lexicalTag = 0;
            ULONGLONG symbolBytes = 0, typeBytes = 0;
            const bool hasLocation = optional(data->get_locationType(&location), "Data location");
            const bool hasKind = optional(data->get_dataKind(&kind), "Data kind");
            const bool hasSection = optional(data->get_addressSection(&section), "Data section");
            const bool hasOffset = optional(data->get_addressOffset(&offset), "Data offset");
            const bool hasRva = optional(data->get_relativeVirtualAddress(&rva), "Data RVA");
            const bool hasSymbolBytes = optional(data->get_length(&symbolBytes), "Data symbol length");
            ComPtr<IDiaSymbol> type, lexical, parent;
            const bool hasType = optional(data->get_type(&type), "Data type") && type;
            const bool hasTypeBytes = hasType && optional(type->get_length(&typeBytes), "Data type length");
            const bool hasTypeId = hasType && optional(type->get_symIndexId(&typeId), "Data type ID");
            const bool hasTypeTag = hasType && optional(type->get_symTag(&typeTag), "Data type tag");
            const bool hasLexical = optional(data->get_lexicalParent(&lexical), "Data lexical parent") && lexical;
            const bool hasLexicalTag = hasLexical && optional(lexical->get_symTag(&lexicalTag), "Data lexical tag");
            const bool hasParent = optional(data->get_classParent(&parent), "Data class parent") && parent;
            const auto number = [](bool present, unsigned long long value) { return present ? std::to_string(value) : "null"; };
            std::ostringstream text;
            text << "\"id\":" << id << ",\"name\":" << symbolName(data.Get())
                << ",\"location\":" << number(hasLocation, location) << ",\"data_kind\":" << number(hasKind, kind)
                << ",\"section\":" << number(hasSection, section) << ",\"offset\":" << number(hasOffset, offset)
                << ",\"rva\":" << number(hasRva, rva) << ",\"symbol_bytes\":" << number(hasSymbolBytes, symbolBytes)
                << ",\"type_bytes\":" << number(hasTypeBytes, typeBytes) << ",\"type_id\":" << number(hasTypeId, typeId)
                << ",\"type_tag\":" << number(hasTypeTag, typeTag)
                << ",\"type_name\":" << (hasType ? symbolName(type.Get()) : "null")
                << ",\"lexical_parent\":" << (hasLexical ? symbolName(lexical.Get()) : "null")
                << ",\"lexical_tag\":" << number(hasLexicalTag, lexicalTag)
                << ",\"class_parent\":" << (hasParent ? symbolName(parent.Get()) : "null");
            row.properties = text.str();
        }
        if (observed != expected) throw std::runtime_error("DIA data count changed");
    };
    ComPtr<IDiaEnumSymbols> compilands;
    require(global->findChildren(SymTagCompiland, nullptr, nsNone, &compilands), "Enumerate data compilands");
    for (;;)
    {
        ComPtr<IDiaSymbol> compiland; ULONG count = 0;
        const auto next = compilands->Next(1, &compiland, &count);
        if (next == S_FALSE) break;
        require(next, "Read data compiland");
        if (count != 1) throw std::runtime_error("Unexpected data compiland count");
        ++compilandCount;
        scan(compiland.Get(), symbolName(compiland.Get()));
    }
    scan(global, {});
    std::cout << "{\"schema\":\"singleeditor.dia-data-symbols\",\"version\":1,\"identity\":{\"guid\":"
        << jsonString(guidText, static_cast<int>(wcslen(guidText))) << ",\"age\":" << age << "},\"data\":[\n";
    bool first = true;
    for (const auto& item : rows)
    {
        if (!first) std::cout << ",\n";
        first = false;
        const auto& row = item.second;
        std::cout << '{' << row.properties << ",\"enumerated_global\":" << (row.global ? "true" : "false") << ",\"compilands\":[";
        bool firstCompiland = true;
        for (const auto& name : row.compilands)
        { if (!firstCompiland) std::cout << ','; firstCompiland = false; std::cout << name; }
        std::cout << "]}";
    }
    std::cout << "\n],\"count\":" << rows.size() << ",\"enumerated\":" << enumerated
        << ",\"compiland_count\":" << compilandCount
        << ",\"scope\":\"Direct global and compiland data children only; function-local scopes are not recursively enumerated.\"}\n";
}

// This mode uses the same-session Symbols table and follows each static data
// symbol's own lexical parents. It never joins DIA IDs from another process.
void allDataSymbols(IDiaSession* session, const wchar_t* guidText, DWORD age)
{
    ComPtr<IDiaEnumTables> tables;
    require(session->getEnumTables(&tables), "Enumerate all-data tables");
    LONG tableCount = 0, tablesRead = 0;
    require(tables->get_Count(&tableCount), "All-data table count");
    require(tables->Reset(), "Reset all-data tables");
    ComPtr<IDiaEnumSymbols> symbols;
    LONG expected = -1;
    std::string tableName;
    for (;;)
    {
        ComPtr<IDiaTable> table; ULONG fetched = 0;
        const auto next = tables->Next(1, &table, &fetched);
        if (next == S_FALSE)
        {
            if (fetched != 0 || table) throw std::runtime_error("Incomplete all-data table terminal");
            break;
        }
        require(next, "Read all-data table");
        if (fetched != 1 || !table) throw std::runtime_error("Missing all-data table");
        ++tablesRead;
        ComPtr<IDiaEnumSymbols> candidate;
        const auto qi = table.As(&candidate);
        if (qi == E_NOINTERFACE) continue;
        require(qi, "All-data Symbols interface");
        if (symbols) throw std::runtime_error("Multiple all-data Symbols tables");
        symbols = candidate;
        LONG tableRows = -1;
        require(table->get_Count(&tableRows), "All-data Symbols table count");
        require(symbols->get_Count(&expected), "All-data Symbols enum count");
        if (expected < 0 || expected != tableRows) throw std::runtime_error("All-data Symbols count disagreement");
        BSTR name = nullptr; require(table->get_name(&name), "All-data Symbols table name");
        tableName = jsonString(name, static_cast<int>(SysStringLen(name))); SysFreeString(name);
    }
    if (tablesRead != tableCount || !symbols) throw std::runtime_error("Incomplete all-data table enumeration");
    require(symbols->Reset(), "Reset all-data Symbols");
    const auto optional = [](HRESULT value, const char* operation) {
        if (value != S_FALSE) require(value, operation);
        return value == S_OK;
    };
    const auto number = [](bool present, unsigned long long value) { return present ? std::to_string(value) : "null"; };
    std::map<DWORD, DWORD> seen;
    std::map<DWORD, std::string> rows;
    unsigned long long enumerated = 0, duplicates = 0, dataCount = 0, staticPositions = 0, compilandCount = 0;
    for (;;)
    {
        ComPtr<IDiaSymbol> data; ULONG fetched = 0;
        const auto next = symbols->Next(1, &data, &fetched);
        if (next == S_FALSE)
        {
            if (fetched != 0 || data) throw std::runtime_error("Incomplete all-data Symbols terminal");
            break;
        }
        require(next, "Read all-data symbol");
        if (fetched != 1 || !data) throw std::runtime_error("Missing all-data symbol");
        ++enumerated;
        DWORD id = 0, tag = 0;
        require(data->get_symIndexId(&id), "All-data symbol ID");
        require(data->get_symTag(&tag), "All-data symbol tag");
        const auto [position, inserted] = seen.emplace(id, tag);
        if (!inserted)
        {
            ++duplicates;
            if (position->second != tag) throw std::runtime_error("Conflicting all-data symbol tag");
        }
        if (inserted && tag == SymTagCompiland) ++compilandCount;
        if (tag != SymTagData) continue;
        if (inserted) ++dataCount;
        DWORD location = 0;
        if (!optional(data->get_locationType(&location), "All-data location") || location != LocIsStatic) continue;
        ++staticPositions;
        DWORD kind = 0, section = 0, offset = 0, rva = 0, typeId = 0, typeTag = 0;
        ULONGLONG symbolBytes = 0, typeBytes = 0;
        const bool hasKind = optional(data->get_dataKind(&kind), "All-data kind");
        const bool hasSection = optional(data->get_addressSection(&section), "All-data section");
        const bool hasOffset = optional(data->get_addressOffset(&offset), "All-data offset");
        const bool hasRva = optional(data->get_relativeVirtualAddress(&rva), "All-data RVA");
        const bool hasSymbolBytes = optional(data->get_length(&symbolBytes), "All-data symbol length");
        ComPtr<IDiaSymbol> type, classParent;
        const bool hasType = optional(data->get_type(&type), "All-data type");
        if (hasType && !type) throw std::runtime_error("Null successful all-data type");
        const bool hasTypeBytes = hasType && optional(type->get_length(&typeBytes), "All-data type length");
        const bool hasTypeId = hasType && optional(type->get_symIndexId(&typeId), "All-data type ID");
        const bool hasTypeTag = hasType && optional(type->get_symTag(&typeTag), "All-data type tag");
        const bool hasClassParent = optional(data->get_classParent(&classParent), "All-data class parent");
        if (hasClassParent && !classParent) throw std::runtime_error("Null successful all-data class parent");

        std::ostringstream chain;
        std::set<DWORD> ancestors { id };
        std::string status, compilandName, lexicalName = "null";
        DWORD lexicalTag = 0; bool hasLexical = false, first = true;
        ComPtr<IDiaSymbol> cursor = data;
        for (;;)
        {
            ComPtr<IDiaSymbol> parent;
            const auto hr = cursor->get_lexicalParent(&parent);
            if (hr == S_FALSE)
            {
                if (parent) throw std::runtime_error("Unexpected unavailable lexical parent");
                status = "unavailable"; break;
            }
            require(hr, "All-data lexical parent");
            if (!parent) throw std::runtime_error("Null successful all-data lexical parent");
            DWORD parentId = 0, parentTag = 0;
            require(parent->get_symIndexId(&parentId), "All-data parent ID");
            require(parent->get_symTag(&parentTag), "All-data parent tag");
            if (!ancestors.insert(parentId).second) throw std::runtime_error("Cyclic all-data lexical parents");
            const auto name = symbolName(parent.Get());
            if (first) { lexicalName = name; lexicalTag = parentTag; hasLexical = true; }
            else chain << ',';
            first = false;
            chain << "{\"id\":" << parentId << ",\"tag\":" << parentTag << ",\"name\":" << name << '}';
            if (parentTag == SymTagCompiland) { status = "compiland"; compilandName = name; break; }
            if (parentTag == SymTagExe) { status = "global"; break; }
            cursor = parent;
        }
        if (status == "compiland" && compilandName == "null") throw std::runtime_error("Unnamed all-data compiland");
        std::ostringstream text;
        text << "{\"id\":" << id << ",\"name\":" << symbolName(data.Get())
            << ",\"location\":1,\"data_kind\":" << number(hasKind, kind)
            << ",\"section\":" << number(hasSection, section) << ",\"offset\":" << number(hasOffset, offset)
            << ",\"rva\":" << number(hasRva, rva) << ",\"symbol_bytes\":" << number(hasSymbolBytes, symbolBytes)
            << ",\"type_bytes\":" << number(hasTypeBytes, typeBytes) << ",\"type_id\":" << number(hasTypeId, typeId)
            << ",\"type_tag\":" << number(hasTypeTag, typeTag)
            << ",\"type_name\":" << (hasType ? symbolName(type.Get()) : "null")
            << ",\"lexical_parent\":" << lexicalName << ",\"lexical_tag\":" << number(hasLexical, lexicalTag)
            << ",\"class_parent\":" << (hasClassParent ? symbolName(classParent.Get()) : "null")
            << ",\"enumerated_global\":false,\"compilands\":[" << compilandName
            << "],\"parent_chain\":[" << chain.str() << "],\"parent_chain_status\":\"" << status << "\"}";
        const auto value = text.str();
        const auto [old, fresh] = rows.emplace(id, value);
        if (!fresh && old->second != value) throw std::runtime_error("Conflicting duplicate all-data row");
    }
    if (enumerated != static_cast<unsigned long long>(expected)) throw std::runtime_error("All-data Symbols count changed");
    std::cout << "{\"schema\":\"singleeditor.dia-data-symbols\",\"version\":2,\"identity\":{\"guid\":"
        << jsonString(guidText, static_cast<int>(wcslen(guidText))) << ",\"age\":" << age
        << "},\"enumeration_policy\":\"complete-symbols-static-data-v1\",\"data\":[\n";
    bool first = true;
    for (const auto& row : rows) { if (!first) std::cout << ",\n"; first = false; std::cout << row.second; }
    std::cout << "\n],\"count\":" << rows.size() << ",\"enumerated\":" << enumerated << ",\"compiland_count\":" << compilandCount
        << ",\"table_enumeration\":{\"table\":" << tableName << ",\"reported_count\":" << expected
        << ",\"enumerated_count\":" << enumerated << ",\"unique_symbols\":" << seen.size() << ",\"duplicate_count\":" << duplicates
        << ",\"data_symbols\":" << dataCount << ",\"static_positions\":" << staticPositions
        << ",\"static_symbols\":" << rows.size() << ",\"terminal_hresult\":\"0x00000001\"}"
        << ",\"scope\":\"Complete Symbols table; all LocIsStatic Data with same-session lexical parent chains. No public-symbol length or name-only source inference.\"}\n";
}

void inspect(const wchar_t* dllPath, const wchar_t* pdbPath, bool originsOnly, bool functionsOnly, bool dataOnly, bool allData)
{
    // Load the explicitly selected SDK DLL without changing COM registration.
    const auto module = LoadLibraryExW(dllPath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) throw std::runtime_error("Cannot load the selected DIA SDK DLL");
    struct LibraryLease { HMODULE value; ~LibraryLease() { FreeLibrary(value); } } library { module };
    using FactoryFunction = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
    const auto getFactory = reinterpret_cast<FactoryFunction>(GetProcAddress(module, "DllGetClassObject"));
    if (!getFactory) throw std::runtime_error("DIA DLL has no class factory");
    ComPtr<IClassFactory> factory;
    require(getFactory(__uuidof(DiaSource), IID_PPV_ARGS(&factory)), "DIA factory");
    ComPtr<IDiaDataSource> source;
    require(factory->CreateInstance(nullptr, IID_PPV_ARGS(&source)), "DIA data source");
    require(source->loadDataFromPdb(pdbPath), "Load PDB");
    ComPtr<IDiaSession> session;
    require(source->openSession(&session), "Open DIA session");
    ComPtr<IDiaSymbol> global;
    require(session->get_globalScope(&global), "PDB global scope");
    GUID guid {}; DWORD age = 0;
    require(global->get_guid(&guid), "PDB GUID");
    require(global->get_age(&age), "PDB age");
    wchar_t guidText[40] {};
    if (!StringFromGUID2(guid, guidText, 40)) throw std::runtime_error("Cannot encode PDB GUID");
    if (allData) { allDataSymbols(session.Get(), guidText, age); return; }
    if (functionsOnly) { functionSymbols(global.Get(), guidText, age); return; }
    if (dataOnly) { dataSymbols(global.Get(), guidText, age); return; }
    ComPtr<IDiaEnumTables> tables;
    require(session->getEnumTables(&tables), "Enumerate DIA tables");
    ComPtr<IDiaEnumSectionContribs> contributions;
    for (;;)
    {
        ComPtr<IDiaTable> table; ULONG count = 0;
        const auto result = tables->Next(1, &table, &count);
        if (result == S_FALSE) break;
        require(result, "Read DIA table");
        if (count != 1) throw std::runtime_error("Unexpected DIA table count");
        if (table.As(&contributions) == S_OK) break;
    }
    if (!contributions) throw std::runtime_error("PDB has no section contribution table");
    LONG expected = 0;
    require(contributions->get_Count(&expected), "Contribution count");
    std::cout << "{\"schema\":\"singleeditor.dia-section-contributions\",\"version\":" << (originsOnly ? 3 : 2)
        << ",\"identity\":{\"guid\":"
        << jsonString(guidText, static_cast<int>(wcslen(guidText))) << ",\"age\":" << age
        << "},\"count\":" << expected << ",\"contributions\":[\n";
    LONG observed = 0;
    std::map<DWORD, std::string> originRows;
    for (;;)
    {
        ComPtr<IDiaSectionContrib> contribution; ULONG count = 0;
        const auto result = contributions->Next(1, &contribution, &count);
        if (result == S_FALSE) break;
        require(result, "Read contribution");
        if (count != 1) throw std::runtime_error("Unexpected contribution count");
        DWORD section = 0, offset = 0, rva = 0, length = 0, compilandId = 0;
        BOOL code = FALSE, initialized = FALSE, uninitialized = FALSE;
        require(contribution->get_addressSection(&section), "Contribution section");
        require(contribution->get_addressOffset(&offset), "Contribution offset");
        require(contribution->get_relativeVirtualAddress(&rva), "Contribution RVA");
        require(contribution->get_length(&length), "Contribution length");
        require(contribution->get_compilandId(&compilandId), "Contribution compiland ID");
        require(contribution->get_code(&code), "Contribution code flag");
        require(contribution->get_initializedData(&initialized), "Contribution initialized flag");
        require(contribution->get_uninitializedData(&uninitialized), "Contribution uninitialized flag");
        ComPtr<IDiaSymbol> compiland;
        require(contribution->get_compiland(&compiland), "Contribution compiland");
        BSTR name = nullptr;
        require(compiland->get_name(&name), "Compiland name");
        const auto encoded = jsonString(name, static_cast<int>(SysStringLen(name)));
        SysFreeString(name);
        if (originsOnly && originRows.find(compilandId) == originRows.end())
        {
            // DIA IDs belong to this session. Obtain origin metadata through
            // the contribution's own symbol, not a separately enumerated table.
            BSTR library = nullptr, sourceFile = nullptr;
            const auto libraryResult = compiland->get_libraryName(&library);
            const auto sourceResult = compiland->get_sourceFileName(&sourceFile);
            if (libraryResult != S_FALSE) require(libraryResult, "Compiland library");
            if (sourceResult != S_FALSE) require(sourceResult, "Compiland source");
            std::ostringstream origin;
            origin << "{\"id\":" << compilandId << ",\"name\":" << encoded
                << ",\"library_name\":" << (libraryResult == S_OK ? jsonString(library, static_cast<int>(SysStringLen(library))) : "null")
                << ",\"source_file\":" << (sourceResult == S_OK ? jsonString(sourceFile, static_cast<int>(SysStringLen(sourceFile))) : "null") << '}';
            SysFreeString(library); SysFreeString(sourceFile);
            originRows.emplace(compilandId, origin.str());
        }
        if (observed++) std::cout << ",\n";
        std::cout << "{\"section\":" << section << ",\"offset\":" << offset << ",\"rva\":" << rva
            << ",\"bytes\":" << length << ",\"compiland_id\":" << compilandId << ",\"compiland\":" << encoded
            << ",\"code\":" << (code ? "true" : "false") << ",\"initialized_data\":" << (initialized ? "true" : "false")
            << ",\"uninitialized_data\":" << (uninitialized ? "true" : "false") << '}';
    }
    if (observed != expected) throw std::runtime_error("DIA contribution count changed");
    std::cout << "\n]";
    if (originsOnly)
    {
        std::cout << ",\"compilands\":[\n";
        bool first = true;
        for (const auto& row : originRows)
        { if (!first) std::cout << ",\n"; first = false; std::cout << row.second; }
        std::cout << "\n]";
    }
    std::cout << "}\n";
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3 && !(argc == 4 && (wcscmp(argv[3], L"--origins") == 0 || wcscmp(argv[3], L"--functions") == 0 || wcscmp(argv[3], L"--data") == 0 || wcscmp(argv[3], L"--data-all") == 0)))
    { std::cerr << "Usage: <absolute-msdia140.dll> <absolute-pdb> [--origins|--functions|--data|--data-all]\n"; return 1; }
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) { std::cerr << "COM initialization failed\n"; return 1; }
    int result = 0;
    try { inspect(argv[1], argv[2], argc == 4 && wcscmp(argv[3], L"--origins") == 0,
        argc == 4 && wcscmp(argv[3], L"--functions") == 0,
        argc == 4 && wcscmp(argv[3], L"--data") == 0,
        argc == 4 && wcscmp(argv[3], L"--data-all") == 0); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    CoUninitialize();
    return result;
}
