// security.as — Permissive security policy for llm_in_loop
void configure_security(SecurityPolicyStore@ policy) {
    policy.allow_all();
}