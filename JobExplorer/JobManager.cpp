#include "stdafx.h"
#include "JobManager.h"
#include "NtDll.h"
#include <algorithm>

int JobManager::JobTypeIndex;

JobManager::JobManager() {
	if (JobTypeIndex == 0) {
		EnableDebugPrivilege();	// if possible
		JobTypeIndex = GetJobObjectType();
	}
}

bool JobManager::EnumJobObjects() {
	ULONG len = 1 << 22;
	std::unique_ptr<BYTE[]> buffer;
	for (;;) {
		buffer = std::make_unique<BYTE[]>(len);
		auto status = ::NtQuerySystemInformation(SystemExtendedHandleInformation, buffer.get(), len, &len);
		if (status == 0)
			break;
		if (status != 0xc0000004)
			return false;
		len += 1 << 17;
	}
	auto p = (SYSTEM_HANDLE_INFORMATION_EX*)buffer.get();
	
	_jobObjects.clear();
	_jobMap.clear();
	_jobObjects.reserve(128);
	_jobMap.reserve(128);

	BYTE nameBuffer[1024];
	for (ULONG i = 0; i < p->NumberOfHandles; i++) {
		auto& hi = p->Handles[i];
		if (hi.ObjectTypeIndex == JobTypeIndex) {
			// job object handle

			if (_jobMap.find(hi.Object) != _jobMap.end())
				continue;

			auto entry = std::make_shared<JobObjectEntry>();
			entry->Handle = reinterpret_cast<HANDLE>(hi.HandleValue);
			entry->ProcessId = (DWORD)hi.UniqueProcessId;
			entry->Object = hi.Object;
			auto hDup = DuplicateJobHandle(entry->Handle, entry->ProcessId);
			if (hDup) {
				int count = 1024;
				DWORD size = sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + (count - 1) * sizeof(ULONG_PTR);
				auto buffer = std::make_unique<BYTE[]>(size);
				if (::QueryInformationJobObject(hDup.get(), JobObjectBasicProcessIdList, buffer.get(), size, nullptr)) {
					auto list = (JOBOBJECT_BASIC_PROCESS_ID_LIST*)buffer.get();
					entry->Processes.resize(list->NumberOfProcessIdsInList);
					::memcpy(entry->Processes.data(), list->ProcessIdList, list->NumberOfProcessIdsInList * sizeof(ULONG_PTR));
					for (auto pid : entry->Processes) {
						_processesInJobs[(DWORD)pid].push_back(entry);
					}
				}
				JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info;
				if (::QueryInformationJobObject(hDup.get(), JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr)) {
					entry->TotalProcesses = info.TotalProcesses;
				}
				ULONG len;
				if (STATUS_SUCCESS == ::NtQueryObject(hDup.get(), ObjectNameInformation, nameBuffer, sizeof(nameBuffer), &len)) {
					auto info = (OBJECT_NAME_INFORMATION*)nameBuffer;
					if(info->Name.Buffer)
						entry->Name = std::wstring(info->Name.Buffer, info->Name.Length / sizeof(WCHAR));
				}
			}
			_jobMap.insert({ entry->Object, entry });
			_jobObjects.push_back(entry);
		}
	}

	AnalyzeJobs();

	return false;
}

const std::vector<std::shared_ptr<JobObjectEntry>>& JobManager::GetJobObjects() const {
	return _jobObjects;
}

wil::unique_handle JobManager::DuplicateJobHandle(HANDLE h, DWORD pid) {
	wil::unique_handle hProcess(::OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid));
	if (!hProcess)
		return nullptr;

	wil::unique_handle hDup;
	::DuplicateHandle(hProcess.get(), h, ::GetCurrentProcess(), hDup.addressof(), JOB_OBJECT_QUERY, FALSE, 0);
	return hDup;
}

int JobManager::GetJobObjectType() {
	ULONG len = 1 << 17;
	auto buffer = std::make_unique<BYTE[]>(len);
	::NtQueryObject(nullptr, ObjectTypesInformation, buffer.get(), len, nullptr);
	auto types = (OBJECT_TYPES_INFORMATION*)buffer.get();
	auto type = (OBJECT_TYPE_INFORMATION*)(buffer.get() + sizeof(void*));
	
	for (ULONG i = 0; i < types->NumberOfTypes; i++) {
		if (::wcscmp(type->TypeName.Buffer, L"Job") == 0)
			return (int)type->TypeIndex;
		auto step = sizeof(OBJECT_TYPE_INFORMATION) + type->TypeName.MaximumLength;
		step = (step + sizeof(void*) - 1) / sizeof(void*) * sizeof(void*);
		type = (OBJECT_TYPE_INFORMATION*)((BYTE*)type + step);
	}
	return 0;
}

void JobManager::AnalyzeJobs() {
	// find parent-child relationships

	for (auto& [pid, jobList] : _processesInJobs) {
		if (jobList.size() == 1)
			continue;

		ATLASSERT(jobList.size() > 1);

		std::sort(jobList.begin(), jobList.end(), [](const auto& j1, const auto& j2) {
			return j1->Processes.size() > j2->Processes.size();
			});

		for (size_t i = 1; i < jobList.size(); i++) {
			jobList[i]->ParentJob = jobList[i - 1].get();
			jobList[i - 1]->ChildJobs.insert(jobList[i].get());
		}
	}
}

bool JobManager::EnableDebugPrivilege() {
	wil::unique_handle hToken;
	if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, hToken.addressof()))
		return false;

	TOKEN_PRIVILEGES tp;
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	if (!::LookupPrivilegeValue(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid))
		return false;

	if (!::AdjustTokenPrivileges(hToken.get(), FALSE, &tp, sizeof(tp), nullptr, nullptr))
		return false;

	return ::GetLastError() == ERROR_SUCCESS;
}
