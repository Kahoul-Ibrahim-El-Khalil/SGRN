// =============================================================================
// Replay.cpp — AngelScript bindings for WalReplayer
// =============================================================================

#include <sgrn/s7shell/bindings/registration.hpp>
#include <sgrn/plcsim/replay/WalReplayer.hpp>

#include <angelscript.h>

namespace sgrn::s7shell::bindings
{

using namespace sgrn::s7shell::shell;
using sgrn::plcsim::replay::WalReplayer;

class WalReplayerWrapper {
public:
    explicit WalReplayerWrapper(const std::string& t_archive_path) {
        replayer_ = std::make_unique<WalReplayer>(t_archive_path);
    }

    WalReplayerWrapper(const std::string& t_archive_path, PlcRuntimeWrapper* tp_rt) {
        if (tp_rt) {
            tp_rt->addRef();
            rt_ref_ = tp_rt;
            replayer_ = std::make_unique<WalReplayer>(t_archive_path, tp_rt->getImpl());
        } else {
            replayer_ = std::make_unique<WalReplayer>(t_archive_path);
        }
    }

    ~WalReplayerWrapper() {
        if (rt_ref_)
            rt_ref_->release();
    }

    void addRef() {
        ref_count_++;
    }
    void release() {
        if (--ref_count_ == 0)
            delete this;
    }

    void speed(double factor) {
        replayer_->speed(factor);
    }
    double speed() const {
        return replayer_->speed();
    }
    bool run() {
        return replayer_->run();
    }

private:
    PlcRuntimeWrapper* rt_ref_{nullptr};
    std::unique_ptr<WalReplayer> replayer_;
    int ref_count_{1};
};

static WalReplayerWrapper* WalReplayer_Factory(const std::string& t_archive_path) {
    return new WalReplayerWrapper(t_archive_path);
}

static WalReplayerWrapper* WalReplayer_FactoryWithRt(const std::string& t_archive_path, PlcRuntimeWrapper* tp_rt) {
    return new WalReplayerWrapper(t_archive_path, tp_rt);
}

Result<void, std::string> registerReplayTypes(asIScriptEngine* tp_engine) {
    int r = 0;

    SGRN_AS_TYPE(tp_engine, "WalReplayer");
    SGRN_AS_REFCOUNTED(tp_engine, "WalReplayer", WalReplayerWrapper);

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour(
        "WalReplayer", asBEHAVE_FACTORY, "WalReplayer@ f(const string &in)", asFUNCTION(WalReplayer_Factory), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour("WalReplayer", asBEHAVE_FACTORY, "WalReplayer@ f(const string &in, PlcRuntime@)",
        asFUNCTION(WalReplayer_FactoryWithRt), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "WalReplayer", "void speed(double)", asMETHODPR(WalReplayerWrapper, speed, (double), void), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "WalReplayer", "double speed() const", asMETHODPR(WalReplayerWrapper, speed, () const, double), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("WalReplayer", "bool run()", asMETHOD(WalReplayerWrapper, run), asCALL_THISCALL));

    return {};
}

} // namespace sgrn::s7shell::bindings
