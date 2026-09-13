#include "Common.h"
#include "Core/Shared/RecordedRomTest.h"
#include "Core/Shared/Emulator.h"
#include "Core/Shared/EmuSettings.h"
#include "Core/Shared/NotificationManager.h"
#include "Core/Shared/Interfaces/INotificationListener.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/StringUtilities.h"

extern unique_ptr<Emulator> _emu;
shared_ptr<RecordedRomTest> _recordedRomTest;

class FamiPixelFrameListener : public INotificationListener
{
private:
	std::atomic<uint32_t> _frameEvents = 0;
	std::mutex _mutex;
	std::condition_variable _cv;

public:
	void ProcessNotification(ConsoleNotificationType type, void* parameter) override
	{
		if(type == ConsoleNotificationType::PpuFrameDone) {
			_frameEvents.fetch_add(1, std::memory_order_release);
			_cv.notify_all();
		}
	}

	uint32_t GetFrameEvents() const
	{
		return _frameEvents.load(std::memory_order_acquire);
	}

	bool WaitForFrameEvents(uint32_t target, uint32_t timeoutMs)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		auto reached = [&]() {
			return (int32_t)(_frameEvents.load(std::memory_order_acquire) - target) >= 0;
		};
		if(timeoutMs == 0) {
			_cv.wait(lock, reached);
			return true;
		}
		return _cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), reached);
	}
};

static shared_ptr<FamiPixelFrameListener> _famiPixelFrameListener;
static Emulator* _famiPixelFrameListenerEmu = nullptr;

static FamiPixelFrameListener* EnsureFamiPixelFrameListener()
{
	if(!_emu) {
		return nullptr;
	}
	if(!_famiPixelFrameListener || _famiPixelFrameListenerEmu != _emu.get()) {
		_famiPixelFrameListener = std::make_shared<FamiPixelFrameListener>();
		_emu->GetNotificationManager()->RegisterNotificationListener(_famiPixelFrameListener);
		_famiPixelFrameListenerEmu = _emu.get();
	}
	return _famiPixelFrameListener.get();
}

extern "C"
{
	DllExport RomTestResult __stdcall RunRecordedTest(const char* filename, bool inBackground)
	{
		if(inBackground) {
			unique_ptr<Emulator> emu(new Emulator());
			emu->Initialize(false);
			emu->GetSettings()->SetFlag(EmulationFlags::TestMode);
			shared_ptr<RecordedRomTest> romTest(new RecordedRomTest(emu.get(), true));
			RomTestResult result = romTest->Run(filename);
			emu->Release();
			return result;
		} else {
			shared_ptr<RecordedRomTest> romTest(new RecordedRomTest(_emu.get(), false));
			return romTest->Run(filename);
		}
	}

	DllExport uint64_t __stdcall RunTest(char* filename, uint32_t address, MemoryType memType)
	{
		unique_ptr<Emulator> emu(new Emulator());
		emu->Initialize();
		emu->GetSettings()->SetFlag(EmulationFlags::TestMode);
		emu->GetSettings()->GetGameboyConfig().Model = GameboyModel::Gameboy;
		emu->GetSettings()->GetGameboyConfig().RamPowerOnState = RamState::AllZeros;
		emu->LoadRom((VirtualFile)filename, VirtualFile());
		emu->GetSettings()->SetFlag(EmulationFlags::MaximumSpeed);

		while(emu->GetFrameCount() < 500) {
			std::this_thread::sleep_for(std::chrono::duration<int, std::milli>(10));
		}

		ConsoleMemoryInfo memInfo = emu->GetMemory(memType);
		uint8_t* memBuffer = (uint8_t*)memInfo.Memory;
		uint64_t result = memBuffer[address];
		for(int i = 1; i < 8; i++) {
			if(address + i < memInfo.Size) {
				result |= ((uint64_t)memBuffer[address + i] << (8 * i));
			} else {
				break;
			}
		}

		emu->Stop(false);
		emu->Release();

		return result;
	}

	// Fami Pixel interop extension.
	//
	// The stock debugger Step() export is asynchronous from the caller's point
	// of view. A completed PPU frame is synchronized through Mesen's native
	// PpuFrameDone notification rather than by polling a frame counter that is
	// owned and updated by the emulation thread.
	//
	// Return codes:
	//   0 = success
	//   1 = emulator is not running
	//   2 = debugger is not initialized
	//   3 = invalid frame count
	//   4 = timeout waiting for PPU frame completion
	//   5 = timeout waiting for debugger stop
	DllExport int32_t __stdcall FamiPixelStepFrame(uint32_t count, uint32_t timeoutMs)
	{
		if(count == 0) {
			return 3;
		}
		if(!_emu || !_emu->IsRunning()) {
			return 1;
		}

		Debugger* debugger = _emu->InternalGetDebugger();
		if(!debugger) {
			return 2;
		}

		FamiPixelFrameListener* listener = EnsureFamiPixelFrameListener();
		if(!listener) {
			return 1;
		}

		uint32_t startEvent = listener->GetFrameEvents();
		uint32_t targetEvent = startEvent + count;
		debugger->Step(CpuType::Nes, count, StepType::PpuFrame);

		auto startTime = std::chrono::steady_clock::now();
		if(!listener->WaitForFrameEvents(targetEvent, timeoutMs)) {
			return 4;
		}

		auto timedOut = [&]() {
			if(timeoutMs == 0) {
				return false;
			}
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - startTime
			).count();
			return elapsed >= timeoutMs;
		};

		while(!debugger->IsExecutionStopped()) {
			if(timedOut()) {
				return 5;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		return 0;
	}

	DllExport uint32_t __stdcall FamiPixelGetFrameCount()
	{
		return _emu ? _emu->GetFrameCount() : 0;
	}

	DllExport void __stdcall RomTestRecord(char* filename, bool reset)
	{
		_recordedRomTest.reset(new RecordedRomTest(_emu.get(), false));
		_recordedRomTest->Record(filename, reset);
	}

	DllExport void __stdcall RomTestStop()
	{
		if(_recordedRomTest) {
			_recordedRomTest->Stop();
			_recordedRomTest.reset();
		}
	}

	DllExport bool __stdcall RomTestRecording()
	{
		return _recordedRomTest != nullptr;
	}

	DllExport bool __stdcall RunCiTests(string testFolder, bool enableDebugger)
	{
		std::replace(testFolder.begin(), testFolder.end(), '\\', '/');
		if(!StringUtilities::EndsWith(testFolder, "/")) {
			testFolder += "/";
		}

		//GBA tests can't run without the GBA BIOS, skip all of them
		vector<string> foldersToSkip = { "GBA/" };

		unordered_set<string> testsToSkip = {
			//These GB tests fail because the test runner can't use the official GB boot rom
			"GB/GBEmulatorShootout/hacktix/bully_gbc.mtp",
			"GB/GBEmulatorShootout/hacktix/bully.mtp",
			"GB/GBEmulatorShootout/mooneye/acceptance/boot_div-dmgABCmgb.mtp",
			"GB/GBEmulatorShootout/mooneye/acceptance/boot_hwio-dmgABCmgb.mtp",
			"GB/GBEmulatorShootout/mooneye/acceptance/serial/boot_sclk_align-dmgABCmgb.mtp",
			"GB/GBEmulatorShootout/mooneye/misc/boot_div-cgbABCDE.mtp",

			//SGB tests can't run properly without the SGB ROM
			"SNES/sgb_packet_test.mtp",
			"SNES/sgb-ext-test.mtp",
			"SNES/sgb-mlt-test.mtp"
		};

		vector<string> tests = FolderUtilities::GetFilesInFolder(testFolder, { ".mtp" }, true, 5);

		vector<string> testsToRun;

		for(string& test : tests) {
			string testPath = test.substr(testFolder.size());
			std::replace(testPath.begin(), testPath.end(), '\\', '/');
			bool include = true;
			for(string& folderToSkip : foldersToSkip) {
				if(StringUtilities::StartsWith(testPath, folderToSkip.c_str())) {
					include = false;
					break;
				}
			}
			if(testsToSkip.find(testPath) != testsToSkip.end()) {
				include = false;
			}
			if(include) {
				testsToRun.push_back(test);
			}
		}

		FolderUtilities::SetHomeFolder(FolderUtilities::CombinePath(testFolder, "MesenHomeFolder"));

		std::cout << "== CI test mode ==\n";
		int threadCount = std::thread::hardware_concurrency();
		int skipCount = (int)(tests.size() - testsToRun.size());
		std::cout << "Running on " << threadCount << " threads\n";
		std::cout << "Test count: " << tests.size() << "\n";
		std::cout << "Tests to skip: " << skipCount << "\n\n";

		std::atomic<int> testNumber = 0;
		std::atomic<int> failCount = 0;
		std::atomic<int> passCount = 0;
		std::atomic<int> progress = 0;

		vector<thread> threads(threadCount);
		for(int i = 0; i < threadCount; i++) {
			threads[i] = std::thread([&]() {
				while(true) {
					int nextTest = testNumber++;
					if(nextTest >= testsToRun.size()) {
						break;
					}
					RomTestResult result = RunRecordedTest(testsToRun[nextTest].c_str(), true);
					if(result.State == RomTestState::Failed) {
						failCount++;
						string testPath = testsToRun[nextTest].substr(testFolder.size());
						std::cout << ("\n== FAILED: " + testPath + "\n");
					} else {
						passCount++;
					}

					int newProgress = (failCount + passCount) * 100 / (uint32_t)testsToRun.size();
					if(newProgress > progress) {
						std::cout << ("\rRunning... (" + std::to_string(newProgress) + "%)");
						progress = newProgress;
					}
				}
			});
		}

		for(int i = 0; i < threadCount; i++) {
			threads[i].join();
		}

		std::cout << "\n\n===================\n";
		std::cout << "Passed: " << passCount << "\n";
		std::cout << "Failed: " << failCount << "\n";
		std::cout << "Skipped: " << skipCount << "\n";
		std::cout << "===================\n";

		return failCount == 0;
	}
}
