#include "libretro_backend.hpp"

#include "libretro.h"

#include "host/backend_registry.hpp"
#include "host/host_log.hpp"
#include "host/system_catalog.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

LibretroBackend* LibretroBackend::sActive = nullptr;

struct LibretroBackend::CoreApi {
    void (*init)(void);
    void (*deinit)(void);
    unsigned (*apiVersion)(void);
    void (*getSystemInfo)(retro_system_info*);
    void (*getSystemAvInfo)(retro_system_av_info*);
    void (*setEnvironment)(retro_environment_t);
    void (*setVideoRefresh)(retro_video_refresh_t);
    void (*setAudioSample)(retro_audio_sample_t);
    void (*setAudioSampleBatch)(retro_audio_sample_batch_t);
    void (*setInputPoll)(retro_input_poll_t);
    void (*setInputState)(retro_input_state_t);
    void (*setControllerPortDevice)(unsigned, unsigned);
    void (*reset)(void);
    void (*run)(void);
    size_t (*serializeSize)(void);
    bool (*serialize)(void*, size_t);
    bool (*unserialize)(const void*, size_t);
    bool (*loadGame)(const retro_game_info*);
    void (*unloadGame)(void);
    unsigned (*getRegion)(void);
    void* (*getMemoryData)(unsigned);
    size_t (*getMemorySize)(unsigned);
};

LibretroBackend::LibretroBackend() {
    for (auto& port : keyHandles_) {
        for (auto& handle : port) handle = -1;
    }
}

LibretroBackend::~LibretroBackend() {
    closeCore(current_);
    closeCore(pending_);
}

const char* LibretroBackend::version() const {
    return versionString_.empty() ? "no core adopted" : versionString_.c_str();
}

std::vector<std::string> LibretroBackend::systems() const {
    // Never claims statically: bring-your-own cores serve only the explicit
    // adoption, so the default engine pool stays bundled-cores-only.
    return {};
}

EmuHost::Capabilities LibretroBackend::capabilities(const std::string& systemId) const {
    EmuHost::Capabilities caps;
    if (!bootTarget().handle || systemId != systemId_) return caps;
    // A loaded game answers serialize honestly; pre-boot the size is unknowable.
    caps.serialize = !loaded_
        || (current_.api->serializeSize && current_.api->serializeSize() > 0);
    caps.cheats       = true;
    caps.memoryAccess = true;
    caps.rateControl  = true;
    caps.options = (loaded_ ? current_ : bootTarget()).optionInfos;
    return caps;
}

std::vector<EmuHost::OptionInfo> LibretroBackend::engineOptions() const {
    const CoreRef& ref = loaded_ ? current_ : bootTarget();
    auto options = ref.optionInfos;
    for (auto& info : options) {
        auto it = ref.optionValues.find(info.key);
        if (it != ref.optionValues.end()) info.current = it->second;
    }
    return options;
}

LibretroBackend::CoreApi* LibretroBackend::loadCoreSymbols(void* handle) {
    auto* api = new CoreApi{};
    api->init          = (void (*)())dlsym(handle, "retro_init");
    api->deinit        = (void (*)())dlsym(handle, "retro_deinit");
    api->apiVersion    = (unsigned (*)())dlsym(handle, "retro_api_version");
    api->getSystemInfo = (void (*)(retro_system_info*))dlsym(handle, "retro_get_system_info");
    api->getSystemAvInfo =
        (void (*)(retro_system_av_info*))dlsym(handle, "retro_get_system_av_info");
    api->setEnvironment  = (void (*)(retro_environment_t))dlsym(handle, "retro_set_environment");
    api->setVideoRefresh =
        (void (*)(retro_video_refresh_t))dlsym(handle, "retro_set_video_refresh");
    api->setAudioSample =
        (void (*)(retro_audio_sample_t))dlsym(handle, "retro_set_audio_sample");
    api->setAudioSampleBatch =
        (void (*)(retro_audio_sample_batch_t))dlsym(handle, "retro_set_audio_sample_batch");
    api->setInputPoll  = (void (*)(retro_input_poll_t))dlsym(handle, "retro_set_input_poll");
    api->setInputState = (void (*)(retro_input_state_t))dlsym(handle, "retro_set_input_state");
    api->setControllerPortDevice =
        (void (*)(unsigned, unsigned))dlsym(handle, "retro_set_controller_port_device");
    api->reset         = (void (*)())dlsym(handle, "retro_reset");
    api->run           = (void (*)())dlsym(handle, "retro_run");
    api->serializeSize = (size_t (*)())dlsym(handle, "retro_serialize_size");
    api->serialize     = (bool (*)(void*, size_t))dlsym(handle, "retro_serialize");
    api->unserialize   = (bool (*)(const void*, size_t))dlsym(handle, "retro_unserialize");
    api->loadGame      = (bool (*)(const retro_game_info*))dlsym(handle, "retro_load_game");
    api->unloadGame    = (void (*)())dlsym(handle, "retro_unload_game");
    api->getRegion     = (unsigned (*)())dlsym(handle, "retro_get_region");
    api->getMemoryData = (void* (*)(unsigned))dlsym(handle, "retro_get_memory_data");
    api->getMemorySize = (size_t (*)(unsigned))dlsym(handle, "retro_get_memory_size");

    bool complete = api->init && api->deinit && api->apiVersion && api->getSystemInfo
        && api->getSystemAvInfo && api->setEnvironment && api->setVideoRefresh
        && api->setAudioSample && api->setAudioSampleBatch && api->setInputPoll
        && api->setInputState && api->reset && api->run && api->loadGame
        && api->unloadGame && api->getMemoryData && api->getMemorySize;
    if (!complete) {
        delete api;
        return nullptr;
    }
    return api;
}

void LibretroBackend::closeCore(CoreRef& core) {
    if (envTarget_ == &core) envTarget_ = nullptr;
    if (core.initialized) core.api->deinit();
    if (core.handle) dlclose(core.handle);
    delete core.api;
    core = CoreRef{};
}

// "snes9x" from "/data/local/tmp/libsnes9x_libretro_android.so" — the
// display name is path-independent so save-state backend tags match no
// matter how the same core was adopted.
static std::string deriveCoreName(const std::string& request) {
    auto slash = request.find_last_of('/');
    std::string base = slash == std::string::npos ? request : request.substr(slash + 1);
    if (base.rfind("lib", 0) == 0) base = base.substr(3);
    auto suffix = base.find("_libretro");
    if (suffix != std::string::npos) return base.substr(0, suffix);
    auto dot = base.find_last_of('.');
    return dot == std::string::npos ? base : base.substr(0, dot);
}

bool LibretroBackend::probeCore(const std::string& name, CoreRef& out) {
    // A path is used verbatim; a bare name resolves through the packaged
    // naming conventions per platform.
    std::vector<std::string> candidates;
    if (name.find('/') != std::string::npos) {
        candidates.push_back(name);
    } else {
#if defined(__ANDROID__)
        candidates.push_back("lib" + name + "_libretro_android.so");
        candidates.push_back("lib" + name + "_libretro.so");
#else
        candidates.push_back(name + "_libretro_ios.dylib");
        candidates.push_back(name + "_libretro.dylib");
#endif
    }

    void* handle = nullptr;
    for (auto& candidate : candidates) {
        handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle) break;
    }
    if (!handle) {
        EMUHOST_LOGE("libretro: no loadable core for '%s' (%s)", name.c_str(), dlerror());
        return false;
    }

    auto* api = loadCoreSymbols(handle);
    if (!api || api->apiVersion() != RETRO_API_VERSION) {
        EMUHOST_LOGE("libretro: '%s' is not a v%d libretro core",
                     name.c_str(), RETRO_API_VERSION);
        delete api;
        dlclose(handle);
        return false;
    }

    retro_system_info info {};
    api->getSystemInfo(&info);
    out.handle         = handle;
    out.api            = api;
    out.requestName    = name;
    out.coreName       = deriveCoreName(name);
    out.libraryName    = info.library_name ? info.library_name : out.coreName;
    out.libraryVersion = info.library_version ? info.library_version : "";
    out.needFullpath   = info.need_fullpath;
    out.pixelFormat    = RETRO_PIXEL_FORMAT_0RGB1555;
    std::string extensions = info.valid_extensions ? info.valid_extensions : "";
    for (size_t start = 0; start <= extensions.size();) {
        auto bar = extensions.find('|', start);
        if (bar == std::string::npos) bar = extensions.size();
        if (bar > start) out.extensions.push_back(extensions.substr(start, bar - start));
        start = bar + 1;
    }

    // retro_init runs here, not at boot, so the option schema the core
    // declares (SET_VARIABLES) exists while loadSystem is still staging —
    // engineOptions validate synchronously at the call site.
    sActive = this;
    auto* prevTarget = envTarget_;
    envTarget_ = &out;
    api->setEnvironment((retro_environment_t)onEnvironment);
    api->init();
    out.initialized = true;
    envTarget_ = prevTarget;

    EMUHOST_LOGI("libretro: adopted %s %s as '%s' (extensions: %s, %zu options)",
                 out.libraryName.c_str(), out.libraryVersion.c_str(),
                 out.coreName.c_str(),
                 info.valid_extensions ? info.valid_extensions : "",
                 out.optionInfos.size());
    return true;
}

bool LibretroBackend::adoptDynamicCore(const std::string& name, const std::string& systemId) {
    if (!SystemCatalog::find(systemId)) return false;
    if (pending_.handle && name == pending_.requestName) {
        systemId_ = systemId;
        return true;
    }
    if (!pending_.handle && current_.handle && name == current_.requestName) {
        systemId_ = systemId;
        return true;
    }

    CoreRef fresh;
    if (!probeCore(name, fresh)) return false;

    if (loaded_) {
        // current_ still runs a game; it must keep serving callbacks until
        // the host unloads it. The swap completes at the next boot.
        closeCore(pending_);
        pending_ = fresh;
    } else {
        closeCore(pending_);
        closeCore(current_);
        current_ = fresh;
    }
    systemId_ = systemId;
    versionString_ = fresh.libraryName + " " + fresh.libraryVersion;
    return true;
}

EmuHost::Analysis LibretroBackend::analyze(const std::string& systemId,
                                           const uint8_t* rom, size_t size) {
    EmuHost::Analysis analysis;
    const CoreRef& core = bootTarget();
    if (!core.handle || systemId != systemId_) {
        analysis.error = "no libretro core adopted for: " + systemId;
        return analysis;
    }
    if (size == 0) {
        analysis.error = "empty ROM";
        return analysis;
    }
    analysis.ok = true;
    analysis.token = std::make_shared<std::vector<uint8_t>>(rom, rom + size);
    return analysis;
}

// --- libretro callbacks ---------------------------------------------------

bool LibretroBackend::onEnvironment(unsigned cmd, void* data) {
    auto* self = sActive;
    if (!self) return false;
    // Option/format state is per-core: the probing core's retro_init and
    // the running core's retro_run each read/write their own CoreRef.
    auto* ref = self->envTarget_;
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        if (!ref) return false;
        int format = *(const int*)data;
        if (format != RETRO_PIXEL_FORMAT_0RGB1555 &&
            format != RETRO_PIXEL_FORMAT_XRGB8888 &&
            format != RETRO_PIXEL_FORMAT_RGB565) {
            return false;
        }
        ref->pixelFormat = format;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool*)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
        if (self->systemDir_.empty()) return false;
        *(const char**)data = self->systemDir_.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        auto* callback = (retro_log_callback*)data;
        callback->log = (retro_log_printf_t)onLog;
        return true;
    }
    // GET_CORE_OPTIONS_VERSION stays unanswered, pinning cores to this
    // legacy SET_VARIABLES path — one parser covers every core.
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        if (!ref) return false;
        // A re-declaration replaces the schema; keep values the new schema
        // still declares (a core re-listing options must not reset a dev's
        // engineOptions).
        ref->optionInfos.clear();
        for (auto* var = (const retro_variable*)data; var->key; var++) {
            // Value format: "Description; default|choice|choice".
            std::string value = var->value ? var->value : "";
            auto semicolon = value.find("; ");
            std::string choices = semicolon == std::string::npos
                ? value : value.substr(semicolon + 2);
            EmuHost::OptionInfo info;
            info.key   = var->key;
            info.stage = EmuHost::OptionInfo::Stage::Runtime;
            info.kind  = EmuHost::OptionInfo::Kind::Enum;
            size_t start = 0;
            while (start <= choices.size()) {
                auto bar = choices.find('|', start);
                if (bar == std::string::npos) bar = choices.size();
                if (bar > start) info.values.push_back(choices.substr(start, bar - start));
                start = bar + 1;
            }
            if (!info.values.empty() && !ref->optionValues.count(info.key)) {
                ref->optionValues[info.key] = info.values.front();
            }
            ref->optionInfos.push_back(std::move(info));
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        if (!ref) return false;
        auto* var = (retro_variable*)data;
        auto it = ref->optionValues.find(var->key ? var->key : "");
        if (it == ref->optionValues.end()) return false;
        var->value = it->second.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        if (!ref) return false;
        *(bool*)data = ref->optionsUpdated;
        ref->optionsUpdated = false;
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        auto* geometry = cmd == RETRO_ENVIRONMENT_SET_GEOMETRY
            ? (const retro_game_geometry*)data
            : &((const retro_system_av_info*)data)->geometry;
        self->applyGeometry(*geometry);
        if (cmd == RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO) {
            self->sourceRate_ = ((const retro_system_av_info*)data)->timing.sample_rate;
        }
        return true;
    }
    default:
        return false;
    }
}

void LibretroBackend::applyGeometry(const retro_game_geometry& geometry) {
    frameWidth_  = geometry.base_width;
    frameHeight_ = geometry.base_height;
    aspectRatio_ = geometry.aspect_ratio;
}

void LibretroBackend::onVideoRefresh(const void* data, unsigned width, unsigned height,
                                     size_t pitch) {
    auto* self = sActive;
    // NULL data = dupe frame; the host keeps presenting the previous one.
    if (!self || !self->host_ || self->hidden_ || !data) return;

    EmuHost::FrameGeometry geometry;
    geometry.width  = width;
    geometry.height = height;
    // libretro aspect is the full display ratio r = w/h; the host multiplies
    // width by aspectX/aspectY, so aspectX = r·height, aspectY = width.
    if (self->aspectRatio_ > 0) {
        geometry.aspectX = self->aspectRatio_ * height;
        geometry.aspectY = width;
    }

    self->converted_.resize((size_t)width * height);
    if (self->current_.pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
        auto* pixels = (const uint32_t*)data;
        size_t strideWords = pitch / sizeof(uint32_t);
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                self->converted_[(size_t)y * width + x] =
                    0xFF000000u | (pixels[(size_t)y * strideWords + x] & 0x00FFFFFFu);
            }
        }
    } else if (self->current_.pixelFormat == RETRO_PIXEL_FORMAT_RGB565) {
        auto* pixels = (const uint16_t*)data;
        size_t strideWords = pitch / sizeof(uint16_t);
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                uint16_t c = pixels[(size_t)y * strideWords + x];
                uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
                self->converted_[(size_t)y * width + x] = 0xFF000000u
                    | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8)
                    | (b << 3 | b >> 2);
            }
        }
    } else {   // 0RGB1555, the libretro default
        auto* pixels = (const uint16_t*)data;
        size_t strideWords = pitch / sizeof(uint16_t);
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                uint16_t c = pixels[(size_t)y * strideWords + x];
                uint32_t r = (c >> 10) & 0x1F, g = (c >> 5) & 0x1F, b = c & 0x1F;
                self->converted_[(size_t)y * width + x] = 0xFF000000u
                    | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8)
                    | (b << 3 | b >> 2);
            }
        }
    }
    self->host_->pushFrame(self->converted_.data(), width, height, width, geometry);
}

void LibretroBackend::pushResampled(const int16_t* frames, size_t count) {
    if (!host_ || sourceRate_ <= 0) return;
    // Linear interpolation from the core's rate to the DRC-skewed 48 kHz
    // ring; phase advances by source-per-destination each output sample.
    const double step = sourceRate_ / drcRate_;
    for (size_t i = 0; i < count; i++) {
        int16_t l = frames[i * 2];
        int16_t r = frames[i * 2 + 1];
        if (!havePrev_) {
            prevL_ = l; prevR_ = r;
            havePrev_ = true;
            continue;
        }
        while (resamplePhase_ < 1.0) {
            double t = resamplePhase_;
            host_->pushAudioFrame((prevL_ + (l - prevL_) * t) / 32768.0,
                                  (prevR_ + (r - prevR_) * t) / 32768.0);
            resamplePhase_ += step;
        }
        resamplePhase_ -= 1.0;
        prevL_ = l; prevR_ = r;
    }
}

void LibretroBackend::onAudioSample(int16_t left, int16_t right) {
    auto* self = sActive;
    if (!self || self->hidden_) return;
    int16_t frame[2] = {left, right};
    self->pushResampled(frame, 1);
}

size_t LibretroBackend::onAudioBatch(const int16_t* data, size_t frames) {
    auto* self = sActive;
    if (!self || self->hidden_) return frames;
    self->pushResampled(data, frames);
    return frames;
}

void LibretroBackend::onInputPoll() {}

int16_t LibretroBackend::onInputState(unsigned port, unsigned device, unsigned, unsigned id) {
    auto* self = sActive;
    if (!self || !self->host_) return 0;
    if ((device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD) return 0;
    if (port >= kMaxPorts || id >= 16) return 0;
    int handle = self->keyHandles_[port][id];
    if (handle < 0) return 0;
    return self->host_->sampleButton(handle) ? 1 : 0;
}

void LibretroBackend::onLog(int level, const char* fmt, ...) {
    if (level < RETRO_LOG_WARN) return;
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    EMUHOST_LOGI("libretro core: %s", message);
}

// --- lifecycle -------------------------------------------------------------

EmuHost::BootResult LibretroBackend::boot(const EmuHost::BootSpec& spec,
                                          EmuHost::HostPort& host,
                                          EmuHost::SaveMediaIO& saves) {
    EmuHost::BootResult result;
    auto rom = std::static_pointer_cast<std::vector<uint8_t>>(spec.token);
    if (rom == nullptr || rom->empty() || spec.systemId != systemId_) return result;

    if (pending_.handle) {
        // The host unloaded any running game before this boot; the staged
        // adoption takes over now.
        closeCore(current_);
        current_ = pending_;
        pending_ = CoreRef{};
    }
    if (!current_.handle) return result;

    host_ = &host;
    sActive = this;
    envTarget_ = &current_;
    hidden_ = false;
    resamplePhase_ = 0;
    havePrev_ = false;
    drcRate_ = 48000.0;

    // Environment + retro_init already ran at probe time (the schema must
    // exist during staging); the remaining callbacks bind pre-load.
    current_.api->setVideoRefresh(onVideoRefresh);
    current_.api->setAudioSample(onAudioSample);
    current_.api->setAudioSampleBatch(onAudioBatch);
    current_.api->setInputPoll(onInputPoll);
    current_.api->setInputState(onInputState);

    retro_game_info game {};
    game.size = rom->size();
    // Every boot stages the ROM beside the game's saves and passes its path
    // even when the core also gets the bytes — cores identify formats by
    // the path's extension (Mesen rejects extensionless memory loads), and
    // the extension must be one the CORE declares, not the catalog's first
    // (Mesen: .nes but never .fc). The file stays while loaded (disk-swap
    // cores re-read it) and is overwritten on the next boot.
    auto* sys = SystemCatalog::find(systemId_);
    std::string ext;
    for (auto& coreExt : current_.extensions) {
        if (sys && SystemCatalog::extensionSupported(*sys, coreExt)) {
            ext = coreExt;
            break;
        }
    }
    if (ext.empty()) {
        ext = !current_.extensions.empty() ? current_.extensions.front()
            : (sys && !sys->extensions.empty() ? sys->extensions.front()
                                               : std::string("bin"));
    }
    std::string staged = "rom." + ext;
    std::string stagedPath = saves.pathFor(staged);
    if (!stagedPath.empty() && saves.write(staged, rom->data(), rom->size())) {
        game.path = stagedPath.c_str();
        auto slash = stagedPath.find_last_of('/');
        if (slash != std::string::npos) {
            systemDir_ = stagedPath.substr(0, slash);
        }
    } else {
        stagedPath.clear();
    }
    if (current_.needFullpath && stagedPath.empty()) {
        EMUHOST_LOGE("libretro: %s needs a ROM file (need_fullpath) and no "
                     "save path is configured to stage one",
                     current_.libraryName.c_str());
        return result;
    }
    if (!current_.needFullpath) game.data = rom->data();
    if (!current_.api->loadGame(&game)) {
        EMUHOST_LOGE("libretro: %s rejected the ROM", current_.libraryName.c_str());
        return result;
    }
    loaded_ = true;

    retro_system_av_info avInfo {};
    current_.api->getSystemAvInfo(&avInfo);
    applyGeometry(avInfo.geometry);
    sourceRate_ = avInfo.timing.sample_rate;
    if (avInfo.timing.fps > 0) host_->setRefreshRateHint(avInfo.timing.fps);

    auto battery = saves.read("save.ram");
    if (!battery.empty()) {
        size_t size = current_.api->getMemorySize(RETRO_MEMORY_SAVE_RAM);
        void* sram = current_.api->getMemoryData(RETRO_MEMORY_SAVE_RAM);
        if (sram && size) {
            std::memcpy(sram, battery.data(), std::min(size, battery.size()));
        }
    }

    if (current_.api->setControllerPortDevice) {
        auto* sys = SystemCatalog::find(systemId_);
        int ports = sys ? std::max(1, sys->ports) : 1;
        for (int i = 0; i < std::min(ports, kMaxPorts); i++) {
            current_.api->setControllerPortDevice(i, RETRO_DEVICE_JOYPAD);
        }
    }
    if (spec.bindings) bindPorts(*spec.bindings, host);

    result.ok = true;
    return result;
}

void LibretroBackend::unload(EmuHost::SaveMediaIO& saves) {
    if (loaded_) {
        collectSaveMedia(saves);
        current_.api->unloadGame();
        loaded_ = false;
    }
    host_ = nullptr;
    cheatWrites_.clear();
    for (auto& port : keyHandles_) {
        for (auto& handle : port) handle = -1;
    }
    if (sActive == this) sActive = nullptr;
}

void LibretroBackend::bindPorts(const std::vector<EmuHost::PortBinding>& bindings,
                                EmuHost::HostPort& host) {
    host_ = &host;
    for (auto& port : keyHandles_) {
        for (auto& handle : port) handle = -1;
    }
    auto* sys = SystemCatalog::find(systemId_);
    if (!sys) return;

    for (auto& binding : bindings) {
        for (auto& assignment : binding.logical) {
            int port = assignment.logicalPort;
            if (port < 1 || port > kMaxPorts) continue;
            SystemCatalog::DeviceDescriptor descriptor;
            auto device = assignment.device.empty()
                ? std::string(sys->device ? sys->device : "")
                : assignment.device;
            if (binding.physicalPort == 0) {
                descriptor.buttons = sys->buttons;
            } else if (!SystemCatalog::resolveDevice(*sys, device, descriptor)) {
                continue;
            }
            // RetroPad ids equal the catalog's positional bit indices, so
            // each button's handle lands at its own id.
            for (auto& [buttonName, bit] : descriptor.buttons) {
                int id = 0;
                uint32_t value = bit;
                while (value >>= 1) id++;
                if (id < 16) {
                    keyHandles_[port - 1][id] = host.buttonHandle(port, buttonName);
                }
            }
        }
    }
}

bool LibretroBackend::tick(bool hidden) {
    if (!loaded_) return false;
    hidden_ = hidden;
    current_.api->run();
    hidden_ = false;
    if (!cheatWrites_.empty()) {
        auto* sys = SystemCatalog::find(systemId_);
        size_t size = current_.api->getMemorySize(RETRO_MEMORY_SYSTEM_RAM);
        auto* ram = (uint8_t*)current_.api->getMemoryData(RETRO_MEMORY_SYSTEM_RAM);
        if (sys && ram) {
            for (auto& [address, value] : cheatWrites_) {
                if (address >= sys->memBase && address - sys->memBase < size) {
                    ram[address - sys->memBase] = value;
                }
            }
        }
    }
    return true;
}

bool LibretroBackend::serialize(std::vector<uint8_t>& out) {
    if (!loaded_ || !current_.api->serializeSize || !current_.api->serialize) return false;
    size_t size = current_.api->serializeSize();
    if (!size) return false;
    out.resize(size);
    return current_.api->serialize(out.data(), size);
}

bool LibretroBackend::unserialize(const uint8_t* data, size_t size) {
    if (!loaded_ || !current_.api->unserialize || !data || size == 0) return false;
    return current_.api->unserialize(data, size);
}

void LibretroBackend::syncSave() {
    // SAVE_RAM is the live battery memory — nothing to make current first.
}

bool LibretroBackend::collectSaveMedia(EmuHost::SaveMediaIO& saves) {
    if (!loaded_) return false;
    size_t size = current_.api->getMemorySize(RETRO_MEMORY_SAVE_RAM);
    void* sram = current_.api->getMemoryData(RETRO_MEMORY_SAVE_RAM);
    if (!sram || !size) return false;
    return saves.write("save.ram", (const uint8_t*)sram, size);
}

int LibretroBackend::readMemory(uint32_t offset, uint8_t* out, uint32_t length) {
    if (!loaded_) return -1;
    size_t size = current_.api->getMemorySize(RETRO_MEMORY_SYSTEM_RAM);
    auto* ram = (const uint8_t*)current_.api->getMemoryData(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram || offset > size || length > size - offset) return -1;
    std::memcpy(out, ram + offset, length);
    return (int)length;
}

void LibretroBackend::writeMemory(uint32_t offset, const uint8_t* data, uint32_t length) {
    if (!loaded_) return;
    size_t size = current_.api->getMemorySize(RETRO_MEMORY_SYSTEM_RAM);
    auto* ram = (uint8_t*)current_.api->getMemoryData(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram || offset > size || length > size - offset) return;
    std::memcpy(ram + offset, data, length);
}

bool LibretroBackend::applyRateControl(double fillLevel) {
    if (!loaded_) return false;
    // Same convention as the SameBoy skew: an empty ring asks for a faster
    // output rate. ±0.5% bound, unity at half full.
    constexpr double kMaxDelta = 0.005;
    drcRate_ = 48000.0 * ((1.0 + kMaxDelta) - 2.0 * fillLevel * kMaxDelta);
    return true;
}

void LibretroBackend::syncCheats(const std::unordered_map<uint32_t, uint32_t>& table) {
    cheatWrites_.clear();
    cheatWrites_.reserve(table.size());
    for (auto& [address, value] : table) {
        cheatWrites_.push_back({address, (uint8_t)value});
    }
}

std::string LibretroBackend::setEngineOption(const std::string& key, const std::string& value,
                                             bool staged) {
    CoreRef& ref = staged ? bootTarget() : current_;
    if (!ref.handle) return "no libretro core is adopted";
    if (!staged && !loaded_) {
        return "no game is running — set boot-time engine options in the loadSystem config";
    }

    const EmuHost::OptionInfo* match = nullptr;
    for (auto& info : ref.optionInfos) {
        if (info.key == key) { match = &info; break; }
    }
    if (!match) {
        std::string declared;
        size_t listed = 0;
        for (auto& info : ref.optionInfos) {
            if (listed == 12) { declared += ", …"; break; }
            if (listed++) declared += ", ";
            declared += info.key;
        }
        return "'" + key + "' is not an option " + ref.coreName + " declares — declared ("
             + std::to_string(ref.optionInfos.size()) + "): " + declared;
    }

    bool legal = false;
    std::string choices;
    for (auto& choice : match->values) {
        if (choice == value) { legal = true; break; }
        if (!choices.empty()) choices += "|";
        choices += choice;
    }
    if (!legal) {
        return "'" + value + "' is not a value " + ref.coreName + " declares for '" + key
             + "' — declared: " + choices;
    }

    ref.optionValues[key] = value;
    // Cores re-read options on the next retro_run via GET_VARIABLE_UPDATE.
    ref.optionsUpdated = true;
    return "";
}

static EmuHost::BackendRegistrar kRegistrar{
    "libretro", [] { return std::make_unique<LibretroBackend>(); }};

// Link anchor for static builds — named in ios/core_link.cpp's sum; a bare
// call-site read is elided at -O2 and the archive drops this object.
extern "C" int emu_backend_libretro_link = 1;
