#include "../EngineInstance.h"
#include <aclapi.h>
#include <cassert>
#include <iostream>

using namespace harukoto::engine;
static std::wstring scope() { const auto name = pipe::newName(); return L"test-"+name.substr(name.size()-32); }
static void busy(const std::wstring &key) {
    bool rejected = false;
    try { EngineInstance contender(key); } catch (const EngineInstance::Busy &) { rejected = true; }
    assert(rejected);
}
static pipe::Handle event() {
    pipe::Handle result(CreateEventW(nullptr,TRUE,FALSE,nullptr)); pipe::check(bool(result)); return result;
}
// This child deliberately escrows one inherited lease exactly as the host does.
static int child(int argc, wchar_t **argv) {
    assert(argc == 5);
    std::array<pipe::Handle,3> handles;
    for (size_t i=0;i<handles.size();++i) {
        handles[i].reset(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(argv[i+2]))));
        DWORD flags = 0; pipe::check(GetHandleInformation(handles[i].get(),&flags)); assert(flags & HANDLE_FLAG_INHERIT);
        pipe::check(SetHandleInformation(handles[i].get(),HANDLE_FLAG_INHERIT,0));
    }
    (void)handles[0].release();
    pipe::check(SetEvent(handles[1].get()));
    assert(WaitForSingleObject(handles[2].get(),15000) == WAIT_OBJECT_0); return 0;
}
static void inheritedLifetime(bool crash) {
    const auto key = scope(); auto ready = event(), finish = event();
    pipe::Handle process, job(CreateJobObjectW(nullptr,nullptr)); pipe::check(bool(job));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    pipe::check(SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)));
    {
        EngineInstance owner(key);
        std::array<pipe::Handle,3> copies; const HANDLE sources[]{owner.handle(),ready.get(),finish.get()};
        std::vector<HANDLE> inherited;
        for (size_t i=0;i<copies.size();++i) {
            HANDLE raw = nullptr; pipe::check(DuplicateHandle(GetCurrentProcess(),sources[i],GetCurrentProcess(),&raw,0,TRUE,DUPLICATE_SAME_ACCESS));
            copies[i].reset(raw); inherited.push_back(raw);
        }
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr,1,0,&size); std::vector<uint8_t> bytes(size);
        auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        pipe::check(InitializeProcThreadAttributeList(attributes,1,0,&size));
        struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Delete() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
        pipe::check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited.data(),inherited.size()*sizeof(HANDLE),nullptr,nullptr));
        std::wstring path(32768,L'\0'); const auto count = GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
        assert(count && count < path.size()); path.resize(count);
        std::wstring command = L"\""+path+L"\" --child";
        for (auto handle : inherited) command += L" "+std::to_wstring(reinterpret_cast<uintptr_t>(handle));
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        PROCESS_INFORMATION info{};
        pipe::check(CreateProcessW(path.c_str(),command.data(),nullptr,nullptr,TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,nullptr,nullptr,&startup.StartupInfo,&info));
        process.reset(info.hProcess); pipe::Handle thread(info.hThread);
        try {
            pipe::check(AssignProcessToJobObject(job.get(),process.get()));
            for (auto &copy : copies) copy.reset();
            assert(ResumeThread(thread.get()) != DWORD(-1));
        } catch (...) { TerminateProcess(process.get(),99); throw; }
        const HANDLE events[]{process.get(),ready.get()};
        assert(WaitForMultipleObjects(2,events,FALSE,15000) == WAIT_OBJECT_0+1);
    } // The launcher/owner copy is gone; the live child must still block reuse.
    busy(key);
    if (crash) pipe::check(TerminateProcess(process.get(),77));
    else pipe::check(SetEvent(finish.get()));
    assert(WaitForSingleObject(process.get(),15000) == WAIT_OBJECT_0);
    DWORD code = 1; pipe::check(GetExitCodeProcess(process.get(),&code)); assert(code == (crash ? 77u : 0u));
    EngineInstance successor(key); // Retained exited process object is not a lease.
}
int wmain(int argc, wchar_t **argv) {
    if (argc > 1) return child(argc,argv);
    for (const auto &key : {std::wstring(),std::wstring(L"UPPER"),std::wstring(L"../bad"),std::wstring(L"a b"),std::wstring(65,L'a'),std::wstring(L"a\0b",3)})
    {
        assert(!EngineInstance::validScope(key)); bool rejected = false;
        try { EngineInstance invalid(key); } catch (const std::invalid_argument &) { rejected = true; }
        assert(rejected);
    }
    assert(EngineInstance::validScope(std::wstring(64,L'a')));
    const auto key = scope();
    {
        EngineInstance first(key); busy(key); busy(key);
        EngineInstance other(scope());
        DWORD flags = 0; pipe::check(GetHandleInformation(first.handle(),&flags)); assert(!(flags & HANDLE_FLAG_INHERIT));
        PSECURITY_DESCRIPTOR descriptor = nullptr; PACL acl = nullptr;
        assert(GetSecurityInfo(first.handle(),SE_KERNEL_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&acl,nullptr,&descriptor) == ERROR_SUCCESS);
        SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
        pipe::check(GetSecurityDescriptorControl(descriptor,&control,&revision));
        assert(acl && acl->AceCount == 1 && (control & SE_DACL_PROTECTED));
        void *raw = nullptr; pipe::check(GetAce(acl,0,&raw)); const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(raw);
        const auto identity = pipe::LogonIdentity::fromProcess(GetCurrentProcess());
        assert(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Mask == (SYNCHRONIZE | READ_CONTROL));
        assert(EqualSid(const_cast<DWORD *>(&ace->SidStart),const_cast<uint8_t *>(identity.logon.data()))); LocalFree(descriptor);
    }
    { EngineInstance next(key); }
    {
        const auto collision = scope(); pipe::Handle eventCollision(CreateEventW(nullptr,TRUE,FALSE,EngineInstance::name(collision).c_str()));
        pipe::check(bool(eventCollision)); bool rejected = false;
        try { EngineInstance attempt(collision); } catch (const pipe::Error &error) { rejected = error.code == ERROR_INVALID_HANDLE; }
        assert(rejected);
    }
    {
        const auto denied = scope(); ACL acl{}; pipe::check(InitializeAcl(&acl,sizeof(acl),ACL_REVISION));
        SECURITY_DESCRIPTOR descriptor{}; pipe::check(InitializeSecurityDescriptor(&descriptor,SECURITY_DESCRIPTOR_REVISION));
        pipe::check(SetSecurityDescriptorDacl(&descriptor,TRUE,&acl,FALSE));
        SECURITY_ATTRIBUTES attributes{sizeof(attributes),&descriptor,FALSE};
        pipe::Handle blocked(CreateMutexExW(&attributes,EngineInstance::name(denied).c_str(),0,0)); pipe::check(bool(blocked));
        bool rejected = false;
        try { EngineInstance attempt(denied); } catch (const pipe::Error &error) { rejected = error.code == ERROR_ACCESS_DENIED; }
        assert(rejected);
    }
    inheritedLifetime(false); inheritedLifetime(true);
    std::cout << "engine_instance: scope, protected logon ACL, collision, inherited lifetime and reclamation passed\n";
}
