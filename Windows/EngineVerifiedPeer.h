#pragma once
#include "EngineDeployment.h"

namespace harukoto::engine {
// Independent deployment validation of a PRIVATE trusted process handoff.
// Never manufacture that handoff with OpenProcess(publicPid). The endpoint
// name is only a locator; Channel still binds it to this exact kernel object.
class EngineVerifiedPeer {
    std::shared_ptr<const EngineDeployment> deployment_;
    pipe::Handle process_;
public:
    EngineVerifiedPeer(std::shared_ptr<const EngineDeployment> deployment, HANDLE trustedProcess)
        : deployment_(std::move(deployment)) {
        if (!deployment_) throw std::invalid_argument("verified peer requires deployment policy");
        pipe::processIdentityOnly();
        HANDLE raw=nullptr;
        pipe::check(DuplicateHandle(GetCurrentProcess(),trustedProcess,GetCurrentProcess(),&raw,
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,FALSE,0),pipe::Failure::Authentication);
        process_.reset(raw);
        if (!(pipe::LogonIdentity::fromProcess(process_.get()) == pipe::LogonIdentity::fromProcess(GetCurrentProcess())))
            throw pipe::Error(pipe::Failure::Authentication,ERROR_ACCESS_DENIED);
        deployment_->verify(process_.get());
    }
    EngineVerifiedPeer(const EngineVerifiedPeer &)=delete;
    EngineVerifiedPeer &operator=(const EngineVerifiedPeer &)=delete;
    std::unique_ptr<pipe::Channel> connect(const std::wstring &name) const {
        // Check actual exit before opening the locator, then check again after
        // the connected pipe has been bound to the retained process object.
        deployment_->verify(process_.get());
        auto channel=pipe::Channel::connect(name,process_.get());
        deployment_->verify(process_.get());
        channel->deployment_=deployment_;
        return channel; // No Hello, control request or query was sent here.
    }
    HANDLE processHandle() const { return process_.get(); } // Borrowed query/wait only.
};
} // namespace harukoto::engine
