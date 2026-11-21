#include "pe_loader.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "../shims/shims.h"

namespace rt {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

namespace {

std::vector<uint8_t> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}

template <class T>
T* at(uint8_t* base, uint32_t rva) { return reinterpret_cast<T*>(base + rva); }

// Every RVA followed below comes out of the image itself, so each one is checked first. A truncated or corrupt
// image then fails with a message instead of reading, writing or protecting memory outside its allocation.
void require_range(const std::string& name, uint64_t rva, uint64_t len, uint32_t image_size, const char* what) {
    if (rva > image_size || len > image_size - rva)
        throw std::runtime_error(name + ": " + what + " runs past the image");
}

std::string read_string(const std::string& name, uint8_t* base, uint32_t image_size, uint32_t rva, const char* what) {
    if (rva >= image_size) throw std::runtime_error(name + ": " + what + " is outside the image");
    auto* s = reinterpret_cast<const char*>(base + rva);
    auto* end = static_cast<const char*>(std::memchr(s, 0, image_size - rva));
    if (!end) throw std::runtime_error(name + ": " + what + " isn't terminated inside the image");
    return {s, end};
}

void validate_section_layout(const std::string& name, size_t file_size, uint32_t nt_offset, IMAGE_NT_HEADERS64& nt) {
    const IMAGE_OPTIONAL_HEADER64& oh = nt.OptionalHeader;
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const uint32_t page_size = system_info.dwPageSize;
    if (!oh.SectionAlignment || !oh.FileAlignment)
        throw std::runtime_error(name + ": zero section or file alignment");
    if (oh.SectionAlignment < oh.FileAlignment)
        throw std::runtime_error(name + ": section alignment is smaller than file alignment");
    if (oh.SectionAlignment < page_size && oh.FileAlignment != oh.SectionAlignment)
        throw std::runtime_error(name + ": sub-page section alignment requires matching file alignment");
    if (oh.SizeOfImage % oh.SectionAlignment)
        throw std::runtime_error(name + ": image size is not aligned to section alignment");
    if (oh.SizeOfHeaders % oh.FileAlignment)
        throw std::runtime_error(name + ": header size is not aligned to file alignment");
    if (oh.ImageBase % 0x10000)
        throw std::runtime_error(name + ": image base is not 64-KB aligned");

    const uint64_t section_table_end = static_cast<uint64_t>(nt_offset) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                                       nt.FileHeader.SizeOfOptionalHeader +
                                       static_cast<uint64_t>(nt.FileHeader.NumberOfSections) *
                                           sizeof(IMAGE_SECTION_HEADER);
    if (section_table_end > oh.SizeOfHeaders)
        throw std::runtime_error(name + ": section table extends beyond the mapped headers");

    uint64_t previous_end = 0;
    uint32_t previous_rva = 0;
    bool have_previous = false;
    auto* section = IMAGE_FIRST_SECTION(&nt);
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i, ++section) {
        if (section->VirtualAddress % oh.SectionAlignment)
            throw std::runtime_error(name + ": a section RVA is not aligned to section alignment");
        if (section->SizeOfRawData && section->SizeOfRawData % oh.FileAlignment)
            throw std::runtime_error(name + ": a section raw size is not aligned to file alignment");
        if (section->SizeOfRawData && section->PointerToRawData % oh.FileAlignment)
            throw std::runtime_error(name + ": a section file offset is not aligned to file alignment");
        if (section->SizeOfRawData && oh.SectionAlignment < page_size &&
            section->PointerToRawData != section->VirtualAddress)
            throw std::runtime_error(name + ": sub-page section file offset does not match its RVA");
        if (section->SizeOfRawData) {
            const size_t raw_offset = section->PointerToRawData;
            if (raw_offset > file_size || static_cast<size_t>(section->SizeOfRawData) > file_size - raw_offset)
                throw std::runtime_error(name + ": a section's raw data runs past the end of the file");
        }

        // VirtualSize is the in-memory extent. SizeOfRawData can be larger because of file alignment;
        // those trailing padding bytes are not part of the section's mapped range.
        const uint64_t mapped_size = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
        if (!mapped_size) continue;
        if (section->VirtualAddress < oh.SizeOfHeaders)
            throw std::runtime_error(name + ": a section overlaps the mapped headers");
        require_range(name, section->VirtualAddress, mapped_size, oh.SizeOfImage, "a section's mapped range");
        if (have_previous && section->VirtualAddress < previous_rva)
            throw std::runtime_error(name + ": sections are not ordered by virtual address");
        if (have_previous && section->VirtualAddress < previous_end)
            throw std::runtime_error(name + ": sections overlap in the mapped image");
        previous_rva = section->VirtualAddress;
        previous_end = static_cast<uint64_t>(section->VirtualAddress) + mapped_size;
        have_previous = true;
    }
}

// The Xbox builds of the MSVC runtime import the same CRT the desktop does, so the host's own copies are used.
bool prefer_host_copy(const std::string& dll) {
    static const char* names[] = {"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll",
                                  "msvcp140_2.dll", "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll",
                                  "concrt140.dll", "vccorlib140.dll", "vcomp140.dll"};
    for (auto* n : names)
        if (dll == n) return true;
    return false;
}

}  // namespace

Loader::~Loader() {
    for (auto* m : order_) {
        // Before the image is freed: a stale table would let a later stack walk read unmapped memory.
        if (m->function_table) RtlDeleteFunctionTable(static_cast<PRUNTIME_FUNCTION>(m->function_table));
        if (m->tls_block) VirtualFree(m->tls_block, 0, MEM_RELEASE);
        if (m->has_tls_slot) TlsFree(m->tls_slot);
        if (m->base) VirtualFree(m->base, 0, MEM_RELEASE);
        delete m;
    }
}

Module& Loader::load(const std::filesystem::path& image) {
    std::string key = lower(image.filename().string());
    if (auto it = by_name_.find(key); it != by_name_.end()) return *it->second;
    Module& m = map_image(image);
    bind_imports(m);  // dependencies land in init_order_ before this importer
    protect_sections(m);
    register_unwind(m);
    init_order_.push_back(&m);
    return m;
}

// x64 has no frame-pointer chain to fall back on: without its .pdata registered, an exception raised in (or passing
// through) a mapped image can't be dispatched past that frame, and the process dies. Images the system loader maps
// are found through its own module list; ours are VirtualAlloc'd, so they are registered as dynamic function tables.
//
// The table is checked before it is handed over, because RtlLookupFunctionEntry trusts it: it binary-searches the
// entries by BeginAddress and follows UnwindData without bounds checks. Measured over 3,780 x64 images in
// System32 (5,023,071 entries) and the package's FidelityFX DLLs: every table is sorted and non-overlapping and in
// bounds, and one shipped image (symcryptk.dll) has a zero-length entry (BeginAddress == EndAddress), so that is
// allowed. No entry in that set uses the indirect form (UnwindData bit 0).
void Loader::register_unwind(Module& m) {
    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) return;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    // Like the system loader, a trailing partial entry is ignored.
    const uint32_t count = dir.Size / sizeof(RUNTIME_FUNCTION);
    if (!count) return;
    require_range(m.name, dir.VirtualAddress, static_cast<uint64_t>(count) * sizeof(RUNTIME_FUNCTION), m.size,
                  "the exception directory");
    auto* table = at<RUNTIME_FUNCTION>(m.base, dir.VirtualAddress);
    auto reject = [&](uint32_t i, const char* why) {
        throw std::runtime_error(m.name + ": unwind entry " + std::to_string(i) + " " + why);
    };
    uint32_t previous_end = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const RUNTIME_FUNCTION& f = table[i];
        if (f.EndAddress < f.BeginAddress || f.EndAddress > m.size) reject(i, "covers code outside the image");
        if (f.BeginAddress < previous_end) reject(i, "is out of order or overlaps the one before it");
        // UNWIND_INFO starts with a 4-byte header; its codes and handler data are the unwinder's to read.
        const uint32_t info = f.UnwindData & ~1u;
        if (info > m.size || m.size - info < 4) reject(i, "has unwind data outside the image");
        previous_end = f.EndAddress;
    }
    if (!RtlAddFunctionTable(table, count, reinterpret_cast<DWORD64>(m.base)))
        throw std::runtime_error(m.name + ": RtlAddFunctionTable failed");
    m.function_table = table;
    m.function_count = count;
}

void Loader::initialise() {
    // Resolve the full import graph before calling TLS callbacks or DllMain in any module.
    for (auto* module : init_order_) {
        for (const ImportRecord& import : module->imports) {
            if (import.binding == Binding::Unimplemented)
                throw std::runtime_error(module->name + ": unresolved import blocks initialization: " + import.dll +
                                         "!" + import.name);
        }
    }
    for (auto* module : init_order_) initialise(*module);
}

void Loader::initialise(Module& m) {
    if (m.initialised) return;
    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    const uint64_t base = reinterpret_cast<uint64_t>(m.base);

    // The TLS directory holds virtual addresses, which relocations have already moved to this mapping.
    auto rva_of = [&](uint64_t va, const char* what) {
        if (va < base || va - base >= m.size)
            throw std::runtime_error(m.name + ": " + what + " is outside the image");
        return va - base;
    };

    auto& tls_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tls_dir.Size) {
        require_range(m.name, tls_dir.VirtualAddress, sizeof(IMAGE_TLS_DIRECTORY64), m.size, "the TLS directory");
        auto* tls = at<IMAGE_TLS_DIRECTORY64>(m.base, tls_dir.VirtualAddress);

        uint64_t start = 0, template_size = 0;
        if (tls->StartAddressOfRawData || tls->EndAddressOfRawData) {
            start = rva_of(tls->StartAddressOfRawData, "the TLS template");
            uint64_t end = rva_of(tls->EndAddressOfRawData, "the end of the TLS template");
            if (end < start) throw std::runtime_error(m.name + ": the TLS template ends before it starts");
            template_size = end - start;
            require_range(m.name, start, template_size, m.size, "the TLS template");
        }
        m.tls_block_size = template_size + tls->SizeOfZeroFill;
        if (m.tls_block_size > (1u << 20)) throw std::runtime_error(m.name + ": the TLS block is larger than 1 MiB");
        if (m.tls_block_size) {
            m.tls_block = VirtualAlloc(nullptr, static_cast<SIZE_T>(m.tls_block_size), MEM_RESERVE | MEM_COMMIT,
                                       PAGE_READWRITE);
            if (!m.tls_block) throw std::runtime_error(m.name + ": cannot allocate the TLS block");
            if (template_size) std::memcpy(m.tls_block, m.base + start, static_cast<size_t>(template_size));
        }

        if (tls->AddressOfIndex) {
            uint64_t index_rva = rva_of(tls->AddressOfIndex, "the TLS index");
            require_range(m.name, index_rva, sizeof(uint32_t), m.size, "the TLS index");
            DWORD slot = TlsAlloc();
            if (slot == TLS_OUT_OF_INDEXES) throw std::runtime_error(m.name + ": no TLS slot is free");
            m.tls_slot = slot;
            m.has_tls_slot = true;
            if (m.tls_block) TlsSetValue(slot, m.tls_block);
            // The index lives in a writable section, but protect_sections already ran, so lift it for the write.
            auto* index = m.base + index_rva;
            auto* page = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(index) & ~uintptr_t(0xFFF));
            DWORD old = 0, ignored = 0;
            if (!VirtualProtect(page, 0x1000, PAGE_READWRITE, &old))
                throw std::runtime_error(m.name + ": the TLS index can't be written");
            *reinterpret_cast<uint32_t*>(index) = slot;
            VirtualProtect(page, 0x1000, old, &ignored);
        }

        if (tls->AddressOfCallBacks) {
            uint64_t table_rva = rva_of(tls->AddressOfCallBacks, "the TLS callback table");
            for (uint64_t i = 0;; ++i) {
                uint64_t entry_rva = table_rva + i * sizeof(uint64_t);
                require_range(m.name, entry_rva, sizeof(uint64_t), m.size, "a TLS callback table entry");
                uint64_t fn = *at<uint64_t>(m.base, static_cast<uint32_t>(entry_rva));
                if (!fn) break;
                auto callback = reinterpret_cast<void(NTAPI*)(void*, DWORD, void*)>(m.base + rva_of(fn, "a TLS callback"));
                callback(m.base, DLL_PROCESS_ATTACH, nullptr);
            }
        }
    }

    // Only a DLL gets DllMain. An EXE's entry point has a different signature and is the caller's to run.
    if ((nt->FileHeader.Characteristics & IMAGE_FILE_DLL) && m.entry_rva) {
        if (m.entry_rva >= m.size) throw std::runtime_error(m.name + ": the entry point is outside the image");
        auto entry = reinterpret_cast<BOOL(NTAPI*)(void*, DWORD, void*)>(m.base + m.entry_rva);
        if (!entry(m.base, DLL_PROCESS_ATTACH, nullptr)) {
            m.initialised = true;  // it ran: a second DLL_PROCESS_ATTACH would be wrong
            throw std::runtime_error(m.name + ": DllMain returned FALSE");
        }
    }
    m.initialised = true;
}

Module& Loader::map_image(const std::filesystem::path& path) {
    auto file = read_file(path);
    const std::string name = path.filename().string();
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    if (file.size() < sizeof(IMAGE_DOS_HEADER) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        throw std::runtime_error(path.string() + ": not an MZ image");
    if (dos->e_lfanew < 0 || static_cast<uint64_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file.size())
        throw std::runtime_error(name + ": the NT headers are outside the file");
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
        throw std::runtime_error(name + ": not an x86-64 PE");
    if (nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        throw std::runtime_error(name + ": not a PE32+ image");
    auto& oh = nt->OptionalHeader;
    uint64_t sections = static_cast<uint64_t>(dos->e_lfanew) + 4 + sizeof(IMAGE_FILE_HEADER) +
                        nt->FileHeader.SizeOfOptionalHeader;
    if (sections + static_cast<uint64_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER) > file.size())
        throw std::runtime_error(name + ": the section table runs past the end of the file");
    if (!oh.SizeOfImage || oh.SizeOfHeaders > oh.SizeOfImage || oh.SizeOfHeaders > file.size())
        throw std::runtime_error(name + ": SizeOfHeaders and SizeOfImage don't fit the file");
    validate_section_layout(name, file.size(), static_cast<uint32_t>(dos->e_lfanew), *nt);

    auto* m = new Module;
    m->name = name;
    m->path = path;
    m->preferred_base = oh.ImageBase;
    m->size = oh.SizeOfImage;
    m->entry_rva = oh.AddressOfEntryPoint;
    by_name_[lower(m->name)] = m;
    order_.push_back(m);

    m->base = static_cast<uint8_t*>(
        VirtualAlloc(reinterpret_cast<void*>(oh.ImageBase), oh.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!m->base) {
        auto& relocs = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (!relocs.Size)
            throw std::runtime_error(m->name + ": preferred base is taken and the image has no relocations");
        m->base = static_cast<uint8_t*>(VirtualAlloc(nullptr, oh.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!m->base) throw std::runtime_error(m->name + ": VirtualAlloc failed");
    }

    std::memcpy(m->base, file.data(), oh.SizeOfHeaders);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        uint32_t n = std::min(sec->SizeOfRawData, sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData);
        if (!n) continue;
        if (sec->PointerToRawData > file.size() || n > file.size() - sec->PointerToRawData)
            throw std::runtime_error(m->name + ": section " + std::to_string(i) + " reads past the end of the file");
        require_range(m->name, sec->VirtualAddress, n, m->size, "a section's data");
        std::memcpy(m->base + sec->VirtualAddress, file.data() + sec->PointerToRawData, n);
    }

    int64_t delta = reinterpret_cast<int64_t>(m->base) - static_cast<int64_t>(oh.ImageBase);
    if (delta) {
        m->relocated = true;
        auto& dir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (!dir.Size) throw std::runtime_error(m->name + ": the image has no relocations and isn't at its base");
        require_range(m->name, dir.VirtualAddress, dir.Size, m->size, "the relocation table");
        auto* block = at<IMAGE_BASE_RELOCATION>(m->base, dir.VirtualAddress);
        auto* end = reinterpret_cast<uint8_t*>(block) + dir.Size;
        while (reinterpret_cast<uint8_t*>(block) < end) {
            if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
                reinterpret_cast<uint8_t*>(block) + block->SizeOfBlock > end)
                throw std::runtime_error(m->name + ": a relocation block doesn't fit the relocation table");
            auto* entries = reinterpret_cast<uint16_t*>(block + 1);
            size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
            for (size_t i = 0; i < count; ++i) {
                uint16_t type = entries[i] >> 12, off = entries[i] & 0xFFF;
                if (type == IMAGE_REL_BASED_DIR64) {
                    require_range(m->name, static_cast<uint64_t>(block->VirtualAddress) + off, sizeof(uint64_t), m->size,
                                  "a relocation");
                    *at<uint64_t>(m->base, block->VirtualAddress + off) += delta;
                } else if (type != IMAGE_REL_BASED_ABSOLUTE) {
                    throw std::runtime_error(m->name + ": unsupported relocation type " + std::to_string(type));
                }
            }
            block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(reinterpret_cast<uint8_t*>(block) + block->SizeOfBlock);
        }
    }
    parse_exports(*m);
    return *m;
}

void Loader::parse_exports(Module& m) {
    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.Size) return;
    require_range(m.name, dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), m.size, "the export directory");
    auto* ed = at<IMAGE_EXPORT_DIRECTORY>(m.base, dir.VirtualAddress);
    require_range(m.name, ed->AddressOfFunctions, static_cast<uint64_t>(ed->NumberOfFunctions) * sizeof(uint32_t), m.size,
                  "the export address table");
    require_range(m.name, ed->AddressOfNames, static_cast<uint64_t>(ed->NumberOfNames) * sizeof(uint32_t), m.size,
                  "the export name table");
    require_range(m.name, ed->AddressOfNameOrdinals, static_cast<uint64_t>(ed->NumberOfNames) * sizeof(uint16_t), m.size,
                  "the export ordinal table");
    auto* funcs = at<uint32_t>(m.base, ed->AddressOfFunctions);
    auto* names = at<uint32_t>(m.base, ed->AddressOfNames);
    auto* ords = at<uint16_t>(m.base, ed->AddressOfNameOrdinals);
    // An export whose RVA lands inside the export directory is a forwarder string ("KERNEL32.Sleep",
    // "api-ms-win-core-file-l1-1-0.CreateFileW"), not code. Storing base + that RVA would hand callers a pointer
    // into the string, so forwarders are kept apart and followed when an import asks for one.
    const uint64_t dir_begin = dir.VirtualAddress, dir_end = dir_begin + dir.Size;
    auto add_export = [&](const std::string& key, uint32_t rva) {
        if (rva >= dir_begin && rva < dir_end)
            m.forwarders[key] = read_string(m.name, m.base, m.size, rva, "a forwarded export");
        else
            m.exports[key] = m.base + rva;
    };
    for (uint32_t i = 0; i < ed->NumberOfFunctions; ++i)
        if (funcs[i]) add_export("#" + std::to_string(ed->Base + i), funcs[i]);
    for (uint32_t i = 0; i < ed->NumberOfNames; ++i) {
        if (ords[i] >= ed->NumberOfFunctions)
            throw std::runtime_error(m.name + ": an export name points past the address table");
        add_export(read_string(m.name, m.base, m.size, names[i], "an export name"), funcs[ords[i]]);
    }
}

// Follows a forwarder string to the module and export it names. Bundled modules are searched first, with or without
// the extension, because a forwarder may point at another mapped module; system DLLs go through LoadLibraryEx, whose
// own GetProcAddress already follows its forwarders. Errors are "not found": the caller decides what to bind.
void* Loader::resolve_forwarder(const std::string& spec, Binding& binding, int depth) {
    if (depth >= 8) return nullptr;  // a longer chain is a loop between modules, not a re-export
    auto dot = spec.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= spec.size()) return nullptr;
    const std::string module = spec.substr(0, dot), symbol = spec.substr(dot + 1);

    // A forwarder can name a module the loader shims. Shims take precedence over a bundled or host copy, exactly as
    // they do for a direct import in resolve(); without this, a forwarded xmem or d3d12_xs call becomes a failure thunk.
    std::string shim_key = lower(module);
    if (shim_key.size() < 4 || shim_key.compare(shim_key.size() - 4, 4, ".dll") != 0) shim_key += ".dll";
    if (void* p = shims::find(shim_key, symbol)) {
        binding = Binding::Shim;
        return p;
    }

    const bool by_ordinal = symbol.size() > 1 && symbol[0] == '#';
    uint32_t ordinal = 0;
    if (by_ordinal) {
        for (size_t i = 1; i < symbol.size(); ++i) {
            if (symbol[i] < '0' || symbol[i] > '9' || ordinal > 0xFFFF) return nullptr;
            ordinal = ordinal * 10 + static_cast<uint32_t>(symbol[i] - '0');
        }
    }

    const std::filesystem::path candidates[] = {game_dir_ / module, game_dir_ / (module + ".dll")};
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (prefer_host_copy(lower(module)) || !std::filesystem::exists(candidate, ec)) continue;
        Module& dep = load(candidate);
        if (auto it = dep.exports.find(symbol); it != dep.exports.end()) {
            binding = Binding::Module;
            return it->second;
        }
        if (auto it = dep.forwarders.find(symbol); it != dep.forwarders.end())
            return resolve_forwarder(it->second, binding, depth + 1);
        return nullptr;  // the bundled module exists and does not export it; don't look for a host namesake
    }

    HMODULE h = LoadLibraryExA(module.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) return nullptr;
    FARPROC p = GetProcAddress(h, by_ordinal ? MAKEINTRESOURCEA(static_cast<WORD>(ordinal)) : symbol.c_str());
    if (!p) return nullptr;
    binding = Binding::Host;
    return reinterpret_cast<void*>(p);
}

void* Loader::resolve(Module& m, const std::string& dll, const std::string& name, uint16_t ordinal, bool by_ordinal) {
    std::string key = lower(dll);
    std::string sym = by_ordinal ? "#" + std::to_string(ordinal) : name;

    if (void* p = shims::find(key, sym)) {
        m.imports.push_back({dll, sym, Binding::Shim});
        return p;
    }
    // Bundled game module (an Xbox build that ships beside the game)?
    auto bundled = game_dir_ / dll;
    if (!prefer_host_copy(key) && std::filesystem::exists(bundled)) {
        Module& dep = load(bundled);
        if (auto it = dep.exports.find(sym); it != dep.exports.end()) {
            m.imports.push_back({dll, sym, Binding::Module});
            return it->second;
        }
        if (auto it = dep.forwarders.find(sym); it != dep.forwarders.end()) {
            Binding binding = Binding::Unimplemented;
            if (void* p = resolve_forwarder(it->second, binding, 0)) {
                m.imports.push_back({dll, sym, binding});
                return p;
            }
        }
    }
    // Host Windows: system DLLs and API sets. LOAD_LIBRARY_SEARCH_SYSTEM32 keeps the game folder out of the search.
    if (HMODULE h = LoadLibraryExA(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        FARPROC p = GetProcAddress(h, by_ordinal ? MAKEINTRESOURCEA(ordinal) : name.c_str());
        if (p) {
            m.imports.push_back({dll, sym, Binding::Host});
            return reinterpret_cast<void*>(p);
        }
    }
    m.imports.push_back({dll, sym, Binding::Unimplemented});
    return shims::make_unimplemented_thunk(dll, sym);
}

void Loader::bind_imports(Module& m) {
    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);

    // Thunk tables have no length field: they end at a zero entry, so each step is checked against the image.
    auto bind_thunks = [&](const std::string& dll, uint64_t lookup_rva, uint64_t iat_rva) {
        for (;; lookup_rva += sizeof(IMAGE_THUNK_DATA64), iat_rva += sizeof(IMAGE_THUNK_DATA64)) {
            require_range(m.name, lookup_rva, sizeof(IMAGE_THUNK_DATA64), m.size, "an import lookup table");
            require_range(m.name, iat_rva, sizeof(IMAGE_THUNK_DATA64), m.size, "an import address table");
            auto* lookup = at<IMAGE_THUNK_DATA64>(m.base, static_cast<uint32_t>(lookup_rva));
            if (!lookup->u1.AddressOfData) return;
            bool by_ord = IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal);
            uint16_t ord = by_ord ? (uint16_t)IMAGE_ORDINAL64(lookup->u1.Ordinal) : 0;
            std::string name;
            if (!by_ord) {
                uint32_t hint = static_cast<uint32_t>(lookup->u1.AddressOfData);
                require_range(m.name, hint, sizeof(uint16_t), m.size, "an import name");
                name = read_string(m.name, m.base, m.size, hint + sizeof(uint16_t), "an import name");
            }
            at<IMAGE_THUNK_DATA64>(m.base, static_cast<uint32_t>(iat_rva))->u1.Function =
                reinterpret_cast<uint64_t>(resolve(m, dll, name, ord, by_ord));
        }
    };

    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.Size) {
        for (uint64_t i = 0;; ++i) {
            uint64_t rva = dir.VirtualAddress + i * sizeof(IMAGE_IMPORT_DESCRIPTOR);
            require_range(m.name, rva, sizeof(IMAGE_IMPORT_DESCRIPTOR), m.size, "the import table");
            auto* d = at<IMAGE_IMPORT_DESCRIPTOR>(m.base, static_cast<uint32_t>(rva));
            if (!d->Name) break;
            std::string dll = read_string(m.name, m.base, m.size, d->Name, "an imported DLL name");
            uint32_t lookup = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk;
            bind_thunks(dll, lookup, d->FirstThunk);
        }
    }
    // Delay imports are bound eagerly: simpler, and the full import surface shows up in --dry-run.
    auto& ddir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (ddir.Size) {
        for (uint64_t i = 0;; ++i) {
            uint64_t rva = ddir.VirtualAddress + i * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
            require_range(m.name, rva, sizeof(IMAGE_DELAYLOAD_DESCRIPTOR), m.size, "the delay-import table");
            auto* d = at<IMAGE_DELAYLOAD_DESCRIPTOR>(m.base, static_cast<uint32_t>(rva));
            if (!d->DllNameRVA) break;
            // x64 descriptors always carry dlattrRva (0x1). Without it the fields would be 32-bit virtual
            // addresses, which cannot express a 64-bit image base, so the descriptor is not usable as written.
            if (!d->Attributes.RvaBased)
                throw std::runtime_error(m.name + ": a delay-import descriptor without the RVA attribute");
            if (!d->ImportAddressTableRVA)
                throw std::runtime_error(m.name + ": a delay-import descriptor has no address table");
            std::string dll = read_string(m.name, m.base, m.size, d->DllNameRVA, "a delay-imported DLL name");
            // The name table is optional: without it, entries are read back through the address table itself.
            bind_thunks(dll, d->ImportNameTableRVA ? d->ImportNameTableRVA : d->ImportAddressTableRVA,
                        d->ImportAddressTableRVA);
        }
    }
}

void Loader::protect_sections(Module& m) {
    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const size_t page_size = system_info.dwPageSize;
    const size_t page_count = (static_cast<size_t>(m.size) + page_size - 1) / page_size;
    constexpr uint8_t kRead = 1, kWrite = 2, kExecute = 4;
    std::vector<uint8_t> page_access(page_count, 0);

    auto mark_pages = [&](uint64_t rva, uint64_t size, uint8_t access) {
        if (!size) return;
        const size_t first = static_cast<size_t>(rva / page_size);
        const size_t last = static_cast<size_t>((rva + size - 1) / page_size);
        for (size_t page = first; page <= last; ++page) page_access[page] |= access;
    };

    mark_pages(0, nt->OptionalHeader.SizeOfHeaders, kRead);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        uint8_t access = 0;
        if (sec->Characteristics & IMAGE_SCN_MEM_READ) access |= kRead;
        if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) access |= kWrite;
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) access |= kExecute;
        uint64_t size = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        mark_pages(sec->VirtualAddress, size, access);
    }

    auto protection_for = [](uint8_t access) -> DWORD {
        const bool readable = (access & kRead) != 0;
        const bool writable = (access & kWrite) != 0;
        const bool executable = (access & kExecute) != 0;
        if (executable) return writable ? PAGE_EXECUTE_READWRITE : (readable ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
        if (writable) return PAGE_READWRITE;
        return readable ? PAGE_READONLY : PAGE_NOACCESS;
    };

    DWORD old = 0;
    if (!VirtualProtect(m.base, m.size, PAGE_NOACCESS, &old))
        throw std::runtime_error(m.name + ": failed to set default image page protection");
    for (size_t first = 0; first < page_count;) {
        const DWORD protection = protection_for(page_access[first]);
        size_t end = first + 1;
        while (end < page_count && protection_for(page_access[end]) == protection) ++end;
        const SIZE_T bytes = (end - first) * page_size;
        if (!VirtualProtect(m.base + first * page_size, bytes, protection, &old))
            throw std::runtime_error(m.name + ": failed to set PE section page protection");
        first = end;
    }
    FlushInstructionCache(GetCurrentProcess(), m.base, m.size);
}


}  // namespace rt
