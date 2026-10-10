/*
  JACK "multi-out" audio device for the Standalone (GitHub issue #3).

  JUCE's own JACK device only creates as many output ports as the target client has input ports (8 for
  "system") and auto-connects them, so it can never offer the 18 channels needed for separate Part outputs.
  This device registers 18 free, named output ports (main L/R, then each Performance Part L/R) and leaves
  all connections to the user (qpwgraph, Carla, Catia...). libjack is loaded at run time, so the program
  still starts without JACK (or PipeWire's JACK layer) installed; the type then simply lists no device.

  Linux only. Channel layout matches VirtualJVProcessor::processBlock(): 0-1 main, 2+2k / 3+2k Part k+1.
*/

#pragma once

#include <JuceHeader.h>

#if JUCE_LINUX

namespace jivjack
{
inline const char *kTypeName = "JACK (JiV-881 Part outputs)";
constexpr int kNumChannels = 18;

// The few libjack entry points we need, resolved with dlopen.
struct Api
{
    using Client = void;
    using Port = void;
    using ProcessCb = int (*)(unsigned int, void *);

    juce::DynamicLibrary lib;
    Client *(*client_open)(const char *, int, int *, ...) = nullptr;
    int (*client_close)(Client *) = nullptr;
    Port *(*port_register)(Client *, const char *, const char *, unsigned long, unsigned long) = nullptr;
    void *(*port_get_buffer)(Port *, unsigned int) = nullptr;
    int (*set_process_callback)(Client *, ProcessCb, void *) = nullptr;
    int (*activate)(Client *) = nullptr;
    int (*deactivate)(Client *) = nullptr;
    unsigned int (*get_sample_rate)(Client *) = nullptr;
    unsigned int (*get_buffer_size)(Client *) = nullptr;
    bool loaded = false;

    static Api &get()
    {
        static Api api;
        return api;
    }

    Api()
    {
        if (!lib.open("libjack.so.0") && !lib.open("libjack.so"))
            return;
        client_open = (decltype(client_open))lib.getFunction("jack_client_open");
        client_close = (decltype(client_close))lib.getFunction("jack_client_close");
        port_register = (decltype(port_register))lib.getFunction("jack_port_register");
        port_get_buffer = (decltype(port_get_buffer))lib.getFunction("jack_port_get_buffer");
        set_process_callback = (decltype(set_process_callback))lib.getFunction("jack_set_process_callback");
        activate = (decltype(activate))lib.getFunction("jack_activate");
        deactivate = (decltype(deactivate))lib.getFunction("jack_deactivate");
        get_sample_rate = (decltype(get_sample_rate))lib.getFunction("jack_get_sample_rate");
        get_buffer_size = (decltype(get_buffer_size))lib.getFunction("jack_get_buffer_size");
        loaded = client_open && client_close && port_register && port_get_buffer && set_process_callback && activate
                 && deactivate && get_sample_rate && get_buffer_size;
    }
};

class Device final : public juce::AudioIODevice
{
public:
    explicit Device(const juce::String &name) : juce::AudioIODevice(name, kTypeName)
    {
        auto &api = Api::get();
        if (!api.loaded)
        {
            lastError = "libjack not found";
            return;
        }
        int status = 0;
        client = api.client_open("JiV-881", 1 /* JackNoStartServer */, &status);
        if (client == nullptr)
        {
            lastError = "No JACK server running";
            return;
        }
        static const char *names[kNumChannels] = {"main_L",   "main_R",   "part1_L", "part1_R", "part2_L", "part2_R",
                                                  "part3_L",  "part3_R",  "part4_L", "part4_R", "part5_L", "part5_R",
                                                  "part6_L",  "part6_R",  "part7_L", "part7_R", "part8_L", "part8_R"};
        for (int i = 0; i < kNumChannels; i++)
        {
            ports[i] = api.port_register(client, names[i], "32 bit float mono audio", 0x2 /* JackPortIsOutput */, 0);
            channelNames.add(names[i]);
        }
    }

    ~Device() override
    {
        close();
        if (client != nullptr)
            Api::get().client_close(client);
    }

    bool isUsable() const { return client != nullptr; }

    juce::StringArray getOutputChannelNames() override { return channelNames; }
    juce::StringArray getInputChannelNames() override { return {}; }

    // All 18 channels are always processed: the selector's per-channel ticks do not apply to this device.
    std::optional<juce::BigInteger> getDefaultOutputChannels() const override { return allChannels(); }

    juce::Array<double> getAvailableSampleRates() override
    {
        return {client != nullptr ? (double)Api::get().get_sample_rate(client) : 44100.0};
    }
    juce::Array<int> getAvailableBufferSizes() override { return {getCurrentBufferSizeSamples()}; }
    int getDefaultBufferSize() override { return getCurrentBufferSizeSamples(); }

    juce::String open(const juce::BigInteger &, const juce::BigInteger &, double, int) override
    {
        if (client == nullptr)
            return lastError;
        lastError.clear();
        if (!deviceIsOpen)
        {
            Api::get().set_process_callback(client, &Device::processTrampoline, this);
            Api::get().activate(client);
            deviceIsOpen = true;
        }
        return lastError;
    }

    void close() override
    {
        stop();
        if (client != nullptr && deviceIsOpen)
            Api::get().deactivate(client);
        deviceIsOpen = false;
    }

    bool isOpen() override { return deviceIsOpen; }

    void start(juce::AudioIODeviceCallback *newCallback) override
    {
        if (!deviceIsOpen || newCallback == callback)
            return;
        if (newCallback != nullptr)
            newCallback->audioDeviceAboutToStart(this);
        juce::AudioIODeviceCallback *old;
        {
            const juce::ScopedLock sl(callbackLock);
            old = callback;
            callback = newCallback;
        }
        if (old != nullptr)
            old->audioDeviceStopped();
    }

    void stop() override { start(nullptr); }
    bool isPlaying() override { return callback != nullptr; }
    juce::String getLastError() override { return lastError; }

    int getCurrentBufferSizeSamples() override
    {
        return client != nullptr ? (int)Api::get().get_buffer_size(client) : 512;
    }
    double getCurrentSampleRate() override
    {
        return client != nullptr ? (double)Api::get().get_sample_rate(client) : 44100.0;
    }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return allChannels(); }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return getCurrentBufferSizeSamples(); }
    int getInputLatencyInSamples() override { return 0; }

private:
    static juce::BigInteger allChannels()
    {
        juce::BigInteger b;
        b.setRange(0, kNumChannels, true);
        return b;
    }

    static int processTrampoline(unsigned int nframes, void *arg) { return static_cast<Device *>(arg)->process(nframes); }

    int process(unsigned int nframes)
    {
        auto &api = Api::get();
        float *outs[kNumChannels];
        for (int i = 0; i < kNumChannels; i++)
            outs[i] = static_cast<float *>(api.port_get_buffer(ports[i], nframes));

        const juce::ScopedLock sl(callbackLock);
        if (callback != nullptr)
            callback->audioDeviceIOCallbackWithContext(nullptr, 0, outs, kNumChannels, (int)nframes, {});
        else
            for (int i = 0; i < kNumChannels; i++)
                juce::FloatVectorOperations::clear(outs[i], (int)nframes);
        return 0;
    }

    Api::Client *client = nullptr;
    Api::Port *ports[kNumChannels] = {};
    juce::StringArray channelNames;
    juce::String lastError;
    bool deviceIsOpen = false;
    juce::AudioIODeviceCallback *callback = nullptr;
    juce::CriticalSection callbackLock;
};

class DeviceType final : public juce::AudioIODeviceType
{
public:
    DeviceType() : juce::AudioIODeviceType(kTypeName) {}

    void scanForDevices() override
    {
        // Listed only when a JACK server answers right now (a probe client, opened and closed at once).
        available = false;
        auto &api = Api::get();
        if (!api.loaded)
            return;
        int status = 0;
        if (auto *probe = api.client_open("JiV-881-probe", 1, &status))
        {
            api.client_close(probe);
            available = true;
        }
    }

    juce::StringArray getDeviceNames(bool wantInputNames = false) const override
    {
        if (wantInputNames || !available)
            return {};
        return {"JiV-881 (main + 8 Parts)"};
    }
    int getDefaultDeviceIndex(bool) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice *device, bool asInput) const override
    {
        return (device != nullptr && !asInput && device->getTypeName() == kTypeName) ? 0 : -1;
    }
    bool hasSeparateInputsAndOutputs() const override { return false; }

    juce::AudioIODevice *createDevice(const juce::String &outputDeviceName, const juce::String &) override
    {
        if (outputDeviceName.isEmpty() || !available)
            return nullptr;
        auto device = std::make_unique<Device>(outputDeviceName);
        return device->isUsable() ? device.release() : nullptr;
    }

private:
    bool available = false;
};
} // namespace jivjack

#endif // JUCE_LINUX

// Reports the audio device type the Standalone's device manager switches to (so it can be restored at the next launch).
struct DeviceTypeWatcher final : private juce::ChangeListener
{
    DeviceTypeWatcher(juce::AudioDeviceManager &m, std::function<void(const juce::String &)> f) : manager(m), onType(std::move(f))
    {
        manager.addChangeListener(this);
    }
    ~DeviceTypeWatcher() override { manager.removeChangeListener(this); }

private:
    void changeListenerCallback(juce::ChangeBroadcaster *) override
    {
        if (onType)
            onType(manager.getCurrentAudioDeviceType());
    }
    juce::AudioDeviceManager &manager;
    std::function<void(const juce::String &)> onType;
};
