#include <sgrn/s7shell/S7Shell.hpp>
#include <set>
namespace sgrn::s7shell::shell
{

S7Shell* S7Shell::p_s_active_for_completion = nullptr;

static std::vector<std::string> g_completion_candidates;

char** S7Shell::completionDispatch(const char* tp_text, int /*start*/, int /*end*/) {
    rl_attempted_completion_over = 1; // don't fall back to filename completion
    if (!p_s_active_for_completion)
        return nullptr;

    std::string full(tp_text);
    size_t dot = full.find_last_of('.');
    std::string prefix_to_prepend;
    std::vector<std::string> names;

    if (dot == std::string::npos) {
        names = p_s_active_for_completion->completeTopLevel(full);
    } else {
        std::string t_object_chain = full.substr(0, dot);
        std::string t_member_prefix = full.substr(dot + 1);
        names = p_s_active_for_completion->completeMember(t_object_chain, t_member_prefix);
        prefix_to_prepend = t_object_chain + ".";
    }
    if (names.empty())
        return nullptr;

    g_completion_candidates.clear();
    for (auto& n : names)
        g_completion_candidates.push_back(prefix_to_prepend + n);

    return rl_completion_matches(tp_text, [](const char*, int t_state) -> char* {
        static size_t idx;
        if (t_state == 0)
            idx = 0;
        if (idx < g_completion_candidates.size())
            return strdup(g_completion_candidates[idx++].c_str());
        return nullptr;
    });
}

std::vector<std::string> S7Shell::completeTopLevel(const std::string& t_prefix) const {
    std::set<std::string> out;

    // Global functions (print, prettyJson, help, ...)
    for (asUINT i = 0; i < p_script_engine_->GetGlobalFunctionCount(); ++i) {
        asIScriptFunction* p_f = p_script_engine_->GetGlobalFunctionByIndex(i);
        std::string p_name = p_f->GetName();
        if (p_name.rfind(t_prefix, 0) == 0)
            out.insert(p_name);
    }

    // Global vars (REPL session variables: c, plc, db1, ...)
    asIScriptModule* p_mod = p_script_engine_->GetModule("main");
    if (!p_mod)
        p_mod = p_repl_module_;
    if (p_mod) {
        for (asUINT i = 0; i < p_mod->GetGlobalVarCount(); ++i) {
            const char* p_name = nullptr;
            p_mod->GetGlobalVar(i, &p_name);
            if (p_name && std::string(p_name).rfind(t_prefix, 0) == 0)
                out.insert(p_name);
        }
    }

    // Registered types (constructors: S7Client(), DTL(), ...)
    for (asUINT i = 0; i < p_script_engine_->GetObjectTypeCount(); ++i) {
        std::string p_name = p_script_engine_->GetObjectTypeByIndex(i)->GetName();
        if (p_name.rfind(t_prefix, 0) == 0)
            out.insert(p_name);
    }

    for (const char* kw : {"help", "exit", "quit"})
        if (std::string(kw).rfind(t_prefix, 0) == 0)
            out.insert(kw);

    return {out.begin(), out.end()};
}

std::vector<std::string> S7Shell::completeMember(const std::string& t_object_chain, const std::string& t_member_prefix) const {
    asIScriptModule* p_mod = p_script_engine_->GetModule("main");
    if (!p_mod)
        p_mod = p_repl_module_;
    if (!p_mod)
        return {};

    // Resolve the type of the first segment via the global-var table
    auto segments = [&] {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : t_object_chain) {
            if (c == '.') {
                parts.push_back(cur);
                cur.clear();
            } else
                cur += c;
        }
        parts.push_back(cur);
        return parts;
    }();

    int type_id = -1;
    for (asUINT i = 0; i < p_mod->GetGlobalVarCount(); ++i) {
        const char* p_name = nullptr;
        int tid = -1;
        p_mod->GetGlobalVar(i, &p_name, nullptr, &tid);
        if (p_name && segments[0] == p_name) {
            type_id = tid;
            break;
        }
    }
    if (type_id < 0)
        return {};

    asITypeInfo* p_ti = p_script_engine_->GetTypeInfoById(type_id);

    // Walk remaining segments through get_<seg>() return types
    for (size_t s = 1; s < segments.size() && p_ti; ++s) {
        asITypeInfo* p_next = nullptr;
        std::string wanted = "get_" + segments[s];
        for (asUINT m = 0; m < p_ti->GetMethodCount(); ++m) {
            asIScriptFunction* p_mf = p_ti->GetMethodByIndex(m);
            if (wanted == p_mf->GetName()) {
                int rtid = p_mf->GetReturnTypeId();
                p_next = p_script_engine_->GetTypeInfoById(rtid);
                break;
            }
        }
        p_ti = p_next;
    }
    if (!p_ti)
        return {};

    std::set<std::string> out;
    for (asUINT m = 0; m < p_ti->GetMethodCount(); ++m) {
        asIScriptFunction* p_mf = p_ti->GetMethodByIndex(m);
        std::string p_name = p_mf->GetName();
        std::string field;
        if (p_name.rfind("get_", 0) == 0)
            field = p_name.substr(4);
        else if (p_name.rfind("set_", 0) == 0)
            field = p_name.substr(4);
        else
            field = p_name; // plain method, e.g. toJson(), print()

        if (field.rfind(t_member_prefix, 0) == 0)
            out.insert(field);
    }
    return {out.begin(), out.end()};
}

} // namespace sgrn::s7shell::shell
