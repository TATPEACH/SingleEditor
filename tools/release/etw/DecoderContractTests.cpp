// Focused export checks. Synthetic records are not an ETL capture or runtime pass.
#define SINGLEEDITOR_ETL_NO_MAIN
#include "SingleEditorEtlDecode.cpp"
int wmain() {
    try {
        if (etl::quoted(L"quote\"/line\n\u4e2d") != "\"quote\\\"/line\\u000A\\u4E2D\"") throw std::runtime_error("json_escape");
        if (etl::decimal(9007199254740993LL) != "\"9007199254740993\"") throw std::runtime_error("integer_precision");
        TRACE_LOGFILE_HEADER h{}; h.ReservedFlags = 2; h.PerfFreq.QuadPart = 123;
        const auto session = etl::session(h);
        if (session.find("\"clock_type\":2") == std::string::npos || session.find("\"perf_frequency\":\"123\"") == std::string::npos)
            throw std::runtime_error("raw_non_qpc_clock");
        BYTE payload[] = {0, 0xFF, 0x22, 0x5C}; EVENT_RECORD record{};
        record.EventHeader.ProviderId = {0xE153A9F1,0x8C09,0x41EC,{0x9D,0x98,0x7B,0x0B,0x35,0x7F,0xC2,0x39}};
        record.EventHeader.ProcessId = 37; record.EventHeader.ThreadId = 41;
        record.EventHeader.TimeStamp.QuadPart = 9007199254740993LL;
        record.EventHeader.EventDescriptor.Id = 123; record.EventHeader.EventDescriptor.Version = 99;
        record.UserData = payload; record.UserDataLength = sizeof(payload);
        const auto row = etl::decode(record, 1, 8);
        if (!row.gaps || row.json.find("tdh_metadata_unavailable") == std::string::npos || row.json.find("00FF225C") == std::string::npos)
            throw std::runtime_error("unknown_tdh_keeps_raw");
        puts(row.json.c_str());
        // This is a synthetic payload interpreted with the actual installed
        // manifest. It is not an observed file operation or an ETL capture.
        GUID provider = {0xEDD08927,0x9CC4,0x4E65,{0xB9,0x70,0xC2,0x56,0x0F,0xB5,0xC2,0x89}};
        EVENT_DESCRIPTOR desc{}; desc.Id = 10; desc.Version = 0;
        ULONG metadataBytes = 0;
        if (TdhGetManifestEventInformation(&provider, &desc, nullptr, &metadataBytes) != ERROR_INSUFFICIENT_BUFFER)
            throw std::runtime_error("installed_manifest_metadata_unavailable");
        std::vector<BYTE> metadata(metadataBytes);
        auto* info = reinterpret_cast<TRACE_EVENT_INFO*>(metadata.data());
        if (TdhGetManifestEventInformation(&provider, &desc, info, &metadataBytes) != ERROR_SUCCESS ||
            info->PropertyCount != info->TopLevelPropertyCount) throw std::runtime_error("installed_manifest_shape_changed");
        std::vector<BYTE> synthetic;
        for (ULONG i = 0; i < info->PropertyCount; ++i) {
            const auto& p = info->EventPropertyInfoArray[i];
            if ((p.Flags & ~(PropertyParamFixedLength | PropertyHasTags)) || p.count != 1)
                throw std::runtime_error("installed_manifest_shape_changed:flags=" + etl::number(p.Flags) + ":count=" + etl::number(p.count));
            if (p.nonStructType.InType == TDH_INTYPE_UINT64 || p.nonStructType.InType == TDH_INTYPE_POINTER) synthetic.resize(synthetic.size() + 8);
            else if (p.nonStructType.InType == TDH_INTYPE_UNICODESTRING) {
                const wchar_t path[] = L"test\\\"\n\u4e2d";
                const auto* begin = reinterpret_cast<const BYTE*>(path); synthetic.insert(synthetic.end(), begin, begin + sizeof(path));
            } else throw std::runtime_error("installed_manifest_type_changed");
        }
        record.EventHeader.ProviderId = provider; record.EventHeader.EventDescriptor = info->EventDescriptor;
        record.EventHeader.Flags = EVENT_HEADER_FLAG_64_BIT_HEADER; record.UserData = synthetic.data();
        record.UserDataLength = static_cast<USHORT>(synthetic.size());
        const auto known = etl::decode(record, 2, 8);
        if (known.gaps || known.json.find("\"tdh_status\":0") == std::string::npos || known.json.find("\\u4E2D") == std::string::npos)
            throw std::runtime_error("actual_tdh_synthetic_payload_format");
        puts(known.json.c_str());
        puts("{\"schema\":\"singleeditor.etl-decoder-contract-tests\",\"version\":1,\"status\":\"pass\",\"checks\":[\"json_escape\",\"integer_precision\",\"raw_non_qpc_clock\",\"unknown_tdh_keeps_raw\",\"actual_tdh_synthetic_payload\"],\"real_etl_decoded\":false}");
        return 0;
    } catch (const std::exception& ex) { fprintf(stderr, "%s\n", ex.what()); return 1; }
}
