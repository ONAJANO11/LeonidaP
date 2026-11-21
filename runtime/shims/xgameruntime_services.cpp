// xgameruntime services: user, save storage, package, threads, networking hint and streaming, offline.
//
// Part 2 of the offline GDK runtime (part 1 is xgameruntime.cpp). A title signs in a user, opens save storage and
// asks about its package during boot. Here there is one silent local user, saves are files in a local folder and the
// package is the folder the game runs from. Signatures, enum values and size constants follow Microsoft's public GDK
// documentation (learn.microsoft.com/gaming/gdk); no GDK header or library code is used. Values marked VERIFY come
// from that documentation and should be checked against the executable when it exists.
//
// Deliberately not here: XStore licence queries. Answering them would mean telling the game it is licensed whatever
// the truth is, which is a protection workaround (CONTRIBUTING.md). Those imports stay unimplemented and are logged.
//
// Nothing here goes online.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <XAsync.h>
#include <XAsyncProvider.h>
#include <XTaskQueue.h>

#include "shims.h"

namespace fs = std::filesystem;

namespace {

std::string env(const char* name) {
    char* s = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&s, &len, name) == 0 && s) {
        out = s;
        std::free(s);
    }
    return out;
}

HRESULT copy_string(const std::string& value, size_t size, char* out, size_t* used) {
    if (!out && size) return E_INVALIDARG;
    const size_t need = value.size() + 1;
    if (used) *used = need;
    if (size < need) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(out, value.c_str(), need);
    return S_OK;
}

// =====================================================================================================  XUser

// One local, signed-in, non-guest user. It is the only user, so every XUserHandle refers to it; handles are counted
// so duplicate/close pairs balance as in the GDK.
struct LocalUser {
    std::atomic<int> refs{0};
    uint64_t local_id = 1;
    uint64_t xuid = 0x0009000000000001ull;  // a fixed offline id; nothing ever sends it anywhere
    std::string gamertag = "Player";
};
LocalUser g_user;

enum class XUserState : uint32_t { SignedIn = 0, SigningOut = 1, SignedOut = 2 };
enum class XUserAgeGroup : uint32_t { Unknown = 0, Child = 1, Teen = 2, Adult = 3 };
enum class XUserGamertagComponent : uint32_t { Classic = 0, Modern = 1, ModernSuffix = 2, UniqueModern = 3 };
// Buffer sizes including the terminator (XUserGamertagComponent*_MaxBytes). VERIFY.
constexpr size_t kGamertagMax[] = {16, 97, 15, 101};

struct XUserLocalId {
    uint64_t value;
};

void* user_handle() {
    ++g_user.refs;
    return &g_user;
}
bool is_user(void* h) { return h == &g_user; }

const int kUserAddIdentity = 0;

HRESULT CALLBACK user_add_provider(XAsyncOp op, const XAsyncProviderData* data) {
    switch (op) {
        case XAsyncOp::Begin: return XAsyncSchedule(data->async, 0);
        case XAsyncOp::DoWork: XAsyncComplete(data->async, S_OK, sizeof(void*)); return S_OK;
        case XAsyncOp::GetResult: *static_cast<void**>(data->buffer) = user_handle(); return S_OK;
        default: return S_OK;
    }
}

// HRESULT XUserAddAsync(XUserAddOptions options, XAsyncBlock* async). Every option resolves to the local user,
// silently: there is no sign-in UI.
HRESULT __stdcall XUserAddAsync(uint32_t /*options*/, XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    return XAsyncBegin(async, nullptr, &kUserAddIdentity, "XUserAddAsync", user_add_provider);
}

// HRESULT XUserAddResult(XAsyncBlock* async, XUserHandle* newUser)
HRESULT __stdcall XUserAddResult(XAsyncBlock* async, void** user) {
    if (!async || !user) return E_INVALIDARG;
    return XAsyncGetResult(async, &kUserAddIdentity, sizeof(void*), user, nullptr);
}

HRESULT __stdcall XUserDuplicateHandle(void* user, void** dup) {
    if (!is_user(user) || !dup) return E_INVALIDARG;
    *dup = user_handle();
    return S_OK;
}

void __stdcall XUserCloseHandle(void* user) {
    if (is_user(user) && g_user.refs.load() > 0) --g_user.refs;
}

int32_t __stdcall XUserCompare(void* a, void* b) { return a == b ? 0 : (a < b ? -1 : 1); }

HRESULT __stdcall XUserGetMaxUsers(uint32_t* max_users) {
    if (!max_users) return E_INVALIDARG;
    *max_users = 1;
    return S_OK;
}

HRESULT __stdcall XUserGetLocalId(void* user, XUserLocalId* id) {
    if (!is_user(user) || !id) return E_INVALIDARG;
    id->value = g_user.local_id;
    return S_OK;
}

HRESULT __stdcall XUserFindUserByLocalId(XUserLocalId id, void** user) {
    if (!user) return E_INVALIDARG;
    if (id.value != g_user.local_id) return E_INVALIDARG;
    *user = user_handle();
    return S_OK;
}

HRESULT __stdcall XUserGetId(void* user, uint64_t* xuid) {
    if (!is_user(user) || !xuid) return E_INVALIDARG;
    *xuid = g_user.xuid;
    return S_OK;
}

HRESULT __stdcall XUserIsGuest(void* user, bool* guest) {
    if (!is_user(user) || !guest) return E_INVALIDARG;
    *guest = false;
    return S_OK;
}

HRESULT __stdcall XUserGetState(void* user, XUserState* state) {
    if (!is_user(user) || !state) return E_INVALIDARG;
    *state = XUserState::SignedIn;
    return S_OK;
}

HRESULT __stdcall XUserGetAgeGroup(void* user, XUserAgeGroup* group) {
    if (!is_user(user) || !group) return E_INVALIDARG;
    *group = XUserAgeGroup::Unknown;
    return S_OK;
}

// HRESULT XUserGetGamertag(XUserHandle user, XUserGamertagComponent component, size_t gamertagSize,
//                          char* gamertag, size_t* gamertagUsed)
HRESULT __stdcall XUserGetGamertag(void* user, XUserGamertagComponent component, size_t size, char* out,
                                   size_t* used) {
    if (!is_user(user)) return E_INVALIDARG;
    auto c = static_cast<uint32_t>(component);
    if (c > 3) return E_INVALIDARG;
    std::string value = component == XUserGamertagComponent::ModernSuffix ? std::string() : g_user.gamertag;
    if (value.size() + 1 > kGamertagMax[c]) value.resize(kGamertagMax[c] - 1);
    return copy_string(value, size, out, used);
}

// The local user never changes, so change events are accepted and never fire.
std::atomic<uint64_t> g_next_token{1};

HRESULT __stdcall XUserRegisterForChangeEvent(XTaskQueueHandle, void*, void*, XTaskQueueRegistrationToken* token) {
    if (!token) return E_INVALIDARG;
    token->token = g_next_token++;
    return S_OK;
}

bool __stdcall XUserUnregisterForChangeEvent(XTaskQueueRegistrationToken, bool) { return true; }

// =====================================================================================================  XGameSave

// Error codes from the public XGameSave documentation. VERIFY.
constexpr HRESULT E_GS_INVALID_CONTAINER_NAME = static_cast<HRESULT>(0x80830001);
constexpr HRESULT E_GS_PROVIDED_BUFFER_TOO_SMALL = static_cast<HRESULT>(0x80830007);
constexpr HRESULT E_GS_BLOB_NOT_FOUND = static_cast<HRESULT>(0x80830008);
constexpr HRESULT E_GS_CONTAINER_NOT_FOUND = static_cast<HRESULT>(0x80830010);  // VERIFY: name and value
constexpr int64_t kQuotaBytes = 256ll << 20;  // the default per-user quota

struct XGameSaveBlobInfo {
    const char* name;
    uint32_t size;
};
struct XGameSaveBlob {
    XGameSaveBlobInfo info;
    uint8_t* data;
};
struct XGameSaveContainerInfo {
    const char* name;
    const char* displayName;
    uint32_t blobCount;
    uint64_t totalSize;
    time_t lastModifiedTime;
    bool needsSync;
};
using BlobInfoCallback = bool(CALLBACK*)(const XGameSaveBlobInfo*, void*);
using ContainerInfoCallback = bool(CALLBACK*)(const XGameSaveContainerInfo*, void*);

// Container and blob names become folder and file names. Anything outside [A-Za-z0-9._-], a leading or trailing '.',
// and the first character of a Windows device name are written as %XX, so every name round-trips, none can leave
// the save folder, and no encoded name starts with '~', which marks the temporary files of an update in progress.
// Names that differ only in case still share a file on NTFS (whether XGameSave names are case-sensitive: VERIFY).
// Windows device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9, also with an extension) can't be file or folder names,
// and Win32 strips a trailing '.' or ' ': escaping the first character of the one and the last of the other keeps
// every name a distinct, real file.
bool reserved_device_name(const std::string& name) {
    std::string stem = name.substr(0, name.find('.'));
    for (auto& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    return stem.size() == 4 && (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) && stem[3] >= '1' &&
           stem[3] <= '9';
}

std::string encode_name(const std::string& name) {
    static const char* hex = "0123456789ABCDEF";
    const bool device = reserved_device_name(name);
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        const bool last = i + 1 == name.size();
        const bool plain = std::isalnum(c) || c == '_' || c == '-' || (c == '.' && i > 0 && !last);
        if (plain && !(device && i == 0)) out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

std::string decode_name(const std::string& enc) {
    std::string out;
    for (size_t i = 0; i < enc.size(); ++i) {
        if (enc[i] == '%' && i + 2 < enc.size()) {
            out += static_cast<char>(std::stoi(enc.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += enc[i];
        }
    }
    return out;
}

bool valid_name(const char* name) {
    if (!name || !*name) return false;
    size_t n = std::strlen(name);
    return n <= 255;
}

struct Provider {
    fs::path root;  // <save dir>/<user>
};
struct Container {
    Provider* provider;
    std::string name;
    fs::path dir;
};
struct Update {
    Container* container;
    std::string display_name;
    std::map<std::string, std::vector<uint8_t>> writes;
    std::set<std::string> deletes;
};

std::mutex g_save_mutex;

// Saves live in LEONIDA_SAVE_DIR, or %LOCALAPPDATA%\Leonida\saves, one folder per user.
fs::path save_root() {
    std::string d = env("LEONIDA_SAVE_DIR");
    if (d.empty()) {
        std::string local = env("LOCALAPPDATA");
        d = (fs::path(local.empty() ? "." : local) / "Leonida" / "saves").string();
    }
    return fs::path(d) / "local-user";
}

const int kProviderIdentity = 0;
const int kSubmitIdentity = 0;

// HRESULT XGameSaveInitializeProvider(XUserHandle requestingUser, const char* configurationId, bool syncOnDemand,
//                                     XGameSaveProviderHandle* provider)
HRESULT __stdcall XGameSaveInitializeProvider(void* user, const char* /*scid*/, bool /*sync_on_demand*/,
                                              void** provider) {
    if (!is_user(user) || !provider) return E_INVALIDARG;
    auto* p = new Provider{save_root()};
    std::error_code ec;
    fs::create_directories(p->root, ec);
    if (ec) {
        delete p;
        return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    }
    *provider = p;
    return S_OK;
}

struct ProviderBegin {
    void* user;
    void* provider = nullptr;
    HRESULT hr = S_OK;
};

HRESULT CALLBACK provider_init_op(XAsyncOp op, const XAsyncProviderData* data) {
    auto* st = static_cast<ProviderBegin*>(data->context);
    switch (op) {
        case XAsyncOp::Begin: return XAsyncSchedule(data->async, 0);
        case XAsyncOp::DoWork:
            st->hr = XGameSaveInitializeProvider(st->user, nullptr, false, &st->provider);
            XAsyncComplete(data->async, st->hr, SUCCEEDED(st->hr) ? sizeof(void*) : 0);
            return S_OK;
        case XAsyncOp::GetResult: *static_cast<void**>(data->buffer) = st->provider; return S_OK;
        case XAsyncOp::Cleanup: delete st; return S_OK;
        default: return S_OK;
    }
}

// HRESULT XGameSaveInitializeProviderAsync(XUserHandle, const char* configurationId, bool syncOnDemand, XAsyncBlock*)
HRESULT __stdcall XGameSaveInitializeProviderAsync(void* user, const char*, bool, XAsyncBlock* async) {
    if (!is_user(user) || !async) return E_INVALIDARG;
    auto* st = new ProviderBegin{user};
    HRESULT hr = XAsyncBegin(async, st, &kProviderIdentity, "XGameSaveInitializeProviderAsync", provider_init_op);
    if (FAILED(hr)) delete st;
    return hr;
}

HRESULT __stdcall XGameSaveInitializeProviderResult(XAsyncBlock* async, void** provider) {
    if (!async || !provider) return E_INVALIDARG;
    return XAsyncGetResult(async, &kProviderIdentity, sizeof(void*), provider, nullptr);
}

void __stdcall XGameSaveCloseProvider(void* provider) { delete static_cast<Provider*>(provider); }

HRESULT __stdcall XGameSaveGetRemainingQuota(void* provider, int64_t* remaining) {
    auto* p = static_cast<Provider*>(provider);
    if (!p || !remaining) return E_INVALIDARG;
    std::lock_guard lock(g_save_mutex);
    int64_t used = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(p->root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_regular_file(ec)) used += static_cast<int64_t>(it->file_size(ec));
    *remaining = std::max<int64_t>(0, kQuotaBytes - used);
    return S_OK;
}

HRESULT __stdcall XGameSaveCreateContainer(void* provider, const char* name, void** container) {
    auto* p = static_cast<Provider*>(provider);
    if (!p || !container) return E_INVALIDARG;
    if (!valid_name(name)) return E_GS_INVALID_CONTAINER_NAME;
    *container = new Container{p, name, p->root / encode_name(name)};
    return S_OK;
}

void __stdcall XGameSaveCloseContainer(void* container) { delete static_cast<Container*>(container); }

HRESULT __stdcall XGameSaveDeleteContainer(void* provider, const char* name) {
    auto* p = static_cast<Provider*>(provider);
    if (!p) return E_INVALIDARG;
    if (!valid_name(name)) return E_GS_INVALID_CONTAINER_NAME;
    std::lock_guard lock(g_save_mutex);
    std::error_code ec;
    fs::path dir = p->root / encode_name(name);
    if (!fs::exists(dir, ec)) return E_GS_CONTAINER_NOT_FOUND;
    fs::remove_all(dir, ec);
    return ec ? E_FAIL : S_OK;
}

struct BlobFile {
    std::string name;
    fs::path path;
    uint64_t size;
};

std::vector<BlobFile> list_blobs(const fs::path& dir) {
    std::vector<BlobFile> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().filename().string().rfind('~', 0) != 0)
            out.push_back({decode_name(it->path().filename().string()), it->path(), it->file_size(ec)});
    std::sort(out.begin(), out.end(), [](const BlobFile& a, const BlobFile& b) { return a.name < b.name; });
    return out;
}

time_t mtime(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    return std::chrono::system_clock::to_time_t(sys);
}

// Enumerates the user's containers (prefix filter optional, as in the GDK); the callback returns false to stop.
HRESULT __stdcall XGameSaveEnumerateContainerInfoByName(void* provider, const char* prefix, void* context,
                                                        ContainerInfoCallback callback) {
    auto* p = static_cast<Provider*>(provider);
    if (!p || !callback) return E_INVALIDARG;
    std::vector<std::pair<std::string, fs::path>> dirs;
    {
        std::lock_guard lock(g_save_mutex);
        std::error_code ec;
        for (auto it = fs::directory_iterator(p->root, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->is_directory(ec)) dirs.emplace_back(decode_name(it->path().filename().string()), it->path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (auto& [name, dir] : dirs) {
        if (prefix && *prefix && name.rfind(prefix, 0) != 0) continue;
        auto blobs = list_blobs(dir);
        uint64_t total = 0;
        for (auto& b : blobs) total += b.size;
        XGameSaveContainerInfo info{name.c_str(), name.c_str(), static_cast<uint32_t>(blobs.size()), total, mtime(dir),
                                    false};
        if (!callback(&info, context)) break;
    }
    return S_OK;
}

HRESULT __stdcall XGameSaveEnumerateContainerInfo(void* provider, void* context, ContainerInfoCallback callback) {
    return XGameSaveEnumerateContainerInfoByName(provider, nullptr, context, callback);
}

HRESULT __stdcall XGameSaveGetContainerInfo(void* provider, const char* name, void* context,
                                            ContainerInfoCallback callback) {
    auto* p = static_cast<Provider*>(provider);
    if (!p || !callback) return E_INVALIDARG;
    if (!valid_name(name)) return E_GS_INVALID_CONTAINER_NAME;
    fs::path dir = p->root / encode_name(name);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return E_GS_CONTAINER_NOT_FOUND;
    auto blobs = list_blobs(dir);
    uint64_t total = 0;
    for (auto& b : blobs) total += b.size;
    XGameSaveContainerInfo info{name, name, static_cast<uint32_t>(blobs.size()), total, mtime(dir), false};
    callback(&info, context);
    return S_OK;
}

HRESULT __stdcall XGameSaveEnumerateBlobInfoByName(void* container, const char* prefix, void* context,
                                                   BlobInfoCallback callback) {
    auto* c = static_cast<Container*>(container);
    if (!c || !callback) return E_INVALIDARG;
    std::vector<BlobFile> blobs;
    {
        std::lock_guard lock(g_save_mutex);
        blobs = list_blobs(c->dir);
    }
    for (auto& b : blobs) {
        if (prefix && *prefix && b.name.rfind(prefix, 0) != 0) continue;
        XGameSaveBlobInfo info{b.name.c_str(), static_cast<uint32_t>(b.size)};
        if (!callback(&info, context)) break;
    }
    return S_OK;
}

HRESULT __stdcall XGameSaveEnumerateBlobInfo(void* container, void* context, BlobInfoCallback callback) {
    return XGameSaveEnumerateBlobInfoByName(container, nullptr, context, callback);
}

// HRESULT XGameSaveReadBlobData(XGameSaveContainerHandle container, const char** blobNames, uint32_t* countOfBlobs,
//                               size_t blobsSize, XGameSaveBlob* blobData)
// blobData is one buffer: an XGameSaveBlob array, then each blob's name and data, which the array points into.
// With blobNames null every blob in the container is read. On return *countOfBlobs is the number read.
HRESULT __stdcall XGameSaveReadBlobData(void* container, const char** names, uint32_t* count, size_t size,
                                        XGameSaveBlob* out) {
    auto* c = static_cast<Container*>(container);
    if (!c || !count) return E_INVALIDARG;
    std::lock_guard lock(g_save_mutex);
    std::vector<BlobFile> want;
    if (names) {
        for (uint32_t i = 0; i < *count; ++i) {
            if (!names[i]) return E_INVALIDARG;
            fs::path path = c->dir / encode_name(names[i]);
            std::error_code ec;
            if (!fs::is_regular_file(path, ec)) return E_GS_BLOB_NOT_FOUND;
            want.push_back({names[i], path, fs::file_size(path, ec)});
        }
    } else {
        want = list_blobs(c->dir);
    }
    size_t need = want.size() * sizeof(XGameSaveBlob);
    for (auto& b : want) need += b.name.size() + 1 + b.size;
    if (!out || size < need) {
        *count = static_cast<uint32_t>(want.size());
        return E_GS_PROVIDED_BUFFER_TOO_SMALL;
    }
    auto* cursor = reinterpret_cast<uint8_t*>(out + want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        std::memcpy(cursor, want[i].name.c_str(), want[i].name.size() + 1);
        out[i].info.name = reinterpret_cast<const char*>(cursor);
        cursor += want[i].name.size() + 1;
        std::ifstream f(want[i].path, std::ios::binary);
        f.read(reinterpret_cast<char*>(cursor), static_cast<std::streamsize>(want[i].size));
        out[i].info.size = static_cast<uint32_t>(want[i].size);
        out[i].data = cursor;
        cursor += want[i].size;
    }
    *count = static_cast<uint32_t>(want.size());
    return S_OK;
}

HRESULT __stdcall XGameSaveCreateUpdate(void* container, const char* display_name, void** update) {
    auto* c = static_cast<Container*>(container);
    if (!c || !update) return E_INVALIDARG;
    *update = new Update{c, display_name ? display_name : "", {}, {}};
    return S_OK;
}

void __stdcall XGameSaveCloseUpdate(void* update) { delete static_cast<Update*>(update); }

HRESULT __stdcall XGameSaveSubmitBlobWrite(void* update, const char* name, const uint8_t* data, size_t size) {
    auto* u = static_cast<Update*>(update);
    if (!u || !valid_name(name) || (size && !data)) return E_INVALIDARG;
    u->deletes.erase(name);
    u->writes[name].assign(data, data + size);
    return S_OK;
}

HRESULT __stdcall XGameSaveSubmitBlobDelete(void* update, const char* name) {
    auto* u = static_cast<Update*>(update);
    if (!u || !valid_name(name)) return E_INVALIDARG;
    u->writes.erase(name);
    u->deletes.insert(name);
    return S_OK;
}

// Applies an update. Each blob is written to a temporary file and renamed over the old one, so a crash mid-save
// leaves either the old or the new blob, never a torn one.
HRESULT __stdcall XGameSaveSubmitUpdate(void* update) {
    auto* u = static_cast<Update*>(update);
    if (!u) return E_INVALIDARG;
    std::lock_guard lock(g_save_mutex);
    std::error_code ec;
    fs::create_directories(u->container->dir, ec);
    if (ec) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    for (auto& [name, bytes] : u->writes) {
        fs::path final_path = u->container->dir / encode_name(name);
        fs::path tmp = u->container->dir / ("~" + encode_name(name));
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!f) return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        }
        fs::rename(tmp, final_path, ec);
        if (ec) return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    }
    for (auto& name : u->deletes) fs::remove(u->container->dir / encode_name(name), ec);
    return S_OK;
}

HRESULT CALLBACK submit_op(XAsyncOp op, const XAsyncProviderData* data) {
    switch (op) {
        case XAsyncOp::Begin: return XAsyncSchedule(data->async, 0);
        case XAsyncOp::DoWork: XAsyncComplete(data->async, XGameSaveSubmitUpdate(data->context), 0); return S_OK;
        default: return S_OK;
    }
}

HRESULT __stdcall XGameSaveSubmitUpdateAsync(void* update, XAsyncBlock* async) {
    if (!update || !async) return E_INVALIDARG;
    return XAsyncBegin(async, update, &kSubmitIdentity, "XGameSaveSubmitUpdateAsync", submit_op);
}

HRESULT __stdcall XGameSaveSubmitUpdateResult(XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    return XAsyncGetStatus(async, false);
}

// =====================================================================================================  XPackage

constexpr size_t kPackageIdMax = 33;  // XPACKAGE_IDENTIFIER_MAX_LENGTH, including the terminator. VERIFY.

bool __stdcall XPackageIsPackagedProcess() { return true; }

// The installed package is the folder the game runs from; its identifier is a fixed name.
HRESULT __stdcall XPackageGetCurrentProcessPackageIdentifier(size_t size, char* out) {
    static const std::string id = "Leonida.Local";
    static_assert(sizeof("Leonida.Local") <= kPackageIdMax, "package id must fit XPACKAGE_IDENTIFIER_MAX_LENGTH");
    return copy_string(id, size, out, nullptr);
}

// HRESULT XPackageGetUserLocale(size_t localeSize, char* locale): the host user's locale, e.g. "en-US".
HRESULT __stdcall XPackageGetUserLocale(size_t size, char* out) {
    wchar_t wide[LOCALE_NAME_MAX_LENGTH] = {};
    if (!GetUserDefaultLocaleName(wide, LOCALE_NAME_MAX_LENGTH)) wcscpy_s(wide, L"en-US");
    char narrow[LOCALE_NAME_MAX_LENGTH * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof(narrow), nullptr, nullptr);
    return copy_string(narrow, size, out, nullptr);
}

// =====================================================================================================  XThread

thread_local bool t_time_sensitive = false;

HRESULT __stdcall XThreadSetTimeSensitive(bool value) {
    t_time_sensitive = value;
    return S_OK;
}
bool __stdcall XThreadIsTimeSensitive() { return t_time_sensitive; }
void __stdcall XThreadAssertNotTimeSensitive() {}

// =====================================================================================================  Offline

enum class ConnectivityLevel : uint32_t { Unknown = 0, None = 1, LocalAccess = 2, InternetAccess = 3,
                                          ConstrainedInternetAccess = 4 };
struct XNetworkingConnectivityHint {
    ConnectivityLevel connectivityLevel;
    uint32_t connectivityCost;
    uint32_t ianaInterfaceType;
    bool networkInitialized;
    bool approachingDataLimit;
    bool overDataLimit;
    bool roaming;
};

// The runtime is offline by design: report an initialized network with no connectivity, so the title takes its
// offline path instead of waiting for one.
HRESULT __stdcall XNetworkingGetConnectivityHint(XNetworkingConnectivityHint* hint) {
    if (!hint) return E_INVALIDARG;
    *hint = {};
    hint->connectivityLevel = ConnectivityLevel::None;
    hint->networkInitialized = true;
    return S_OK;
}

HRESULT __stdcall XNetworkingRegisterConnectivityHintChanged(XTaskQueueHandle, void*, void*,
                                                             XTaskQueueRegistrationToken* token) {
    if (!token) return E_INVALIDARG;
    token->token = g_next_token++;
    return S_OK;
}
bool __stdcall XNetworkingUnregisterConnectivityHintChanged(XTaskQueueRegistrationToken, bool) { return true; }

bool __stdcall XGameStreamingIsStreaming() { return false; }

// HRESULT XLaunchUri(XUserHandle requestingUser, const char* uri): nothing is launched offline.
HRESULT __stdcall XLaunchUri(void*, const char* uri) {
    std::fprintf(stderr, "[rt] xgameruntime: XLaunchUri(%s) ignored (offline)\n", uri ? uri : "(null)");
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

#define XGR(name) {"xgameruntime.dll", #name, reinterpret_cast<void*>(&name)}

}  // namespace

SHIM_TABLE(xgameruntime_services,
    // XUser
    XGR(XUserAddAsync), XGR(XUserAddResult), XGR(XUserDuplicateHandle), XGR(XUserCloseHandle), XGR(XUserCompare),
    XGR(XUserGetMaxUsers), XGR(XUserGetLocalId), XGR(XUserFindUserByLocalId), XGR(XUserGetId), XGR(XUserIsGuest),
    XGR(XUserGetState), XGR(XUserGetAgeGroup), XGR(XUserGetGamertag), XGR(XUserRegisterForChangeEvent),
    XGR(XUserUnregisterForChangeEvent),
    // XGameSave
    XGR(XGameSaveInitializeProvider), XGR(XGameSaveInitializeProviderAsync), XGR(XGameSaveInitializeProviderResult),
    XGR(XGameSaveCloseProvider), XGR(XGameSaveGetRemainingQuota), XGR(XGameSaveCreateContainer),
    XGR(XGameSaveCloseContainer), XGR(XGameSaveDeleteContainer), XGR(XGameSaveEnumerateContainerInfo),
    XGR(XGameSaveEnumerateContainerInfoByName), XGR(XGameSaveGetContainerInfo), XGR(XGameSaveEnumerateBlobInfo),
    XGR(XGameSaveEnumerateBlobInfoByName), XGR(XGameSaveReadBlobData), XGR(XGameSaveCreateUpdate),
    XGR(XGameSaveCloseUpdate), XGR(XGameSaveSubmitBlobWrite), XGR(XGameSaveSubmitBlobDelete),
    XGR(XGameSaveSubmitUpdate), XGR(XGameSaveSubmitUpdateAsync), XGR(XGameSaveSubmitUpdateResult),
    // XPackage
    XGR(XPackageIsPackagedProcess), XGR(XPackageGetCurrentProcessPackageIdentifier), XGR(XPackageGetUserLocale),
    // XThread
    XGR(XThreadSetTimeSensitive), XGR(XThreadIsTimeSensitive), XGR(XThreadAssertNotTimeSensitive),
    // Offline answers
    XGR(XNetworkingGetConnectivityHint), XGR(XNetworkingRegisterConnectivityHintChanged),
    XGR(XNetworkingUnregisterConnectivityHintChanged), XGR(XGameStreamingIsStreaming), XGR(XLaunchUri),
);
