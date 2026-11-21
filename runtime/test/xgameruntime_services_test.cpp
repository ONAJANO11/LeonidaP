// Checks the xgameruntime services shim (user, save storage, package, threads, offline answers) the way a GDK title
// uses them at boot: initialize the runtime, add the default user silently, open save storage, write a save, read it
// back, enumerate, delete, and ask about the package, the network and streaming.
//
// Everything goes through rt::shims::find("xgameruntime.dll", ...). Saves go to a fresh folder under %TEMP% (set
// as LEONIDA_SAVE_DIR) that the test removes again. No game data; nothing online.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include <XAsync.h>
#include <XTaskQueue.h>

#include "shims.h"

namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

void* raw(const char* name) { return rt::shims::find("xgameruntime.dll", name); }
template <class Fn>
Fn get(const char* name) {
    return reinterpret_cast<Fn>(raw(name));
}

struct XUserLocalId { uint64_t value; };
struct XGameSaveBlobInfo { const char* name; uint32_t size; };
struct XGameSaveBlob { XGameSaveBlobInfo info; uint8_t* data; };
struct XGameSaveContainerInfo {
    const char* name; const char* displayName; uint32_t blobCount; uint64_t totalSize; time_t lastModifiedTime;
    bool needsSync;
};
struct XNetworkingConnectivityHint {
    uint32_t connectivityLevel; uint32_t connectivityCost; uint32_t ianaInterfaceType; bool networkInitialized;
    bool approachingDataLimit; bool overDataLimit; bool roaming;
};

const char* const kNames[] = {
    "XUserAddAsync", "XUserAddResult", "XUserDuplicateHandle", "XUserCloseHandle", "XUserCompare", "XUserGetMaxUsers",
    "XUserGetLocalId", "XUserFindUserByLocalId", "XUserGetId", "XUserIsGuest", "XUserGetState", "XUserGetAgeGroup",
    "XUserGetGamertag", "XUserRegisterForChangeEvent", "XUserUnregisterForChangeEvent",
    "XGameSaveInitializeProvider", "XGameSaveInitializeProviderAsync", "XGameSaveInitializeProviderResult",
    "XGameSaveCloseProvider", "XGameSaveGetRemainingQuota", "XGameSaveCreateContainer", "XGameSaveCloseContainer",
    "XGameSaveDeleteContainer", "XGameSaveEnumerateContainerInfo", "XGameSaveEnumerateContainerInfoByName",
    "XGameSaveGetContainerInfo", "XGameSaveEnumerateBlobInfo", "XGameSaveEnumerateBlobInfoByName",
    "XGameSaveReadBlobData", "XGameSaveCreateUpdate", "XGameSaveCloseUpdate", "XGameSaveSubmitBlobWrite",
    "XGameSaveSubmitBlobDelete", "XGameSaveSubmitUpdate", "XGameSaveSubmitUpdateAsync", "XGameSaveSubmitUpdateResult",
    "XPackageIsPackagedProcess", "XPackageGetCurrentProcessPackageIdentifier", "XPackageGetUserLocale",
    "XThreadSetTimeSensitive", "XThreadIsTimeSensitive", "XThreadAssertNotTimeSensitive",
    "XNetworkingGetConnectivityHint", "XNetworkingRegisterConnectivityHintChanged",
    "XNetworkingUnregisterConnectivityHintChanged", "XGameStreamingIsStreaming", "XLaunchUri",
};

bool CALLBACK collect_containers(const XGameSaveContainerInfo* info, void* ctx) {
    static_cast<std::vector<std::string>*>(ctx)->push_back(std::string(info->name) + ":" +
                                                           std::to_string(info->blobCount));
    return true;
}

bool CALLBACK collect_blobs(const XGameSaveBlobInfo* info, void* ctx) {
    static_cast<std::vector<std::string>*>(ctx)->push_back(std::string(info->name) + ":" + std::to_string(info->size));
    return true;
}

}  // namespace

int main() {
    std::printf("xgameruntime_services_test: offline user, saves, package and offline answers, through the registry\n");

    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    fs::path save_dir = fs::path(tmp) / ("leonida-save-test-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(save_dir);
    _putenv_s("LEONIDA_SAVE_DIR", save_dir.string().c_str());

    int missing = 0;
    for (const char* n : kNames)
        if (!raw(n)) {
            std::printf("     not registered: %s\n", n);
            ++missing;
        }
    check(missing == 0, "every services function in the table resolves through rt::shims::find");
    check(raw("XGameRuntimeInitialize") != nullptr, "part 1 (XGameRuntimeInitialize) is linked alongside");
    check(rt::shims::find("xgameruntime.dll", "XStoreQueryGameLicenseAsync") == nullptr,
          "XStore licence queries are deliberately not shimmed");
    if (missing) {
        std::printf("xgameruntime_services_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    get<HRESULT(__stdcall*)()>("XGameRuntimeInitialize")();

    // ---- user: XUserAddAsync(AddDefaultUserSilently) on the process queue, then its properties.
    XAsyncBlock add{};
    void* user = nullptr;
    check(get<HRESULT(__stdcall*)(uint32_t, XAsyncBlock*)>("XUserAddAsync")(1, &add) == S_OK,
          "XUserAddAsync(AddDefaultUserSilently) starts");
    check(XAsyncGetStatus(&add, true) == S_OK, "XUserAddAsync completes with S_OK");
    check(get<HRESULT(__stdcall*)(XAsyncBlock*, void**)>("XUserAddResult")(&add, &user) == S_OK && user,
          "XUserAddResult returns a user handle");

    uint32_t max_users = 0;
    get<HRESULT(__stdcall*)(uint32_t*)>("XUserGetMaxUsers")(&max_users);
    check(max_users == 1, "XUserGetMaxUsers reports one user");
    uint32_t state = 99;
    check(get<HRESULT(__stdcall*)(void*, uint32_t*)>("XUserGetState")(user, &state) == S_OK && state == 0,
          "the user is SignedIn");
    bool guest = true;
    check(get<HRESULT(__stdcall*)(void*, bool*)>("XUserIsGuest")(user, &guest) == S_OK && !guest,
          "the user is not a guest");
    char tag[128] = {};
    size_t used = 0;
    auto gamertag = get<HRESULT(__stdcall*)(void*, uint32_t, size_t, char*, size_t*)>("XUserGetGamertag");
    check(gamertag(user, 0, sizeof(tag), tag, &used) == S_OK && std::strcmp(tag, "Player") == 0 && used == 7,
          "XUserGetGamertag(Classic) gives \"Player\" (7 bytes with the terminator)");
    check(gamertag(user, 0, 3, tag, &used) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && used == 7,
          "XUserGetGamertag reports the size it needs when the buffer is short");
    XUserLocalId lid{};
    get<HRESULT(__stdcall*)(void*, XUserLocalId*)>("XUserGetLocalId")(user, &lid);
    void* found = nullptr;
    check(get<HRESULT(__stdcall*)(XUserLocalId, void**)>("XUserFindUserByLocalId")(lid, &found) == S_OK &&
              get<int32_t(__stdcall*)(void*, void*)>("XUserCompare")(found, user) == 0,
          "XUserFindUserByLocalId finds the same user");
    get<void(__stdcall*)(void*)>("XUserCloseHandle")(found);

    // ---- saves: provider (async), container, update with two blobs, read back, enumerate, delete one, quota.
    XAsyncBlock pinit{};
    void* provider = nullptr;
    check(get<HRESULT(__stdcall*)(void*, const char*, bool, XAsyncBlock*)>("XGameSaveInitializeProviderAsync")(
              user, "00000000-0000-0000-0000-000000000000", false, &pinit) == S_OK &&
              XAsyncGetStatus(&pinit, true) == S_OK &&
              get<HRESULT(__stdcall*)(XAsyncBlock*, void**)>("XGameSaveInitializeProviderResult")(&pinit, &provider) ==
                  S_OK &&
              provider,
          "XGameSaveInitializeProviderAsync opens save storage");

    int64_t quota_before = 0, quota_after = 0;
    auto quota = get<HRESULT(__stdcall*)(void*, int64_t*)>("XGameSaveGetRemainingQuota");
    quota(provider, &quota_before);
    check(quota_before == (256ll << 20), "a fresh save folder has the full 256 MiB quota");

    void* container = nullptr;
    check(get<HRESULT(__stdcall*)(void*, const char*, void**)>("XGameSaveCreateContainer")(provider, "slot 1/main",
                                                                                           &container) == S_OK,
          "XGameSaveCreateContainer accepts a name with a space and a slash");
    void* update = nullptr;
    get<HRESULT(__stdcall*)(void*, const char*, void**)>("XGameSaveCreateUpdate")(container, "Chapter 1", &update);
    auto write = get<HRESULT(__stdcall*)(void*, const char*, const uint8_t*, size_t)>("XGameSaveSubmitBlobWrite");
    const uint8_t a[] = {1, 2, 3, 4, 5};
    const uint8_t b[] = {0xAA, 0xBB};
    write(update, "profile", a, sizeof(a));
    write(update, "world.dat", b, sizeof(b));
    XAsyncBlock submit{};
    check(get<HRESULT(__stdcall*)(void*, XAsyncBlock*)>("XGameSaveSubmitUpdateAsync")(update, &submit) == S_OK &&
              XAsyncGetStatus(&submit, true) == S_OK &&
              get<HRESULT(__stdcall*)(XAsyncBlock*)>("XGameSaveSubmitUpdateResult")(&submit) == S_OK,
          "XGameSaveSubmitUpdateAsync writes the update");
    get<void(__stdcall*)(void*)>("XGameSaveCloseUpdate")(update);

    auto read = get<HRESULT(__stdcall*)(void*, const char**, uint32_t*, size_t, XGameSaveBlob*)>("XGameSaveReadBlobData");
    uint32_t count = 0;
    check(read(container, nullptr, &count, 0, nullptr) != S_OK && count == 2,
          "XGameSaveReadBlobData with no buffer reports two blobs and fails");
    std::vector<uint8_t> buf(4096);
    count = 0;
    auto* blobs = reinterpret_cast<XGameSaveBlob*>(buf.data());
    check(read(container, nullptr, &count, buf.size(), blobs) == S_OK && count == 2,
          "XGameSaveReadBlobData reads both blobs");
    bool content_ok = count == 2 && std::strcmp(blobs[0].info.name, "profile") == 0 && blobs[0].info.size == 5 &&
                      std::memcmp(blobs[0].data, a, 5) == 0 && std::strcmp(blobs[1].info.name, "world.dat") == 0 &&
                      blobs[1].info.size == 2 && std::memcmp(blobs[1].data, b, 2) == 0;
    check(content_ok, "names, sizes and bytes match what was written");
    const char* one[] = {"world.dat"};
    count = 1;
    check(read(container, one, &count, buf.size(), blobs) == S_OK && count == 1 && blobs[0].info.size == 2,
          "XGameSaveReadBlobData reads one named blob");
    const char* none[] = {"missing"};
    count = 1;
    check(read(container, none, &count, buf.size(), blobs) != S_OK, "reading a blob that doesn't exist fails");

    std::vector<std::string> containers, blob_list;
    get<HRESULT(__stdcall*)(void*, void*, bool(CALLBACK*)(const XGameSaveContainerInfo*, void*))>(
        "XGameSaveEnumerateContainerInfo")(provider, &containers, collect_containers);
    check(containers.size() == 1 && containers[0] == "slot 1/main:2",
          "XGameSaveEnumerateContainerInfo lists the container by its original name with 2 blobs");
    get<HRESULT(__stdcall*)(void*, void*, bool(CALLBACK*)(const XGameSaveBlobInfo*, void*))>(
        "XGameSaveEnumerateBlobInfo")(container, &blob_list, collect_blobs);
    check(blob_list.size() == 2 && blob_list[0] == "profile:5" && blob_list[1] == "world.dat:2",
          "XGameSaveEnumerateBlobInfo lists both blobs with their sizes");

    // The container name holds a '/', so it must become one encoded folder: every file sits at
    // local-user/<container>/<blob>, nothing deeper and nothing outside.
    bool escaped = true;
    int files = 0;
    for (auto& e : fs::recursive_directory_iterator(save_dir))
        if (e.is_regular_file()) {
            ++files;
            auto rel = e.path().lexically_relative(save_dir);
            escaped = escaped && std::distance(rel.begin(), rel.end()) == 3 && *rel.begin() == "local-user";
        }
    escaped = escaped && files == 2;
    check(escaped, "the two blobs are files at LEONIDA_SAVE_DIR/local-user/<container>/<blob>, nothing else");

    get<HRESULT(__stdcall*)(void*, const char*, void**)>("XGameSaveCreateUpdate")(container, "", &update);
    get<HRESULT(__stdcall*)(void*, const char*)>("XGameSaveSubmitBlobDelete")(update, "profile");
    check(get<HRESULT(__stdcall*)(void*)>("XGameSaveSubmitUpdate")(update) == S_OK, "a delete-only update succeeds");
    get<void(__stdcall*)(void*)>("XGameSaveCloseUpdate")(update);
    count = 0;
    check(read(container, nullptr, &count, buf.size(), blobs) == S_OK && count == 1 &&
              std::strcmp(blobs[0].info.name, "world.dat") == 0,
          "after the delete only world.dat is left");
    quota(provider, &quota_after);
    check(quota_after == quota_before - 2, "the quota goes down by the bytes stored");

    // Names Windows can't store as given: a device name and a trailing dot (reported in #279 review).
    for (const char* odd : {"NUL", "com1.dat", "a.", "a"}) {
        void* c = nullptr, *u = nullptr;
        get<HRESULT(__stdcall*)(void*, const char*, void**)>("XGameSaveCreateContainer")(provider, odd, &c);
        get<HRESULT(__stdcall*)(void*, const char*, void**)>("XGameSaveCreateUpdate")(c, "", &u);
        const uint8_t one = static_cast<uint8_t>(std::strlen(odd));
        write(u, "x", &one, 1);
        get<HRESULT(__stdcall*)(void*)>("XGameSaveSubmitUpdate")(u);
        get<void(__stdcall*)(void*)>("XGameSaveCloseUpdate")(u);
        get<void(__stdcall*)(void*)>("XGameSaveCloseContainer")(c);
    }
    containers.clear();
    get<HRESULT(__stdcall*)(void*, void*, bool(CALLBACK*)(const XGameSaveContainerInfo*, void*))>(
        "XGameSaveEnumerateContainerInfo")(provider, &containers, collect_containers);
    int odd_found = 0;
    for (auto& e : containers)
        odd_found += e == "NUL:1" || e == "com1.dat:1" || e == "a.:1" || e == "a:1";
    check(odd_found == 4, "\"NUL\", \"com1.dat\", \"a.\" and \"a\" are four distinct containers that enumerate back by name");
    for (const char* odd : {"NUL", "com1.dat", "a.", "a"})
        get<HRESULT(__stdcall*)(void*, const char*)>("XGameSaveDeleteContainer")(provider, odd);

    get<void(__stdcall*)(void*)>("XGameSaveCloseContainer")(container);
    auto del = get<HRESULT(__stdcall*)(void*, const char*)>("XGameSaveDeleteContainer");
    check(del(provider, "slot 1/main") == S_OK, "XGameSaveDeleteContainer removes the container");
    containers.clear();
    get<HRESULT(__stdcall*)(void*, void*, bool(CALLBACK*)(const XGameSaveContainerInfo*, void*))>(
        "XGameSaveEnumerateContainerInfo")(provider, &containers, collect_containers);
    check(containers.empty(), "no containers are left");
    check(del(provider, "slot 1/main") != S_OK, "deleting it again fails");
    get<void(__stdcall*)(void*)>("XGameSaveCloseProvider")(provider);

    // ---- package, threads, offline answers.
    check(get<bool(__stdcall*)()>("XPackageIsPackagedProcess")(), "XPackageIsPackagedProcess is true");
    char id[64] = {};
    check(get<HRESULT(__stdcall*)(size_t, char*)>("XPackageGetCurrentProcessPackageIdentifier")(sizeof(id), id) == S_OK &&
              std::strlen(id) > 0 && std::strlen(id) < 33,
          "XPackageGetCurrentProcessPackageIdentifier returns an id under 33 bytes");
    char locale[100] = {};
    check(get<HRESULT(__stdcall*)(size_t, char*)>("XPackageGetUserLocale")(sizeof(locale), locale) == S_OK &&
              std::strchr(locale, '-') != nullptr,
          "XPackageGetUserLocale returns a language-region locale");
    std::printf("     locale: %s\n", locale);

    auto set_ts = get<HRESULT(__stdcall*)(bool)>("XThreadSetTimeSensitive");
    auto is_ts = get<bool(__stdcall*)()>("XThreadIsTimeSensitive");
    set_ts(true);
    bool on = is_ts();
    set_ts(false);
    check(on && !is_ts(), "XThreadSetTimeSensitive/XThreadIsTimeSensitive round-trip on this thread");

    XNetworkingConnectivityHint hint{};
    check(get<HRESULT(__stdcall*)(XNetworkingConnectivityHint*)>("XNetworkingGetConnectivityHint")(&hint) == S_OK &&
              hint.networkInitialized && hint.connectivityLevel == 1,
          "XNetworkingGetConnectivityHint: network initialized, no connectivity (offline)");
    check(!get<bool(__stdcall*)()>("XGameStreamingIsStreaming")(), "XGameStreamingIsStreaming is false");
    check(get<HRESULT(__stdcall*)(void*, const char*)>("XLaunchUri")(user, "https://example.invalid/") != S_OK,
          "XLaunchUri does nothing and reports failure");

    get<void(__stdcall*)(void*)>("XUserCloseHandle")(user);
    get<void(__stdcall*)()>("XGameRuntimeUninitialize")();
    std::error_code ec;
    fs::remove_all(save_dir, ec);

    if (g_failures) {
        std::printf("xgameruntime_services_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("xgameruntime_services_test: OK (%d checks)\n", g_checks);
    return 0;
}
