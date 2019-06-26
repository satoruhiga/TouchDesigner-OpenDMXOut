#if OPERATOR_TYPE_CHOP == 1

#include "CPlusPlus_Common.h"
#include "CHOP_CPlusPlusBase.h"

#include <iostream>
#include <vector>
#include <array>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <chrono>

#include "FTD2XX.H"

////

class OPERATOR_NAME : public CHOP_CPlusPlusBase
{
public:

	std::vector<std::string> serials;
	std::array<uint8_t, 513> universe;

	std::thread sender_thread;
	std::atomic_bool running;
	std::mutex mtx;

	FT_HANDLE ftHandle = nullptr;
	bool active = false;

	OPERATOR_NAME(const OP_NodeInfo* info)
	{
		updateSerials();
	}

	virtual ~OPERATOR_NAME()
	{
		stop();
		close();
	}

	void updateSerials()
	{
		DWORD num = 0;
		FT_STATUS status = FT_CreateDeviceInfoList(&num);
		if (status != FT_OK || num <= 0)
		{
			std::cerr << "Error in FT_CreateDeviceInfoList" << std::endl;
			return;
		}

		FT_DEVICE_LIST_INFO_NODE* devInfo = new FT_DEVICE_LIST_INFO_NODE[num];
		if (FT_GetDeviceInfoList(devInfo, &num) != FT_OK)
		{
			std::cerr << "Error in FT_CreateDeviceInfoList" << std::endl;
			return;
		}
		
		serials.clear();

		int id = 0;
		for (DWORD i = 0; i < num; i++)
		{
			DWORD deviceIndex = i;
			serials.push_back(devInfo[i].SerialNumber);
		}
	}

	bool open(std::string serial)
	{
		if (ftHandle)
			close();

		int index = -1;
		for (int i = 0; i < serials.size(); i++)
		{
			if (serials[i] == serial)
			{
				index = i;
				break;
			}
		}

		if (index < 0)
		{
			std::cerr << "Invalid serial" << std::endl;
			return false;
		}

		ftHandle = nullptr;
		FT_STATUS status = FT_Open(index, &ftHandle);
		if (status != FT_OK)
		{
			std::cerr << "Error in FT_Open" << std::endl;
			return false;
		}

		status = FT_SetBaudRate(ftHandle, 250000);
		if (status != FT_OK)
		{
			std::cerr << "Error in FT_SetBaudRate" << std::endl;
			return false;
		}

		status = FT_SetDataCharacteristics(ftHandle, FT_BITS_8, FT_STOP_BITS_2, FT_PARITY_NONE);
		if (status != FT_OK)
		{
			std::cerr << "Error in FT_SetDataCharacteristics" << std::endl;
			return false;
		}

		status = FT_SetFlowControl(ftHandle, 0, 0, 0);
		if (status != FT_OK)
		{
			std::cerr << "Error in FT_SetFlowControl" << std::endl;
			return false;
		}

		//std::cout << "open serial successful" << std::endl;

		return true;
	}

	void close()
	{
		if (!ftHandle) return;
		FT_Close(ftHandle);
		ftHandle = nullptr;

		//std::cout << "close serial successful" << std::endl;
	}

	void start()
	{
		assert(ftHandle);

		sender_thread = std::thread([this]() {
			float inv_fps = 1.0 / 30;
			auto last_timestamp = std::chrono::steady_clock::now();

			this->running = true;

			while (running)
			{
				auto now = std::chrono::steady_clock::now();
				auto d = now - last_timestamp;

				if (d > std::chrono::milliseconds(int(inv_fps * 1000)))
				{
					last_timestamp = now;

					this->sendBrake(true);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));

					this->sendBrake(false);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));

					{
						std::lock_guard<std::mutex> lock(this->mtx);
						this->sendUniverse();
					}
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});
	}

	void stop()
	{
		running = false;
		sender_thread.join();
	}

	bool sendBrake(bool yn)
	{
		assert(ftHandle);

		FT_STATUS status;
		if (yn == true)
			status = FT_SetBreakOn(ftHandle);
		else
			status = FT_SetBreakOff(ftHandle);

		if (status != FT_OK)
		{
			std::cerr << "Error in FT_SetBreakOff" << std::endl;
			return false;
		}

		return true;
	}

	bool sendUniverse()
	{
		assert(ftHandle);

		DWORD written = 0;
		FT_STATUS status = FT_Write(ftHandle, (char*)universe.data(), universe.size(), &written);

		if (status != FT_OK)
		{
			std::cerr << "Error in FT_Write" << std::endl;
			return false;
		}
		else
		{
			return true;
		}
	}
	
	void setupParameters(OP_ParameterManager* manager, void* reserved1) override
	{
		{
			OP_StringParameter sp;
			sp.name = "Serial";
			sp.label = "Serial";
			sp.page = "OpenDMX";
			manager->appendString(sp);
		}

		{
			OP_NumericParameter np;
			np.name = "Active";
			np.label = "Active";
			np.page = "OpenDMX";
			manager->appendToggle(np);
		}
	}

	void getGeneralInfo(CHOP_GeneralInfo* info, const OP_Inputs *inputs, void* reserved1) override
	{}

	bool getOutputInfo(CHOP_OutputInfo* info, const OP_Inputs *inputs, void *reserved1) override
	{
		return false;
	}

	void getChannelName(int32_t index, OP_String *name,
		const OP_Inputs *inputs, void* reserved1) override
	{}

	void execute(CHOP_Output* output, const OP_Inputs* inputs, void* reserved1) override
	{
		{
			bool v = inputs->getParInt("Active");
			if (active != v)
			{
				if (v)
				{
					// Off to On
					auto s = inputs->getParString("Serial");
					if (s)
					{
						if (open(s))
						{
							start();
						}
					}
				}
				else
				{
					// On to Off
					stop();
					close();
				}
				active = v;
			}
		}

		if (inputs->getNumInputs() == 0)
			return;

		std::array<uint8_t, 513> arr;
		std::fill(arr.begin(), arr.end(), 0);

		const OP_CHOPInput* input = inputs->getInputCHOP(0);
		const int N = input->numSamples - 1;

		for (int i = 0; i < input->numChannels; i++)
		{
			const float* d = input->getChannelData(i);
			auto v = static_cast<uint8_t>(std::max(std::min(d[N], 255.0f), 0.0f));
			arr[i + 1] = v;
		}

		{
			std::lock_guard<std::mutex> lock(this->mtx);
			this->universe = arr;
		}
	}

	//////////////////////////////////////////////////////////////////////////

	bool getInfoDATSize(OP_InfoDATSize* infoSize, void *reserved1) override
	{
		infoSize->cols = 1;
		infoSize->rows = serials.size() + 1;
		return true;
	}

	void getInfoDATEntries(int32_t index, int32_t nEntries,
		OP_InfoDATEntries* entries,
		void *reserved1) override
	{
		entries->values[0]->setString("Serials");
		for (int i = 0; i < serials.size(); i++)
		{
			entries->values[i + 1]->setString(serials[i].c_str());
		}
	}
};

////

extern "C"
{
	DLLEXPORT void FillCHOPPluginInfo(CHOP_PluginInfo *info)
	{
		// Always set this to CHOPCPlusPlusAPIVersion.
		info->apiVersion = CHOPCPlusPlusAPIVersion;

		// The opType is the unique name for this CHOP. It must start with a 
		// capital A-Z character, and all the following characters must lower case
		// or numbers (a-z, 0-9)
		info->customOPInfo.opType->setString("Opendmxout");

		// The opLabel is the text that will show up in the OP Create Dialog
		info->customOPInfo.opLabel->setString("OpenDMX Out");

		// Information about the author of this OP
		info->customOPInfo.authorName->setString("Satoru Higa");
		info->customOPInfo.authorEmail->setString("satoruhiga@gmail.com");

		// This CHOP can work with 0 inputs
		info->customOPInfo.minInputs = 1;

		// It can accept up to 1 input though, which changes it's behavior
		info->customOPInfo.maxInputs = 1;
	}

	DLLEXPORT CHOP_CPlusPlusBase* CreateCHOPInstance(const OP_NodeInfo* info)
	{
		return new OPERATOR_NAME(info);
	}

	DLLEXPORT void DestroyCHOPInstance(CHOP_CPlusPlusBase* instance)
	{
		delete (OPERATOR_NAME*)instance;
	}
};

#endif
