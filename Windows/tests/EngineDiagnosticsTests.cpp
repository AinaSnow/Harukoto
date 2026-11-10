#include "../EngineDiagnostics.h"
#include <cassert>
#include <iostream>
#include <atomic>
#include <thread>

using namespace harukoto::engine;
namespace dg = harukoto::engine::diagnostics;
static dg::Record record() {
    dg::Record r;
    r.root=r.publisher=r.host=dg::identity(GetCurrentProcess());
    r.published=r.polled=GetTickCount64(); r.state=1; r.configured=2;
    return r;
}
static void packets() {
    auto r=record(); auto p=dg::encode(r); dg::Record out;
    assert(dg::decode(p,out) && out.root.pid==r.root.pid && out.configured==2);
    for (size_t offset : {size_t(0),size_t(4),size_t(6),size_t(44),size_t(64),size_t(68),size_t(76),size_t(84),size_t(96),size_t(108),size_t(120),size_t(160),size_t(208),size_t(255)}) {
        auto bad=p; bad[offset]=255;
        dg::Record unchanged; unchanged.claimed=987;
        assert(!dg::decode(bad,unchanged) && unchanged.claimed==987);
    }
    r.owner=true; r.controlState=1; r.admissionState=2;
    r.registrationCreated=24; r.registrationIssued=23; r.registrationActive=8;
    r.registrationBound=8; r.registrationRevoked=16;
    r.capacity={r.published,17,2,0,1,1,2};
    assert(dg::decode(dg::encode(r),out) && out.capacity.generation==17 && out.registrationCreated==24);
    r.state=4; r.exitCode=77;
    assert(dg::decode(dg::encode(r),out) && out.exitCode==DWORD(77));
    r.capacity.jobs=257; assert(!dg::valid(r)); r.capacity.jobs=0;
    r.polled=r.published+1; assert(!dg::valid(r)); r.polled=0;
    r.admissionState=0; r.admissionFailure=5; assert(!dg::valid(r));
}
static void mapping() {
    auto r=record(); const auto objectName=dg::name(r.root);
    assert(dg::read(GetCurrentProcess()).availability==dg::Availability::Unavailable);
    {
        dg::Writer writer(r.root);
        assert(dg::read(GetCurrentProcess()).availability==dg::Availability::Inconsistent);
        writer.publish(r);
        auto o=dg::read(GetCurrentProcess()); assert(o.availability==dg::Availability::Observed && o.record->state==1);
        pipe::Handle forbidden(OpenFileMappingW(FILE_MAP_WRITE,FALSE,objectName.c_str()));
        assert(!forbidden && GetLastError()==ERROR_ACCESS_DENIED);
        pipe::Handle reader(OpenFileMappingW(FILE_MAP_READ,FALSE,objectName.c_str())); assert(reader);
        DWORD flags=0; pipe::check(GetHandleInformation(reader.get(),&flags)); assert(!(flags&HANDLE_FLAG_INHERIT));
        assert(!MapViewOfFile(reader.get(),FILE_MAP_WRITE,0,0,dg::MappingSize) && GetLastError()==ERROR_ACCESS_DENIED);
        // Copying metadata cannot grant a client endpoint/control/stop handle.
        bool rejected=false;
        try { dg::Writer collision(r.root); } catch (const pipe::Error &) { rejected=true; }
        assert(rejected && dg::read(GetCurrentProcess()).record->state==1);
        r.published=GetTickCount64(); r.polled=0; writer.publish(r);
        o=dg::read(GetCurrentProcess()); assert(!o.record->polled);
    }
    assert(dg::read(GetCurrentProcess()).availability==dg::Availability::Unavailable);
    // An accessible precreated object still cannot be adopted by the writer.
    pipe::Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,dg::MappingSize,objectName.c_str())); assert(mapping);
    bool rejected=false;
    try { dg::Writer writer(r.root); } catch (const pipe::Error &error) { rejected=error.code==ERROR_ALREADY_EXISTS; }
    assert(rejected);
    auto *view=static_cast<dg::Shared *>(MapViewOfFile(mapping.get(),FILE_MAP_WRITE,0,0,dg::MappingSize)); assert(view);
    auto publish=[&](dg::Record value) {
        const auto bytes=dg::encode(value); view->sequence=1;
        std::copy(bytes.begin(),bytes.end(),view->packet); view->sequence=2;
    };
    publish(r); view->reserved=1;
    assert(dg::read(GetCurrentProcess()).availability==dg::Availability::Invalid); view->reserved=0;
    auto wrong=r; ++wrong.root.created; publish(wrong);
    assert(dg::read(GetCurrentProcess()).availability==dg::Availability::Invalid);
    wrong=r; ++wrong.publisher.created; publish(wrong);
    assert(dg::read(GetCurrentProcess()).availability==dg::Availability::PublisherUnavailable);
    wrong=r; wrong.published=GetTickCount64()+100000; publish(wrong);
    assert(dg::read(GetCurrentProcess()).availability==dg::Availability::Invalid);
    publish(r); view->sequence=3;
    assert(dg::read(GetCurrentProcess()).availability==dg::Availability::Inconsistent);
    UnmapViewOfFile(view);
}
// Separate-process producer for the real CLI tests. Its stdin never enters the
// mapping. Holding this pipe idle deliberately freezes the observation age.
static int serve() {
    auto r=record(); dg::Writer writer(r.root); writer.publish(r);
    std::cout << r.root.pid << std::endl;
    std::string command;
    while (std::getline(std::cin,command)) {
        if (command=="exit") return 0;
        if (command=="burst") {
            std::atomic<bool> stop{false};
            pipe::Handle ready(CreateEventW(nullptr,TRUE,FALSE,nullptr)); pipe::check(bool(ready));
            std::thread updates([&] {
                auto current=r;
                current.state=1; current.claimed=345; writer.publish(current);
                pipe::check(SetEvent(ready.get()));
                while (!stop.load()) {
                    current.state=current.state==1 ? 2 : 1;
                    current.claimed=current.state==1 ? 345 : 987;
                    current.published=current.polled=GetTickCount64(); writer.publish(current);
                }
            });
            assert(WaitForSingleObject(ready.get(),10000)==WAIT_OBJECT_0);
            std::cout << "bursting" << std::endl;
            assert(std::getline(std::cin,command) && command=="stop-burst");
            stop.store(true); updates.join();
            writer.publish(r); std::cout << "stopped" << std::endl; continue;
        }
        assert(command=="update");
        r.published=r.polled=GetTickCount64(); r.state=2; writer.publish(r);
        std::cout << "updated" << std::endl;
    }
    return 0;
}
int wmain(int argc, wchar_t **argv) {
    if (argc==2 && std::wstring(argv[1])==L"--serve") return serve();
    packets(); mapping(); std::cout << "engine_diagnostics: bounded packets, read-only ACL, collision and stale identity passed\n";
}
