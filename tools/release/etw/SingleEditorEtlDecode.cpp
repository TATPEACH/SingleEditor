// ToolTestOnly: offline ETL reader. No trace session creation or product code.
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace etl {
constexpr ULONG mode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
constexpr ULONG metadataLimit = 1024 * 1024;
std::string number(unsigned long long n) { return std::to_string(n); }
std::string quoted(const std::wstring& s) {
    std::string r = "\"";
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { r += '\\'; r += static_cast<char>(c); }
        else if (c >= 32 && c < 127) r += static_cast<char>(c);
        else { char b[7]; sprintf_s(b, "\\u%04X", static_cast<unsigned>(c)); r += b; }
    }
    return r + "\"";
}
std::string quoted(const std::string& s) { return quoted(std::wstring(s.begin(), s.end())); }
std::string decimal(LONGLONG n) { return quoted(std::to_string(n)); }
std::string hex(const void* data, size_t size) {
    const auto* p = static_cast<const BYTE*>(data); const char* digits = "0123456789ABCDEF";
    std::string r; r.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) { r += digits[p[i] >> 4]; r += digits[p[i] & 15]; }
    return r;
}
std::string guid(const GUID& g) {
    char b[40]; sprintf_s(b, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1,
        g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return quoted(std::string(b));
}
struct Object {
    std::string s = "{"; bool first = true;
    void add(const char* key, const std::string& value) { if (!first) s += ','; first = false; s += quoted(std::string(key)) + ':' + value; }
    std::string done() const { return s + '}'; }
};
std::string array(const std::vector<std::string>& rows) {
    std::string s = "["; for (const auto& row : rows) { if (s.size() > 1) s += ','; s += row; } return s + ']';
}
std::string gap(const char* code, ULONG status = 0, long property = -1) {
    Object o; o.add("code", quoted(std::string(code))); o.add("win32_status", number(status));
    o.add("property_index", property < 0 ? "null" : number(property)); return o.done();
}
struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    File(const std::wstring& p, bool write) {
        h = CreateFileW(p.c_str(), write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, nullptr,
            write ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("file_open:" + number(GetLastError()));
    }
    ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    File(const File&) = delete; File& operator=(const File&) = delete;
    void write(const std::string& s) {
        if (s.size() > MAXDWORD) throw std::runtime_error("output_line_too_large");
        DWORD written = 0;
        if (!WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &written, nullptr) || written != s.size())
            throw std::runtime_error("output_write:" + number(GetLastError()));
    }
};
struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    Hash() {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("sha256_provider");
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr; throw std::runtime_error("sha256_create");
        }
    }
    ~Hash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
};
std::string fact(const std::wstring& path) {
    File f(path, false); Hash h; LARGE_INTEGER size{};
    if (!GetFileSizeEx(f.h, &size)) throw std::runtime_error("file_size");
    std::vector<BYTE> b(65536); DWORD count = 0; ULONGLONG read = 0;
    for (;;) {
        if (!ReadFile(f.h, b.data(), static_cast<DWORD>(b.size()), &count, nullptr)) throw std::runtime_error("file_read");
        if (!count) break; read += count;
        if (BCryptHashData(h.hash, b.data(), count, 0) < 0) throw std::runtime_error("sha256_update");
    }
    if (read != static_cast<ULONGLONG>(size.QuadPart)) throw std::runtime_error("file_size_changed");
    BYTE digest[32]; if (BCryptFinishHash(h.hash, digest, sizeof(digest), 0) < 0) throw std::runtime_error("sha256_finish");
    Object o; o.add("path", quoted(path)); o.add("bytes", number(read)); o.add("sha256", quoted(hex(digest, sizeof(digest)))); return o.done();
}
std::wstring metadataString(const std::vector<BYTE>& data, ULONG offset) {
    if (!offset) return {};
    if (offset % sizeof(wchar_t) || offset >= data.size()) throw std::runtime_error("tdh_string_offset");
    std::wstring s;
    for (size_t i = offset; i + sizeof(wchar_t) <= data.size(); i += sizeof(wchar_t)) {
        wchar_t c; memcpy(&c, data.data() + i, sizeof(c)); if (!c) return s; s += c;
    }
    throw std::runtime_error("tdh_string_unterminated");
}
std::string descriptor(const EVENT_DESCRIPTOR& d) {
    Object o; o.add("id", number(d.Id)); o.add("version", number(d.Version)); o.add("channel", number(d.Channel));
    o.add("level", number(d.Level)); o.add("opcode", number(d.Opcode)); o.add("task", number(d.Task));
    char keyword[19]; sprintf_s(keyword, "0x%016llX", d.Keyword); o.add("keyword", quoted(std::string(keyword))); return o.done();
}
struct Decoded { std::string json; bool gaps; };
Decoded decode(EVENT_RECORD& e, ULONGLONG sequence, ULONG sessionPointerSize) {
    std::vector<std::string> gaps, properties;
    Object row; row.add("schema", quoted("singleeditor.etl-event")); row.add("version", "1"); row.add("sequence", number(sequence));
    row.add("pid", number(e.EventHeader.ProcessId)); row.add("tid", number(e.EventHeader.ThreadId));
    row.add("timestamp_raw", decimal(e.EventHeader.TimeStamp.QuadPart)); row.add("provider", guid(e.EventHeader.ProviderId));
    row.add("descriptor", descriptor(e.EventHeader.EventDescriptor)); row.add("header_flags", number(e.EventHeader.Flags));
    row.add("header_properties", number(e.EventHeader.EventProperty)); row.add("activity", guid(e.EventHeader.ActivityId));
    row.add("payload_bytes", number(e.UserDataLength)); row.add("payload_hex", quoted(hex(e.UserData, e.UserDataLength)));
    row.add("extended_data_count", number(e.ExtendedDataCount));
    std::vector<std::string> extended;
    for (USHORT i = 0; i < e.ExtendedDataCount; ++i) {
        const auto& item = e.ExtendedData[i]; Object x;
        x.add("type", number(item.ExtType)); x.add("linkage", number(item.Linkage)); x.add("bytes", number(item.DataSize));
        x.add("raw_hex", quoted(hex(reinterpret_cast<const void*>(static_cast<ULONG_PTR>(item.DataPtr)), item.DataSize)));
        extended.push_back(x.done());
    }
    row.add("extended_data", array(extended));
    // Extended items are retained but not confused with ordinary UserData properties.
    if (e.ExtendedDataCount) gaps.push_back(gap("extended_data_not_interpreted"));
    ULONG size = 0, status = TdhGetEventInformation(&e, 0, nullptr, nullptr, &size);
    std::vector<BYTE> data;
    if (status == ERROR_INSUFFICIENT_BUFFER && size <= metadataLimit) {
        data.resize(size); status = TdhGetEventInformation(&e, 0, nullptr, reinterpret_cast<TRACE_EVENT_INFO*>(data.data()), &size);
        if (status == ERROR_SUCCESS && size <= data.size()) data.resize(size);
    }
    Object tdh; tdh.add("api", quoted("TdhGetEventInformation/TdhFormatProperty")); tdh.add("status", number(status));
    tdh.add("metadata_bytes", number(size));
    if (status != ERROR_SUCCESS) gaps.push_back(gap(size > metadataLimit ? "tdh_metadata_limit" : "tdh_metadata_unavailable", status));
    else try {
        if (size > data.size() || data.size() < offsetof(TRACE_EVENT_INFO, EventPropertyInfoArray)) throw std::runtime_error("tdh_metadata_size");
        auto* info = reinterpret_cast<TRACE_EVENT_INFO*>(data.data());
        if (info->PropertyCount > (data.size() - offsetof(TRACE_EVENT_INFO, EventPropertyInfoArray)) / sizeof(EVENT_PROPERTY_INFO) ||
            info->TopLevelPropertyCount > info->PropertyCount) throw std::runtime_error("tdh_property_bounds");
        tdh.add("decoding_source", number(info->DecodingSource)); tdh.add("provider_guid", guid(info->ProviderGuid));
        tdh.add("descriptor", descriptor(info->EventDescriptor)); tdh.add("provider_name", quoted(metadataString(data, info->ProviderNameOffset)));
        tdh.add("event_name", quoted(metadataString(data, info->DecodingSource == DecodingSourceXMLFile || info->DecodingSource == DecodingSourceTlg ? info->EventNameOffset : 0)));
        tdh.add("task_name", quoted(metadataString(data, info->TaskNameOffset))); tdh.add("opcode_name", quoted(metadataString(data, info->OpcodeNameOffset)));
        tdh.add("property_count", number(info->PropertyCount)); tdh.add("top_level_property_count", number(info->TopLevelPropertyCount));
        ULONG pointerSize = (e.EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 :
            (e.EventHeader.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER) ? 8 : sessionPointerSize;
        tdh.add("pointer_size", number(pointerSize));
        if (pointerSize != 4 && pointerSize != 8) throw std::runtime_error("pointer_size_unavailable");
        if (info->DecodingSource == DecodingSourceWPP || (e.EventHeader.Flags & EVENT_HEADER_FLAG_STRING_ONLY))
            throw std::runtime_error("wpp_or_string_only_not_supported");
        size_t offset = 0;
        std::vector<std::optional<ULONGLONG>> integers(info->PropertyCount);
        for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i) {
            const auto& p = info->EventPropertyInfoArray[i]; ULONG flags = p.Flags;
            if (flags & (PropertyStruct | PropertyWBEMXmlFragment | PropertyHasCustomSchema) || (flags & ~0xF7UL)) {
                gaps.push_back(gap("tdh_property_shape_unsupported", 0, i)); break;
            }
            auto resolve = [&](USHORT index) -> ULONGLONG {
                if (index >= i || !integers[index]) throw std::runtime_error("tdh_length_or_count_not_prior_unsigned_scalar"); return *integers[index];
            };
            ULONGLONG count = flags & PropertyParamCount ? resolve(p.countPropertyIndex) : p.count;
            ULONGLONG length = flags & PropertyParamLength ? resolve(p.lengthPropertyIndex) : p.length;
            if (count > 65535 || length > 65535) { gaps.push_back(gap("tdh_property_bounds_unsupported", 0, i)); break; }
            // TDH interprets a zero string length as nul-terminated. Do not let
            // an explicitly zero-length counted field consume its successor.
            if (length == 0 && (flags & (PropertyParamLength | PropertyParamFixedLength)) &&
                (p.nonStructType.InType == TDH_INTYPE_UNICODESTRING || p.nonStructType.InType == TDH_INTYPE_ANSISTRING)) {
                gaps.push_back(gap("zero_length_counted_string_unsupported", 0, i)); break;
            }
            const auto name = metadataString(data, p.NameOffset);
            for (ULONGLONG a = 0; a < count; ++a) {
                if (offset > e.UserDataLength) throw std::runtime_error("payload_offset_out_of_bounds");
                BYTE empty = 0; auto* raw = e.UserData ? static_cast<BYTE*>(e.UserData) + offset : &empty;
                const auto left = static_cast<USHORT>(e.UserDataLength - offset);
                ULONG bytes = 0; USHORT used = 0;
                ULONG formatted = TdhFormatProperty(info, nullptr, pointerSize, p.nonStructType.InType, p.nonStructType.OutType,
                    static_cast<USHORT>(length), left, raw, &bytes, nullptr, &used);
                std::vector<wchar_t> text;
                if (formatted == ERROR_INSUFFICIENT_BUFFER && bytes <= metadataLimit && bytes % sizeof(wchar_t) == 0) {
                    text.resize(bytes / sizeof(wchar_t)); formatted = TdhFormatProperty(info, nullptr, pointerSize,
                        p.nonStructType.InType, p.nonStructType.OutType, static_cast<USHORT>(length), left, raw, &bytes, text.data(), &used);
                }
                Object prop; prop.add("index", number(i)); prop.add("array_index", number(a)); prop.add("name", quoted(name));
                prop.add("flags", number(flags)); prop.add("in_type", number(p.nonStructType.InType)); prop.add("out_type", number(p.nonStructType.OutType));
                prop.add("map_name", quoted(metadataString(data, p.nonStructType.MapNameOffset))); prop.add("map_applied", "false");
                prop.add("declared_length", number(length)); prop.add("declared_count", number(count)); prop.add("offset", number(offset));
                prop.add("tdh_status", number(formatted)); prop.add("formatted_buffer_bytes", number(bytes));
                if (formatted != ERROR_SUCCESS || used > left || text.empty() || text.back() != 0) {
                    prop.add("bytes", "null"); prop.add("raw_hex", "null"); prop.add("formatted", "null"); properties.push_back(prop.done());
                    gaps.push_back(gap("tdh_property_format_failed", formatted, i)); goto propertiesDone;
                }
                prop.add("bytes", number(used)); prop.add("raw_hex", quoted(hex(raw, used))); prop.add("formatted", quoted(std::wstring(text.data())));
                properties.push_back(prop.done());
                if (count == 1 && !(flags & (PropertyParamCount | PropertyParamFixedCount)) &&
                    ((p.nonStructType.InType == TDH_INTYPE_UINT8 && used == 1) ||
                    (p.nonStructType.InType == TDH_INTYPE_UINT16 && used == 2) || (p.nonStructType.InType == TDH_INTYPE_UINT32 && used == 4))) {
                    ULONGLONG v = 0; memcpy(&v, raw, used); integers[i] = v;
                }
                offset += used;
            }
        }
    propertiesDone:
        tdh.add("decoded_payload_bytes", number(offset));
        if (offset != e.UserDataLength) gaps.push_back(gap("payload_bytes_not_decoded"));
    } catch (const std::exception& ex) { gaps.push_back(gap(ex.what())); }
    tdh.add("properties", array(properties)); row.add("tdh", tdh.done()); row.add("gaps", array(gaps));
    return {row.done(), !gaps.empty()};
}
std::string session(const TRACE_LOGFILE_HEADER& h) {
    Object o; o.add("clock_type", number(h.ReservedFlags)); o.add("perf_frequency", decimal(h.PerfFreq.QuadPart));
    o.add("start_time", decimal(h.StartTime.QuadPart)); o.add("end_time", decimal(h.EndTime.QuadPart)); o.add("boot_time", decimal(h.BootTime.QuadPart));
    o.add("events_lost", number(h.EventsLost)); o.add("buffers_lost", number(h.BuffersLost)); o.add("buffers_written", number(h.BuffersWritten));
    o.add("pointer_size", number(h.PointerSize)); o.add("log_file_mode", number(h.LogFileMode)); o.add("os_version", number(h.Version));
    o.add("provider_version", number(h.ProviderVersion)); o.add("processor_count", number(h.NumberOfProcessors)); return o.done();
}
struct Context {
    File* output = nullptr; ULONGLONG events = 0, gapEvents = 0; ULONG buffers = 0; TRACE_LOGFILE_HEADER header{};
    std::string failure;
};
void WINAPI eventCallback(EVENT_RECORD* e) {
    auto& c = *static_cast<Context*>(e->UserContext); if (!c.failure.empty()) return;
    try { auto row = decode(*e, ++c.events, c.header.PointerSize); if (row.gaps) ++c.gapEvents; c.output->write(row.json + '\n'); }
    catch (const std::exception& ex) { c.failure = ex.what(); }
    catch (...) { c.failure = "event_callback_exception"; }
}
ULONG WINAPI bufferCallback(EVENT_TRACE_LOGFILEW* log) {
    auto& c = *static_cast<Context*>(log->Context); c.header = log->LogfileHeader; c.buffers = log->BuffersRead;
    return c.failure.empty() ? TRUE : FALSE;
}
int run(const std::wstring& input, const std::wstring& output) {
    // Retain a read-only file lease to deny writer/delete races while OpenTrace
    // opens its own reader. Independently hash both sides of ProcessTrace.
    File lease(input, false);
    if (!CreateDirectoryW(output.c_str(), nullptr)) throw std::runtime_error("new_output_directory_required:" + number(GetLastError()));
    const auto eventsPath = (std::filesystem::path(output) / L"events.jsonl").wstring();
    const auto headerPath = (std::filesystem::path(output) / L"header.json").wstring();
    std::string before = "null", after = "null", executable = "null", eventsFact = "null", error;
    ULONG openStatus = ERROR_SUCCESS, processStatus = ERROR_INVALID_STATE, closeStatus = ERROR_INVALID_STATE;
    Context context; bool opened = false; TRACEHANDLE trace = INVALID_PROCESSTRACE_HANDLE;
    try {
        before = fact(input); wchar_t ownPath[32768]; DWORD chars = GetModuleFileNameW(nullptr, ownPath, 32768);
        if (!chars || chars >= 32768) throw std::runtime_error("decoder_path"); executable = fact(std::wstring(ownPath, chars));
        {
            File events(eventsPath, true); context.output = &events;
            EVENT_TRACE_LOGFILEW log{}; log.LogFileName = const_cast<LPWSTR>(input.c_str()); log.ProcessTraceMode = mode;
            log.EventRecordCallback = eventCallback; log.BufferCallback = bufferCallback; log.Context = &context;
            trace = OpenTraceW(&log);
            if (trace == INVALID_PROCESSTRACE_HANDLE) { openStatus = GetLastError(); throw std::runtime_error("OpenTrace:" + number(openStatus)); }
            opened = true; context.header = log.LogfileHeader;
            processStatus = ProcessTrace(&trace, 1, nullptr, nullptr);
            closeStatus = CloseTrace(trace); trace = INVALID_PROCESSTRACE_HANDLE;
            if (!context.failure.empty()) throw std::runtime_error(context.failure);
            if (processStatus != ERROR_SUCCESS) throw std::runtime_error("ProcessTrace:" + number(processStatus));
            if (closeStatus != ERROR_SUCCESS) throw std::runtime_error("CloseTrace:" + number(closeStatus));
        }
    } catch (const std::exception& ex) { error = ex.what(); }
    if (trace != INVALID_PROCESSTRACE_HANDLE) closeStatus = CloseTrace(trace);
    try { after = fact(input); if (before != "null" && before != after) error = "input_changed_during_decode"; }
    catch (const std::exception& ex) { error = ex.what(); }
    try { if (std::filesystem::exists(eventsPath)) eventsFact = fact(eventsPath); }
    catch (const std::exception& ex) { error = ex.what(); }
    std::vector<std::string> gaps;
    if (opened && context.header.ReservedFlags != 1) gaps.push_back(gap("non_qpc_clock"));
    if (opened && context.header.ReservedFlags == 1 && context.header.PerfFreq.QuadPart <= 0) gaps.push_back(gap("qpc_frequency_missing"));
    if (opened && (context.header.EventsLost || context.header.BuffersLost)) gaps.push_back(gap("trace_loss_reported"));
    if (opened && context.header.EndTime.QuadPart == 0) gaps.push_back(gap("trace_not_finalized"));
    if (opened && !context.events) gaps.push_back(gap("no_events"));
    const char* status = !error.empty() ? "failed" : context.gapEvents || !gaps.empty() ? "decoded_with_gaps" : "decoded";
    Object h; h.add("schema", quoted("singleeditor.etl-decoder")); h.add("version", "1"); h.add("status", quoted(status));
    h.add("input", before); h.add("input_after", after); h.add("input_unchanged", before == after && before != "null" ? "true" : "false");
    h.add("decoder", executable); h.add("events", eventsFact); h.add("process_trace_mode", number(mode));
    h.add("session", opened ? session(context.header) : "null"); h.add("open_trace_status", number(openStatus));
    h.add("process_trace_status", number(processStatus)); h.add("close_trace_status", number(closeStatus));
    h.add("buffers_read", number(context.buffers)); h.add("event_count", number(context.events)); h.add("gap_event_count", number(context.gapEvents));
    h.add("gaps", array(gaps)); h.add("error", error.empty() ? "null" : etl::quoted(error)); h.add("release_eligible", "false");
    h.add("scope", quoted("Offline raw ETL/TDH facts only. No event semantic mapping, read/instance inference, coverage/absence claim or collection. Process filters and System/other-process coverage must be established separately from the capture profile."));
    File header(headerPath, true); header.write(h.done() + '\n');
    return !error.empty() ? 1 : context.gapEvents || !gaps.empty() ? 3 : 0;
}
} // namespace etl

#ifndef SINGLEEDITOR_ETL_NO_MAIN
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { fputs("Usage: SingleEditorEtlDecode.exe input.etl new-output-directory\n", stderr); return 1; }
    try { return etl::run(std::filesystem::absolute(argv[1]).lexically_normal().wstring(), std::filesystem::absolute(argv[2]).lexically_normal().wstring()); }
    catch (const std::exception& ex) { fprintf(stderr, "ETL_DECODE_ERROR=%s\n", ex.what()); return 1; }
}
#endif
