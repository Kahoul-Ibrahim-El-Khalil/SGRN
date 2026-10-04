// security.as — Permissive policy for the Tennessee Eastman demo skid.
// Production deployments should switch to strict + per-namespace rules.
void configure_security(SecurityPolicyStore@ policy) {
    policy.allow_all();
}
