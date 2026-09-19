#include "Common.h"
#include <atomic>
#include <sstream>
#include <string>

#include "Core/Shared/Emulator.h"
#include "Core/Shared/BaseControlDevice.h"
#include "Core/Shared/BaseControlManager.h"
#include "Core/Shared/EmuSettings.h"
#include "Core/Shared/MemoryType.h"
#include "Core/Shared/RomInfo.h"
#include "Core/Shared/SaveStateManager.h"
#include "Core/Shared/Interfaces/IConsole.h"
#include "Core/Shared/Interfaces/IInputProvider.h"
#include "Core/NES/Input/NesController.h"

extern unique_ptr<Emulator> _emu;

namespace
{
	class FamiPixelSpecInputProvider : public IInputProvider
	{
	private:
		std::atomic<uint8_t> _buttons[2] = { 0, 0 };
		std::atomic<bool> _enabled[2] = { false, false };

	public:
		void SetButtons(uint32_t port, uint8_t buttons)
		{
			_buttons[port] = buttons;
			_enabled[port] = true;
		}

		bool SetInput(BaseControlDevice* device) override
		{
			if(!device) {
				return false;
			}

			uint8_t port = device->GetPort();
			if(port >= 2 || !_enabled[port]) {
				return false;
			}

			NesController* controller = dynamic_cast<NesController*>(device);
			if(!controller) {
				return false;
			}

			uint8_t buttons = _buttons[port].load();
			controller->SetBitValue(NesController::Buttons::A, (buttons & 0x01) != 0);
			controller->SetBitValue(NesController::Buttons::B, (buttons & 0x02) != 0);
			controller->SetBitValue(NesController::Buttons::Select, (buttons & 0x04) != 0);
			controller->SetBitValue(NesController::Buttons::Start, (buttons & 0x08) != 0);
			controller->SetBitValue(NesController::Buttons::Up, (buttons & 0x10) != 0);
			controller->SetBitValue(NesController::Buttons::Down, (buttons & 0x20) != 0);
			controller->SetBitValue(NesController::Buttons::Left, (buttons & 0x40) != 0);
			controller->SetBitValue(NesController::Buttons::Right, (buttons & 0x80) != 0);
			return true;
		}
	};

	unique_ptr<Emulator> _famiPixelSpecEmu;
	unique_ptr<FamiPixelSpecInputProvider> _famiPixelSpecInputProvider;
	bool _famiPixelSpecInputRegistered = false;
	string _famiPixelSpecRootState;
	string _famiPixelSpecRomSha1;
	ConsoleType _famiPixelSpecConsoleType = {};

	void ReleaseSpecRunner()
	{
		if(_famiPixelSpecEmu) {
			if(_famiPixelSpecInputRegistered && _famiPixelSpecInputProvider && _famiPixelSpecEmu->IsRunning()) {
				_famiPixelSpecEmu->UnregisterInputProvider(_famiPixelSpecInputProvider.get());
			}
			_famiPixelSpecInputRegistered = false;

			// The speculative instance must not persist battery/recent-game state.
			_famiPixelSpecEmu->Stop(false, true, false);
			_famiPixelSpecEmu->Release();
		}
		_famiPixelSpecInputRegistered = false;
		_famiPixelSpecInputProvider.reset();
		_famiPixelSpecEmu.reset();
		_famiPixelSpecRootState.clear();
		_famiPixelSpecRomSha1.clear();
		_famiPixelSpecConsoleType = {};
	}

	bool CaptureLiveState(string& state, ConsoleType& consoleType, string& romSha1)
	{
		if(!_emu || !_emu->IsRunning()) {
			return false;
		}

		std::stringstream stream;
		{
			auto lock = _emu->AcquireLock();
			consoleType = _emu->GetConsoleType();
			romSha1 = _emu->GetHash(HashType::Sha1);
			// Cross-instance cloning must include emulation-impacting settings.
			// In particular, NES controller types are part of EmuSettings::Serialize;
			// loading them before the console state lets NesControlManager recreate
			// the same devices before their serialized state is restored.
			_emu->Serialize(stream, true, 0);
		}
		state = stream.str();
		return !state.empty();
	}

	DeserializeResult LoadSpecState(const string& state, ConsoleType consoleType)
	{
		std::stringstream stream(state);
		return _famiPixelSpecEmu->Deserialize(
			stream,
			SaveStateManager::FileFormatVersion,
			true,
			consoleType,
			false
		);
	}

	shared_ptr<NesController> GetSpecController(uint32_t port)
	{
		if(!_famiPixelSpecEmu || !_famiPixelSpecEmu->IsRunning() || port >= 2) {
			return nullptr;
		}
		shared_ptr<IConsole> console = _famiPixelSpecEmu->GetConsole();
		if(!console) {
			return nullptr;
		}
		shared_ptr<BaseControlDevice> device = console->GetControlManager()->GetControlDevice((uint8_t)port, 0);
		return std::dynamic_pointer_cast<NesController>(device);
	}
}

extern "C"
{
	// Create one dedicated speculative NES Emulator from the current live root.
	// The speculative instance intentionally has no emulation thread and no
	// debugger. Frames are advanced only by FamiPixelSpecRunFrames().
	//
	// Return codes:
	//   0 = success
	//   1 = live emulator is not running / root capture failed
	//   2 = live console is not NES
	//   3 = speculative ROM load failed
	//   4 = speculative root deserialize failed
	DllExport int32_t __stdcall FamiPixelSpecInitFromLive()
	{
		if(!_emu || !_emu->IsRunning()) {
			return 1;
		}
		if(_emu->GetConsoleType() != ConsoleType::Nes) {
			return 2;
		}

		string rootState;
		string romSha1;
		ConsoleType consoleType = {};
		if(!CaptureLiveState(rootState, consoleType, romSha1)) {
			return 1;
		}

		RomInfo romInfo = _emu->GetRomInfo();
		ReleaseSpecRunner();

		unique_ptr<Emulator> specEmu(new Emulator());
		specEmu->Initialize(false);
		specEmu->GetSettings()->SetFlag(EmulationFlags::TestMode);
		if(!specEmu->LoadRom(romInfo.RomFile, romInfo.PatchFile, false)) {
			specEmu->Stop(false, true, false);
			specEmu->Release();
			return 3;
		}

		_famiPixelSpecEmu = std::move(specEmu);
		_famiPixelSpecInputProvider.reset(new FamiPixelSpecInputProvider());
		_famiPixelSpecConsoleType = consoleType;
		_famiPixelSpecRomSha1 = romSha1;
		_famiPixelSpecRootState = rootState;

		if(LoadSpecState(_famiPixelSpecRootState, _famiPixelSpecConsoleType) != DeserializeResult::Success) {
			ReleaseSpecRunner();
			return 4;
		}

		// Match the live fami-pixel input contract exactly: the provider owns the
		// requested byte, while the NES core applies it at its native InputScanline
		// through BaseControlManager::UpdateInputState(). Do not mutate the
		// controller eagerly at the API boundary.
		_famiPixelSpecEmu->RegisterInputProvider(_famiPixelSpecInputProvider.get());
		_famiPixelSpecInputRegistered = true;
		return 0;
	}

	// Recapture the current live machine state as the speculative decision root
	// and reset the speculative instance to that exact root.
	//
	// Return codes:
	//   0 = success
	//   1 = live/spec emulator unavailable or root capture failed
	//   2 = live console is not NES
	//   3 = live ROM differs from the ROM used to initialize the spec runner
	//   4 = speculative root deserialize failed
	DllExport int32_t __stdcall FamiPixelSpecCaptureRootFromLive()
	{
		if(!_famiPixelSpecEmu || !_famiPixelSpecEmu->IsRunning() || !_emu || !_emu->IsRunning()) {
			return 1;
		}
		if(_emu->GetConsoleType() != ConsoleType::Nes) {
			return 2;
		}

		string rootState;
		string romSha1;
		ConsoleType consoleType = {};
		if(!CaptureLiveState(rootState, consoleType, romSha1)) {
			return 1;
		}
		if(consoleType != _famiPixelSpecConsoleType || romSha1 != _famiPixelSpecRomSha1) {
			return 3;
		}

		if(LoadSpecState(rootState, consoleType) != DeserializeResult::Success) {
			return 4;
		}
		_famiPixelSpecRootState = std::move(rootState);
		return 0;
	}

	// Restore the cached current-root state before evaluating another candidate.
	// Return codes: 0 = success, 1 = spec runner unavailable, 4 = deserialize failed.
	DllExport int32_t __stdcall FamiPixelSpecResetToRoot()
	{
		if(!_famiPixelSpecEmu || !_famiPixelSpecEmu->IsRunning() || _famiPixelSpecRootState.empty()) {
			return 1;
		}
		return LoadSpecState(_famiPixelSpecRootState, _famiPixelSpecConsoleType) == DeserializeResult::Success ? 0 : 4;
	}

	// Set the deterministic controller byte on the dedicated speculative input
	// provider. The NES core consumes it at its normal input-poll point, matching
	// FamiPixelSetNesControllerState() on the live emulator. This is deliberately
	// not an eager mutation of NesController::_state.
	//
	// Return codes:
	//   0 = success
	//   1 = spec runner unavailable
	//   2 = invalid port
	//   3 = NES controller device unavailable
	DllExport int32_t __stdcall FamiPixelSpecSetNesControllerState(uint32_t port, uint8_t buttons)
	{
		if(!_famiPixelSpecEmu || !_famiPixelSpecEmu->IsRunning() || !_famiPixelSpecInputProvider || !_famiPixelSpecInputRegistered) {
			return 1;
		}
		if(port >= 2) {
			return 2;
		}
		if(!GetSpecController(port)) {
			return 3;
		}

		_famiPixelSpecInputProvider->SetButtons(port, buttons);
		return 0;
	}

	// Advance the speculative NES through the Emulator-owned direct-frame gate.
	// That gate reuses Mesen's run-ahead semantics so a threadless speculative
	// instance bypasses output/pacing hooks without touching the live emulator.
	//
	// Return codes:
	//   0 = success
	//   1 = unavailable
	//   2 = non-NES console
	//   3 = count=0
	//   4 = speculative frame gate rejected execution
	DllExport int32_t __stdcall FamiPixelSpecRunFrames(uint32_t count)
	{
		if(!_famiPixelSpecEmu || !_famiPixelSpecEmu->IsRunning()) {
			return 1;
		}
		if(_famiPixelSpecEmu->GetConsoleType() != ConsoleType::Nes) {
			return 2;
		}
		if(count == 0) {
			return 3;
		}

		for(uint32_t i = 0; i < count; i++) {
			if(!_famiPixelSpecEmu->RunSpeculativeFrame()) {
				return 4;
			}
		}
		return 0;
	}

	// Read the actual current controller byte in the speculative instance.
	// Values 0..255 are valid; -1 means unavailable/invalid port/device.
	DllExport int32_t __stdcall FamiPixelSpecGetNesControllerState(uint32_t port)
	{
		shared_ptr<NesController> controller = GetSpecController(port);
		return controller ? (int32_t)controller->ToByte() : -1;
	}

	// Copy a range from the speculative NES 2 KiB internal RAM image.
	// Return codes: 0 = success, 1 = unavailable, 2 = invalid range, 3 = null output.
	DllExport int32_t __stdcall FamiPixelSpecReadNesInternalRam(uint32_t address, uint8_t* output, uint32_t length)
	{
		if(!_famiPixelSpecEmu || !_famiPixelSpecEmu->IsRunning()) {
			return 1;
		}
		if(!output) {
			return 3;
		}
		ConsoleMemoryInfo memory = _famiPixelSpecEmu->GetMemory(MemoryType::NesInternalRam);
		if(!memory.Memory || address > memory.Size || length > memory.Size - address) {
			return 2;
		}
		memcpy(output, (uint8_t*)memory.Memory + address, length);
		return 0;
	}

	DllExport uint32_t __stdcall FamiPixelSpecGetFrameCount()
	{
		return _famiPixelSpecEmu ? _famiPixelSpecEmu->GetFrameCount() : 0;
	}

	DllExport void __stdcall FamiPixelSpecRelease()
	{
		ReleaseSpecRunner();
	}
}
