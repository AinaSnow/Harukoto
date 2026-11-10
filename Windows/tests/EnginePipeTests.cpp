#include "../EnginePipe.h"
#include <aclapi.h>
#include <cassert>
#include <iostream>
#include <functional>

using namespace harukoto::engine;
namespace ep = harukoto::engine::pipe;
using ep::Handle;

static constexpr uint64_t Timeout = 5000;
static Frame hello() { return {Kind::Hello,{1,0,0,0,1},{}}; }
static Frame data(size_t size = 0) {
    Frame frame{Kind::Key,{1,42,1,1,2},{}};
    for (size_t i = 0; i < size; ++i) frame.payload.push_back(static_cast<uint8_t>(i * 37));
    return frame;
}
static void waitEvents(const std::vector<HANDLE> &events, uint64_t deadline) {
    const auto time = ep::now(); assert(time < deadline);
    if (events.empty()) return;
    const auto status = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE,
        static_cast<DWORD>(deadline - time));
    assert(status >= WAIT_OBJECT_0 && status < WAIT_OBJECT_0 + events.size());
}
static void drive(const std::function<bool()> &done, std::initializer_list<ep::Channel *> channels) {
    const auto deadline = ep::now() + Timeout;
    for (;;) {
        for (auto *channel : channels) channel->poll(ep::now());
        if (done()) return;
        std::vector<HANDLE> events;
        for (auto *channel : channels)
            for (auto handle : {channel->readEvent(),channel->writeEvent()}) if (handle) events.push_back(handle);
        waitEvents(events,deadline);
    }
}
static std::unique_ptr<ep::Channel> accept(ep::Listener &listener) {
    const auto deadline = ep::now() + Timeout;
    for (;;) {
        listener.poll(ep::now());
        if (auto channel = listener.take()) return channel;
        assert(!listener.closed());
        waitEvents({listener.event()},deadline);
    }
}
struct Pair {
    std::unique_ptr<ep::Channel> client, server;
    Pair() {
        const auto name = ep::newName(); const auto time = ep::now();
        ep::Listener listener(name,time,time + Timeout);
        client = ep::Channel::connect(name,GetCurrentProcess()); server = accept(listener);
        assert(client->authenticated() && !server->authenticated());
    }
    Frame transfer(const Frame &frame, bool toServer = true) {
        auto &from = toServer ? client : server, &to = toServer ? server : client;
        const auto time = ep::now();
        assert(to->beginRead(time,time + Timeout));
        assert(from->beginWrite(frame,time,time + Timeout));
        std::optional<Frame> received;
        drive([&] { if (!received) received = to->take(); return received && from->writeDone(); },{from.get(),to.get()});
        assert(received->id == frame.id && received->kind == frame.kind && received->payload == frame.payload);
        return *received;
    }
    void handshake() {
        transfer(hello()); assert(server->authenticated());
        auto ack = hello(); ack.kind = Kind::HelloAck; ack.id.engine = 42; transfer(ack,false);
    }
};
struct RawPair {
    Handle client;
    std::unique_ptr<ep::Channel> server;
    explicit RawPair(DWORD level = SECURITY_IDENTIFICATION) {
        const auto name = ep::newName(); const auto time = ep::now();
        ep::Listener listener(name,time,time + Timeout);
        client.reset(CreateFileW(name.c_str(),ep::ClientAccess,0,nullptr,OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | level,nullptr));
        ep::check(bool(client)); server = accept(listener);
    }
    void write(const uint8_t *bytes, size_t size) {
        DWORD transferred = 0;
        ep::check(WriteFile(client.get(),bytes,static_cast<DWORD>(size),&transferred,nullptr));
        assert(transferred == size);
    }
    void write(const std::vector<uint8_t> &bytes) { write(bytes.data(),bytes.size()); }
    void handshake() {
        const auto time = ep::now(); assert(server->beginRead(time,time + Timeout)); write(encode(hello()));
        std::optional<Frame> received;
        drive([&] { received = server->take(); return bool(received); },{server.get()});
        assert(server->authenticated());
    }
};
static void framingAndCapacity() {
    Pair pair;
    assert(!pair.server->beginWrite(hello(),ep::now(),ep::now() + Timeout));
    pair.handshake();
    pair.transfer(data(MaxPayload));
    auto response = data(MaxPayload); response.kind = Kind::Reply; pair.transfer(response,false);
    const auto time = ep::now();
    assert(pair.server->beginRead(time,time + Timeout));
    assert(!pair.server->beginRead(time,time + Timeout));
    assert(pair.client->beginWrite(data(64),time,time + Timeout));
    assert(!pair.client->beginWrite(data(),time,time + Timeout));
    assert(!pair.server->drain()); // A live drain must not discard a read completion.
    // The write completes once the bytes enter the kernel. Pump the server, too.
    drive([&] { return pair.client->writeDone(); },{pair.client.get(),pair.server.get()});
    const auto deadline = ep::now() + Timeout;
    while (pair.server->readEvent()) {
        waitEvents({pair.server->readEvent()},deadline); pair.server->poll(ep::now());
    }
    assert(!pair.server->beginRead(time,time + Timeout)); // Unclaimed frame still occupies the slot.
    auto result = pair.server->take(); assert(result && result->payload == data(64).payload);
    assert(!pair.server->take());
    auto oversize = data(MaxPayload + 1); bool rejected = false;
    try { pair.client->beginWrite(oversize,time,time + Timeout); } catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected && !pair.client->closed());
    pair.server->close(ep::Failure::None); assert(pair.server->closed());
}
static void fragmentsAndCoalescing() {
    RawPair pair; pair.handshake();
    auto frame = data(300); auto bytes = encode(frame);
    const auto time = ep::now(); assert(pair.server->beginRead(time,time + Timeout));
    size_t offset = 0;
    for (size_t size : {size_t(1),size_t(7),size_t(47),size_t(1),size_t(13),size_t(287)}) {
        pair.write(bytes.data() + offset,size); offset += size; pair.server->poll(ep::now());
        if (offset < bytes.size()) assert(!pair.server->take());
    }
    assert(offset == bytes.size());
    std::optional<Frame> received;
    drive([&] { received = pair.server->take(); return bool(received); },{pair.server.get()});
    assert(received->payload == frame.payload);
    auto next = data(17); next.id.request++;
    bytes = encode(frame); const auto second = encode(next); bytes.insert(bytes.end(),second.begin(),second.end());
    pair.write(bytes);
    for (const auto &expected : {frame,next}) {
        const auto current = ep::now(); assert(pair.server->beginRead(current,current + Timeout));
        drive([&] { received = pair.server->take(); return bool(received); },{pair.server.get()});
        assert(received->id == expected.id && received->payload == expected.payload);
    }
}
static void invalidFrames() {
    for (int fault = 0; fault < 7; ++fault) {
        RawPair pair;
        auto bytes = encode(hello());
        switch (fault) {
        case 0: bytes[0] = 'X'; break;
        case 1: bytes[4]++; break;
        case 2: bytes[8] = 1; bytes[10] = 1; break; // 65,537, header only: reject before body allocation.
        case 3: bytes[12] = 1; break;
        case 4: bytes = encode(data()); break; // First frame must be Hello.
        case 5: { auto frame = hello(); frame.payload = {1}; bytes = encode(frame); break; }
        case 6: bytes[6] = 99; break;
        }
        const auto time = ep::now(); assert(pair.server->beginRead(time,time + Timeout)); pair.write(bytes);
        drive([&] { return pair.server->closed(); },{pair.server.get()});
        assert(pair.server->failure() == ep::Failure::Protocol && !pair.server->authenticated() && !pair.server->take());
    }
}
static void anonymousClient() {
    RawPair pair(SECURITY_ANONYMOUS);
    const auto time = ep::now(); assert(pair.server->beginRead(time,time + Timeout)); pair.write(encode(hello()));
    drive([&] { return pair.server->closed(); },{pair.server.get()});
    assert(pair.server->failure() == ep::Failure::Authentication && !pair.server->authenticated() && !pair.server->take());
    // Authentication failed inside impersonation; the owner's thread must still be restored.
    ep::processIdentityOnly();
}
static void deadlinesAndDisconnect() {
    { const auto time = ep::now(); ep::Listener listener(ep::newName(),time,time + Timeout);
      listener.poll(time + Timeout); assert(listener.closed() && !listener.take()); }
    for (bool partial : {false,true}) {
        RawPair pair;
        const auto time = ep::now(); assert(pair.server->beginRead(time,time + 100)); pair.server->poll(time);
        assert(pair.server->readEvent());
        if (partial) { const auto bytes = encode(hello()); pair.write(bytes.data(),13); pair.server->poll(time + 99); }
        pair.server->poll(time + 100);
        assert(pair.server->closed() && pair.server->failure() == ep::Failure::Deadline && !pair.server->take());
        drive([&] { return pair.server->drain(); },{pair.server.get()}); assert(!pair.server->nativeHandle());
    }
    { Pair pair; pair.handshake(); const auto time = ep::now();
      assert(pair.server->beginRead(time,time + Timeout)); pair.server->poll(time); pair.client.reset();
      drive([&] { return pair.server->closed(); },{pair.server.get()});
      assert(pair.server->failure() == ep::Failure::Io && !pair.server->take()); }
    { RawPair pair; pair.handshake(); const auto time = ep::now();
      assert(pair.server->beginRead(time,time + Timeout)); pair.write(encode(data())); pair.server->poll(time);
      pair.server->close(); assert(!pair.server->take()); } // Never deliver an unclaimed result after close.
    { Pair pair; const auto time = ep::now(); assert(!pair.client->beginWrite(hello(),time,time));
      assert(pair.client->failure() == ep::Failure::Deadline); }
    { Pair pair; const auto time = ep::now(); assert(!pair.client->beginRead(time,time));
      assert(pair.client->failure() == ep::Failure::Deadline); }
    // Fill the kernel output buffer without a reader, then cancel an actual pending write.
    { Pair pair; const auto time = ep::now(); auto frame = data(MaxPayload);
      assert(pair.client->beginRead(time,time + Timeout));
      for (int i = 0; i < 8; ++i) {
          assert(pair.client->beginWrite(frame,time,time + Timeout)); pair.client->poll(time);
          if (pair.client->writeEvent()) break;
          assert(pair.client->writeDone());
      }
      assert(pair.client->writeEvent() && pair.client->readEvent()); pair.client->poll(time + Timeout);
      assert(pair.client->failure() == ep::Failure::Deadline);
      drive([&] { return pair.client->drain(); },{pair.client.get()}); }
    // Destructor owns cancellation and completion while OVERLAPPED and buffer are still alive.
    for (int i = 0; i < 20; ++i) {
        Pair pair; const auto time = ep::now();
        assert(pair.server->beginRead(time,time + Timeout)); pair.server->poll(time); assert(pair.server->readEvent());
    }
}
static void namesAndAcl() {
    const auto name = ep::newName(), other = ep::newName(); assert(name != other && ep::validName(name));
    for (const auto &invalid : {std::wstring{},L"\\\\remote\\pipe\\Harukoto.Engine.v1." + std::wstring(64,L'a'),name + L"x"}) {
        assert(!ep::validName(invalid)); bool rejected = false;
        try { ep::Channel::connect(invalid,GetCurrentProcess()); } catch (const ep::Error &e) { rejected = e.reason == ep::Failure::Authentication; }
        assert(rejected);
    }
    const auto time = ep::now(); ep::Listener listener(name,time,time + Timeout);
    bool collision = false;
    try { ep::Listener duplicate(name,time,time + Timeout); } catch (const ep::Error &) { collision = true; }
    assert(collision);
    BOOL inherit = FALSE; DWORD flags = 0;
    ep::check(GetHandleInformation(listener.nativeHandle(),&flags)); inherit = (flags & HANDLE_FLAG_INHERIT) != 0; assert(!inherit);
    PSECURITY_DESCRIPTOR descriptor = nullptr; PACL acl = nullptr;
    assert(GetSecurityInfo(listener.nativeHandle(),SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION,
        nullptr,nullptr,&acl,nullptr,&descriptor) == ERROR_SUCCESS);
    struct Free { PSECURITY_DESCRIPTOR p; ~Free() { LocalFree(p); } } free{descriptor};
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
    ep::check(GetSecurityDescriptorControl(descriptor,&control,&revision)); assert(control & SE_DACL_PROTECTED);
    assert(acl && acl->AceCount == 1);
    void *rawAce = nullptr; ep::check(GetAce(acl,0,&rawAce));
    auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(rawAce);
    auto identity = ep::LogonIdentity::fromProcess(GetCurrentProcess());
    assert(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Mask == ep::ClientAccess);
    assert(EqualSid(&ace->SidStart,identity.logon.data()));
    assert(!(ace->Mask & FILE_CREATE_PIPE_INSTANCE));
    HANDLE rawToken = nullptr, rawImpersonation = nullptr;
    ep::check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY | TOKEN_DUPLICATE,&rawToken)); Handle token(rawToken);
    ep::check(DuplicateToken(token.get(),SecurityIdentification,&rawImpersonation)); Handle impersonation(rawImpersonation);
    GENERIC_MAPPING mapping{FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};
    for (DWORD requested : {ep::ClientAccess,DWORD(FILE_CREATE_PIPE_INSTANCE)}) {
        std::vector<uint8_t> privileges(4096); DWORD length = static_cast<DWORD>(privileges.size()), granted = 0; BOOL allowed = FALSE;
        ep::check(AccessCheck(descriptor,impersonation.get(),requested,&mapping,
            reinterpret_cast<PRIVILEGE_SET *>(privileges.data()),&length,&granted,&allowed));
        assert(bool(allowed) == (requested == ep::ClientAccess));
    }
    // Each component participates in identity matching; no account/session is created by this test.
    auto changed = identity; changed.user[0] ^= 1; assert(!(changed == identity));
    changed = identity; changed.logon[0] ^= 1; assert(!(changed == identity));
    changed = identity; changed.session++; assert(!(changed == identity));
    changed = identity; changed.authentication.LowPart++; assert(!(changed == identity));
    changed = identity; changed.authentication.HighPart++; assert(!(changed == identity));
    ep::check(ImpersonateSelf(SecurityImpersonation));
    struct Revert { ~Revert() { ep::check(RevertToSelf()); } } revert;
    bool rejected = false;
    try { ep::Channel::connect(name,GetCurrentProcess()); } catch (const ep::Error &e) { rejected = e.reason == ep::Failure::Authentication; }
    assert(rejected);
    rejected = false;
    try { ep::Listener nested(other,time,time + Timeout); } catch (const ep::Error &e) { rejected = e.reason == ep::Failure::Authentication; }
    assert(rejected);
    HANDLE threadToken = nullptr; ep::check(OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&threadToken)); CloseHandle(threadToken);
}
static int serverChild(const std::wstring &name, HANDLE ready, bool disconnect) {
    const auto time = ep::now(); ep::Listener listener(name,time,time + Timeout); ep::check(SetEvent(ready)); CloseHandle(ready);
    auto channel = accept(listener);
    assert(channel->beginRead(ep::now(),ep::now() + Timeout)); std::optional<Frame> frame;
    drive([&] { frame = channel->take(); return frame || channel->closed(); },{channel.get()});
    if (disconnect) { assert(channel->closed() && !frame); return 0; }
    assert(frame && channel->authenticated() && frame->kind == Kind::Hello);
    frame->kind = Kind::HelloAck; frame->id.engine = 42;
    assert(channel->beginWrite(*frame,ep::now(),ep::now() + Timeout));
    drive([&] { return channel->writeDone(); },{channel.get()});
    assert(channel->beginRead(ep::now(),ep::now() + Timeout));
    drive([&] { frame = channel->take(); return bool(frame); },{channel.get()});
    assert(frame->payload == data(MaxPayload).payload); frame->kind = Kind::Reply;
    assert(channel->beginWrite(*frame,ep::now(),ep::now() + Timeout));
    drive([&] { return channel->writeDone(); },{channel.get()});
    // Wait for peer shutdown: do not close the pipe while the echo remains unread.
    assert(channel->beginRead(ep::now(),ep::now() + Timeout));
    drive([&] { return channel->closed(); },{channel.get()}); return 0;
}
struct Child {
    Handle process;
    std::wstring name = ep::newName();
    explicit Child(bool disconnect = false) {
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE}; Handle ready(CreateEventW(&security,TRUE,FALSE,nullptr)); ep::check(bool(ready));
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<uint8_t> storage(size); auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        ep::check(InitializeProcThreadAttributeList(attributes,1,0,&size));
        struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Delete() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
        HANDLE inherited = ready.get(); ep::check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&inherited,sizeof(inherited),nullptr,nullptr));
        std::vector<wchar_t> executable(32768); const auto length = GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
        assert(length && length < executable.size());
        std::wstring command = L"\"" + std::wstring(executable.data(),length) + L"\" --server " + name + L" " +
            std::to_wstring(reinterpret_cast<uintptr_t>(ready.get())) + (disconnect ? L" disconnect" : L" echo");
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        PROCESS_INFORMATION info{};
        ep::check(CreateProcessW(executable.data(),command.data(),nullptr,nullptr,TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,nullptr,nullptr,&startup.StartupInfo,&info));
        process.reset(info.hProcess); CloseHandle(info.hThread);
        HANDLE events[]{ready.get(),process.get()};
        assert(WaitForMultipleObjects(2,events,FALSE,static_cast<DWORD>(Timeout)) == WAIT_OBJECT_0);
    }
    ~Child() { if (WaitForSingleObject(process.get(),0) == WAIT_TIMEOUT) { TerminateProcess(process.get(),99); WaitForSingleObject(process.get(),static_cast<DWORD>(Timeout)); } }
    void finish() {
        assert(WaitForSingleObject(process.get(),static_cast<DWORD>(Timeout)) == WAIT_OBJECT_0);
        DWORD code = 0; ep::check(GetExitCodeProcess(process.get(),&code)); assert(code == 0);
    }
};
static void separateProcess() {
    { Child child(true); bool rejected = false;
      try { ep::Channel::connect(child.name,GetCurrentProcess()); } catch (const ep::Error &e) { rejected = e.reason == ep::Failure::Authentication; }
      assert(rejected); child.finish(); }
    { Child child; auto channel = ep::Channel::connect(child.name,child.process.get());
      auto exchange = [&](Frame request) {
          assert(channel->beginRead(ep::now(),ep::now() + Timeout));
          assert(channel->beginWrite(request,ep::now(),ep::now() + Timeout));
          std::optional<Frame> response;
          drive([&] { if (!response) response = channel->take(); return response && channel->writeDone(); },{channel.get()});
          assert(response->payload == request.payload && response->id.connection == request.id.connection && response->id.request == request.id.request);
          return *response;
      };
      assert(exchange(hello()).kind == Kind::HelloAck);
      assert(exchange(data(MaxPayload)).kind == Kind::Reply); channel.reset(); child.finish();
      bool rejected = false;
      try { ep::Channel::connect(child.name,child.process.get()); } catch (const ep::Error &e) { rejected = e.reason == ep::Failure::Authentication; }
      assert(rejected); }
}
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 5 && std::wstring(argv[1]) == L"--server")
            return serverChild(argv[2],reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(argv[3]))),std::wstring(argv[4]) == L"disconnect");
        assert(argc == 1);
        namesAndAcl(); framingAndCapacity(); fragmentsAndCoalescing(); invalidFrames(); anonymousClient(); deadlinesAndDisconnect(); separateProcess();
        std::cout << "engine_pipe: authenticated local pipe, framing, deadlines, cancellation, child-process echo passed\n";
        return 0;
    } catch (const ep::Error &error) {
        std::cerr << "engine_pipe failure=" << static_cast<int>(error.reason) << " win32=" << error.code << '\n'; return 1;
    }
}
