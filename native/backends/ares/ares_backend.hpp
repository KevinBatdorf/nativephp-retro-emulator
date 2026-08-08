// AresBackend — ares as backend #1 behind the seam (host/backend.hpp).
// Owns everything ares-shaped that used to live in the platform bridges: the
// Platform callback implementation, the node caches, pak plumbing, the boot
// machinery, and the per-core SystemDef registry (system_registry.hpp, now an
// implementation detail of this backend).
#pragma once

#include "host/backend.hpp"

#include <ares/ares.hpp>

#include "system_registry.hpp"

#include <memory>
#include <string>
#include <vector>

class AresBackend final : public EmuHost::Backend, public ares::Platform {
public:
    AresBackend();
    ~AresBackend() override;

    // EmuHost::Backend -------------------------------------------------------
    const char* name() const override { return "ares"; }
    const char* version() const override;
    std::vector<std::string> systems() const override;
    EmuHost::Capabilities capabilities(const std::string& systemId) const override;

    EmuHost::Analysis analyze(const std::string& systemId,
                              const uint8_t* rom, size_t size) override;
    EmuHost::BootResult boot(const EmuHost::BootSpec& spec,
                             EmuHost::HostPort& host,
                             EmuHost::SaveMediaIO& saves) override;
    void unload(EmuHost::SaveMediaIO& saves) override;
    void bindPorts(const std::vector<EmuHost::PortBinding>& bindings,
                   EmuHost::HostPort& host) override;

    bool tick(bool hidden) override;
    bool serialize(std::vector<uint8_t>& out) override;
    bool unserialize(const uint8_t* data, size_t size) override;

    void syncSave() override;
    bool collectSaveMedia(EmuHost::SaveMediaIO& saves) override;

    int  readMemory(uint32_t offset, uint8_t* out, uint32_t length) override;
    void writeMemory(uint32_t offset, const uint8_t* data, uint32_t length) override;

    void setVideoSettings(float luminance, float saturation, float gamma,
                          bool colorBleed, bool overscan) override;
    void setOverscan(bool overscan) override;
    bool applyRuntimeToggle(const std::string& key, bool value) override;
    int  readRuntimeToggle(const std::string& key) override;
    std::string readBootOption(const std::string& name) override;
    bool applyRateControl(double fillLevel) override;

    // ares::Platform ---------------------------------------------------------
    auto status(string_view message) -> void override;
    auto attach(ares::Node::Object node) -> void override;
    auto detach(ares::Node::Object node) -> void override;
    auto pak(ares::Node::Object node) -> std::shared_ptr<vfs::directory> override;
    auto video(ares::Node::Video::Screen node, const u32* data,
               u32 pitch, u32 width, u32 height) -> void override;
    auto audio(ares::Node::Audio::Stream stream) -> void override;
    auto input(ares::Node::Input::Input node) -> void override;
    auto cheat(u32 address) -> maybe<u32> override;
    auto refreshRateHint(double refreshRate) -> void override;

private:
    // Cache a device's button + axis nodes under `parent`, acquiring host
    // input handles for `logicalPort` (the ares half of the old cacheDevice).
    void cacheDevice(const ares::Node::Object& parent, int logicalPort,
                     EmuHost::HostPort& host);
    // Engine teardown shared by unload() and mid-boot failures.
    void teardown();

    double outputGain() const;

    const SystemRegistry::SystemDef* def_ = nullptr;
    ares::Node::System root_;
    bool loaded_ = false;

    std::shared_ptr<vfs::directory> systemPak_;
    std::shared_ptr<vfs::directory> cartridgePak_;
    std::shared_ptr<vfs::directory> slotPak_[2];

    // Cached mapping from an ares input node to its host handle. Built on
    // the emulation thread in bindPorts, read there in Platform::input.
    struct CachedButton {
        ares::Node::Input::Button node;
        int handle;
    };
    std::vector<CachedButton> buttonCache_;
    struct CachedAxis {
        ares::Node::Input::Axis node;
        int handle;
    };
    std::vector<CachedAxis> axisCache_;

    std::vector<ares::Node::Audio::Stream> audioStreams_;

    // Host services. Set at boot; callbacks only fire while a core runs.
    EmuHost::HostPort* host_ = nullptr;
    const std::unordered_map<uint32_t, uint32_t>* cheatTable_ = nullptr;
};
