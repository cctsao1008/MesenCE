#include "Common.h"
#include <atomic>
#include <sstream>
#include <string>

#include "Core/Debugger/Debugger.h"
#include "Core/Debugger/IDebugger.h"
#include "Core/Shared/Emulator.h"
#include "Core/Shared/BaseControlDevice.h"
#include "Core/Shared/BaseControlManager.h"
#include "Core/Shared/EmuSettings.h"
#include "Core/Shared/MemoryType.h"
#include "Core/Shared/NotificationManager.h"
#include "Core/Shared/RomInfo.h"
#include "Core/Shared/SaveStateManager.h"
#include "Core/Shared/Interfaces/IConsole.h"
#include "Core/Shared/Interfaces/IInputProvider.h"
#include "Core/Shared/Interfaces/INotificationListener.h"
#include "Core/NES/Input/NesController.h"

extern unique_ptr<Emulator> _emu;

namespace
{
	constexpr uint32_t SpecNesRamSize = 0x800;

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
	shared_ptr<INotificationListener> _famiPixelSpecScheduleListener;
	string _famiPixelSpecRootState;
	string _famiPixelSpecRomSha1;
	ConsoleType _famiPixelSpecConsoleType = {};

	void ReleaseSpecRunner()
	{
		_famiPixelSpecScheduleListener.reset();
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
			_famiPixelSpecEmu->IsDebugging()
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

	bool ArmSpecPpuFrame(Debugger* debugger)
	{
		if(!debugger) {
			return false;
		}
		IDebugger* nesDebugger = debugger->GetCpuDebugger(CpuType::Nes);
		if(!nesDebugger) {
			return false;
		}

		// Bypass Debugger::Step() here because the speculative Emulator has no
		// emulation thread. The underlying NES step request is the exact same
		// PpuFrame countdown used by the live authoritative path.
		nesDebugger->ResetStepBackCache();
		nesDebugger->Step(1, StepType::PpuFrame);
		return true;
	}

	class FamiPixelSpecScheduleListener : public INotificationListener
	{
	private:
		Emulator* _emu = nullptr;
		Debugger* _debugger = nullptr;
		FamiPixelSpecInputProvider* _provider = nullptr;
		uint32_t _port = 0;
		const uint8_t* _buttons = nullptr;
		uint32_t _count = 0;
		uint8_t* _ramOutput = nullptr;
		uint8_t* _controllerOutput = nullptr;
		uint32_t* _frameCountOutput = nullptr;
		uint32_t _index = 0;
		bool _failed = false;
		bool _complete = false;

	public:
		FamiPixelSpecScheduleListener(
			Emulator* emu,
			Debugger* debugger,
			FamiPixelSpecInputProvider* provider,
			uint32_t port,
			const uint8_t* buttons,
			uint32_t count,
			uint8_t* ramOutput,
			uint8_t* controllerOutput,
			uint32_t* frameCountOutput
		) :
			_emu(emu),
			_debugger(debugger),
			_provider(provider),
			_port(port),
			_buttons(buttons),
			_count(count),
			_ramOutput(ramOutput),
			_controllerOutput(controllerOutput),
			_frameCountOutput(frameCountOutput)
		{
		}

		void ProcessNotification(ConsoleNotificationType type, void* parameter) override
		{
			if(type != ConsoleNotificationType::CodeBreak || _complete || _failed) {
				return;
			}

			if(!_emu || !_debugger || !_provider || _index >= _count) {
				_failed = true;
				if(_debugger) {
					_debugger->Run();
				}
				return;
			}

			ConsoleMemoryInfo memory = _emu->GetMemory(MemoryType::NesInternalRam);
			shared_ptr<NesController> controller = GetSpecController(_port);
			if(!memory.Memory || memory.Size < SpecNesRamSize || !controller) {
				_failed = true;
				_debugger->Run();
				return;
			}

			// SleepUntilResume() calls NesDebugger::OnBeforeBreak() before CodeBreak,
			// so the APU has already been caught up exactly as on the live path.
			memcpy(
				_ramOutput + ((size_t)_index * SpecNesRamSize),
				memory.Memory,
				SpecNesRamSize
			);
			_controllerOutput[_index] = controller->ToByte();
			_frameCountOutput[_index] = _emu->GetFrameCount();
			_index++;

			// Synchronously release this exact debugger boundary. Notification
			// delivery is synchronous, so no emulation cycle can run between
			// changing the requested input and arming the next PPU-frame period.
			_debugger->Run();

			if(_index >= _count) {
				_complete = true;
				return;
			}

			_provider->SetButtons(_port, _buttons[_index]);
			if(!ArmSpecPpuFrame(_debugger)) {
				_failed = true;
			}
		}

		bool Failed() const { return _failed; }
		bool Complete() const { return _complete; }
		uint32_t CapturedCount() const { return _index; }
	};
}

extern "C"
{
	// Create one dedicated speculative NES Emulator from the current live root.
	// The speculative instance intentionally has no emulation thread. The exact
	// multi-frame schedule path may create an internal debugger only to reuse
	// Mesen's authoritative PPU-frame boundary counter; it never performs a
	// host-side debugger sleep/wake round trip.
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

	// Low-level frame-count-edge primitive retained for diagnostics only.
	// NesConsole::RunFrame() stops when the PPU frame counter changes; that is
	// NOT equivalent to the live Debugger::Step(PpuFrame) full-period boundary
	// when execution begins at an arbitrary PPU phase. Exact rollouts should use
	// FamiPixelSpecRunSchedule().
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

	// Execute a variable-input schedule continuously while sampling witnesses at
	// the exact same PPU-frame-period boundaries as the live debugger path.
	// The internal CodeBreak listener immediately captures the witness, changes
	// the requested input for the next period, and resumes synchronously. This
	// preserves mid-instruction continuity without host polling or serialization
	// at the intermediate boundaries.
	//
	// ramOutput layout: frameCount consecutive 0x800-byte NES internal-RAM images.
	// Return codes:
	//   0 = success
	//   1 = spec runner unavailable
	//   2 = non-NES console
	//   3 = invalid port/count/buttons
	//   4 = NES controller unavailable
	//   5 = output pointer/capacity invalid
	//   6 = speculative debugger unavailable
	//   7 = boundary capture/next-step failure
	//   8 = direct speculative frame gate rejected execution
	//   9 = expected boundary count was not reached
	DllExport int32_t __stdcall FamiPixelSpecRunSchedule(
		uint32_t port,
		const uint8_t* buttons,
		uint32_t frameCount,
		uint8_t* ramOutput,
		uint32_t ramCapacity,
		uint8_t* controllerOutput,
		uint32_t controllerCapacity,
		uint32_t* frameCountOutput,
		uint32_t frameCountCapacity
	)
	{
		if(!_famiPixelSpecEmu || !_famiPixelSpecEmu->IsRunning() || !_famiPixelSpecInputProvider || !_famiPixelSpecInputRegistered) {
			return 1;
		}
		if(_famiPixelSpecEmu->GetConsoleType() != ConsoleType::Nes) {
			return 2;
		}
		if(port >= 2 || !buttons || frameCount == 0) {
			return 3;
		}
		if(!GetSpecController(port)) {
			return 4;
		}
		if(!ramOutput || !controllerOutput || !frameCountOutput || frameCount > UINT32_MAX / SpecNesRamSize) {
			return 5;
		}
		uint32_t requiredRam = frameCount * SpecNesRamSize;
		if(ramCapacity < requiredRam || controllerCapacity < frameCount || frameCountCapacity < frameCount) {
			return 5;
		}

		if(!_famiPixelSpecEmu->IsDebugging()) {
			_famiPixelSpecEmu->InitDebugger();
		}
		Debugger* debugger = _famiPixelSpecEmu->InternalGetDebugger();
		if(!debugger) {
			return 6;
		}

		shared_ptr<FamiPixelSpecScheduleListener> listener(new FamiPixelSpecScheduleListener(
			_famiPixelSpecEmu.get(),
			debugger,
			_famiPixelSpecInputProvider.get(),
			port,
			buttons,
			frameCount,
			ramOutput,
			controllerOutput,
			frameCountOutput
		));
		_famiPixelSpecScheduleListener = listener;
		_famiPixelSpecEmu->GetNotificationManager()->RegisterNotificationListener(listener);

		_famiPixelSpecInputProvider->SetButtons(port, buttons[0]);
		if(!ArmSpecPpuFrame(debugger)) {
			_famiPixelSpecScheduleListener.reset();
			return 6;
		}

		// RunFrame() is only the coarse execution pump here. Exact witnesses are
		// captured by the synchronous debugger boundary listener inside PPU-cycle
		// processing. A full PPU period can span two RunFrame() calls when the root
		// begins after the PPU frame-counter edge, so use a bounded generous guard.
		uint64_t maxPumps = (uint64_t)frameCount * 2 + 4;
		uint64_t pumps = 0;
		while(!listener->Complete() && !listener->Failed() && pumps < maxPumps) {
			pumps++;
			if(!_famiPixelSpecEmu->RunSpeculativeFrame()) {
				debugger->Run();
				_famiPixelSpecScheduleListener.reset();
				return 8;
			}
		}

		bool failed = listener->Failed();
		bool complete = listener->Complete();
		uint32_t captured = listener->CapturedCount();
		_famiPixelSpecScheduleListener.reset();

		if(failed) {
			return 7;
		}
		if(!complete || captured != frameCount) {
			debugger->Run();
			return 9;
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
