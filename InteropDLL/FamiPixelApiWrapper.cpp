#include "Common.h"
#include "Core/Shared/Emulator.h"
#include "Core/Shared/BaseControlDevice.h"
#include "Core/Shared/BaseControlManager.h"
#include "Core/Shared/Interfaces/IConsole.h"
#include "Core/Shared/Interfaces/IInputProvider.h"
#include "Core/Debugger/Debugger.h"
#include "Core/NES/Input/NesController.h"

extern unique_ptr<Emulator> _emu;

class FamiPixelInputProvider : public IInputProvider
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

static unique_ptr<FamiPixelInputProvider> _famiPixelInputProvider;
static bool _famiPixelInputRegistered = false;

extern "C"
{
	// Inject a deterministic NES controller state through Mesen's native
	// IInputProvider path. The byte layout matches NesController::ToByte():
	// bit0=A, bit1=B, bit2=Select, bit3=Start,
	// bit4=Up, bit5=Down, bit6=Left, bit7=Right.
	//
	// Unlike Debugger::SetInputOverrides(), an all-zero state is meaningful and
	// therefore RELEASE is represented exactly as 0x00.
	//
	// Return codes:
	//   0 = success
	//   1 = emulator is not running
	//   2 = invalid controller port (only NES ports 0 and 1 are supported)
	DllExport int32_t __stdcall FamiPixelSetNesControllerState(uint32_t port, uint8_t buttons)
	{
		if(!_emu || !_emu->IsRunning()) {
			return 1;
		}
		if(port >= 2) {
			return 2;
		}

		if(!_famiPixelInputProvider) {
			_famiPixelInputProvider.reset(new FamiPixelInputProvider());
		}
		if(!_famiPixelInputRegistered) {
			_emu->RegisterInputProvider(_famiPixelInputProvider.get());
			_famiPixelInputRegistered = true;
		}

		_famiPixelInputProvider->SetButtons(port, buttons);
		return 0;
	}

	// Return the actual current state stored in the emulated NES controller.
	// This is a witness for provider -> controller delivery, independent of
	// game-specific RAM. Values 0..255 are valid controller bytes; -1 means the
	// emulator/port/device is unavailable.
	DllExport int32_t __stdcall FamiPixelGetNesControllerState(uint32_t port)
	{
		if(!_emu || !_emu->IsRunning() || port >= 2) {
			return -1;
		}

		shared_ptr<IConsole> console = _emu->GetConsole();
		if(!console) {
			return -1;
		}

		shared_ptr<BaseControlDevice> device = console->GetControlManager()->GetControlDevice((uint8_t)port, 0);
		shared_ptr<NesController> controller = std::dynamic_pointer_cast<NesController>(device);
		if(!controller) {
			return -1;
		}
		return (int32_t)controller->ToByte();
	}

	// Copy the canonical raw NES PPU frame into caller-owned memory.
	//
	// The source is IConsole::GetPpuFrame(), which for NES maps directly to
	// NesPpu::GetScreenBuffer(false). That storage is owned by the PPU and uses
	// two alternating uint16_t buffers, so no pointer is exposed across the DLL
	// boundary. The caller receives a stable copy instead.
	//
	// Pixel layout is Mesen's raw NES PPU color word after grayscale/emphasis
	// processing: palette color in bits 0..5 and emphasis bits in 6..8.
	// Width/height are currently 256x240 for NES.
	//
	// To prevent torn reads while the emulation thread is drawing or swapping
	// buffers, this API requires an initialized debugger that is currently
	// stopped (FamiPixelStepFrame provides that synchronization contract).
	//
	// Return codes:
	//   0 = success
	//   1 = emulator is not running
	//   2 = console/framebuffer is unavailable or not the canonical NES size
	//   3 = output buffer is null
	//   4 = output capacity is too small (capacity is measured in uint16_t pixels)
	//   5 = debugger is not initialized
	//   6 = execution is not stopped
	DllExport int32_t __stdcall FamiPixelCopyNesFrame(
		uint16_t* output,
		uint32_t pixelCapacity,
		uint32_t* outWidth,
		uint32_t* outHeight,
		uint32_t* outFrameCount
	)
	{
		if(!_emu || !_emu->IsRunning()) {
			return 1;
		}
		if(!output) {
			return 3;
		}

		Debugger* debugger = _emu->InternalGetDebugger();
		if(!debugger) {
			return 5;
		}
		if(!debugger->IsExecutionStopped()) {
			return 6;
		}

		shared_ptr<IConsole> console = _emu->GetConsole();
		if(!console) {
			return 2;
		}

		PpuFrameInfo frame = console->GetPpuFrame();
		constexpr uint32_t NesWidth = 256;
		constexpr uint32_t NesHeight = 240;
		constexpr uint32_t NesPixelCount = NesWidth * NesHeight;
		if(!frame.FrameBuffer || frame.Width != NesWidth || frame.Height != NesHeight || frame.FrameBufferSize != NesPixelCount * sizeof(uint16_t)) {
			return 2;
		}
		if(pixelCapacity < NesPixelCount) {
			return 4;
		}

		memcpy(output, frame.FrameBuffer, frame.FrameBufferSize);
		if(outWidth) {
			*outWidth = frame.Width;
		}
		if(outHeight) {
			*outHeight = frame.Height;
		}
		if(outFrameCount) {
			*outFrameCount = frame.FrameCount;
		}
		return 0;
	}
}
