// security.as — Permissive security policy for llm_in_loop
void setup() {
    http().allow();
    ws().allow();
    opcua().allow();
}