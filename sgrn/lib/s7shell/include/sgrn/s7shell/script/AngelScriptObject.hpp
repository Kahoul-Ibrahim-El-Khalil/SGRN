#pragma once

namespace sgrn::s7shell
{

// Common root for every AngelScript reference-counted object: the engine
// calls addRef()/release() through asBEHAVE_ADDREF/asBEHAVE_RELEASE, and
// C++ holders (proxies, batches, wrappers keeping a runtime alive) use the
// same pair. Plain non-atomic counter — the same behavior the dozen
// hand-rolled copies had; classes with teardown logic put it in their
// destructor (virtual dispatch runs it from release()).
class AngelScriptObject {
public:
    void addRef() {
        ++ref_count_;
    }
    void release() {
        if (--ref_count_ == 0)
            delete this;
    }

protected:
    virtual ~AngelScriptObject() = default;

private:
    int ref_count_{1};
};

} // namespace sgrn::s7shell
