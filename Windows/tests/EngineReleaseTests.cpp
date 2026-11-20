#include "../EngineRelease.h"
#include <shlobj.h>
#include <cassert>
#include <cstdio>
#include <iostream>

// Only HKCU test keys and temporary folders are written. HKLM / Program Files
// are read-only probes for the protection predicate.
namespace release=harukoto::engine::release;
using release::Entry;
using release::Parse;
using release::Status;

static const std::wstring Id=L"0123456789AB-CDEF01234567";
static const std::wstring Folder=L"C:\\Program Files\\Meowpuut\\IME\\"+Id;
static const std::wstring HashA(64,L'a'), HashB(64,L'b');

static std::vector<std::wstring> lines(const std::wstring &id=Id, const std::wstring &sequence=L"1",
        const std::wstring &protocol=L"1", const std::wstring &folder=Folder) {
    return {release::FormatLine,L"id="+id,L"sequence="+sequence,L"protocol="+protocol,
        L"host="+folder+L"\\harukoto_engine_host.exe",L"host-sha256="+HashA,
        L"owner="+folder+L"\\harukoto_engine_owner.exe",L"owner-sha256="+HashB};
}
static Parse parse(const std::vector<std::wstring> &value, const std::wstring &name=Id) {
    Entry entry; return release::parse(name,value,entry);
}
static void parsing() {
    Entry entry;
    assert(release::parse(Id,lines(),entry)==Parse::Ok);
    assert(entry.id==Id && entry.sequence==1 && entry.protocol==1);
    assert(entry.host==Folder+L"\\harukoto_engine_host.exe" && entry.owner==Folder+L"\\harukoto_engine_owner.exe");
    assert(entry.hostSha256==std::string(64,'a') && entry.ownerSha256==std::string(64,'b'));
    assert(parse(lines(Id,L"9007199254740991",L"65535"))==Parse::Ok);
    // Unknown generations are skipped; malformed headers are invalid.
    auto value=lines(); value[0]=L"format=harukoto-engine-release/2";
    assert(parse(value)==Parse::Unsupported);
    value.resize(1); assert(parse(value)==Parse::Unsupported);
    for (const auto *header : {L"format=harukoto-engine-release/", L"format=harukoto-engine-release/02",
            L"format=other/1", L"", L"FORMAT=harukoto-engine-release/1"}) {
        value=lines(); value[0]=header; assert(parse(value)==Parse::Invalid);
    }
    assert(parse({})==Parse::Invalid);
    value=lines(); value.push_back(L"extra=1"); assert(parse(value)==Parse::Invalid);
    value=lines(); value.pop_back(); assert(parse(value)==Parse::Invalid);
    value=lines(); std::swap(value[4],value[6]); assert(parse(value)==Parse::Invalid);
    value=lines(); std::swap(value[2],value[3]); assert(parse(value)==Parse::Invalid);
    // Value name and id line must agree; ids are a restricted charset.
    assert(parse(lines(),L"other")==Parse::Invalid);
    for (const auto *id : {L"", L"-abc", L".abc", L"a b", L"a\\b", L"a/b", L"a:b"}) assert(parse(lines(id),id)==Parse::Invalid);
    assert(parse(lines(std::wstring(241,L'A')),std::wstring(241,L'A'))==Parse::Invalid);
    for (const auto *sequence : {L"0", L"01", L"-1", L"+1", L"1.0", L"9007199254740992", L"99999999999999999", L" 1", L""})
        assert(parse(lines(Id,sequence))==Parse::Invalid);
    for (const auto *protocol : {L"0", L"65536", L"01", L"x"}) assert(parse(lines(Id,L"1",protocol))==Parse::Invalid);
    // Digests are canonical lower-case hex, exactly 64 characters.
    value=lines(); value[5]=L"host-sha256="+std::wstring(64,L'A'); assert(parse(value)==Parse::Invalid);
    value=lines(); value[5]=L"host-sha256="+std::wstring(63,L'a'); assert(parse(value)==Parse::Invalid);
    value=lines(); value[7]=L"owner-sha256="+std::wstring(64,L'g'); assert(parse(value)==Parse::Invalid);
    // Paths: absolute drive paths, no relative / UNC / device / stream forms.
    for (const auto *folder : {L"Program Files\\x", L"C:Program Files", L"\\\\server\\share", L"\\\\?\\C:\\x",
            L"C:/Program Files", L"C:\\a\\..", L"C:\\a\\.\\b", L"C:\\a\\\\b", L"C:\\a.\\b", L"C:\\a \\b",
            L"C:\\a:stream\\b", L"C:\\a\x0001\\b"}) {
        const std::wstring prefix(folder);
        value=lines(Id,L"1",L"1",prefix+L"\\"+Id); assert(parse(value)==Parse::Invalid);
    }
    assert(parse(lines(Id,L"1",L"1",L"C:\\"+std::wstring(1100,L'x')+L"\\"+Id))==Parse::Invalid);
    // Fixed program names, shared parent named by the id.
    value=lines(); value[4]=L"host="+Folder+L"\\other.exe"; assert(parse(value)==Parse::Invalid);
    value=lines(); value[6]=L"owner="+Folder+L"\\harukoto_engine_host.exe"; assert(parse(value)==Parse::Invalid);
    value=lines(); value[6]=L"owner=C:\\Program Files\\Meowpuut\\IME\\other\\harukoto_engine_owner.exe"; assert(parse(value)==Parse::Invalid);
    assert(parse(lines(Id,L"1",L"1",L"C:\\Program Files\\Meowpuut\\IME\\other"))==Parse::Invalid);
    assert(parse(lines(Id,L"1",L"1",L"C:\\"+Id))==Parse::Ok);
    value=lines(); value[1].push_back(L'\0'); assert(parse(value)==Parse::Invalid);
    std::wcout<<L"PASS: strict release entry parsing\n";
}
static std::vector<BYTE> multi(const std::vector<std::wstring> &value) {
    std::wstring text;
    for (const auto &line : value) { text+=line; text.push_back(L'\0'); }
    text.push_back(L'\0');
    std::vector<BYTE> bytes(text.size()*2); std::memcpy(bytes.data(),text.data(),bytes.size()); return bytes;
}
static void splitting() {
    std::vector<std::wstring> out;
    assert(release::splitMulti(multi(lines()),out) && out==lines());
    auto bytes=multi(lines()); bytes.pop_back(); assert(!release::splitMulti(bytes,out));
    bytes=multi(lines()); bytes.resize(bytes.size()-2); assert(!release::splitMulti(bytes,out));
    bytes=multi(lines()); bytes.push_back(L'x'); bytes.push_back(0); assert(!release::splitMulti(bytes,out));
    bytes=multi({}); assert(!release::splitMulti(bytes,out));
    auto value=lines(); value.insert(value.begin()+2,L""); assert(!release::splitMulti(multi(value),out));
    assert(!release::splitMulti(multi(std::vector<std::wstring>(17,L"x")),out));
    assert(!release::splitMulti(multi({std::wstring(5000,L'x')}),out));
    std::wcout<<L"PASS: bounded REG_MULTI_SZ decoding\n";
}
static bool sddl(const wchar_t *text, release::Object object) {
    release::detail::Local descriptor;
    assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(text,SDDL_REVISION_1,&descriptor.value,nullptr));
    return release::protectedDescriptor(descriptor.value,object);
}
static void descriptors() {
    using release::Object;
    const wchar_t *installer=L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";
    // HKLM\SOFTWARE-like: admins write, users / app containers read.
    assert(sddl(L"O:BAG:SYD:(A;;KA;;;SY)(A;;KA;;;BA)(A;;KR;;;BU)(A;;KR;;;AC)(A;CIIO;GA;;;CO)",Object::RegistryKey));
    // Program Files-like with TrustedInstaller owner.
    const std::wstring files=std::wstring(L"O:")+installer+L"D:(A;;FA;;;"+installer+L")(A;;0x1301bf;;;SY)(A;;0x1301bf;;;BA)(A;;0x1200a9;;;BU)(A;;0x1200a9;;;AC)(A;OICIIO;GA;;;CO)(D;;FA;;;AN)";
    assert(sddl(files.c_str(),Object::Directory));
    assert(!sddl(L"O:BAG:SYD:(A;;KA;;;SY)(A;;KA;;;BA)(A;;KW;;;BU)",Object::RegistryKey));
    assert(!sddl(L"O:BAG:SYD:(A;;KA;;;SY)(A;;KA;;;BA)(A;;0x2;;;WD)",Object::RegistryKey));    // KEY_SET_VALUE
    assert(!sddl(L"O:BAG:SYD:(A;;KA;;;SY)(A;;KA;;;BA)(A;;WD;;;IU)",Object::RegistryKey));     // WRITE_DAC
    assert(!sddl(L"O:BAG:SYD:(A;;KA;;;SY)(A;;KA;;;BA)(A;;GA;;;AU)",Object::RegistryKey));
    assert(!sddl(L"O:BUG:SYD:(A;;KA;;;SY)(A;;KA;;;BA)",Object::RegistryKey));                 // non-admin owner
    assert(!sddl(L"O:BAG:SYD:NO_ACCESS_CONTROL",Object::RegistryKey));                        // NULL DACL
    assert(!sddl(L"O:BAG:SY",Object::RegistryKey));                                           // no DACL
    assert(!sddl(L"O:BAD:(A;;0x2;;;BU)(A;;FA;;;BA)",Object::Directory));                     // FILE_ADD_FILE
    assert(!sddl(L"O:BAD:(A;;0x4;;;BU)(A;;FA;;;BA)",Object::Directory));                     // FILE_ADD_SUBDIRECTORY
    assert(!sddl(L"O:BAD:(A;;0x40;;;AC)(A;;FA;;;BA)",Object::Directory));                    // FILE_DELETE_CHILD
    assert(!sddl(L"O:BAD:(A;;SD;;;OW)(A;;FA;;;BA)",Object::Directory));                      // owner rights delete
    assert(sddl(L"O:BAD:(A;OICIIO;GA;;;BU)(A;;FA;;;BA)",Object::Directory));                 // inherit-only only
    // Real HKLM\SOFTWARE carries an effective CREATOR OWNER ACE.
    assert(sddl(L"O:SYG:SYD:PAI(A;CI;KA;;;CO)(A;CI;KA;;;SY)(A;CI;KA;;;BA)(A;CI;KR;;;BU)(A;CI;KR;;;AC)",Object::RegistryKey));
    assert(sddl(L"O:BAD:(A;;FA;;;CG)(A;;FA;;;BA)",Object::Directory));
    // The exact descriptor the installer applies to its keys.
    assert(sddl(L"O:BAD:PAI(A;CI;KA;;;SY)(A;CI;KA;;;BA)(A;CI;KR;;;BU)(A;CI;KR;;;AC)(A;CI;KR;;;S-1-15-3-1024-1065365936-1281604716-3511738428-1654721687-432734479-3232135806-4053264122-3456934681)",Object::RegistryKey));
    assert(!sddl(L"O:BAD:(XA;;FA;;;WD;(WIN://SYSAPPID Contains \"x\"))(A;;FA;;;BA)",Object::Directory));
    assert(!release::protectedDescriptor(nullptr,Object::Directory));
    std::wcout<<L"PASS: security descriptor writer allowlist\n";
}
static std::wstring known(REFKNOWNFOLDERID id) {
    PWSTR value=nullptr; assert(SHGetKnownFolderPath(id,0,nullptr,&value)==S_OK);
    std::wstring result(value); CoTaskMemFree(value); return result;
}
static void realObjects() {
    wchar_t system[MAX_PATH]{}; assert(GetSystemDirectoryW(system,MAX_PATH));
    assert(release::protectedDirectory(system));
    assert(release::protectedDirectory(known(FOLDERID_ProgramFiles)));
    wchar_t temporary[MAX_PATH+1]{}; assert(GetTempPathW(MAX_PATH+1,temporary));
    std::wstring temp(temporary); while (!temp.empty() && temp.back()==L'\\') temp.pop_back();
    assert(!release::protectedDirectory(temp));
    assert(!release::protectedDirectory(temp+L"\\harukoto-missing-"+std::to_wstring(GetCurrentProcessId())));
    assert(!release::protectedDirectory(L"relative\\path"));
    release::detail::Key software;
    assert(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE",0,READ_CONTROL | KEY_WOW64_64KEY,&software.value)==ERROR_SUCCESS);
    assert(release::protectedHandle(software.value,SE_REGISTRY_KEY,release::Object::RegistryKey));
    release::detail::Key user;
    assert(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software",0,READ_CONTROL,&user.value)==ERROR_SUCCESS);
    assert(!release::protectedHandle(user.value,SE_REGISTRY_KEY,release::Object::RegistryKey));
    // The production location is either absent or a protected HKLM key; this
    // machine's state is only reported, never modified.
    const auto production=release::read();
    assert(production.status==Status::Missing || production.status==Status::Ready);
    std::wcout<<L"PASS: real protected / user-writable objects (production source "
        <<(production.status==Status::Missing ? L"missing" : L"present")<<L")\n";
}
struct TestKey {
    std::wstring path;
    release::detail::Key key;
    TestKey() {
        path=L"Software\\HarukotoTest\\Release-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
        assert(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key.value,nullptr)==ERROR_SUCCESS);
    }
    ~TestKey() {
        RegCloseKey(key.value); key.value=nullptr;
        RegDeleteTreeW(HKEY_CURRENT_USER,path.c_str()); RegDeleteKeyW(HKEY_CURRENT_USER,path.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER,L"Software\\HarukotoTest");
    }
    void put(const std::wstring &name, const std::vector<BYTE> &data, DWORD type=REG_MULTI_SZ) {
        assert(RegSetValueExW(key.value,name.c_str(),0,type,data.data(),static_cast<DWORD>(data.size()))==ERROR_SUCCESS);
    }
    void put(const std::wstring &name, const std::vector<std::wstring> &value) { put(name,multi(value)); }
    void remove(const std::wstring &name) { assert(RegDeleteValueW(key.value,name.c_str())==ERROR_SUCCESS); }
    release::Source source(bool protect=false) const { return {HKEY_CURRENT_USER,path,protect}; }
};
static std::wstring folder(const std::wstring &id) { return L"C:\\Program Files\\Meowpuut\\IME\\"+id; }
static void registry() {
    release::Source missing{HKEY_CURRENT_USER,L"Software\\HarukotoTest\\Absent-"+std::to_wstring(GetCurrentProcessId()),false};
    assert(release::read(missing).status==Status::Missing);
    assert(!release::select(release::read(missing),1));
    assert(release::read({HKEY_CURRENT_USER,L"",false}).status==Status::Invalid);
    TestKey key;
    auto snapshot=release::read(key.source());
    assert(snapshot.status==Status::Ready && snapshot.entries.empty() && !release::select(snapshot,1));
    // Upgrade window: two versions side by side, plus one for another protocol.
    key.put(L"OLD",lines(L"OLD",L"3",L"1",folder(L"OLD")));
    key.put(L"NEW",lines(L"NEW",L"4",L"1",folder(L"NEW")));
    key.put(L"NEXT",lines(L"NEXT",L"5",L"2",folder(L"NEXT")));
    snapshot=release::read(key.source());
    assert(snapshot.status==Status::Ready && snapshot.entries.size()==3 && !snapshot.rejected);
    assert(release::select(snapshot,1)->id==L"NEW" && release::select(snapshot,2)->id==L"NEXT" && !release::select(snapshot,3));
    // Corrupt entries are rejected individually; valid siblings stay usable.
    key.put(L"TYPE",multi(lines(L"TYPE",L"9",L"1",folder(L"TYPE"))),REG_BINARY);
    auto text=multi({L"x"}); key.put(L"SZ",text,REG_SZ);
    auto truncated=multi(lines(L"CUT",L"9",L"1",folder(L"CUT"))); truncated.resize(truncated.size()-4);
    key.put(L"CUT",truncated);
    snapshot=release::read(key.source());
    assert(snapshot.status==Status::Ready && snapshot.entries.size()==3 && snapshot.rejected==3);
    assert(release::select(snapshot,1)->id==L"NEW");
    key.remove(L"TYPE"); key.remove(L"SZ"); key.remove(L"CUT");
    // Oversize value and a mismatched name are rejected, unknown format skipped.
    key.put(L"BIG",multi({std::wstring(release::MaxValueBytes,L'x')}));
    auto renamed=lines(L"OTHER",L"9",L"1",folder(L"OTHER")); key.put(L"RENAMED",renamed);
    auto future=lines(L"FUTURE",L"9",L"1",folder(L"FUTURE")); future[0]=L"format=harukoto-engine-release/2"; key.put(L"FUTURE",future);
    snapshot=release::read(key.source());
    assert(snapshot.status==Status::Ready && snapshot.entries.size()==3 && snapshot.rejected==2 && snapshot.unsupported==1);
    assert(release::select(snapshot,1)->id==L"NEW");
    key.remove(L"BIG"); key.remove(L"RENAMED"); key.remove(L"FUTURE");
    // Overlong value name.
    const std::wstring longName(release::MaxIdLength+5,L'L');
    key.put(longName,lines(longName,L"9",L"1",folder(longName)));
    snapshot=release::read(key.source());
    assert(snapshot.status==Status::Ready && snapshot.entries.size()==3 && snapshot.rejected==1);
    key.remove(longName);
    // A tie is ambiguous and never approves either entry.
    key.put(L"TWIN",lines(L"TWIN",L"4",L"1",folder(L"TWIN")));
    snapshot=release::read(key.source());
    assert(snapshot.entries.size()==4 && !release::select(snapshot,1) && release::select(snapshot,2)->id==L"NEXT");
    key.remove(L"TWIN");
    // More values than the bound fails the whole source closed.
    for (int i=0;i<6;++i) { const auto id=L"EXTRA"+std::to_wstring(i); key.put(id,lines(id,std::to_wstring(10+i),L"1",folder(id))); }
    snapshot=release::read(key.source());
    assert(snapshot.status==Status::Invalid && snapshot.error==ERROR_TOO_MANY_NAMES && snapshot.entries.empty() && !release::select(snapshot,1));
    for (int i=0;i<6;++i) key.remove(L"EXTRA"+std::to_wstring(i));
    // A user-writable key is never trusted when protection is required.
    snapshot=release::read(key.source(true));
    assert(snapshot.status==Status::Unprotected && snapshot.entries.empty() && !release::select(snapshot,1));
    std::wcout<<L"PASS: registry source, upgrade window, corruption, bounds and ACL rejection\n";
}
static std::string utf8(const std::wstring &text) {
    if (text.empty()) return {};
    const int size=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string out(size,'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),size,nullptr,nullptr);
    return out;
}
// Used by the installer test: read what the PowerShell writer produced.
static int dump(const std::wstring &subkey, uint16_t protocol) {
    const auto snapshot=release::read({HKEY_CURRENT_USER,subkey,false});
    const char *names[]={"ready","missing","unprotected","invalid"};
    std::printf("status\t%s\t%zu\t%zu\n",names[static_cast<int>(snapshot.status)],snapshot.rejected,snapshot.unsupported);
    for (const auto &entry : snapshot.entries) {
        std::printf("entry\t%s\t%llu\t%u\t%s\t%s\t%s\t%s\n",utf8(entry.id).c_str(),static_cast<unsigned long long>(entry.sequence),
            static_cast<unsigned>(entry.protocol),utf8(entry.host.native()).c_str(),entry.hostSha256.c_str(),
            utf8(entry.owner.native()).c_str(),entry.ownerSha256.c_str());
    }
    const auto chosen=release::select(snapshot,protocol);
    std::printf("selected\t%s\n",chosen ? utf8(chosen->id).c_str() : "");
    return 0;
}
int wmain(int argc, wchar_t **argv) {
    if (argc==4 && std::wstring(argv[1])==L"--dump") return dump(argv[2],static_cast<uint16_t>(std::stoul(argv[3])));
    parsing(); splitting(); descriptors(); realObjects(); registry();
    std::wcout<<L"PASS: engine release source\n";
    return 0;
}
